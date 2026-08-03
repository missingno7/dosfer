package org.dosfer.receiver;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.*;
import java.util.zip.CRC32;

public final class Reconstructor {
    public interface Progress { void update(String message); }
    public static final class Result { public final boolean ok;public final String message;Result(boolean o,String m){ok=o;message=m;} }
    private static final class OpenFile {long id,size,written;String finalName;Uri temp;OutputStream out;CRC32 crc=new CRC32();}
    private final ContentResolver resolver;private final Uri root;private final Map<String,Uri> directories=new HashMap<>();
    public Reconstructor(ContentResolver r,Uri tree){resolver=r;root=DocumentsContract.buildDocumentUriUsingTree(tree,DocumentsContract.getTreeDocumentId(tree));directories.put("",root);}

    public Result reconstruct(List<File> frames,Progress progress) {
        OpenFile open=null;long expectedRecord=0;int files=0,dirs=0;long total=0;
        try {
            for(File file:frames){byte[] raw=read(file);Protocol.Frame frame=Protocol.parseFrame(raw);if(frame.kind!=Protocol.DATA)continue;
                Protocol.Record rec=Protocol.parseRecord(frame.payload);if(rec.recordId!=expectedRecord++)throw new IOException("record sequence gap at "+rec.recordId);
                ByteBuffer b=ByteBuffer.wrap(rec.body).order(ByteOrder.BIG_ENDIAN);
                switch(rec.type){
                    case Protocol.SESSION: break;
                    case Protocol.DIRECTORY:{String path=parseMetadataPath(rec.body,false);ensureDirectory(path);dirs++;progress.update("Directory "+path);break;}
                    case Protocol.FILE_BEGIN:{if(open!=null)throw new IOException("nested file");long size=Integer.toUnsignedLong(b.getInt(6));int plen=Short.toUnsignedInt(b.getShort(10));String path=Protocol.safePath(rec.body,12,plen);open=createFile(rec.fileId,path,size);progress.update("Receiving "+path);break;}
                    case Protocol.FILE_DATA:{if(open==null||open.id!=rec.fileId)throw new IOException("data without matching file");long off=Integer.toUnsignedLong(b.getInt());if(off!=open.written)throw new IOException("non-contiguous file data at "+off);int n=rec.body.length-4;open.out.write(rec.body,4,n);open.crc.update(rec.body,4,n);open.written+=n;total+=n;break;}
                    case Protocol.FILE_END:{if(open==null||open.id!=rec.fileId)throw new IOException("end without file");long size=Integer.toUnsignedLong(b.getInt()),crc=Integer.toUnsignedLong(b.getInt());open.out.flush();open.out.close();open.out=null;
                        if(size!=open.size||open.written!=open.size||open.crc.getValue()!=crc)throw new IOException("file size/CRC mismatch; partial retained");
                        Uri renamed=DocumentsContract.renameDocument(resolver,open.temp,open.finalName);if(renamed==null)throw new IOException("destination rename failed; partial retained");files++;progress.update("Verified "+open.finalName);open=null;break;}
                    case Protocol.TRANSFER_END:{long ef=Integer.toUnsignedLong(b.getInt()),ed=Integer.toUnsignedLong(b.getInt());b.getInt();long et=Integer.toUnsignedLong(b.getInt());if(open!=null||ef!=files||ed!=dirs||et!=total)throw new IOException("transfer summary mismatch");return new Result(true,"Complete and verified: "+files+" files, "+total+" bytes");}
                    default:throw new IOException("unknown record");
                }
            }
            return new Result(false,"Transfer-end record not reached");
        }catch(Exception e){if(open!=null&&open.out!=null)try{open.out.close();}catch(IOException ignored){}return new Result(false,e.getMessage());}
    }
    private static String parseMetadataPath(byte[] body,boolean file){int lenOff=file?10:6,pathOff=file?12:8;if(body.length<pathOff)throw new IllegalArgumentException("short metadata");int n=Short.toUnsignedInt(ByteBuffer.wrap(body,lenOff,2).order(ByteOrder.BIG_ENDIAN).getShort());return Protocol.safePath(body,pathOff,n);}
    private OpenFile createFile(long id,String path,long size)throws IOException {
        int slash=path.lastIndexOf('/');String parent=slash<0?"":path.substring(0,slash),name=slash<0?path:path.substring(slash+1);Uri p=ensureDirectory(parent);
        String finalName=unusedName(p,name),tempName=unusedName(p,finalName+".partial");Uri temp=DocumentsContract.createDocument(resolver,p,"application/octet-stream",tempName);if(temp==null)throw new IOException("cannot create partial file");
        OutputStream out=resolver.openOutputStream(temp,"w");if(out==null)throw new IOException("cannot open partial file");OpenFile f=new OpenFile();f.id=id;f.size=size;f.finalName=finalName;f.temp=temp;f.out=out;return f;
    }
    private Uri ensureDirectory(String path)throws IOException {if(path.isEmpty())return root;Uri known=directories.get(path);if(known!=null)return known;String[] parts=path.split("/");String built="";Uri parent=root;
        for(String part:parts){built=built.isEmpty()?part:built+"/"+part;Uri u=directories.get(built);if(u==null){u=find(parent,part);if(u==null)u=DocumentsContract.createDocument(resolver,parent,DocumentsContract.Document.MIME_TYPE_DIR,part);if(u==null)throw new IOException("cannot create directory "+built);directories.put(built,u);}parent=u;}return parent;}
    private String unusedName(Uri parent,String requested)throws IOException {if(find(parent,requested)==null)return requested;int dot=requested.lastIndexOf('.');String base=dot>0?requested.substring(0,dot):requested,ext=dot>0?requested.substring(dot):"";for(int i=1;i<10000;i++){String n=base+" ("+i+")"+ext;if(find(parent,n)==null)return n;}throw new IOException("no unused destination name");}
    private Uri find(Uri parent,String name)throws IOException {Uri children=DocumentsContract.buildChildDocumentsUriUsingTree(parent,DocumentsContract.getDocumentId(parent));String[] cols={DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME};
        try(Cursor c=resolver.query(children,cols,null,null,null)){if(c==null)return null;while(c.moveToNext())if(name.equals(c.getString(1)))return DocumentsContract.buildDocumentUriUsingTree(parent,c.getString(0));}return null;}
    private static byte[] read(File f)throws IOException{try(InputStream in=new FileInputStream(f);ByteArrayOutputStream out=new ByteArrayOutputStream()){byte[] b=new byte[4096];int n;while((n=in.read(b))>0)out.write(b,0,n);return out.toByteArray();}}
}

