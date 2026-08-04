package org.dosfer.receiver;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Matrix;
import android.graphics.Rect;
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
import androidx.camera.core.ImageInfo;
import androidx.camera.core.ImageProxy;
import androidx.camera.core.impl.TagBundle;
import androidx.camera.core.impl.utils.ExifData;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import zxingcpp.BarcodeReader;

public final class CameraScanner {
    private static final String TAG="DOSFER-Camera";
    private static final int DECODE_WORKER_COUNT=2;
    private static final int TRY_HARDER_EVERY_FAST_MISS=3;
    public interface Listener {
        void payload(byte[] bytes,long decodeLatencyNanos);
        void error(String message);
    }
    public static final class Stats {
        public int width,height,decodeWidth,decodeHeight,targetFps,workerCount;
        public long cameraFrames,attempts,successes,failures,busyDrops,fallbackAttempts,fallbackSuccesses;
        public double cameraFps,attemptFps,avgAttemptMs,maxAttemptMs;
    }

    private final Context context;
    private final TextureView preview;
    private final Listener listener;
    private final Object statsLock=new Object();
    private final Object payloadLock=new Object();
    private final AtomicLong fastMissOrdinal=new AtomicLong();
    private HandlerThread cameraThread;
    private Handler cameraHandler;
    private DecodeWorker[] decodeWorkers;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private CaptureRequest.Builder request;
    private ImageReader reader;
    private volatile boolean running;
    private volatile byte[] lastPayload;
    private int captureWidth,captureHeight,decodeWidth,decodeHeight,targetFps;
    private long firstCameraNanos,lastCameraNanos,cameraFrames,attempts,successes,failures,busyDrops,fallbackAttempts,fallbackSuccesses,totalAttemptNanos,maxAttemptNanos;

    public CameraScanner(Context c,TextureView p,Listener l) {
        context=c;preview=p;listener=l;
    }

    public void start() {
        running=true;
        cameraThread=new HandlerThread("dosfer-camera");cameraThread.start();cameraHandler=new Handler(cameraThread.getLooper());
        decodeWorkers=new DecodeWorker[DECODE_WORKER_COUNT];
        for(int i=0;i<decodeWorkers.length;i++)decodeWorkers[i]=new DecodeWorker(i);
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
        int side=Math.min(image.getWidth(),image.getHeight());
        Rect crop=new Rect((image.getWidth()-side)/2,(image.getHeight()-side)/2,(image.getWidth()+side)/2,(image.getHeight()+side)/2);
        decodeWidth=crop.width();decodeHeight=crop.height();
        DecodeWorker[] workers=decodeWorkers;
        if(workers!=null)for(DecodeWorker worker:workers)if(worker.submit(image,crop))return;
        image.close();
        synchronized(statsLock){busyDrops++;}
    }

    private final class DecodeWorker {
        private final HandlerThread thread;
        private final Handler handler;
        private final AtomicBoolean busy=new AtomicBoolean();
        private final BarcodeReader qrReader;
        private volatile boolean accepting=true;

        DecodeWorker(int index) {
            BarcodeReader.Options options=new BarcodeReader.Options();
            options.setFormats(Collections.singleton(BarcodeReader.Format.QR_CODE));
            options.setMaxNumberOfSymbols(1);
            options.setTryHarder(false);
            options.setTryRotate(false);
            options.setTryInvert(false);
            options.setTryDownscale(false);
            options.setBinarizer(BarcodeReader.Binarizer.LOCAL_AVERAGE);
            qrReader=new BarcodeReader(options);
            thread=new HandlerThread("dosfer-decode-"+index);thread.start();handler=new Handler(thread.getLooper());
        }

        boolean submit(Image image,Rect crop) {
            if(!accepting||!busy.compareAndSet(false,true))return false;
            if(handler.post(()->decodeFrame(this,image,crop)))return true;
            busy.set(false);return false;
        }

        void release(){busy.set(false);}
        void shutdown(){accepting=false;thread.quitSafely();}
        void await(){try{thread.join(1000);}catch(InterruptedException e){Thread.currentThread().interrupt();}}
    }

    private void decodeFrame(DecodeWorker worker,Image image,Rect crop) {
        long start=System.nanoTime();byte[] payload=null;boolean fallbackTried=false;
        MediaImageProxy proxy=new MediaImageProxy(image,crop);
        try {
            payload=decode(worker.qrReader,proxy,false);
            if(payload==null&&fastMissOrdinal.incrementAndGet()%TRY_HARDER_EVERY_FAST_MISS==0){
                fallbackTried=true;
                payload=decode(worker.qrReader,proxy,true);
            }
        } catch(RuntimeException e){Log.w(TAG,"Native QR decode failed",e);}
        finally {
            worker.qrReader.getOptions().setTryHarder(false);
            proxy.close();
            long elapsed=System.nanoTime()-start;
            try {
                synchronized(statsLock){
                    attempts++;totalAttemptNanos+=elapsed;if(elapsed>maxAttemptNanos)maxAttemptNanos=elapsed;
                    if(payload==null)failures++;else successes++;
                    if(fallbackTried){fallbackAttempts++;if(payload!=null)fallbackSuccesses++;}
                }
                if(payload!=null&&running){
                    boolean fresh;
                    synchronized(payloadLock){fresh=!Arrays.equals(payload,lastPayload);if(fresh)lastPayload=payload;}
                    if(fresh)listener.payload(payload,elapsed);
                }
            } finally {
                worker.release();
            }
        }
    }

    private static byte[] decode(BarcodeReader reader,ImageProxy image,boolean tryHarder) {
        reader.getOptions().setTryHarder(tryHarder);
        for(BarcodeReader.Result result:reader.read(image)){
            byte[] b=result.getBytes();
            if(b!=null&&b.length>=4&&b[0]=='D'&&b[1]=='Q'&&b[2]=='R'&&b[3]=='1')return b;
        }
        return null;
    }

    /** CameraX adapter used only to hand the retained Camera2 Y plane to the
     * ZXing-C++ JNI wrapper. No RGB conversion and no luminance copy occurs. */
    @SuppressLint({"RestrictedApi","UnsafeOptInUsageError"})
    private static final class MediaImageProxy implements ImageProxy {
        private final Image image;
        private final PlaneProxy[] planes;
        private final ImageInfo info;
        private final AtomicBoolean closed=new AtomicBoolean();
        private Rect crop;

        MediaImageProxy(Image image,Rect crop) {
            this.image=image;this.crop=new Rect(crop);
            Image.Plane[] source=image.getPlanes();planes=new PlaneProxy[source.length];
            for(int i=0;i<source.length;i++){
                Image.Plane plane=source[i];
                planes[i]=new PlaneProxy(){
                    public int getRowStride(){return plane.getRowStride();}
                    public int getPixelStride(){return plane.getPixelStride();}
                    public ByteBuffer getBuffer(){return plane.getBuffer();}
                };
            }
            long timestamp=image.getTimestamp();
            info=new ImageInfo(){
                public TagBundle getTagBundle(){return TagBundle.emptyBundle();}
                public long getTimestamp(){return timestamp;}
                public int getRotationDegrees(){return 0;}
                public void populateExifData(ExifData.Builder builder){}
            };
        }

        public void close(){if(closed.compareAndSet(false,true))image.close();}
        public Rect getCropRect(){return new Rect(crop);}
        public void setCropRect(Rect rect){crop=new Rect(rect);}
        public int getFormat(){return image.getFormat();}
        public int getHeight(){return image.getHeight();}
        public int getWidth(){return image.getWidth();}
        public PlaneProxy[] getPlanes(){return planes;}
        public ImageInfo getImageInfo(){return info;}
        public Image getImage(){return image;}
    }

    public void clearLastPayload(){synchronized(payloadLock){lastPayload=null;}}

    public Stats stats() {
        synchronized(statsLock) {
            Stats s=new Stats();s.width=captureWidth;s.height=captureHeight;s.decodeWidth=decodeWidth;s.decodeHeight=decodeHeight;s.targetFps=targetFps;s.workerCount=DECODE_WORKER_COUNT;
            s.cameraFrames=cameraFrames;s.attempts=attempts;s.successes=successes;s.failures=failures;s.busyDrops=busyDrops;s.fallbackAttempts=fallbackAttempts;s.fallbackSuccesses=fallbackSuccesses;
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
        try {
            if(session!=null)session.close();if(camera!=null)camera.close();
            DecodeWorker[] workers=decodeWorkers;
            if(workers!=null){for(DecodeWorker worker:workers)worker.shutdown();for(DecodeWorker worker:workers)worker.await();}
            if(reader!=null)reader.close();
        } finally {
            session=null;camera=null;reader=null;decodeWorkers=null;
            if(cameraThread!=null)cameraThread.quitSafely();cameraThread=null;cameraHandler=null;
            synchronized(payloadLock){lastPayload=null;}
        }
    }
}
