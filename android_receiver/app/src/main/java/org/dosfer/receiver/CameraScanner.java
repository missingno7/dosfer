package org.dosfer.receiver;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Range;
import android.util.Size;
import android.util.Log;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;

import androidx.camera.core.ImageInfo;
import androidx.camera.core.ImageProxy;
import androidx.camera.core.impl.TagBundle;
import androidx.camera.core.impl.utils.ExifData;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

import zxingcpp.BarcodeReader;

public final class CameraScanner {
    private static final String TAG = "DOSFER-Camera";
    private static final int DECODE_WORKER_COUNT = 2;
    private static final int TRY_HARDER_EVERY_MISS = 3;
    private static final long CLASSIFICATION_WARMUP_NANOS = 3_000_000_000L;

    public interface Listener {
        void payload(byte[] bytes, long decodeLatencyNanos);
        void error(String message);
        default void modes(List<CameraMode> modes) {}
    }

    public static final class Stats {
        public int width, height, decodeWidth, decodeHeight, targetFps, workerCount;
        public String cameraId = "-", requestedFps = "Auto", requestRange = "-";
        public String classification = "warming up", sessionStatus = "opening";
        public String decoderCrop = "-";
        public int previewWidth, previewHeight, displayRotation, sensorOrientation, relativeRotation;
        public float previewScale;
        public long cameraFrames, sensorFrames, imageReaderFrames, attempts, successes, failures;
        public long busyDrops, fallbackAttempts, fallbackSuccesses, exposureTimeNanos, sensorFrameDurationNanos;
        public long fullDetectorAttempts, recoveryAttempts, recoverySuccesses;
        public long fullDetectorTotalNanos, fullDetectorMaxNanos, recoveryTotalNanos, recoveryMaxNanos;
        public double cameraFps, sensorFps, imageReaderFps, attemptFps, avgAttemptMs, maxAttemptMs;
    }

    private final Context context;
    private final TextureView preview;
    private final Listener listener;
    private final CameraSettings.Selection selection;
    private final Object statsLock = new Object();
    private final Object payloadLock = new Object();
    private final AtomicLong missOrdinal = new AtomicLong();

    private HandlerThread cameraThread;
    private Handler cameraHandler;
    private DecodeWorker[] decodeWorkers;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private CaptureRequest.Builder request;
    private ImageReader reader;
    private volatile boolean running;
    private volatile byte[] lastPayload;
    private CameraMode selectedMode;
    private CameraCrop captureCrop;
    private Range<Integer> configuredFpsRange;
    private int captureWidth, captureHeight, decodeWidth, decodeHeight, targetFps;
    private long firstImageTimestamp, lastImageTimestamp, firstSensorTimestamp, lastSensorTimestamp;
    private long firstRuntimeNanos;
    private long cameraFrames, sensorFrames, imageReaderFrames, attempts, successes, failures, busyDrops;
    private long fallbackAttempts, fallbackSuccesses, totalAttemptNanos, maxAttemptNanos;
    private long fullDetectorAttempts, recoveryAttempts, recoverySuccesses;
    private long fullDetectorTotalNanos, fullDetectorMaxNanos, recoveryTotalNanos, recoveryMaxNanos;
    private long exposureTimeNanos, sensorFrameDurationNanos;
    private String cameraId = "-", sessionStatus = "opening";
    private int sensorOrientation, lensFacing = CameraCharacteristics.LENS_FACING_BACK;
    private int previewWidth, previewHeight, previewDisplayRotation, previewRelativeRotation;
    private float previewScale;
    private final View.OnLayoutChangeListener previewLayoutListener = (v, left, top, right, bottom,
            oldLeft, oldTop, oldRight, oldBottom) -> requestPreviewTransform();
    private boolean previewLayoutListenerAttached;

    public CameraScanner(Context c, TextureView p, Listener l, CameraSettings.Selection config) {
        context = c;
        preview = p;
        listener = l;
        selection = config == null ? new CameraSettings.Selection(null, 0, "auto", true) : config;
    }

    public void start() {
        running = true;
        firstRuntimeNanos = System.nanoTime();
        cameraThread = new HandlerThread("dosfer-camera");
        cameraThread.start();
        cameraHandler = new Handler(cameraThread.getLooper());
        if (!previewLayoutListenerAttached) {
            preview.addOnLayoutChangeListener(previewLayoutListener);
            previewLayoutListenerAttached = true;
        }
        decodeWorkers = new DecodeWorker[DECODE_WORKER_COUNT];
        for (int i = 0; i < decodeWorkers.length; i++) decodeWorkers[i] = new DecodeWorker(i);
        if (preview.isAvailable()) open();
        else preview.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
            public void onSurfaceTextureAvailable(SurfaceTexture s, int w, int h) { open(); }
            public void onSurfaceTextureSizeChanged(SurfaceTexture s, int w, int h) {}
            public boolean onSurfaceTextureDestroyed(SurfaceTexture s) { return true; }
            public void onSurfaceTextureUpdated(SurfaceTexture s) {}
        });
    }

    @SuppressLint("MissingPermission")
    private void open() {
        try {
            CameraManager manager = (CameraManager) context.getSystemService(Context.CAMERA_SERVICE);
            String chosen = chooseCameraId(manager, selection.cameraId);
            if (chosen == null) throw new IllegalStateException("no rear camera");
            cameraId = chosen;
            CameraCharacteristics cc = manager.getCameraCharacteristics(chosen);
            Integer orientation = cc.get(CameraCharacteristics.SENSOR_ORIENTATION);
            Integer facing = cc.get(CameraCharacteristics.LENS_FACING);
            sensorOrientation = orientation == null ? 0 : orientation;
            lensFacing = facing == null ? CameraCharacteristics.LENS_FACING_BACK : facing;
            CameraMode[] discovered = discover(cc);
            listener.modes(Arrays.asList(discovered));
            selectedMode = selectMode(Arrays.asList(discovered), selection);
            if (selectedMode == null) selectedMode = new CameraMode(1280, 720, 0);
            captureWidth = selectedMode.width;
            captureHeight = selectedMode.height;
            captureCrop = CameraCrop.forCapture(captureWidth, captureHeight);
            decodeWidth = captureCrop.side;
            decodeHeight = captureCrop.side;
            targetFps = selection.targetFps == 30 || selection.targetFps == 60
                    ? selection.targetFps : (selectedMode.theoretically60Fps ? 60 : 30);
            Range<Integer>[] ranges = cc.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
            configuredFpsRange = CameraMode.fixedOrCompatibleRange(ranges, targetFps);
            sessionStatus = "opening";
            Log.i(TAG, "camera=" + cameraId + " mode=" + selectedMode.key()
                    + " target=" + targetFps + " requestRange=" + configuredFpsRange);
            manager.openCamera(chosen, new CameraDevice.StateCallback() {
                public void onOpened(CameraDevice c) {
                    if (!running) { c.close(); return; }
                    camera = c;
                    createSession(captureWidth, captureHeight, configuredFpsRange);
                }
                public void onDisconnected(CameraDevice c) { c.close(); sessionStatus = "unsupported"; }
                public void onError(CameraDevice c, int e) {
                    c.close(); sessionStatus = "unsupported"; listener.error("Camera error " + e);
                }
            }, cameraHandler);
        } catch (Exception e) {
            sessionStatus = "unsupported";
            listener.error(e.getMessage() == null ? "Camera discovery failed" : e.getMessage());
        }
    }

    private static String chooseCameraId(CameraManager manager, String requested) throws Exception {
        String firstRear = null;
        for (String id : manager.getCameraIdList()) {
            Integer facing = manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
            if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) {
                if (firstRear == null) firstRear = id;
                if (id.equals(requested)) return id;
            }
        }
        return firstRear;
    }

    private static CameraMode[] discover(CameraCharacteristics cc) {
        android.hardware.camera2.params.StreamConfigurationMap map =
                cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        List<CameraMode> modes = CameraMode.discover(map);
        return modes.toArray(new CameraMode[0]);
    }

    static CameraMode selectMode(List<CameraMode> modes, CameraSettings.Selection config) {
        if (!config.automatic) {
            CameraMode manual = CameraMode.find(modes, config.modeKey);
            if (manual != null) return manual;
        }
        int requested = config.targetFps == 30 || config.targetFps == 60 ? config.targetFps : 60;
        return CameraMode.chooseAutomatic(modes, requested);
    }

    private void createSession(int width, int height, Range<Integer> fps) {
        try {
            reader = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 4);
            reader.setOnImageAvailableListener(this::image, cameraHandler);
            SurfaceTexture texture = preview.getSurfaceTexture();
            texture.setDefaultBufferSize(width, height);
            requestPreviewTransform();
            Surface previewSurface = new Surface(texture);
            request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
            request.addTarget(previewSurface);
            request.addTarget(reader.getSurface());
            request.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
            request.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON);
            request.set(CaptureRequest.CONTROL_AE_ANTIBANDING_MODE,
                    CameraMetadata.CONTROL_AE_ANTIBANDING_MODE_AUTO);
            if (fps != null) request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fps);
            camera.createCaptureSession(Arrays.asList(previewSurface, reader.getSurface()),
                    new CameraCaptureSession.StateCallback() {
                        public void onConfigured(CameraCaptureSession s) { session = s; sessionStatus = "running"; repeat(); }
                        public void onConfigureFailed(CameraCaptureSession s) {
                            sessionStatus = "unsupported";
                            listener.error("Camera configuration failed for " + captureWidth + "x" + captureHeight
                                    + " with " + requestedRangeLabel());
                        }
                    }, cameraHandler);
        } catch (Exception e) {
            sessionStatus = "unsupported";
            listener.error(e.getMessage() == null ? "Camera session failed" : e.getMessage());
        }
    }

    private String requestedRangeLabel() {
        return "[" + targetFps + "," + targetFps + "]";
    }

    private void requestPreviewTransform() {
        preview.post(this::configurePreview);
    }

    private void configurePreview() {
        CameraCrop crop = captureCrop;
        if (crop == null) return;
        int viewWidth = preview.getWidth(), viewHeight = preview.getHeight();
        if (viewWidth <= 0 || viewHeight <= 0) return;

        int displayRotation = preview.getDisplay() == null
                ? Surface.ROTATION_0 : preview.getDisplay().getRotation();
        int relative = PreviewGeometry.relativeRotation(sensorOrientation, displayRotation, lensFacing);
        float centerX = viewWidth / 2f;
        float centerY = viewHeight / 2f;

        /*
         * TextureView already compensates for SENSOR_ORIENTATION. It does not
         * preserve the preview aspect ratio or compensate for display
         * rotation. Undo its non-uniform fit, apply a centered FILL_CENTER
         * scale, and rotate only by the display rotation. The previous code
         * used raw camera pixel coordinates as a TextureView matrix and also
         * applied the sensor-relative rotation a second time, producing the
         * small sideways image in the upper-left corner.
         */
        float[] contentScale = PreviewGeometry.textureFillCenterScale(
                captureWidth, captureHeight, viewWidth, viewHeight,
                sensorOrientation, relative);
        Matrix transform = new Matrix();
        transform.setScale(contentScale[0], contentScale[1], centerX, centerY);
        int displayDegrees = PreviewGeometry.displayDegrees(displayRotation);
        if (displayDegrees != 0) transform.postRotate(-displayDegrees, centerX, centerY);
        preview.setTransform(transform);

        float scale = PreviewGeometry.uniformScale(crop.side, viewWidth, viewHeight);
        previewWidth = viewWidth;
        previewHeight = viewHeight;
        previewDisplayRotation = displayRotation;
        previewRelativeRotation = relative;
        previewScale = scale;
        synchronized (statsLock) {
            this.decodeWidth = crop.side;
            this.decodeHeight = crop.side;
        }
    }

    private void repeat() {
        try {
            session.setRepeatingRequest(request.build(), new CameraCaptureSession.CaptureCallback() {
                @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r, TotalCaptureResult result) {
                    recordCaptureResult(result);
                }
            }, cameraHandler);
        } catch (Exception e) {
            sessionStatus = "unsupported";
            listener.error(e.getMessage() == null ? "Camera repeating request failed" : e.getMessage());
        }
    }

    private void recordCaptureResult(TotalCaptureResult result) {
        Long timestamp = result.get(CaptureResult.SENSOR_TIMESTAMP);
        synchronized (statsLock) {
            sensorFrames++;
            if (timestamp != null) {
                if (firstSensorTimestamp == 0) firstSensorTimestamp = timestamp;
                lastSensorTimestamp = timestamp;
            }
            Long duration = result.get(CaptureResult.SENSOR_FRAME_DURATION);
            Long exposure = result.get(CaptureResult.SENSOR_EXPOSURE_TIME);
            if (duration != null) sensorFrameDurationNanos = duration;
            if (exposure != null) exposureTimeNanos = exposure;
        }
    }

    public void lockStability() {
        if (request == null) return;
        request.set(CaptureRequest.CONTROL_AE_LOCK, true);
        request.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_AUTO);
        request.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_START);
        repeat();
        request.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE);
    }

    private void image(ImageReader source) {
        Image image = source.acquireLatestImage();
        if (image == null) return;
        synchronized (statsLock) {
            cameraFrames++;
            imageReaderFrames++;
            long timestamp = image.getTimestamp();
            if (firstImageTimestamp == 0) firstImageTimestamp = timestamp;
            lastImageTimestamp = timestamp;
        }
        if (!running) { image.close(); return; }
        CameraCrop cropGeometry = CameraCrop.forCapture(image.getWidth(), image.getHeight());
        Rect crop = cropGeometry.toRect();
        decodeWidth = crop.width();
        decodeHeight = crop.height();
        DecodeWorker[] workers = decodeWorkers;
        if (workers != null) for (DecodeWorker worker : workers) if (worker.submit(image, crop)) return;
        image.close();
        synchronized (statsLock) { busyDrops++; }
    }

    private final class DecodeWorker {
        private final HandlerThread thread;
        private final Handler handler;
        private final AtomicBoolean busy = new AtomicBoolean();
        private final BarcodeReader qrReader;
        private volatile boolean accepting = true;

        DecodeWorker(int index) {
            BarcodeReader.Options options = new BarcodeReader.Options();
            options.setFormats(Collections.singleton(BarcodeReader.Format.QR_CODE));
            options.setMaxNumberOfSymbols(1);
            options.setTryHarder(false);
            options.setTryRotate(false);
            options.setTryInvert(false);
            options.setTryDownscale(false);
            options.setTryDenoise(false);
            options.setBinarizer(BarcodeReader.Binarizer.LOCAL_AVERAGE);
            qrReader = new BarcodeReader(options);
            thread = new HandlerThread("dosfer-decode-" + index);
            thread.start();
            handler = new Handler(thread.getLooper());
        }

        boolean submit(Image image, Rect crop) {
            if (!accepting || !busy.compareAndSet(false, true)) return false;
            if (handler.post(() -> decodeFrame(this, image, crop))) return true;
            busy.set(false);
            return false;
        }

        void release() { busy.set(false); }
        void shutdown() { accepting = false; thread.quitSafely(); }
        void await() { try { thread.join(1000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); } }
    }

    private void decodeFrame(DecodeWorker worker, Image image, Rect crop) {
        long start = System.nanoTime();
        byte[] payload = null;
        boolean fallbackTried = false;
        MediaImageProxy proxy = new MediaImageProxy(image, crop);
        try {
            long pathStart = System.nanoTime();
            synchronized (statsLock) { fullDetectorAttempts++; }
            payload = decode(worker.qrReader, proxy, false);
            recordPath(1, System.nanoTime() - pathStart);
            if (payload == null && missOrdinal.incrementAndGet() % TRY_HARDER_EVERY_MISS == 0) {
                fallbackTried = true;
                long recoveryStart = System.nanoTime();
                synchronized (statsLock) { recoveryAttempts++; fullDetectorAttempts++; }
                payload = decode(worker.qrReader, proxy, true);
                recordPath(2, System.nanoTime() - recoveryStart);
                if (payload != null) synchronized (statsLock) { recoverySuccesses++; }
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "Native QR decode failed", e);
        } finally {
            proxy.close();
            worker.qrReader.getOptions().setTryHarder(false);
            long elapsed = System.nanoTime() - start;
            try {
                synchronized (statsLock) {
                    attempts++;
                    totalAttemptNanos += elapsed;
                    if (elapsed > maxAttemptNanos) maxAttemptNanos = elapsed;
                    if (payload == null) failures++; else successes++;
                    if (fallbackTried) { fallbackAttempts++; if (payload != null) fallbackSuccesses++; }
                }
                if (payload != null && running) {
                    boolean fresh;
                    synchronized (payloadLock) {
                        fresh = !Arrays.equals(payload, lastPayload);
                        if (fresh) lastPayload = payload;
                    }
                    if (fresh) listener.payload(payload, elapsed);
                }
            } finally { worker.release(); }
        }
    }

    /** path 1 = normal full detector, path 2 = try-harder recovery */
    private void recordPath(int path, long elapsed) {
        synchronized (statsLock) {
            if (path == 1) {
                fullDetectorTotalNanos += elapsed;
                if (elapsed > fullDetectorMaxNanos) fullDetectorMaxNanos = elapsed;
            } else {
                recoveryTotalNanos += elapsed;
                if (elapsed > recoveryMaxNanos) recoveryMaxNanos = elapsed;
            }
        }
    }

    private static byte[] decode(BarcodeReader reader, ImageProxy image, boolean tryHarder) {
        reader.getOptions().setTryHarder(tryHarder);
        reader.getOptions().setTryRotate(false);
        reader.getOptions().setTryInvert(false);
        reader.getOptions().setTryDownscale(false);
        reader.getOptions().setTryDenoise(false);
        for (BarcodeReader.Result result : reader.read(image)) {
            if (result.getFormat() != BarcodeReader.Format.QR_CODE) continue;
            byte[] b = result.getBytes();
            byte[] frame = V40DecoderPolicy.extractDosferFrame(b);
            if (frame != null) return frame;
        }
        return null;
    }

    /** CameraX adapter that hands the retained Camera2 Y plane to ZXing-C++ without a copy. */
    @SuppressLint({"RestrictedApi", "UnsafeOptInUsageError"})
    private static final class MediaImageProxy implements ImageProxy {
        private final Image image;
        private final PlaneProxy[] planes;
        private final ImageInfo info;
        private final AtomicBoolean closed = new AtomicBoolean();
        private Rect crop;

        MediaImageProxy(Image image, Rect crop) {
            this.image = image;
            this.crop = new Rect(crop);
            Image.Plane[] source = image.getPlanes();
            planes = new PlaneProxy[source.length];
            for (int i = 0; i < source.length; i++) {
                Image.Plane plane = source[i];
                planes[i] = new PlaneProxy() {
                    public int getRowStride() { return plane.getRowStride(); }
                    public int getPixelStride() { return plane.getPixelStride(); }
                    public ByteBuffer getBuffer() { return plane.getBuffer(); }
                };
            }
            long timestamp = image.getTimestamp();
            info = new ImageInfo() {
                public TagBundle getTagBundle() { return TagBundle.emptyBundle(); }
                public long getTimestamp() { return timestamp; }
                public int getRotationDegrees() { return 0; }
                public void populateExifData(ExifData.Builder builder) {}
            };
        }

        public void close() { if (closed.compareAndSet(false, true)) image.close(); }
        public Rect getCropRect() { return new Rect(crop); }
        public void setCropRect(Rect rect) { crop = new Rect(rect); }
        public int getFormat() { return image.getFormat(); }
        public int getHeight() { return image.getHeight(); }
        public int getWidth() { return image.getWidth(); }
        public PlaneProxy[] getPlanes() { return planes; }
        public ImageInfo getImageInfo() { return info; }
        public Image getImage() { return image; }
    }

    public void clearLastPayload() { synchronized (payloadLock) { lastPayload = null; } }

    public Stats stats() {
        synchronized (statsLock) {
            Stats s = new Stats();
            s.cameraId = cameraId;
            s.width = captureWidth; s.height = captureHeight;
            s.decodeWidth = decodeWidth; s.decodeHeight = decodeHeight;
            s.targetFps = targetFps; s.requestedFps = selection.targetFps == 0 ? "Auto (" + targetFps + ")" : String.valueOf(targetFps);
            s.requestRange = configuredFpsRange == null ? "unavailable" : configuredFpsRange.toString();
            s.workerCount = DECODE_WORKER_COUNT;
            s.cameraFrames = cameraFrames; s.sensorFrames = sensorFrames; s.imageReaderFrames = imageReaderFrames;
            s.attempts = attempts; s.successes = successes; s.failures = failures; s.busyDrops = busyDrops;
            s.fallbackAttempts = fallbackAttempts; s.fallbackSuccesses = fallbackSuccesses;
            s.fullDetectorAttempts = fullDetectorAttempts;
            s.recoveryAttempts = recoveryAttempts; s.recoverySuccesses = recoverySuccesses;
            s.fullDetectorTotalNanos = fullDetectorTotalNanos; s.fullDetectorMaxNanos = fullDetectorMaxNanos;
            s.recoveryTotalNanos = recoveryTotalNanos; s.recoveryMaxNanos = recoveryMaxNanos;
            s.exposureTimeNanos = exposureTimeNanos; s.sensorFrameDurationNanos = sensorFrameDurationNanos;
            s.decoderCrop = captureCrop == null ? "-" : captureCrop.left + "," + captureCrop.top + "–" + captureCrop.right + "," + captureCrop.bottom;
            s.previewWidth = previewWidth; s.previewHeight = previewHeight;
            s.displayRotation = previewDisplayRotation; s.sensorOrientation = sensorOrientation;
            s.relativeRotation = previewRelativeRotation; s.previewScale = previewScale;
            s.sensorFps = rate(firstSensorTimestamp, lastSensorTimestamp, sensorFrames);
            s.imageReaderFps = rate(firstImageTimestamp, lastImageTimestamp, imageReaderFrames);
            s.cameraFps = s.imageReaderFps;
            long span = lastImageTimestamp > firstImageTimestamp ? lastImageTimestamp - firstImageTimestamp : 0;
            s.attemptFps = span > 0 ? attempts * 1e9 / span : 0;
            s.avgAttemptMs = attempts == 0 ? 0 : (totalAttemptNanos / 1e6) / attempts;
            s.maxAttemptMs = maxAttemptNanos / 1e6;
            s.sessionStatus = sessionStatus;
            s.classification = classify(s.sensorFps, s.imageReaderFps);
            return s;
        }
    }

    private static double rate(long first, long last, long count) {
        return first != 0 && last > first && count > 1 ? (count - 1) * 1e9 / (last - first) : 0;
    }

    private String classify(double sensorFps, double imageFps) {
        if ("unsupported".equals(sessionStatus)) return "unsupported";
        if (firstRuntimeNanos == 0 || System.nanoTime() - firstRuntimeNanos < CLASSIFICATION_WARMUP_NANOS) return "warming up";
        if (sensorFps <= 0 || imageFps <= 0) return "unstable";
        if (targetFps == 60 && sensorFps >= 52 && imageFps >= 52) return "confirmed 60 FPS";
        if (targetFps == 60 && sensorFps >= 52 && imageFps < 45) return "sensor 60 FPS / ImageReader slow";
        if (targetFps == 60 && sensorFps >= 24 && sensorFps <= 38) return "confirmed 30 FPS (60 fallback)";
        if (targetFps == 30 && sensorFps >= 25 && imageFps >= 25) return "confirmed 30 FPS";
        if (sensorFps > 45 && imageFps < 45) return "sensor faster than ImageReader";
        return "unstable";
    }

    public void stop() {
        running = false;
        try {
            if (session != null) session.close();
            if (camera != null) camera.close();
            DecodeWorker[] workers = decodeWorkers;
            if (workers != null) {
                for (DecodeWorker worker : workers) worker.shutdown();
                for (DecodeWorker worker : workers) worker.await();
            }
            if (reader != null) reader.close();
        } finally {
            session = null; camera = null; reader = null; decodeWorkers = null;
            if (previewLayoutListenerAttached) {
                preview.removeOnLayoutChangeListener(previewLayoutListener);
                previewLayoutListenerAttached = false;
            }
            if (cameraThread != null) cameraThread.quitSafely();
            cameraThread = null; cameraHandler = null;
            synchronized (payloadLock) { lastPayload = null; }
        }
    }
}
