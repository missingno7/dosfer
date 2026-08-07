package org.dosfer.receiver;

import android.content.Context;
import java.io.*;
import java.util.*;

public final class SessionStore {
    public enum Result { STORED, DUPLICATE, INVALID, OTHER_SESSION, CALIBRATION }
    private enum Recovered { STORED, DUPLICATE, CONFLICT }
    public static final class Stats {
        public long session;public int window,expected,uniqueWindow,duplicates,invalid,other;
        public long uniqueTotal,totalPayload,calibrationUnique,calibrationMissed;public double decodedFps,usefulBps,avgLatencyMs;
        public String missing="-",calibration="WAITING FOR PROTOCOL QR";public boolean complete;
    }
    private final Context context;private long active;
    private final Map<Integer,Integer> windowExpected=new HashMap<>();
    private final Map<Integer,BitSet> windowSeen=new HashMap<>();
    private long duplicates,invalid,other,totalPayload,endIndex=-1,firstStoredNanos;
    private final ArrayDeque<Long> decodeTimes=new ArrayDeque<>();
    private double latencySum;private long latencyCount,calLast=-1,calMissed,calUnique,calDuplicates;

    public SessionStore(Context c){context=c.getApplicationContext();active=context.getSharedPreferences("dosfer",0).getLong("active",0);reload();}
    private File dir(){File d=new File(context.getFilesDir(),"sessions/"+Long.toUnsignedString(active,16));d.mkdirs();return d;}
    private File file(long index){return new File(dir(),String.format(Locale.US,"frame_%010d.dqr",index));}
    private File xorFile(long index){return new File(dir(),String.format(Locale.US,"xor_%010d.dqr",index));}
    private File parityFile(long index){return new File(dir(),String.format(Locale.US,"parity_%010d.dqr",index));}
    private File planeFile(long group,int coefficient){return new File(dir(),String.format(Locale.US,"plane_%010d_%02x.dqr",group,coefficient));}
    private static boolean isDataName(String n){return n.startsWith("frame_")&&n.endsWith(".dqr");}
    private static boolean isXorName(String n){return n.startsWith("xor_")&&n.endsWith(".dqr");}
    private static boolean isParityName(String n){return n.startsWith("parity_")&&n.endsWith(".dqr");}
    private static boolean isPlaneName(String n){return n.startsWith("plane_")&&n.endsWith(".dqr");}
    private static int blockStride(Protocol.Frame f){
        return f.flags==Protocol.FLAG_GROUP_XOR_WHITENED?
                (int)(f.streamOffset==0?1:f.streamOffset):1;
    }
    private static boolean validBlockFrame(Protocol.Frame f){
        if(f.kind!=Protocol.BLOCK_XOR)return false;
        int count=(int)f.streamId,stride=blockStride(f);
        if(count<1||count>Protocol.MAX_WINDOW)return false;
        if(f.flags==Protocol.FLAG_GROUP_XOR_WHITENED)
            return count<=3&&stride>=1&&stride<=Protocol.MAX_WINDOW&&
                    f.windowIndex+(count-1L)*stride<f.windowCount;
        return f.flags==Protocol.FLAG_WHITENED&&f.streamOffset==0&&
                f.windowIndex+count<=f.windowCount;
    }
    private void activate(long session){active=session;windowExpected.clear();windowSeen.clear();totalPayload=0;endIndex=-1;firstStoredNanos=0;context.getSharedPreferences("dosfer",0).edit().putLong("active",active).apply();}
    public synchronized Result accept(byte[] raw,long latencyNanos){
        Protocol.Frame f;
        try{f=Protocol.parseFrame(raw);}catch(RuntimeException e){invalid++;return Result.INVALID;}
        noteDecode(raw.length,latencyNanos);
        if(f.kind==Protocol.CALIBRATION){noteCalibration(f);return Result.CALIBRATION;}
        Protocol.Record record=null;
        if(f.kind==Protocol.DATA)try{record=Protocol.parseRecord(f.payload);}catch(RuntimeException e){invalid++;return Result.INVALID;}
        /* An end-window marker can remain visible while the user presses Reset.
           It must not claim the next empty session. A valid data frame from a
           different session may replace an empty stale session automatically. */
        boolean sessionStart=isSessionStart(f);
        if(active==0){if(f.kind==Protocol.END_WINDOW||!sessionStart){other++;return Result.OTHER_SESSION;}activate(f.session);}
        if(active!=f.session){if(sessionStart&&countFrames()==0)activate(f.session);else{other++;return Result.OTHER_SESSION;}}
        if(f.kind==Protocol.END_WINDOW){windowExpected.put(f.window,f.windowCount);return Result.STORED;}
        if(f.kind==Protocol.CHAIN_XOR){
            windowExpected.put(f.window,f.windowCount);File target=xorFile(f.globalIndex);
            if(target.exists()){try{if(Arrays.equals(readAll(target),raw)){duplicates++;return Result.DUPLICATE;}}catch(IOException ignored){}invalid++;return Result.INVALID;}
            if(!storeRaw(target,raw)){invalid++;return Result.INVALID;}recoverAvailable();return Result.STORED;
        }
        if(f.kind==Protocol.BLOCK_XOR){
            if(!validBlockFrame(f)){invalid++;return Result.INVALID;}
            windowExpected.put(f.window,f.windowCount);File target=parityFile(f.globalIndex);
            if(target.exists()){try{if(Arrays.equals(readAll(target),raw)){duplicates++;return Result.DUPLICATE;}}catch(IOException ignored){}invalid++;return Result.INVALID;}
            if(!storeRaw(target,raw)){invalid++;return Result.INVALID;}recoverAvailable();return Result.STORED;
        }
        if(f.kind==Protocol.PLANE_CODED){
            int coefficient=(int)f.globalIndex,basis=-1,width=(int)f.streamOffset;boolean valid=coefficient==1||coefficient==2||coefficient==4||coefficient==7||coefficient==8||coefficient==15;
            boolean expectedWhitened=!(width==4&&coefficient==15);
            if(!valid||f.flags!=(expectedWhitened?Protocol.FLAG_PLANE_WHITENED:0)||(width!=3&&width!=4)||(coefficient==7&&width!=3)||(coefficient==15&&width!=4)||(coefficient==8&&width!=4)||f.windowIndex+width>f.windowCount){invalid++;return Result.INVALID;}
            if(coefficient==1)basis=0;else if(coefficient==2)basis=1;else if(coefficient==4)basis=2;else if(coefficient==8)basis=3;
            if(basis>=0)try{record=Protocol.parseRecord(trimPlaneRecord(f.payload));}catch(RuntimeException e){invalid++;return Result.INVALID;}
            windowExpected.put(f.window,f.windowCount);File target=planeFile(f.streamId,coefficient);
            if(target.exists()){try{if(Arrays.equals(readAll(target),raw)){duplicates++;return Result.DUPLICATE;}}catch(IOException ignored){}invalid++;return Result.INVALID;}
            if(!storeRaw(target,raw)){invalid++;return Result.INVALID;}
            if(basis>=0){Recovered recovered=storeRecovered(f,f.streamId+basis,f.windowIndex+basis,trimPlaneRecord(f.payload));
                if(recovered==Recovered.CONFLICT){invalid++;return Result.INVALID;}
                if(recovered==Recovered.DUPLICATE){duplicates++;return Result.DUPLICATE;}}
            recoverAvailable();return Result.STORED;
        }
        if(record!=null&&record.type==Protocol.TRANSFER_END)endIndex=f.globalIndex;
        windowExpected.put(f.window,f.windowCount);BitSet bits=windowSeen.computeIfAbsent(f.window,k->new BitSet());
        File target=file(f.globalIndex);
        if(target.exists()){
            try{Protocol.Frame old=Protocol.parseFrame(readAll(target));if(Arrays.equals(old.payload,f.payload)){duplicates++;bits.set(f.windowIndex);return Result.DUPLICATE;}}
            catch(Exception ignored){}
            invalid++;return Result.INVALID;
        }
        if(!storeRaw(target,raw)){invalid++;return Result.INVALID;}
        bits.set(f.windowIndex);if(firstStoredNanos==0)firstStoredNanos=System.nanoTime();totalPayload+=f.payloadLength;
        recoverAvailable();return Result.STORED;
    }
    private void noteDecode(int bytes,long latency){long now=System.nanoTime();decodeTimes.addLast(now);while(!decodeTimes.isEmpty()&&now-decodeTimes.peekFirst()>5_000_000_000L)decodeTimes.removeFirst();latencySum+=latency/1e6;latencyCount++;}
    private void noteCalibration(Protocol.Frame f){calUnique++;if(calLast>=0){if(f.globalIndex==calLast)calDuplicates++;else if(f.globalIndex>calLast+1)calMissed+=f.globalIndex-calLast-1;}if(f.globalIndex>calLast)calLast=f.globalIndex;}
    private static byte[] readAll(File f)throws IOException{try(FileInputStream in=new FileInputStream(f);ByteArrayOutputStream o=new ByteArrayOutputStream()){byte[] b=new byte[4096];int n;while((n=in.read(b))>0)o.write(b,0,n);return o.toByteArray();}}
    private static boolean storeRaw(File target,byte[] raw){File temp=new File(target.getPath()+".tmp");
        try(FileOutputStream out=new FileOutputStream(temp)){out.write(raw);}catch(IOException e){temp.delete();return false;}
        if(!temp.renameTo(target)){temp.delete();return false;}return true;}
    private static long bodyU32(byte[] b,int p){return ((long)(b[p]&255)<<24)|((long)(b[p+1]&255)<<16)|((long)(b[p+2]&255)<<8)|(b[p+3]&255);}
    private static byte[] trimPlaneRecord(byte[] payload) {
        if(payload==null||payload.length<24)throw new IllegalArgumentException("plane record");
        int n=24+(int)bodyU32(payload,16);if(n<24||n>payload.length)throw new IllegalArgumentException("plane size");
        /* Plane symbols use deterministic nonzero filler after the embedded
           record, so only the record length and CRC are authoritative. */
        return Arrays.copyOf(payload,n);
    }
    static boolean isPlaneStart(Protocol.Frame f) {
        int width=(int)f.streamOffset;
        if(f.kind!=Protocol.PLANE_CODED||f.flags!=Protocol.FLAG_PLANE_WHITENED||f.globalIndex!=1||(width!=3&&width!=4)||f.windowIndex+width>f.windowCount)return false;
        try{Protocol.parseRecord(trimPlaneRecord(f.payload));return true;}catch(RuntimeException e){return false;}
    }
    static boolean isLegacyRecoveryStart(Protocol.Frame f) {
        if(f.kind==Protocol.CHAIN_XOR) {
            int left=(int)(f.streamId>>>16),right=(int)(f.streamId&0xffff);
            return f.flags==Protocol.FLAG_PAIR_WHITENED&&f.streamOffset==0&&
                    left>=Protocol.RECORD_HEADER&&right>=Protocol.RECORD_HEADER&&
                    f.windowIndex+1<f.windowCount&&f.payloadLength==Math.max(left,right);
        }
        if(f.kind==Protocol.BLOCK_XOR)
            return validBlockFrame(f)&&f.payloadLength>=Protocol.RECORD_HEADER;
        return false;
    }
    private static boolean isSessionStart(Protocol.Frame f) {
        return f.kind==Protocol.DATA||isPlaneStart(f)||isLegacyRecoveryStart(f);
    }
    private Recovered storeRecovered(Protocol.Frame chain,long index,int wi,byte[] payload) {
        Protocol.Record r=Protocol.parseRecord(payload);long sid=0,off=0;
        if(r.type==Protocol.FILE_BEGIN||r.type==Protocol.FILE_DATA||r.type==Protocol.FILE_END)sid=r.fileId;
        if(r.type==Protocol.FILE_DATA){if(r.body.length<4)throw new IllegalArgumentException("file data");off=bodyU32(r.body,0);}
        byte[] raw=Protocol.encodeFrame(Protocol.DATA,Protocol.FLAG_WHITENED,chain.session,chain.window,index,wi,chain.windowCount,sid,off,payload);
        File target=file(index);
        if(target.exists())try{Protocol.Frame old=Protocol.parseFrame(readAll(target));return Arrays.equals(old.payload,payload)?Recovered.DUPLICATE:Recovered.CONFLICT;}catch(Exception e){return Recovered.CONFLICT;}
        if(!storeRaw(target,raw))return Recovered.CONFLICT;
        windowExpected.put(chain.window,chain.windowCount);windowSeen.computeIfAbsent(chain.window,k->new BitSet()).set(wi);
        if(firstStoredNanos==0)firstStoredNanos=System.nanoTime();totalPayload+=payload.length;
        if(r.type==Protocol.TRANSFER_END)endIndex=index;return Recovered.STORED;
    }
    private long recoverEquation(File eq) {
        try{Protocol.Frame chain=Protocol.parseFrame(readAll(eq));if(chain.kind!=Protocol.CHAIN_XOR||chain.windowIndex+1>=chain.windowCount)return -1;
            File left=file(chain.globalIndex),right=file(chain.globalIndex+1);boolean haveLeft=left.exists(),haveRight=right.exists();if(haveLeft==haveRight)return -1;
            Protocol.Frame known=Protocol.parseFrame(readAll(haveLeft?left:right));byte[] recovered=Protocol.recoverChain(chain,known.payload,haveLeft);
            long index=haveLeft?chain.globalIndex+1:chain.globalIndex;return storeRecovered(chain,index,haveLeft?chain.windowIndex+1:chain.windowIndex,recovered)==Recovered.STORED?index:-1;
        }catch(Exception ignored){return -1;}
    }
    private long recoverBlockEquation(File eq) {
        try{Protocol.Frame parity=Protocol.parseFrame(readAll(eq));int count=(int)parity.streamId;
            if(!validBlockFrame(parity))return -1;
            int stride=blockStride(parity);
            byte[][] members=new byte[count][];int missing=-1,missingCount=0;
            for(int i=0;i<count;i++){long index=parity.globalIndex+(long)i*stride;
                File source=file(index);if(!source.exists()){missing=i;missingCount++;continue;}
                Protocol.Frame known=Protocol.parseFrame(readAll(source));
                if(known.kind!=Protocol.DATA||known.session!=parity.session||known.window!=parity.window||
                   known.globalIndex!=index||known.windowIndex!=parity.windowIndex+i*stride)return -1;
                members[i]=known.payload;}
            if(missingCount!=1)return -1;
            byte[] recovered=Protocol.recoverBlock(parity,members,missing);
            long index=parity.globalIndex+(long)missing*stride;
            return storeRecovered(parity,index,parity.windowIndex+missing*stride,recovered)==Recovered.STORED?index:-1;
        }catch(Exception ignored){return -1;}
    }
    private long recoverPlaneEquation(File eq) {
        try{Protocol.Frame parity=Protocol.parseFrame(readAll(eq));int width=(int)parity.streamOffset;int[] bases=parity.globalIndex==7?new int[]{1,2,4}:parity.globalIndex==15?new int[]{1,2,4,8}:null;
            if(parity.kind!=Protocol.PLANE_CODED||bases==null||parity.flags!=(width==3?Protocol.FLAG_PLANE_WHITENED:0)||(parity.globalIndex==7&&width!=3)||(parity.globalIndex==15&&width!=4)||parity.windowIndex+width>parity.windowCount)return -1;
            byte[] recovered=parity.payload;int missing=-1,missingCount=0;
            for(int i=0;i<bases.length;i++){File source=planeFile(parity.streamId,bases[i]);if(!source.exists()){missing=i;missingCount++;continue;}
                Protocol.Frame known=Protocol.parseFrame(readAll(source));
                if(known.kind!=Protocol.PLANE_CODED||known.flags!=Protocol.FLAG_PLANE_WHITENED||known.session!=parity.session||known.window!=parity.window||known.streamId!=parity.streamId||known.streamOffset!=width||known.globalIndex!=bases[i])return -1;
                if(known.payload.length!=recovered.length)return -1;
                for(int j=0;j<recovered.length;j++)recovered[j]^=known.payload[j];
            }
            if(missingCount!=1)return -1;
            byte[] plain=trimPlaneRecord(recovered);Protocol.parseRecord(plain);long index=parity.streamId+missing;
            return storeRecovered(parity,index,parity.windowIndex+missing,plain)==Recovered.STORED?index:-1;
        }catch(Exception ignored){return -1;}
    }
    private void recoverAvailable() {
        boolean changed;do{changed=false;File[] equations=dir().listFiles((d,n)->isXorName(n));
            if(equations!=null)for(File eq:equations)if(recoverEquation(eq)>=0)changed=true;
            equations=dir().listFiles((d,n)->isParityName(n));
            if(equations!=null)for(File eq:equations)if(recoverBlockEquation(eq)>=0)changed=true;
            equations=dir().listFiles((d,n)->isPlaneName(n));
            if(equations!=null)for(File eq:equations)if(recoverPlaneEquation(eq)>=0)changed=true;
        }while(changed);
    }
    private void reload(){if(active==0)return;File[] files=dir().listFiles((d,n)->isDataName(n));
        if(files!=null)for(File file:files)try{byte[] raw=readAll(file);Protocol.Frame f=Protocol.parseFrame(raw);Protocol.Record r=Protocol.parseRecord(f.payload);
            windowExpected.put(f.window,f.windowCount);windowSeen.computeIfAbsent(f.window,k->new BitSet()).set(f.windowIndex);if(firstStoredNanos==0)firstStoredNanos=System.nanoTime();totalPayload+=f.payloadLength;if(r.type==Protocol.TRANSFER_END)endIndex=f.globalIndex;
        }catch(Exception ignored){} recoverAvailable(); }
    public synchronized Stats stats(){Stats s=new Stats();s.session=active;s.uniqueTotal=countFrames();s.duplicates=(int)duplicates;s.invalid=(int)invalid;s.other=(int)other;s.totalPayload=totalPayload;
        int newest=-1;for(int w:windowExpected.keySet())if(w>newest)newest=w;s.window=Math.max(0,newest);s.expected=windowExpected.getOrDefault(s.window,0);BitSet b=windowSeen.getOrDefault(s.window,new BitSet());s.uniqueWindow=b.cardinality();s.missing=ranges(b,s.expected);
        long span=decodeTimes.size()>1?decodeTimes.peekLast()-decodeTimes.peekFirst():0;s.decodedFps=span>0?(decodeTimes.size()-1)*1e9/span:0;s.usefulBps=firstStoredNanos==0?0:totalPayload*1e9/Math.max(1,System.nanoTime()-firstStoredNanos);s.avgLatencyMs=latencyCount==0?0:latencySum/latencyCount;
        s.complete=endIndex>=0&&s.uniqueTotal==endIndex+1;s.calibrationUnique=calUnique;s.calibrationMissed=calMissed;s.calibration=calibrationText();return s;}
    private long countFrames(){File[] f=dir().listFiles((d,n)->isDataName(n));return f==null?0:f.length;}
    private static String ranges(BitSet b,int count){StringBuilder s=new StringBuilder();int i=0;while(i<count){i=b.nextClearBit(i);if(i>=count)break;int e=i;while(e+1<count&&!b.get(e+1))e++;if(s.length()>0)s.append(',');s.append(i+1);if(e>i)s.append('-').append(e+1);i=e+1;}return s.length()==0?"-":s.toString();}
    private String calibrationText(){if(calUnique==0)return "WAITING FOR PROTOCOL QR";double dup=(double)calDuplicates/Math.max(1,calUnique);if(calMissed>calUnique/10)return "TOO FAST — frames are being skipped";if(dup>4)return "EXCESSIVE DUPLICATES — speed can increase";if(invalid>0)return "MARGINAL — invalid frames seen";return "RELIABLE";}
    public synchronized List<File> orderedFrames(){File[] a=dir().listFiles((d,n)->isDataName(n));if(a==null)return Collections.emptyList();Arrays.sort(a,Comparator.comparing(File::getName));return Arrays.asList(a);}
    public synchronized void reset(){File d=dir();File[] a=d.listFiles();if(a!=null)for(File f:a)f.delete();d.delete();active=0;windowExpected.clear();windowSeen.clear();decodeTimes.clear();duplicates=invalid=other=totalPayload=0;endIndex=-1;firstStoredNanos=0;latencySum=0;latencyCount=calMissed=calUnique=calDuplicates=0;calLast=-1;context.getSharedPreferences("dosfer",0).edit().remove("active").apply();}
}
