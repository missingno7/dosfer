package org.dosfer.receiver;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Paint;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.TextureView;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class MainActivity extends Activity {
    private static final int CAMERA_PERMISSION = 10, DESTINATION = 20;
    private TextureView preview;
    private ImageView decoderPreview;
    private TextView status, result, modesText;
    private Button reconstruct, decoderView, decoderSize;
    private Spinner cameraSelector, fpsSelector, resolutionSelector;
    private CheckBox automaticSelector;
    private LinearLayout cameraPage, receiverPage;
    private FrameLayout pages;
    private SessionStore store;
    private CameraScanner scanner;
    private Uri destination;
    private CameraSettings.Selection cameraSelection;
    private final List<String> cameraIds = new ArrayList<>();
    private List<CameraMode> discoveredModes = new ArrayList<>();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private String lastMessage = "";
    private boolean receiverVisible;
    private int decoderViewChannel = -1;
    private int highResolutionDownsample = 3;
    private Bitmap decoderBitmap;
    private int[] decoderPixels;
    private final Runnable refresh = new Runnable() {
        public void run() { showStats(); ui.postDelayed(this, 250); }
    };

    /**
     * Owns the preview and overlay as one square. Keeping the square constraint
     * on the parent is important: a MATCH_PARENT overlay inside a WRAP_CONTENT
     * FrameLayout can otherwise make the whole preview row as tall as the
     * remaining screen, which pushes the receiver UI off-screen.
     */
    private static final class SquarePreviewFrame extends FrameLayout {
        SquarePreviewFrame(Context context) { super(context); }

        @Override protected void onMeasure(int widthSpec, int heightSpec) {
            int widthMode = MeasureSpec.getMode(widthSpec);
            int heightMode = MeasureSpec.getMode(heightSpec);
            int widthSize = MeasureSpec.getSize(widthSpec);
            int heightSize = MeasureSpec.getSize(heightSpec);

            int side;
            if (widthMode == MeasureSpec.UNSPECIFIED) {
                side = heightMode == MeasureSpec.UNSPECIFIED ? 1 : heightSize;
            } else {
                side = widthSize;
            }
            if (heightMode == MeasureSpec.EXACTLY || heightMode == MeasureSpec.AT_MOST) {
                side = Math.min(side, heightSize);
            }
            side = Math.max(1, side);
            int exact = MeasureSpec.makeMeasureSpec(side, MeasureSpec.EXACTLY);
            super.onMeasure(exact, exact);
            setMeasuredDimension(side, side);
        }
    }

    private static final class PreviewOverlayView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        PreviewOverlayView(Context context) { super(context); setWillNotDraw(false); }
        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2); paint.setColor(0x99FFFFFF);
            canvas.drawRect(1, 1, getWidth() - 2, getHeight() - 2, paint);
            paint.setColor(0x6688FF88); paint.setStrokeWidth(1);
            canvas.drawLine(getWidth() / 2f - 18, getHeight() / 2f, getWidth() / 2f + 18, getHeight() / 2f, paint);
            canvas.drawLine(getWidth() / 2f, getHeight() / 2f - 18, getWidth() / 2f, getHeight() / 2f + 18, paint);
        }
    }

    @Override public void onCreate(Bundle b) {
        super.onCreate(b);
        store = new SessionStore(this);
        cameraSelection = CameraSettings.load(this);
        buildUi();
        refreshCameraOptions();
        String saved = getPreferences(0).getString("destination", null);
        if (saved != null) destination = Uri.parse(saved);
        ui.post(refresh);
        receiverVisible = true;
        showReceiverPage();
        preview.post(this::ensureCamera);
    }

    private void buildUi() {
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        pages = new FrameLayout(this);
        pages.setBackgroundColor(Color.rgb(16, 24, 32));
        cameraPage = buildCameraPage();
        receiverPage = buildReceiverPage();
        pages.addView(cameraPage, new FrameLayout.LayoutParams(-1, -1));
        pages.addView(receiverPage, new FrameLayout.LayoutParams(-1, -1));
        setContentView(pages);
    }

    private LinearLayout buildCameraPage() {
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(16, 16, 16, 16);
        TextView heading = text(22, Color.WHITE);
        heading.setText("Camera setup");
        content.addView(heading);
        TextView hint = text(14, Color.LTGRAY);
        hint.setText("Choose the rear camera and capture mode, then open the receiver.");
        content.addView(hint);

        cameraSelector = spinner();
        fpsSelector = spinner();
        resolutionSelector = spinner();
        automaticSelector = new CheckBox(this);
        automaticSelector.setText("Automatic mode selection");
        automaticSelector.setTextColor(Color.WHITE);
        automaticSelector.setChecked(cameraSelection.automatic);
        content.addView(labelled("Rear camera ID", cameraSelector));
        content.addView(labelled("Target FPS (Auto / 60 / 30)", fpsSelector));
        content.addView(labelled("YUV capture resolution", resolutionSelector));
        content.addView(automaticSelector);

        Button openReceiver = new Button(this);
        openReceiver.setText("Apply settings and open receiver");
        openReceiver.setOnClickListener(v -> openReceiver());
        content.addView(openReceiver);

        modesText = text(12, Color.LTGRAY);
        modesText.setTypeface(android.graphics.Typeface.MONOSPACE);
        content.addView(modesText, new LinearLayout.LayoutParams(-1, -2));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        return page;
    }

    private LinearLayout buildReceiverPage() {
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);

        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(8, 8, 8, 8);
        TextView title = text(20, Color.WHITE);
        title.setText("DOSfer Receiver");
        top.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        Button settings = new Button(this);
        settings.setText("Camera setup");
        settings.setOnClickListener(v -> showCameraPage());
        top.addView(settings);
        page.addView(top);

        SquarePreviewFrame previewFrame = new SquarePreviewFrame(this);
        preview = new TextureView(this);
        previewFrame.addView(preview, new FrameLayout.LayoutParams(-1, -1));
        decoderPreview = new ImageView(this);
        decoderPreview.setBackgroundColor(Color.BLACK);
        decoderPreview.setScaleType(ImageView.ScaleType.FIT_XY);
        decoderPreview.setVisibility(View.GONE);
        previewFrame.addView(decoderPreview, new FrameLayout.LayoutParams(-1, -1));
        previewFrame.addView(new PreviewOverlayView(this), new FrameLayout.LayoutParams(-1, -1));
        page.addView(previewFrame, new LinearLayout.LayoutParams(-1, -2));

        status = text(12, Color.WHITE);
        status.setTypeface(android.graphics.Typeface.MONOSPACE);
        ScrollView diagnostics = new ScrollView(this);
        diagnostics.setFillViewport(true);
        diagnostics.addView(status);
        page.addView(diagnostics, new LinearLayout.LayoutParams(-1, 0, 1));

        result = text(20, Color.rgb(100, 255, 140));
        result.setGravity(Gravity.CENTER);
        page.addView(result, new LinearLayout.LayoutParams(-1, -2));
        page.addView(actionBar());
        return page;
    }

    private LinearLayout actionBar() {
        LinearLayout row = new LinearLayout(this);
        row.setPadding(4, 0, 4, 4);

        Button choose = new Button(this);
        choose.setText("Choose destination");
        choose.setOnClickListener(x -> choose());
        row.addView(choose, new LinearLayout.LayoutParams(0, -2, 1));

        Button lock = new Button(this);
        lock.setText("Lock focus/exposure/WB");
        lock.setOnClickListener(x -> { if (scanner != null) scanner.lockStability(); });
        row.addView(lock, new LinearLayout.LayoutParams(0, -2, 1));

        reconstruct = new Button(this);
        reconstruct.setText("Reconstruct");
        reconstruct.setOnClickListener(x -> reconstruct());
        row.addView(reconstruct, new LinearLayout.LayoutParams(0, -2, 1));

        decoderView = new Button(this);
        decoderView.setText("View: camera");
        decoderView.setOnClickListener(x -> cycleDecoderView());
        row.addView(decoderView, new LinearLayout.LayoutParams(0, -2, 1));

        decoderSize = new Button(this);
        decoderSize.setText("Decode: 1020");
        decoderSize.setOnClickListener(x -> cycleDecoderSize());
        row.addView(decoderSize, new LinearLayout.LayoutParams(0, -2, 1));

        Button reset = new Button(this);
        reset.setText("Reset session");
        reset.setOnClickListener(x -> {
            store.reset();
            if (scanner != null) scanner.clearLastPayload();
            lastMessage = "";
        });
        row.addView(reset, new LinearLayout.LayoutParams(0, -2, 1));
        return row;
    }

    private LinearLayout labelled(String label, View view) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView caption = text(12, Color.LTGRAY);
        caption.setText(label);
        row.addView(caption, new LinearLayout.LayoutParams(0, -2, 1));
        row.addView(view, new LinearLayout.LayoutParams(0, -2, 1));
        return row;
    }

    private Spinner spinner() {
        Spinner s = new Spinner(this);
        s.setBackgroundColor(Color.WHITE);
        return s;
    }

    private TextView text(int sp, int color) {
        TextView v = new TextView(this);
        v.setTextSize(sp);
        v.setTextColor(color);
        v.setPadding(8, 4, 8, 4);
        return v;
    }

    private void refreshCameraOptions() {
        try {
            CameraManager manager = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
            cameraIds.clear();
            List<String> labels = new ArrayList<>();
            for (String id : manager.getCameraIdList()) {
                Integer facing = manager.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING);
                if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) {
                    cameraIds.add(id); labels.add("Rear " + id);
                }
            }
            cameraSelector.setAdapter(adapter(labels));
            int selected = cameraIds.indexOf(cameraSelection.cameraId);
            cameraSelector.setSelection(selected < 0 ? 0 : selected);
            cameraSelector.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                public void onItemSelected(AdapterView<?> parent, View view, int position, long id) { discoverSelectedCameraModes(); }
                public void onNothingSelected(AdapterView<?> parent) {}
            });
        } catch (Exception e) { lastMessage = "Camera discovery: " + e.getMessage(); }
        String[] fps = {"Auto", "60", "30"};
        List<String> fpsLabels = new ArrayList<>();
        for (String value : fps) fpsLabels.add(value);
        fpsSelector.setAdapter(adapter(fpsLabels));
        fpsSelector.setSelection(cameraSelection.targetFps == 60 ? 1 : cameraSelection.targetFps == 30 ? 2 : 0);
        automaticSelector.setOnCheckedChangeListener((button, checked) -> resolutionSelector.setEnabled(!checked));
        resolutionSelector.setEnabled(!cameraSelection.automatic);
        discoverSelectedCameraModes();
    }

    private void discoverSelectedCameraModes() {
        int index = cameraSelector.getSelectedItemPosition();
        if (index < 0 || index >= cameraIds.size()) return;
        try {
            CameraManager manager = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
            CameraCharacteristics cc = manager.getCameraCharacteristics(cameraIds.get(index));
            StreamConfigurationMap map = cc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            updateModes(CameraMode.discover(map));
        } catch (Exception e) { lastMessage = "Mode discovery: " + e.getMessage(); }
    }

    private void openReceiver() {
        String cameraId = cameraSelector.getSelectedItemPosition() >= 0 && cameraSelector.getSelectedItemPosition() < cameraIds.size()
                ? cameraIds.get(cameraSelector.getSelectedItemPosition()) : null;
        int fps = fpsSelector.getSelectedItemPosition() == 1 ? 60 : fpsSelector.getSelectedItemPosition() == 2 ? 30 : 0;
        boolean automatic = automaticSelector.isChecked();
        String modeKey = resolutionSelector.getSelectedItem() == null ? "auto"
                : resolutionSelector.getSelectedItem().toString().split(" ", 2)[0];
        cameraSelection = new CameraSettings.Selection(cameraId, fps, modeKey, automatic);
        CameraSettings.save(this, cameraSelection);
        stopCamera();
        receiverVisible = true;
        showReceiverPage();
        preview.post(this::ensureCamera);
    }

    private void showCameraPage() {
        receiverVisible = false;
        stopCamera();
        cameraPage.setVisibility(View.VISIBLE);
        receiverPage.setVisibility(View.GONE);
        refreshCameraOptions();
    }

    private void showReceiverPage() {
        cameraPage.setVisibility(View.GONE);
        receiverPage.setVisibility(View.VISIBLE);
    }

    private void ensureCamera() {
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) startCamera();
        else requestPermissions(new String[]{Manifest.permission.CAMERA}, CAMERA_PERMISSION);
    }

    private void startCamera() {
        if (!receiverVisible || scanner != null) return;
        scanner = new CameraScanner(this, preview, new CameraScanner.Listener() {
            public void payload(byte[] b, long latency) {
                SessionStore.Result r = store.accept(b, latency);
                runOnUiThread(() -> { if (r == SessionStore.Result.INVALID) lastMessage = "INVALID FRAME"; });
            }
            public void error(String m) { runOnUiThread(() -> lastMessage = "Camera: " + m); }
            public void modes(List<CameraMode> modes) { runOnUiThread(() -> updateModes(modes)); }
            public void decoderPlane(int channel, int width, int height, byte[] pixels) {
                runOnUiThread(() -> showDecoderPlane(channel,width,height,pixels));
            }
        }, cameraSelection);
        scanner.start();
        scanner.setDiagnosticChannel(decoderViewChannel);
        scanner.setHighResolutionDownsample(highResolutionDownsample);
    }

    private void stopCamera() {
        if (scanner != null) { scanner.stop(); scanner = null; }
    }

    private void cycleDecoderView() {
        decoderViewChannel++;
        if (decoderViewChannel >= Rgb3Yuv.CHANNELS) decoderViewChannel = -1;
        String label = decoderViewChannel < 0 ? "camera" : decoderViewChannel == Rgb3Yuv.RED ? "R" :
                decoderViewChannel == Rgb3Yuv.GREEN ? "G" : "B";
        decoderView.setText("View: " + label);
        decoderPreview.setVisibility(decoderViewChannel < 0 ? View.GONE : View.VISIBLE);
        if (decoderViewChannel < 0) decoderPreview.setImageBitmap(null);
        if (scanner != null) scanner.setDiagnosticChannel(decoderViewChannel);
    }

    private void showDecoderPlane(int channel, int width, int height, byte[] source) {
        if (channel != decoderViewChannel || decoderPreview == null || source == null || source.length != width*height) return;
        int pixels=width*height;
        if (decoderBitmap == null || decoderBitmap.getWidth() != width || decoderBitmap.getHeight() != height) {
            decoderBitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);
            decoderPixels=new int[pixels];
        }
        for (int i=0;i<pixels;i++) { int value=source[i]&255;decoderPixels[i]=0xff000000|(value<<16)|(value<<8)|value; }
        decoderBitmap.setPixels(decoderPixels,0,width,0,0,width,height);
        decoderPreview.setImageBitmap(decoderBitmap);
    }

    private void cycleDecoderSize() {
        highResolutionDownsample = highResolutionDownsample == 3 ? 4 : 3;
        decoderSize.setText("Decode: " + (highResolutionDownsample == 3 ? "1020" : "765"));
        if (scanner != null) scanner.setHighResolutionDownsample(highResolutionDownsample);
    }

    private void updateModes(List<CameraMode> modes) {
        discoveredModes = new ArrayList<>(modes);
        int target = cameraSelection.targetFps == 30 ? 30 : 60;
        List<CameraMode> ranked = CameraMode.ranked(discoveredModes, target);
        List<String> labels = new ArrayList<>();
        for (CameraMode mode : ranked) labels.add(mode.key() + "  crop " + mode.cropLabel() + "  max " + String.format(Locale.US, "%.1f", mode.theoreticalMaxFps));
        resolutionSelector.setAdapter(adapter(labels));
        int chosen = 0;
        for (int i = 0; i < ranked.size(); i++) if (ranked.get(i).key().equals(cameraSelection.modeKey)) chosen = i;
        resolutionSelector.setSelection(chosen);
        resolutionSelector.setEnabled(!automaticSelector.isChecked());
        showModeList(ranked, scanner == null ? null : scanner.stats());
    }

    private ArrayAdapter<String> adapter(List<String> values) {
        ArrayAdapter<String> result = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, values);
        result.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        return result;
    }

    private void showModeList(List<CameraMode> modes, CameraScanner.Stats current) {
        StringBuilder out = new StringBuilder("Available YUV_420_888 modes\n");
        for (CameraMode mode : modes) {
            boolean selected = current != null && current.width == mode.width && current.height == mode.height;
            out.append(mode.key()).append("  crop ").append(mode.cropLabel())
                    .append("  theoretical ").append(String.format(Locale.US, "%.1f", mode.theoreticalMaxFps)).append(" FPS")
                    .append("  measured ").append(selected && current != null ? String.format(Locale.US, "sensor %.1f / reader %.1f", current.sensorFps, current.imageReaderFps) : "--")
                    .append("  ").append(selected && current != null ? current.classification : (mode.theoretically60Fps ? "60-capable" : "fallback"))
                    .append('\n');
        }
        modesText.setText(out.toString());
    }

    @Override public void onRequestPermissionsResult(int r, String[] p, int[] g) {
        super.onRequestPermissionsResult(r, p, g);
        if (r == CAMERA_PERMISSION && g.length > 0 && g[0] == PackageManager.PERMISSION_GRANTED) {
            refreshCameraOptions();
            if (receiverVisible) startCamera();
        }
        else result.setText("Camera permission is required. Enable it in Android settings.");
    }

    private void choose() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, DESTINATION);
    }

    @Override protected void onActivityResult(int r, int c, Intent data) {
        super.onActivityResult(r, c, data);
        if (r == DESTINATION && c == RESULT_OK && data != null) {
            destination = data.getData();
            if (destination != null) {
                getContentResolver().takePersistableUriPermission(destination, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                getPreferences(0).edit().putString("destination", destination.toString()).apply();
            }
        }
    }

    private void showStats() {
        SessionStore.Stats s = store.stats();
        CameraScanner.Stats c = scanner == null ? new CameraScanner.Stats() : scanner.stats();
        if (status == null) return;
        status.setText(String.format(Locale.US,
                "Session: %08X    DOS window: %d\nUnique: %d / %d in window    Missing: %s\nDuplicate: %d    Invalid: %d    Other session: %d\nDecoded: %.2f fps    Useful: %.0f B/s    Latency: %.1f ms\nCalibration: %d unique    %d missed\nCapture buffer: %dx%d    Decoder crop: %s\nDecode size: %dx%d    RGB downsample: %dx\nPreview view: %dx%d square\nDisplay rotation: %d    Sensor orientation: %d    Relative rotation: %d    Scale: %.3f\nRequested FPS: %s    request range: %s\nSensor FPS: %.1f    ImageReader FPS: %.1f    Attempts: %.1f/s\nNative workers: %d    Busy drops: %d\nPhysical QR frames: %d    No QR: %d    Hard fallback: %d/%d\nTransport mode: %s    RGB channels: %d/%d decoded    Logical frames: %d\nRGB conversion: %d frames    %.1f ms avg\nGPU RGB path: %s    Frames: %d    Busy drops: %d\nGPU arrival->dispatch: %.2f ms    command: %.2f ms    ready: %.2f ms    plane copy: %.2f ms    total: %.2f ms    queue: %.1f\nZXing R/G/B: %.2f / %.2f / %.2f ms\nFull detector: %d    Recovery: %d/%d    %.1f/%.1f ms avg/max\nAttempt time: %.1f ms avg / %.1f ms max\nExposure: %.3f ms    Sensor frame duration: %.3f ms\nCamera state: %s    Classification: %s\nPayload received: %d bytes    Total frames: %d\nDestination: %s\nIntegrity: %s",
                s.session, s.window + 1, s.uniqueWindow, s.expected, s.missing, s.duplicates, s.invalid, s.other,
                s.decodedFps, s.usefulBps, s.avgLatencyMs, s.calibrationUnique, s.calibrationMissed,
                c.width, c.height, c.decoderCrop, c.decodeWidth, c.decodeHeight, c.downsampleFactor, c.previewWidth, c.previewHeight,
                c.displayRotation, c.sensorOrientation, c.relativeRotation, c.previewScale,
                c.requestedFps, c.requestRange,
                c.sensorFps, c.imageReaderFps, c.attemptFps, c.workerCount, c.busyDrops, c.successes, c.failures,
                c.fallbackSuccesses, c.fallbackAttempts,
                c.rgbMode, c.channelSuccesses, c.channelAttempts, c.logicalFrames,
                c.rgbConversions, c.rgbConversions == 0 ? 0 : c.rgbConversionNanos / 1e6 / c.rgbConversions,
                c.gpuRgb ? "GLES3/PBO" : "CPU fallback", c.gpuFrames, c.gpuBusyDrops,
                c.gpuDispatches == 0 ? 0 : c.gpuArrivalToDispatchNanos / 1e6 / c.gpuDispatches,
                c.gpuDispatches == 0 ? 0 : c.gpuCommandNanos / 1e6 / c.gpuDispatches,
                c.gpuReadbacks == 0 ? 0 : c.gpuReadbackNanos / 1e6 / c.gpuReadbacks,
                c.gpuReadbacks == 0 ? 0 : c.gpuCopyNanos / 1e6 / c.gpuReadbacks,
                c.gpuDecodedFrames == 0 ? 0 : c.gpuEndToEndNanos / 1e6 / c.gpuDecodedFrames,
                c.gpuReadbacks == 0 ? 0 : (double) c.gpuQueueDepth / c.gpuReadbacks,
                c.channelDecodeAttempts[Rgb3Yuv.RED] == 0 ? 0 : c.channelDecodeNanos[Rgb3Yuv.RED] / 1e6 / c.channelDecodeAttempts[Rgb3Yuv.RED],
                c.channelDecodeAttempts[Rgb3Yuv.GREEN] == 0 ? 0 : c.channelDecodeNanos[Rgb3Yuv.GREEN] / 1e6 / c.channelDecodeAttempts[Rgb3Yuv.GREEN],
                c.channelDecodeAttempts[Rgb3Yuv.BLUE] == 0 ? 0 : c.channelDecodeNanos[Rgb3Yuv.BLUE] / 1e6 / c.channelDecodeAttempts[Rgb3Yuv.BLUE],
                c.fullDetectorAttempts, c.recoveryAttempts, c.recoverySuccesses, c.recoveryAttempts == 0 ? 0 : c.recoveryTotalNanos / 1e6 / c.recoveryAttempts, c.recoveryMaxNanos / 1e6,
                c.avgAttemptMs, c.maxAttemptMs, c.exposureTimeNanos / 1e6, c.sensorFrameDurationNanos / 1e6,
                c.sessionStatus, c.classification, s.totalPayload, s.uniqueTotal,
                destination == null ? "not selected" : "selected", s.complete ? "all frames present; ready to reconstruct" : "INCOMPLETE"));
        if (!discoveredModes.isEmpty() && modesText != null) showModeList(CameraMode.ranked(discoveredModes, cameraSelection.targetFps == 30 ? 30 : 60), c);
        if (result != null) {
            if (!lastMessage.isEmpty()) result.setText(lastMessage);
            else if (s.session == 0) result.setText(s.calibration);
            else result.setText(s.complete ? "ALL FRAMES PRESENT" : "SCANNING");
        }
        if (reconstruct != null) reconstruct.setEnabled(s.complete && destination != null);
    }

    private void reconstruct() {
        reconstruct.setEnabled(false);
        lastMessage = "Reconstructing…";
        new Thread(() -> {
            Reconstructor.Result r = new Reconstructor(getContentResolver(), destination)
                    .reconstruct(store.orderedFrames(), m -> runOnUiThread(() -> lastMessage = m));
            runOnUiThread(() -> { lastMessage = (r.ok ? "VALID: " : "NOT COMPLETE: ") + r.message; reconstruct.setEnabled(true); });
        }, "dosfer-reconstruct").start();
    }

    @Override protected void onPause() { if (receiverVisible) stopCamera(); super.onPause(); }
    @Override protected void onResume() { super.onResume(); if (receiverVisible && checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) startCamera(); }
    @Override protected void onDestroy() { ui.removeCallbacks(refresh); stopCamera(); super.onDestroy(); }
}
