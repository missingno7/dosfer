package org.dosfer.receiver;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Matrix;
import android.graphics.RectF;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.*;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Range;
import android.util.Size;
import android.util.Log;
import android.view.Surface;
import android.view.TextureView;
import com.google.zxing.*;
import com.google.zxing.common.GlobalHistogramBinarizer;
import com.google.zxing.common.HybridBinarizer;
import com.google.zxing.qrcode.QRCodeReader;
import java.nio.ByteBuffer;
import java.util.*;

public final class CameraScanner {
    private static final String TAG="DOSFER-Camera";
    public interface Listener {
        void payload(byte[] bytes,long decodeLatencyNanos);
        void error(String message);
    }
    public static final class Stats {
        public int width,height,decodeWidth,decodeHeight,targetFps;
        public long cameraFrames,attempts,successes,failures,busyDrops;
        public double cameraFps,attemptFps,avgAttemptMs,maxAttemptMs;
    }

    private final Context context;
    private final TextureView preview;
    private final Listener listener;
    private final Map<DecodeHintType,Object> hints=new EnumMap<>(DecodeHintType.class);
    private final QRCodeReader qrReader=new QRCodeReader();
    private final Object statsLock=new Object();
    private final Object frameLock=new Object();
    private HandlerThread cameraThread,decodeThread;
    private Handler cameraHandler,decodeHandler;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private CaptureRequest.Builder request;
    private ImageReader reader;
    private volatile boolean decoding,running;
    private Frame pendingFrame;
    private volatile byte[] lastPayload;
    private int captureWidth,captureHeight,decodeWidth,decodeHeight,targetFps;
    private long firstCameraNanos,lastCameraNanos,cameraFrames,attempts,successes,failures,busyDrops,totalAttemptNanos,maxAttemptNanos;

    public CameraScanner(Context c,TextureView p,Listener l) {
        context=c;preview=p;listener=l;
        hints.put(DecodeHintType.TRY_HARDER,Boolean.TRUE);
    }

    private static final class Frame {
        final byte[] y;final int width,height;
        Frame(byte[] bytes,int w,int h){y=bytes;width=w;height=h;}
    }

    public void start() {
        running=true;
        cameraThread=new HandlerThread("dosfer-camera");cameraThread.start();cameraHandler=new Handler(cameraThread.getLooper());
        decodeThread=new HandlerThread("dosfer-decode");decodeThread.start();decodeHandler=new Handler(decodeThread.getLooper());
        if(preview.isAvailable())open();
        else preview.setSurfaceTextureListener(new TextureView.SurfaceTextureListener(){
            public void onSurfaceTextureAvailable(SurfaceTexture s,int w,int h){open();}
            public void onSurfaceTextureSizeChanged(SurfaceTexture s,int w,int h){}
            public boolean onSurfaceTextureDestroyed(SurfaceTexture s){return true;}
            public void onSurfaceTextureUpdated(SurfaceTexture s){}
        });
    }

    @SuppressLint("MissingPermission") private void open() {
        try {
            CameraManager manager=(CameraManager)context.getSystemService(Context.CAMERA_SERVICE);
            String chosen=null;
            for(String id:manager.getCameraIdList()) {
                Integer facing=manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
                if(facing!=null&&facing==CameraCharacteristics.LENS_FACING_BACK){chosen=id;break;}
            }
            if(chosen==null)throw new CameraAccessException(CameraAccessException.CAMERA_ERROR,"no rear camera");
            CameraCharacteristics cc=manager.getCameraCharacteristics(chosen);
            StreamConfigurationMap map=cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size capture=chooseCaptureSize(map==null?null:map.getOutputSizes(ImageFormat.YUV_420_888));
            Range<Integer> fps=chooseFps(cc.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES));
            captureWidth=capture.getWidth();captureHeight=capture.getHeight();targetFps=fps==null?0:fps.getUpper();
            Log.i(TAG,"camera="+chosen+" capture="+captureWidth+"x"+captureHeight+" targetFps="+fps);
            manager.openCamera(chosen,new CameraDevice.StateCallback(){
                public void onOpened(CameraDevice c){if(!running){c.close();return;}camera=c;createSession(capture,fps);}
                public void onDisconnected(CameraDevice c){c.close();}
                public void onError(CameraDevice c,int e){c.close();listener.error("Camera error "+e);}
            },cameraHandler);
        } catch(Exception e){listener.error(e.getMessage());}
    }

    private static Size chooseCaptureSize(Size[] sizes) {
        if(sizes==null||sizes.length==0)return new Size(1280,720);
        Size best=null;long bestScore=Long.MAX_VALUE;
        for(Size s:sizes) {
            if(s.getWidth()!=s.getHeight())continue;
            int side=s.getWidth();if(side<640||side>1280)continue;
            long score=Math.abs(side-1088L);
            if(score<bestScore){best=s;bestScore=score;}
        }
        if(best!=null)return best;
        best=null;bestScore=Long.MAX_VALUE;
        for(Size s:sizes) {
            long area=(long)s.getWidth()*s.getHeight();
            if(area>1280L*960)continue;
            long aspect=Math.abs(s.getWidth()-s.getHeight());
            long score=Math.abs(area-1088L*1088)+aspect*1000;
            if(score<bestScore){best=s;bestScore=score;}
        }
        if(best!=null)return best;
        return Collections.min(Arrays.asList(sizes),Comparator.comparingLong(s->(long)s.getWidth()*s.getHeight()));
    }

    private static Range<Integer> chooseFps(Range<Integer>[] ranges) {
        if(ranges==null||ranges.length==0)return null;
        Range<Integer> best=null;long bestScore=Long.MAX_VALUE;
        for(Range<Integer> r:ranges) {
            long score=r.getUpper()>=30?Math.abs(r.getUpper()-30L)*100+Math.abs(r.getLower()-30L):100000-r.getUpper();
            if(score<bestScore){best=r;bestScore=score;}
        }
        return best;
    }

    private void createSession(Size capture,Range<Integer> fps) {
        try {
            reader=ImageReader.newInstance(capture.getWidth(),capture.getHeight(),ImageFormat.YUV_420_888,4);
            reader.setOnImageAvailableListener(this::image,cameraHandler);
            SurfaceTexture texture=preview.getSurfaceTexture();
            texture.setDefaultBufferSize(capture.getWidth(),capture.getHeight());
            configurePreview(capture);
            Surface previewSurface=new Surface(texture);
            request=camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            request.addTarget(previewSurface);request.addTarget(reader.getSurface());
            request.set(CaptureRequest.CONTROL_AF_MODE,CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
            request.set(CaptureRequest.CONTROL_AE_MODE,CaptureRequest.CONTROL_AE_MODE_ON);
            if(fps!=null)request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,fps);
            camera.createCaptureSession(Arrays.asList(previewSurface,reader.getSurface()),new CameraCaptureSession.StateCallback(){
                public void onConfigured(CameraCaptureSession s){session=s;repeat();}
                public void onConfigureFailed(CameraCaptureSession s){listener.error("Camera configuration failed");}
            },cameraHandler);
        } catch(Exception e){listener.error(e.getMessage());}
    }

    private void configurePreview(Size capture) {
        preview.post(()->{
            int w=preview.getWidth(),h=preview.getHeight();if(w==0||h==0)return;
            int rotation=preview.getDisplay()==null?Surface.ROTATION_0:preview.getDisplay().getRotation();
            Matrix matrix=new Matrix();RectF viewRect=new RectF(0,0,w,h),bufferRect;
            if(rotation==Surface.ROTATION_90||rotation==Surface.ROTATION_270)
                bufferRect=new RectF(0,0,capture.getHeight(),capture.getWidth());
            else bufferRect=new RectF(0,0,capture.getWidth(),capture.getHeight());
            bufferRect.offset(viewRect.centerX()-bufferRect.centerX(),viewRect.centerY()-bufferRect.centerY());
            matrix.setRectToRect(viewRect,bufferRect,Matrix.ScaleToFit.FILL);
            float scale=Math.max((float)w/bufferRect.width(),(float)h/bufferRect.height());
            matrix.postScale(scale,scale,viewRect.centerX(),viewRect.centerY());
            if(rotation==Surface.ROTATION_90)matrix.postRotate(-90,viewRect.centerX(),viewRect.centerY());
            else if(rotation==Surface.ROTATION_270)matrix.postRotate(90,viewRect.centerX(),viewRect.centerY());
            else if(rotation==Surface.ROTATION_180)matrix.postRotate(180,viewRect.centerX(),viewRect.centerY());
            preview.setTransform(matrix);
        });
    }

    private void repeat(){try{session.setRepeatingRequest(request.build(),null,cameraHandler);}catch(Exception e){listener.error(e.getMessage());}}

    public void lockStability(){if(request==null)return;request.set(CaptureRequest.CONTROL_AE_LOCK,true);request.set(CaptureRequest.CONTROL_AF_MODE,CaptureRequest.CONTROL_AF_MODE_AUTO);request.set(CaptureRequest.CONTROL_AF_TRIGGER,CaptureRequest.CONTROL_AF_TRIGGER_START);repeat();request.set(CaptureRequest.CONTROL_AF_TRIGGER,CaptureRequest.CONTROL_AF_TRIGGER_IDLE);}

    private void image(ImageReader source) {
        Image image=source.acquireLatestImage();if(image==null)return;
        long now=System.nanoTime();
        synchronized(statsLock){if(firstCameraNanos==0)firstCameraNanos=now;lastCameraNanos=now;cameraFrames++;}
        if(!running){image.close();return;}
        final Frame frame;try{frame=extractY(image);}finally{image.close();}
        decodeWidth=frame.width;decodeHeight=frame.height;
        boolean schedule=false,replaced;
        synchronized(frameLock){
            if(!running)return;replaced=pendingFrame!=null;pendingFrame=frame;
            if(!decoding){decoding=true;schedule=true;}
        }
        if(replaced)synchronized(statsLock){busyDrops++;}
        if(schedule){Handler target=decodeHandler;if(target==null||!target.post(this::drainFrames))synchronized(frameLock){pendingFrame=null;decoding=false;}}
    }

    private void drainFrames() {
        while(running){
            Frame frame;synchronized(frameLock){frame=pendingFrame;pendingFrame=null;if(frame==null){decoding=false;return;}}
            decodeFrame(frame.y,frame.width,frame.height);
        }
        synchronized(frameLock){pendingFrame=null;decoding=false;}
    }

    private void decodeFrame(byte[] y,int width,int height) {
        long start=System.nanoTime();byte[] payload=null;
        try{payload=decode(y,width,height);}
        finally {
            long elapsed=System.nanoTime()-start;
            synchronized(statsLock){attempts++;totalAttemptNanos+=elapsed;if(elapsed>maxAttemptNanos)maxAttemptNanos=elapsed;if(payload==null)failures++;else successes++;}
            if(payload!=null&&running&&!Arrays.equals(payload,lastPayload)){lastPayload=payload;listener.payload(payload,elapsed);}
        }
    }

    private static Frame extractY(Image image) {
        Image.Plane p=image.getPlanes()[0];ByteBuffer src=p.getBuffer();
        int width=image.getWidth(),height=image.getHeight(),row=p.getRowStride(),pixel=p.getPixelStride();
        int side=Math.min(width,height),left=(width-side)/2,top=(height-side)/2;
        byte[] out=new byte[side*side];
        if(pixel==1){for(int y=0;y<side;y++){src.position((top+y)*row+left);src.get(out,y*side,side);}}
        else{for(int y=0;y<side;y++)for(int x=0;x<side;x++)out[y*side+x]=src.get((top+y)*row+(left+x)*pixel);}
        return new Frame(out,side,side);
    }

    private byte[] decode(byte[] y,int width,int height) {
        LuminanceSource src=new PlanarYUVLuminanceSource(y,width,height,0,0,width,height,false);
        byte[] payload=decodeBitmap(new BinaryBitmap(new GlobalHistogramBinarizer(src)));
        if(payload!=null)return payload;
        return decodeBitmap(new BinaryBitmap(new HybridBinarizer(src)));
    }

    private byte[] decodeBitmap(BinaryBitmap bitmap) {
        try {
            Result result=qrReader.decode(bitmap,hints);
            Object meta=result.getResultMetadata()==null?null:result.getResultMetadata().get(ResultMetadataType.BYTE_SEGMENTS);
            if(meta instanceof List)for(Object item:(List<?>)meta)if(item instanceof byte[]){byte[] b=(byte[])item;if(b.length>=4&&b[0]=='D'&&b[1]=='Q'&&b[2]=='R'&&b[3]=='1')return b;}
            return null;
        } catch(ReaderException ignored){return null;}
        finally{qrReader.reset();}
    }

    public void clearLastPayload(){lastPayload=null;}

    public Stats stats() {
        synchronized(statsLock) {
            Stats s=new Stats();s.width=captureWidth;s.height=captureHeight;s.decodeWidth=decodeWidth;s.decodeHeight=decodeHeight;s.targetFps=targetFps;
            s.cameraFrames=cameraFrames;s.attempts=attempts;s.successes=successes;s.failures=failures;s.busyDrops=busyDrops;
            long span=lastCameraNanos>firstCameraNanos?lastCameraNanos-firstCameraNanos:0;
            s.cameraFps=span>0?(cameraFrames-1)*1e9/span:0;
            s.attemptFps=span>0?attempts*1e9/span:0;
            s.avgAttemptMs=attempts==0?0:(totalAttemptNanos/1e6)/attempts;
            s.maxAttemptMs=maxAttemptNanos/1e6;
            return s;
        }
    }

    public void stop() {
        running=false;
        try{if(session!=null)session.close();if(camera!=null)camera.close();if(reader!=null)reader.close();}
        finally{session=null;camera=null;reader=null;synchronized(frameLock){pendingFrame=null;decoding=false;}if(cameraThread!=null)cameraThread.quitSafely();if(decodeThread!=null)decodeThread.quitSafely();cameraThread=decodeThread=null;cameraHandler=decodeHandler=null;lastPayload=null;}
    }
}
