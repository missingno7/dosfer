package org.dosfer.receiver;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.opengl.GLES30;
import android.opengl.Matrix;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * RGB3's high-resolution camera path. Camera2 renders into an external OES
 * texture, which means the camera ISP performs the YUV-to-RGB conversion. A
 * GLES3 MRT draw then samples that RGB texture for every source pixel in the
 * box and writes independent R8 planes. No full-size RGB image exists in
 * Java (or in a CPU buffer), and the PBO ring keeps readback asynchronous.
 */
final class GpuRgbProcessor {
    private static final String TAG = "DOSFER-GpuRgb";
    private static final int RING_SIZE = 2;
    private static final long PREVIEW_INTERVAL_NANOS = 66_666_667L; // 15 FPS
    static final int DROP_COALESCED = 1;
    static final int DROP_PBO_BUSY = 2;
    static final int DROP_DECODER_BUSY = 3;

    interface Listener {
        default void frameArrived(long cameraTimestampNanos) {}
        default void dropped(int reason, int pendingReadbacks) {}
        /** Cheap backpressure test. False means: keep preview current, but do
         * not spend GPU/readback bandwidth on a frame that cannot be decoded. */
        default boolean canAcceptPlanes() { return true; }
        /** Called on the GL thread. The buffers are valid only for this call.
         * arrivalNanos is a local System.nanoTime() captured when the latest
         * SurfaceTexture image was latched; unlike SurfaceTexture.getTimestamp()
         * it is safe to compare with the other local timing values. */
        boolean planes(ByteBuffer[] source, int side, long cameraTimestampNanos,
                       long arrivalNanos, long dispatchNanos, long gpuCommandNanos,
                       long readbackLatencyNanos, int pendingReadbacks);
        void gpuError(String message);
    }

    private static final class Slot {
        int pbo;
        long fence;
        long cameraTimestampNanos;
        long arrivalNanos;
        long dispatchedNanos;
        long commandNanos;
        boolean pending;
    }

    private final SurfaceTexture previewTexture;
    private final int sourceWidth, sourceHeight, cropLeft, cropTop, cropSide;
    private final Listener listener;
    private final HandlerThread thread = new HandlerThread("dosfer-rgb-gpu");
    private final Slot[] slots = new Slot[RING_SIZE];
    private final float[] textureMatrix = new float[16];
    /* uCrop lives in the logical coordinates before SurfaceTexture's
     * transform. This is derived each frame from the raw sensor crop below. */
    private final float[] logicalCrop = new float[4];
    private final float[] inverseTextureMatrix = new float[16];
    private final float[] cropInputPoint = new float[4];
    private final float[] cropOutputPoint = new float[4];
    private final ByteBuffer[] mappedPlanes = new ByteBuffer[3];
    private final int[] drawBuffers = {
            GLES30.GL_COLOR_ATTACHMENT0, GLES30.GL_COLOR_ATTACHMENT1, GLES30.GL_COLOR_ATTACHMENT2};

    private Handler handler;
    private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
    private EGLContext context = EGL14.EGL_NO_CONTEXT;
    private EGLSurface window = EGL14.EGL_NO_SURFACE;
    private Surface previewSurface, cameraSurface;
    private SurfaceTexture cameraTexture;
    private int cameraTextureId;
    private int planeProgram, previewProgram, framebuffer;
    private int planeCameraUniform, planeMatrixUniform, planeCropUniform;
    private int planeFactorUniform, planeOutputSideUniform;
    private int previewCameraUniform, previewMatrixUniform, previewCropUniform;
    private final int[] planeTextures = new int[3];
    private int allocatedSide;
    private int requestedFactor;
    private volatile int nextFactor;
    private int nextSlot;
    private long nextPreviewNanos;
    /* The SurfaceTexture listener can enqueue callbacks faster than a full
     * GLES/readback pass completes. Keep at most one expensive frame task
     * queued; updateTexImage() then latches the newest buffer and intentionally
     * skips stale frames instead of accumulating seconds of latency. */
    private boolean frameTaskQueued;
    private boolean started, stopping;

    GpuRgbProcessor(SurfaceTexture previewTexture, int sourceWidth, int sourceHeight,
                    CameraCrop crop, int downsampleFactor, Listener listener) {
        this.previewTexture = previewTexture;
        this.sourceWidth = sourceWidth;
        this.sourceHeight = sourceHeight;
        this.cropLeft = crop.left;
        this.cropTop = crop.top;
        this.cropSide = crop.side;
        this.listener = listener;
        requestedFactor = normalizeFactor(downsampleFactor);
        nextFactor = requestedFactor;
        for (int i = 0; i < slots.length; i++) slots[i] = new Slot();
    }

    Surface start() throws Exception {
        thread.start();
        handler = new Handler(thread.getLooper());
        CountDownLatch ready = new CountDownLatch(1);
        final Exception[] failure = new Exception[1];
        handler.post(() -> {
            try { initialise(); }
            catch (Exception e) { failure[0] = e; release(); }
            finally { ready.countDown(); }
        });
        if (!ready.await(3, TimeUnit.SECONDS)) throw new IllegalStateException("GPU setup timed out");
        if (failure[0] != null) throw failure[0];
        if (cameraSurface == null) throw new IllegalStateException("GPU camera surface unavailable");
        return cameraSurface;
    }

    void setDownsampleFactor(int factor) { nextFactor = normalizeFactor(factor); }

    private static int normalizeFactor(int factor) {
        return Math.max(1,Math.min(factor,4));
    }

    void stop() {
        stopping = true;
        Handler h = handler;
        if (h == null) return;
        CountDownLatch done = new CountDownLatch(1);
        h.post(() -> { release(); done.countDown(); });
        try { done.await(1200, TimeUnit.MILLISECONDS); }
        catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        thread.quitSafely();
    }

    private void initialise() throws Exception {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (display == EGL14.EGL_NO_DISPLAY) throw new IllegalStateException("no EGL display");
        int[] version = new int[2];
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) throw eglFailure("EGL initialize");
        int[] attrs = {
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                /* EGL_OPENGL_ES3_BIT_KHR is 0x40 but is not exposed as an
                 * EGL14 Java constant on every Android API level. */
                EGL14.EGL_RENDERABLE_TYPE, 0x40,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        if (!EGL14.eglChooseConfig(display, attrs, 0, configs, 0, 1, count, 0) || count[0] == 0)
            throw eglFailure("GLES3 EGL config");
        int[] contextAttrs = {EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE};
        context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT, contextAttrs, 0);
        if (context == null || context == EGL14.EGL_NO_CONTEXT) throw eglFailure("GLES3 context");
        previewSurface = new Surface(previewTexture);
        window = EGL14.eglCreateWindowSurface(display, configs[0], previewSurface,
                new int[]{EGL14.EGL_NONE}, 0);
        if (window == null || window == EGL14.EGL_NO_SURFACE) throw eglFailure("preview EGL surface");
        makeCurrent();
        /* Preview presentation must never pace the scanner. A blocking
         * eglSwapBuffers() in the critical path was enough to let camera frame
         * callbacks accumulate behind the GL thread. */
        EGL14.eglSwapInterval(display, 0);

        int[] maxDrawBuffers = new int[1];
        GLES30.glGetIntegerv(GLES30.GL_MAX_DRAW_BUFFERS, maxDrawBuffers, 0);
        if (maxDrawBuffers[0] < 3) throw new IllegalStateException("GPU has fewer than three draw buffers");
        checkGl("GLES3 capability");

        cameraTextureId = createExternalTexture();
        cameraTexture = new SurfaceTexture(cameraTextureId);
        cameraTexture.setDefaultBufferSize(sourceWidth, sourceHeight);
        cameraTexture.setOnFrameAvailableListener(texture -> scheduleLatestFrame(), handler);
        cameraSurface = new Surface(cameraTexture);
        planeProgram = buildProgram(VERTEX_SHADER, PLANES_FRAGMENT_SHADER);
        previewProgram = buildProgram(VERTEX_SHADER, PREVIEW_FRAGMENT_SHADER);
        cacheUniformLocations();
        int[] handles = new int[1];
        GLES30.glGenFramebuffers(1, handles, 0);
        framebuffer = handles[0];
        GLES30.glGenTextures(3, planeTextures, 0);
        for (Slot slot : slots) {
            int[] handle = new int[1];
            GLES30.glGenBuffers(1, handle, 0);
            slot.pbo = handle[0];
        }
        ensureTargets(requestedFactor);
        started = true;
    }

    private void scheduleLatestFrame() {
        if (!started || stopping) return;
        if (frameTaskQueued) {
            /* Deliberately discard the older signal. SurfaceTexture keeps the
             * latest producer buffer, so the queued task will latch the newest
             * image available when it runs. */
            listener.dropped(DROP_COALESCED, pendingCount());
            return;
        }
        frameTaskQueued = true;
        if (!handler.post(this::frame)) frameTaskQueued = false;
    }

    private void frame() {
        frameTaskQueued = false;
        if (!started || stopping) return;
        try {
            cameraTexture.updateTexImage();
            long arrivalNanos = System.nanoTime();
            cameraTexture.getTransformMatrix(textureMatrix);
            updateLogicalCrop();
            long timestamp = cameraTexture.getTimestamp();
            listener.frameArrived(timestamp);

            /* First retire completed transfers. This keeps the PBO ring moving
             * before any new rendering/presentation work is submitted. */
            drainReadbacks();

            int wanted = nextFactor;
            if (wanted != requestedFactor && !hasPendingReadbacks()) ensureTargets(wanted);

            /* Dispatch scanner work BEFORE preview presentation. eglSwapBuffers
             * belongs to the UI path and must not sit in front of QR decode. */
            Slot slot = slots[nextSlot];
            if (slot.pending) {
                listener.dropped(DROP_PBO_BUSY, pendingCount());
            } else if (!listener.canAcceptPlanes()) {
                listener.dropped(DROP_DECODER_BUSY, pendingCount());
            } else {
                long dispatchStart = System.nanoTime();
                drawPlanes(slot, timestamp, arrivalNanos, dispatchStart);
                nextSlot = (nextSlot + 1) % slots.length;
            }

            /* Preview is best-effort, intentionally last, and capped at 15 FPS
             * so display composition cannot consume every camera interval. */
            long previewNow = System.nanoTime();
            if (previewNow >= nextPreviewNanos) {
                drawPreview();
                nextPreviewNanos = previewNow + PREVIEW_INTERVAL_NANOS;
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "GPU frame failed", e);
            listener.gpuError("GPU preprocessing failed: " + e.getMessage());
        }
    }

    private boolean hasPendingReadbacks() {
        for (Slot slot : slots) if (slot.pending) return true;
        return false;
    }

    private int pendingCount() {
        int count = 0;
        for (Slot slot : slots) if (slot.pending) count++;
        return count;
    }

    private void ensureTargets(int factor) {
        requestedFactor = normalizeFactor(factor);
        int side = cropSide / requestedFactor;
        if (side == allocatedSide) return;
        allocatedSide = side;
        int bytes = side * side;
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, framebuffer);
        for (int channel = 0; channel < 3; channel++) {
            GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, planeTextures[channel]);
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_NEAREST);
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_NEAREST);
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE);
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE);
            GLES30.glTexImage2D(GLES30.GL_TEXTURE_2D, 0, GLES30.GL_R8, side, side, 0,
                    GLES30.GL_RED, GLES30.GL_UNSIGNED_BYTE, null);
            GLES30.glFramebufferTexture2D(GLES30.GL_FRAMEBUFFER,
                    GLES30.GL_COLOR_ATTACHMENT0 + channel, GLES30.GL_TEXTURE_2D, planeTextures[channel], 0);
        }
        GLES30.glDrawBuffers(3, drawBuffers, 0);
        if (GLES30.glCheckFramebufferStatus(GLES30.GL_FRAMEBUFFER) != GLES30.GL_FRAMEBUFFER_COMPLETE)
            throw new IllegalStateException("R8 framebuffer is incomplete");
        for (Slot slot : slots) {
            GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, slot.pbo);
            GLES30.glBufferData(GLES30.GL_PIXEL_PACK_BUFFER, bytes * 3, null, GLES30.GL_STREAM_READ);
        }
        GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, 0);
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0);
        checkGl("allocate R8 planes");
    }

    private void drawPlanes(Slot slot, long timestamp, long arrivalNanos, long dispatchStart) {
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, framebuffer);
        GLES30.glViewport(0, 0, allocatedSide, allocatedSide);
        GLES30.glDrawBuffers(3, drawBuffers, 0);
        GLES30.glUseProgram(planeProgram);
        bindPlaneUniforms();
        GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4);
        GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, slot.pbo);
        /* At 4x the 750px R8 rows are not four-byte aligned. The default
         * GL_PACK_ALIGNMENT would pad every row to 752 bytes and make a
         * 750x750 readback exceed its tightly sized PBO plane. */
        GLES30.glPixelStorei(GLES30.GL_PACK_ALIGNMENT, 1);
        for (int channel = 0; channel < 3; channel++) {
            GLES30.glReadBuffer(GLES30.GL_COLOR_ATTACHMENT0 + channel);
            GLES30.glReadPixels(0, 0, allocatedSide, allocatedSide, GLES30.GL_RED,
                    GLES30.GL_UNSIGNED_BYTE, channel * allocatedSide * allocatedSide);
        }
        GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, 0);
        slot.fence = GLES30.glFenceSync(GLES30.GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        GLES30.glFlush();
        slot.cameraTimestampNanos = timestamp;
        slot.arrivalNanos = arrivalNanos;
        slot.dispatchedNanos = dispatchStart;
        slot.commandNanos = System.nanoTime() - dispatchStart;
        slot.pending = true;
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0);
        checkGl("RGB plane dispatch");
    }

    private void drainReadbacks() {
        for (Slot slot : slots) {
            if (!slot.pending) continue;
            int state = GLES30.glClientWaitSync(slot.fence, 0, 0);
            if (state != GLES30.GL_ALREADY_SIGNALED && state != GLES30.GL_CONDITION_SATISFIED) continue;
            long now = System.nanoTime();
            int bytes = allocatedSide * allocatedSide;
            GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, slot.pbo);
            ByteBuffer mapped = (ByteBuffer) GLES30.glMapBufferRange(GLES30.GL_PIXEL_PACK_BUFFER,
                    0, bytes * 3, GLES30.GL_MAP_READ_BIT);
            if (mapped == null) throw new IllegalStateException("PBO map failed");
            for (int channel = 0; channel < 3; channel++) {
                ByteBuffer plane = mapped.duplicate();
                plane.position(channel * bytes);
                plane.limit((channel + 1) * bytes);
                mappedPlanes[channel] = plane.slice();
            }
            boolean accepted = listener.planes(mappedPlanes, allocatedSide, slot.cameraTimestampNanos,
                    slot.arrivalNanos, slot.dispatchedNanos, slot.commandNanos,
                    now - slot.dispatchedNanos, pendingCount());
            GLES30.glUnmapBuffer(GLES30.GL_PIXEL_PACK_BUFFER);
            GLES30.glBindBuffer(GLES30.GL_PIXEL_PACK_BUFFER, 0);
            GLES30.glDeleteSync(slot.fence);
            slot.fence = 0;
            slot.pending = false;
            if (!accepted) Log.d(TAG, "dropped GPU planes: decoder busy");
        }
    }

    private void drawPreview() {
        int[] value = new int[1];
        EGL14.eglQuerySurface(display, window, EGL14.EGL_WIDTH, value, 0);
        int width = value[0];
        EGL14.eglQuerySurface(display, window, EGL14.EGL_HEIGHT, value, 0);
        int height = value[0];
        if (width <= 0 || height <= 0) return;
        GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER, 0);
        GLES30.glViewport(0, 0, width, height);
        GLES30.glUseProgram(previewProgram);
        bindPreviewUniforms();
        GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4);
        EGL14.eglSwapBuffers(display, window);
    }

    private void bindPlaneUniforms() {
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0);
        GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, cameraTextureId);
        GLES30.glUniform1i(planeCameraUniform, 0);
        GLES30.glUniformMatrix4fv(planeMatrixUniform, 1,
                false, textureMatrix, 0);
        GLES30.glUniform4f(planeCropUniform,
                logicalCrop[0], logicalCrop[1], logicalCrop[2], logicalCrop[3]);
        GLES30.glUniform1i(planeFactorUniform, requestedFactor);
        GLES30.glUniform1i(planeOutputSideUniform, allocatedSide);
    }

    private void bindPreviewUniforms() {
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0);
        GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, cameraTextureId);
        GLES30.glUniform1i(previewCameraUniform, 0);
        GLES30.glUniformMatrix4fv(previewMatrixUniform, 1, false, textureMatrix, 0);
        GLES30.glUniform4f(previewCropUniform,
                logicalCrop[0], logicalCrop[1], logicalCrop[2], logicalCrop[3]);
    }

    private void cacheUniformLocations() {
        planeCameraUniform = uniform(planeProgram, "uCamera");
        planeMatrixUniform = uniform(planeProgram, "uTextureMatrix");
        planeCropUniform = uniform(planeProgram, "uCrop");
        planeFactorUniform = uniform(planeProgram, "uFactor");
        planeOutputSideUniform = uniform(planeProgram, "uOutputSide");
        previewCameraUniform = uniform(previewProgram, "uCamera");
        previewMatrixUniform = uniform(previewProgram, "uTextureMatrix");
        previewCropUniform = uniform(previewProgram, "uCrop");
    }

    private static int uniform(int program, String name) {
        int location = GLES30.glGetUniformLocation(program, name);
        if (location < 0) throw new IllegalStateException("missing shader uniform " + name);
        return location;
    }

    /**
     * CameraCrop is in the requested sensor buffer's coordinates. The OES
     * shader, however, accepts coordinates before SurfaceTexture applies its
     * buffer-to-logical rotation/mirror matrix. Transform the crop rectangle
     * back into that logical space instead of treating raw x/y as UVs.
     */
    private void updateLogicalCrop() {
        if (!Matrix.invertM(inverseTextureMatrix, 0, textureMatrix, 0)) {
            throw new IllegalStateException("SurfaceTexture transform is not invertible");
        }
        float left = cropLeft / (float) sourceWidth;
        float top = cropTop / (float) sourceHeight;
        float right = (cropLeft + cropSide) / (float) sourceWidth;
        float bottom = (cropTop + cropSide) / (float) sourceHeight;
        float minX = Float.POSITIVE_INFINITY, minY = Float.POSITIVE_INFINITY;
        float maxX = Float.NEGATIVE_INFINITY, maxY = Float.NEGATIVE_INFINITY;
        float[] corners = {left, top, right, top, left, bottom, right, bottom};
        for (int i = 0; i < corners.length; i += 2) {
            cropInputPoint[0] = corners[i];
            cropInputPoint[1] = corners[i + 1];
            cropInputPoint[2] = 0f;
            cropInputPoint[3] = 1f;
            Matrix.multiplyMV(cropOutputPoint, 0, inverseTextureMatrix, 0, cropInputPoint, 0);
            float reciprocalW = 1f / cropOutputPoint[3];
            float x = cropOutputPoint[0] * reciprocalW;
            float y = cropOutputPoint[1] * reciprocalW;
            minX = Math.min(minX, x);
            minY = Math.min(minY, y);
            maxX = Math.max(maxX, x);
            maxY = Math.max(maxY, y);
        }
        logicalCrop[0] = minX;
        logicalCrop[1] = minY;
        logicalCrop[2] = maxX - minX;
        logicalCrop[3] = maxY - minY;
    }

    private static int createExternalTexture() {
        int[] handles = new int[1];
        GLES30.glGenTextures(1, handles, 0);
        GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, handles[0]);
        GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR);
        GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR);
        GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE);
        GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE);
        return handles[0];
    }

    private static int buildProgram(String vertex, String fragment) {
        int vs = compile(GLES30.GL_VERTEX_SHADER, vertex);
        int fs = compile(GLES30.GL_FRAGMENT_SHADER, fragment);
        int program = GLES30.glCreateProgram();
        GLES30.glAttachShader(program, vs);
        GLES30.glAttachShader(program, fs);
        GLES30.glLinkProgram(program);
        int[] linked = new int[1];
        GLES30.glGetProgramiv(program, GLES30.GL_LINK_STATUS, linked, 0);
        GLES30.glDeleteShader(vs);
        GLES30.glDeleteShader(fs);
        if (linked[0] == 0) {
            String log = GLES30.glGetProgramInfoLog(program);
            GLES30.glDeleteProgram(program);
            throw new IllegalStateException("shader link: " + log);
        }
        return program;
    }

    private static int compile(int type, String source) {
        int shader = GLES30.glCreateShader(type);
        GLES30.glShaderSource(shader, source);
        GLES30.glCompileShader(shader);
        int[] compiled = new int[1];
        GLES30.glGetShaderiv(shader, GLES30.GL_COMPILE_STATUS, compiled, 0);
        if (compiled[0] == 0) {
            String log = GLES30.glGetShaderInfoLog(shader);
            GLES30.glDeleteShader(shader);
            throw new IllegalStateException("shader compile: " + log);
        }
        return shader;
    }

    private void makeCurrent() {
        if (!EGL14.eglMakeCurrent(display, window, window, context)) throw eglFailure("make EGL current");
    }

    private static IllegalStateException eglFailure(String where) {
        return new IllegalStateException(where + " (EGL 0x" + Integer.toHexString(EGL14.eglGetError()) + ")");
    }

    private static void checkGl(String where) {
        int error = GLES30.glGetError();
        if (error != GLES30.GL_NO_ERROR) throw new IllegalStateException(where + " (GL 0x" + Integer.toHexString(error) + ")");
    }

    private void release() {
        if (display != EGL14.EGL_NO_DISPLAY && context != EGL14.EGL_NO_CONTEXT && window != EGL14.EGL_NO_SURFACE) {
            try {
                EGL14.eglMakeCurrent(display, window, window, context);
                for (Slot slot : slots) {
                    if (slot.fence != 0) GLES30.glDeleteSync(slot.fence);
                    if (slot.pbo != 0) GLES30.glDeleteBuffers(1, new int[]{slot.pbo}, 0);
                }
                if (framebuffer != 0) GLES30.glDeleteFramebuffers(1, new int[]{framebuffer}, 0);
                if (planeTextures[0] != 0) GLES30.glDeleteTextures(3, planeTextures, 0);
                if (cameraTextureId != 0) GLES30.glDeleteTextures(1, new int[]{cameraTextureId}, 0);
                if (planeProgram != 0) GLES30.glDeleteProgram(planeProgram);
                if (previewProgram != 0) GLES30.glDeleteProgram(previewProgram);
            } catch (RuntimeException ignored) {}
        }
        if (cameraSurface != null) cameraSurface.release();
        if (cameraTexture != null) cameraTexture.release();
        if (window != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, window);
        if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context);
        if (display != EGL14.EGL_NO_DISPLAY) EGL14.eglTerminate(display);
        if (previewSurface != null) previewSurface.release();
        cameraSurface = null;
        cameraTexture = null;
        window = EGL14.EGL_NO_SURFACE;
        context = EGL14.EGL_NO_CONTEXT;
        display = EGL14.EGL_NO_DISPLAY;
        started = false;
    }

    private static final String VERTEX_SHADER = "#version 300 es\n"
            + "precision highp float;\n"
            + "out vec2 vUv;\n"
            + "void main() {\n"
            + "  vec2 p;\n"
            + "  if (gl_VertexID == 0) p=vec2(-1.0,-1.0);\n"
            + "  else if (gl_VertexID == 1) p=vec2(1.0,-1.0);\n"
            + "  else if (gl_VertexID == 2) p=vec2(-1.0,1.0);\n"
            + "  else p=vec2(1.0,1.0);\n"
            + "  vUv=(p+1.0)*0.5; gl_Position=vec4(p,0.0,1.0);\n"
            + "}\n";

    private static final String PLANES_FRAGMENT_SHADER = "#version 300 es\n"
            + "#extension GL_OES_EGL_image_external_essl3 : require\n"
            + "precision highp float;\n"
            + "uniform samplerExternalOES uCamera; uniform mat4 uTextureMatrix;\n"
            + "uniform vec4 uCrop; uniform int uFactor; uniform int uOutputSide;\n"
            + "layout(location=0) out vec4 outR; layout(location=1) out vec4 outG; layout(location=2) out vec4 outB;\n"
            + "vec3 sampleRgb(vec2 p) { return texture(uCamera, (uTextureMatrix * vec4(p,0.0,1.0)).xy).rgb; }\n"
            + "vec3 sampleSource(vec2 p) { vec2 uv=uCrop.xy+(p/float(uOutputSide*uFactor))*uCrop.zw; return sampleRgb(uv); }\n"
            + "void main() {\n"
            + "  float outputY = float(int(gl_FragCoord.y - 0.5));\n"
            + "  outputY = float(uOutputSide - 1) - outputY;\n"
            + "  vec2 base=vec2(gl_FragCoord.x-0.5,outputY)*float(uFactor);\n"
            + "  vec3 total;\n"
            + "  if (uFactor==1) total=sampleSource(base+vec2(0.5));\n"
            + "  else if (uFactor==2) total=sampleSource(base+vec2(1.0));\n"
            + "  else {\n"
            + "    float farOffset=uFactor==4?3.0:2.5; float nearWeight=uFactor==4?0.5:0.666666667;\n"
            + "    float farWeight=1.0-nearWeight;\n"
            + "    vec3 nearRow=sampleSource(base+vec2(1.0,1.0))*nearWeight\n"
            + "        +sampleSource(base+vec2(farOffset,1.0))*farWeight;\n"
            + "    vec3 farRow=sampleSource(base+vec2(1.0,farOffset))*nearWeight\n"
            + "        +sampleSource(base+vec2(farOffset,farOffset))*farWeight;\n"
            + "    total=nearRow*nearWeight+farRow*farWeight;\n"
            + "  }\n"
            + "  outR=vec4(total.r); outG=vec4(total.g); outB=vec4(total.b);\n"
            + "}\n";

    private static final String PREVIEW_FRAGMENT_SHADER = "#version 300 es\n"
            + "#extension GL_OES_EGL_image_external_essl3 : require\n"
            + "precision mediump float; in vec2 vUv;\n"
            + "uniform samplerExternalOES uCamera; uniform mat4 uTextureMatrix; uniform vec4 uCrop; out vec4 outColor;\n"
            + "void main() { vec2 p=uCrop.xy+vUv*uCrop.zw; outColor=texture(uCamera,(uTextureMatrix*vec4(p,0.0,1.0)).xy); }\n";
}
