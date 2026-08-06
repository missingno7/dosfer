package org.dosfer.receiver;

import android.graphics.Rect;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.graphics.ImageFormat;
import android.util.Range;
import android.util.Size;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/** A discovered YUV stream and the capability facts Camera2 exposes for it. */
public final class CameraMode {
    public final int width;
    public final int height;
    public final long minFrameDurationNanos;
    public final double theoreticalMaxFps;
    public final boolean theoretically60Fps;
    public final boolean shorterSideAtLeast1000;

    public CameraMode(int width, int height, long minFrameDurationNanos) {
        this.width = width;
        this.height = height;
        this.minFrameDurationNanos = minFrameDurationNanos;
        theoreticalMaxFps = minFrameDurationNanos > 0 ? 1_000_000_000.0 / minFrameDurationNanos : 0.0;
        theoretically60Fps = theoreticallyCapable(theoreticalMaxFps, 60);
        shorterSideAtLeast1000 = Math.min(width, height) >= 1000;
    }

    public String key() { return width + "x" + height; }

    public String cropLabel() {
        int side = Math.min(width, height);
        return side + "x" + side;
    }

    public static List<CameraMode> discover(StreamConfigurationMap map) {
        if (map == null) return Collections.emptyList();
        Size[] sizes = map.getOutputSizes(ImageFormat.YUV_420_888);
        if (sizes == null) return Collections.emptyList();
        List<CameraMode> result = new ArrayList<>();
        for (Size size : sizes) {
            long duration = map.getOutputMinFrameDuration(ImageFormat.YUV_420_888, size);
            result.add(new CameraMode(size.getWidth(), size.getHeight(), duration));
        }
        return result;
    }

    public static List<CameraMode> ranked(List<CameraMode> modes, int requestedFps) {
        List<CameraMode> result = new ArrayList<>(modes);
        result.sort(Comparator.comparingLong((CameraMode mode) -> rank(mode, requestedFps))
                .thenComparingLong((CameraMode mode) -> -(long) mode.width * mode.height));
        return result;
    }

    /** Lower is better. The first five tiers implement the requested automatic policy. */
    public static long rank(CameraMode mode, int requestedFps) {
        int fpsTier = requestedFps == 60 && mode.theoretically60Fps ? 0
                : requestedFps == 30 && theoreticallyCapable(mode.theoreticalMaxFps, 30) ? 1
                : mode.theoretically60Fps ? 2
                : theoreticallyCapable(mode.theoreticalMaxFps, 30) ? 3 : 4;
        int shapeTier;
        if (mode.width == 1440 && mode.height == 1080 || mode.width == 1080 && mode.height == 1440) shapeTier = 0;
        else if (mode.width == 1920 && mode.height == 1080 || mode.width == 1080 && mode.height == 1920) shapeTier = 1;
        else if (mode.width == 1088 && mode.height == 1088) shapeTier = 2;
        else if (mode.shorterSideAtLeast1000) shapeTier = 3;
        else shapeTier = 4;
        return (long) fpsTier * 100 + shapeTier * 10 + (mode.shorterSideAtLeast1000 ? 0 : 1);
    }

    public static CameraMode chooseAutomatic(List<CameraMode> modes, int requestedFps) {
        if (modes == null || modes.isEmpty()) return null;
        return ranked(modes, requestedFps).get(0);
    }

    public static CameraMode find(List<CameraMode> modes, String key) {
        if (modes != null && key != null) {
            for (CameraMode mode : modes) if (mode.key().equals(key)) return mode;
        }
        return null;
    }

    public static Rect centeredCrop(int width, int height) {
        return CameraCrop.forCapture(width, height).toRect();
    }

    /** Pure geometry form used by selection tests and by the Camera2 adapter. */
    public static int[] centeredCropValues(int width, int height) {
        CameraCrop crop = CameraCrop.forCapture(width, height);
        return new int[]{crop.left, crop.top, crop.right, crop.bottom};
    }

    public static boolean theoreticallyCapable(double maximumFps, int requestedFps) {
        return maximumFps > 0 && maximumFps + 0.01 >= requestedFps;
    }

    public static Range<Integer> fixedOrCompatibleRange(Range<Integer>[] ranges, int fps) {
        if (ranges == null) return null;
        Range<Integer> compatible = null;
        for (Range<Integer> range : ranges) {
            if (range.getLower() == fps && range.getUpper() == fps) return range;
            if (range.getLower() <= fps && range.getUpper() >= fps
                    && (compatible == null || range.getLower() > compatible.getLower()
                    || range.getUpper() < compatible.getUpper())) compatible = range;
        }
        return compatible;
    }

    public static String capabilityLabel(CameraMode mode) {
        if (mode == null) return "unsupported";
        return String.format(java.util.Locale.US, "%dx%d, crop %s, max %.1f FPS%s",
                mode.width, mode.height, mode.cropLabel(), mode.theoreticalMaxFps,
                mode.shorterSideAtLeast1000 ? ", >=1000 short side" : "");
    }
}
