package org.dosfer.receiver;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.zip.CRC32;

public final class Protocol {
    public static final int DATA=1, END_WINDOW=2, CALIBRATION=3, CHAIN_XOR=4, BLOCK_XOR=5;
    public static final int SESSION=1, DIRECTORY=2, FILE_BEGIN=3, FILE_DATA=4, FILE_END=5, TRANSFER_END=6;
    public static final int FRAME_HEADER=48, RECORD_HEADER=24;
    public static final int FLAG_REPEATED=0x0001, FLAG_PAIR_WHITENED=0x0004, FLAG_WHITENED=0x0008;

    public static final class Frame {
        public final int kind, flags, window, windowIndex, windowCount, payloadLength;
        public final long session, globalIndex, streamId, streamOffset;
        public final byte[] payload;
        Frame(int kind,int flags,long session,int window,long globalIndex,int wi,int wc,long sid,long off,byte[] payload) {
            this.kind=kind;this.flags=flags;this.session=session;this.window=window;this.globalIndex=globalIndex;
            this.windowIndex=wi;this.windowCount=wc;this.streamId=sid;this.streamOffset=off;
            this.payload=payload;this.payloadLength=payload.length;
        }
    }
    public static final class Record {
        public final int type, flags; public final long recordId,fileId; public final byte[] body;
        Record(int type,int flags,long rid,long fid,byte[] body){this.type=type;this.flags=flags;this.recordId=rid;this.fileId=fid;this.body=body;}
    }
    private static long u32(ByteBuffer b){return Integer.toUnsignedLong(b.getInt());}
    public static long crc(byte[] b,int off,int len){CRC32 c=new CRC32();c.update(b,off,len);return c.getValue();}
    public static Frame parseFrame(byte[] raw) {
        if(raw==null||raw.length<FRAME_HEADER)throw new IllegalArgumentException("short frame");
        ByteBuffer b=ByteBuffer.wrap(raw).order(ByteOrder.BIG_ENDIAN);
        byte[] magic=new byte[4];b.get(magic);
        if(!new String(magic,StandardCharsets.US_ASCII).equals("DQR1"))throw new IllegalArgumentException("magic");
        int ver=Byte.toUnsignedInt(b.get()),kind=Byte.toUnsignedInt(b.get()),flags=Short.toUnsignedInt(b.getShort());
        long session=u32(b);int window=b.getInt();long global=u32(b);int wi=Short.toUnsignedInt(b.getShort()),wc=Short.toUnsignedInt(b.getShort());
        long sid=u32(b),off=u32(b);int plen=Short.toUnsignedInt(b.getShort()),hlen=Short.toUnsignedInt(b.getShort());
        long pcrc=u32(b),hcrc=u32(b),reserved=u32(b);
        if(ver!=1||hlen!=48||reserved!=0||session==0||(flags&~0x000f)!=0||kind<1||kind>5||raw.length!=48+plen)
            throw new IllegalArgumentException("unsupported header");
        byte[] header=raw.clone();header[40]=header[41]=header[42]=header[43]=0;
        if(crc(header,0,48)!=hcrc)throw new IllegalArgumentException("header crc");
        if(crc(raw,48,plen)!=pcrc)throw new IllegalArgumentException("payload crc");
        byte[] payload=new byte[plen];System.arraycopy(raw,48,payload,0,plen);
        if((flags&FLAG_PAIR_WHITENED)!=0)whitenPayloadPair(payload,0,plen,session,global);
        else if((flags&FLAG_WHITENED)!=0)whitenPayload(payload,0,plen,session,global);
        return new Frame(kind,flags,session,window,global,wi,wc,sid,off,payload);
    }
    public static byte[] encodeFrame(int kind,int flags,long session,int window,long global,int wi,int wc,long sid,long off,byte[] plain) {
        if(session==0||kind<1||kind>5||plain==null||plain.length>0xffff)throw new IllegalArgumentException("frame fields");
        byte[] raw=new byte[FRAME_HEADER+plain.length];ByteBuffer b=ByteBuffer.wrap(raw).order(ByteOrder.BIG_ENDIAN);
        b.put("DQR1".getBytes(StandardCharsets.US_ASCII));b.put((byte)1);b.put((byte)kind);b.putShort((short)flags);
        b.putInt((int)session);b.putInt(window);b.putInt((int)global);b.putShort((short)wi);b.putShort((short)wc);
        b.putInt((int)sid);b.putInt((int)off);b.putShort((short)plain.length);b.putShort((short)FRAME_HEADER);
        b.putInt(0);b.putInt(0);b.putInt(0);System.arraycopy(plain,0,raw,FRAME_HEADER,plain.length);
        if((flags&FLAG_PAIR_WHITENED)!=0)whitenPayloadPair(raw,FRAME_HEADER,plain.length,session,global);
        else if((flags&FLAG_WHITENED)!=0)whitenPayload(raw,FRAME_HEADER,plain.length,session,global);
        b.putInt(36,(int)crc(raw,FRAME_HEADER,plain.length));b.putInt(40,0);b.putInt(40,(int)crc(raw,0,FRAME_HEADER));return raw;
    }
    public static byte[] recoverChain(Frame chain,byte[] known,boolean knownIsLeft) {
        if(chain.kind!=CHAIN_XOR||known==null)throw new IllegalArgumentException("chain");
        int left=(int)(chain.streamId>>>16),right=(int)(chain.streamId&0xffff),expected=knownIsLeft?left:right;
        if(left<RECORD_HEADER||right<RECORD_HEADER||known.length!=expected||chain.payload.length!=Math.max(left,right))
            throw new IllegalArgumentException("chain lengths");
        int missing=knownIsLeft?right:left;byte[] out=new byte[missing];
        for(int i=0;i<missing;i++)out[i]=(byte)(chain.payload[i]^(i<known.length?known[i]:0));
        parseRecord(out);return out;
    }
    public static byte[] recoverBlock(Frame parity,byte[][] members,int missing) {
        if(parity.kind!=BLOCK_XOR||parity.flags!=FLAG_WHITENED||members==null||parity.streamOffset!=0||
           parity.streamId<1||parity.streamId>64||members.length!=(int)parity.streamId||
           missing<0||missing>=members.length||members[missing]!=null)
            throw new IllegalArgumentException("block");
        byte[] out=parity.payload.clone();
        for(int m=0;m<members.length;m++)if(m!=missing){byte[] known=members[m];
            if(known==null||known.length>out.length)throw new IllegalArgumentException("block members");
            for(int i=0;i<known.length;i++)out[i]^=known[i];}
        if(out.length<RECORD_HEADER)throw new IllegalArgumentException("block payload");
        long body=((long)(out[16]&255)<<24)|((long)(out[17]&255)<<16)|
            ((long)(out[18]&255)<<8)|(out[19]&255);long total=RECORD_HEADER+body;
        if(total>out.length||total>0xffff)throw new IllegalArgumentException("block length");
        for(int i=(int)total;i<out.length;i++)if(out[i]!=0)throw new IllegalArgumentException("block padding");
        byte[] recovered=new byte[(int)total];System.arraycopy(out,0,recovered,0,(int)total);
        parseRecord(recovered);return recovered;
    }
    static void whitenPayload(byte[] data,int offset,int length,long session,long globalIndex) {
        int state=(int)(session^(globalIndex*0x9E3779B9L)^0xD05FE123L);
        if(state==0)state=0xA5A5A5A5;
        int end=offset+length;
        while(offset<end) {
            state^=state<<13;state^=state>>>17;state^=state<<5;
            int key=state;
            for(int i=0;i<4&&offset<end;i++,offset++){data[offset]^=(byte)key;key>>>=8;}
        }
    }
    static void whitenPayloadPair(byte[] data,int offset,int length,long session,long globalIndex) {
        int left=(int)(session^(globalIndex*0x9E3779B9L)^0xD05FE123L);
        int right=(int)(session^((globalIndex+1)*0x9E3779B9L)^0xD05FE123L);
        if(left==0)left=0xA5A5A5A5;if(right==0)right=0xA5A5A5A5;
        int end=offset+length;
        while(offset<end){left^=left<<13;left^=left>>>17;left^=left<<5;right^=right<<13;right^=right>>>17;right^=right<<5;
            int key=left^right;for(int i=0;i<4&&offset<end;i++,offset++){data[offset]^=(byte)key;key>>>=8;}}
    }
    public static Record parseRecord(byte[] raw) {
        if(raw==null||raw.length<24)throw new IllegalArgumentException("short record");
        ByteBuffer b=ByteBuffer.wrap(raw).order(ByteOrder.BIG_ENDIAN);byte[] magic=new byte[4];b.get(magic);
        int ver=Byte.toUnsignedInt(b.get()),type=Byte.toUnsignedInt(b.get()),flags=Short.toUnsignedInt(b.getShort());
        long rid=u32(b),fid=u32(b),size=u32(b),checksum=u32(b);
        if(!new String(magic,StandardCharsets.US_ASCII).equals("DQRC")||ver!=1||type<1||type>6||flags!=0||size!=raw.length-24)
            throw new IllegalArgumentException("record header");
        if(crc(raw,24,(int)size)!=checksum)throw new IllegalArgumentException("record crc");
        byte[] body=new byte[(int)size];System.arraycopy(raw,24,body,0,(int)size);
        return new Record(type,flags,rid,fid,body);
    }
    public static String safePath(byte[] body,int offset,int length) {
        if(length<=0||length>1024||offset<0||offset+length>body.length)throw new IllegalArgumentException("path length");
        String p=new String(body,offset,length,StandardCharsets.UTF_8);
        if(p.startsWith("/")||p.contains("\\")||p.indexOf('\0')>=0)throw new IllegalArgumentException("absolute path");
        String[] parts=p.split("/",-1);if(parts.length==0||(parts[0].length()>=2&&parts[0].charAt(1)==':'))throw new IllegalArgumentException("drive path");
        for(String part:parts){int bytes=part.getBytes(StandardCharsets.UTF_8).length;
            if(part.isEmpty()||part.equals(".")||part.equals("..")||bytes>255)throw new IllegalArgumentException("unsafe component");
            for(int i=0;i<part.length();i++)if(part.charAt(i)<32)throw new IllegalArgumentException("control in path");}
        return String.join("/",parts);
    }
    private Protocol(){}
}
