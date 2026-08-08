package org.dosfer.receiver;

import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.graphics.ImageFormat;
import android.util.Range;
import android.util.Size;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/** A discovered PRIVATE/OES stream and the capability facts Camera2 exposes for it. */
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
        Size[] sizes = map.getOutputSizes(SurfaceTexture.class);
        boolean privateOutput=sizes!=null&&sizes.length>0;
        if (!privateOutput) sizes=map.getOutputSizes(ImageFormat.YUV_420_888);
        if (sizes == null) return Collections.emptyList();
        List<CameraMode> result = new ArrayList<>();
        for (Size size : sizes) {
            long duration=privateOutput?map.getOutputMinFrameDuration(SurfaceTexture.class,size)
                    :map.getOutputMinFrameDuration(ImageFormat.YUV_420_888,size);
            result.add(new CameraMode(size.getWidth(), size.getHeight(), duration));
        }
        return result;
    }

    public static List<CameraMode> ranked(List<CameraMode> modes, int requestedFps) {
        List<CameraMode> result = new ArrayList<>(modes);
        result.sort(Comparator.comparingLong((CameraMode mode) -> rank(mode, requestedFps))
                /* Once colour detail and frame rate are satisfied, prefer the
                 * least camera/ISP bandwidth. On the tested phone this makes
                 * 1920x1440 the ideal 60 FPS RGB3 stream. If no mode has enough
                 * 4:2:0 chroma detail, retain as much crop resolution as the
                 * camera offers instead of accidentally choosing VGA. */
                .thenComparingLong(CameraMode::bandwidthRank)
                .thenComparingLong((CameraMode mode) -> (long)mode.width*mode.height));
        return result;
    }

    private static long bandwidthRank(CameraMode mode) {
        int cropSide=Math.min(mode.width,mode.height);
        return cropSide>=Rgb3Yuv.MIN_420_CHROMA_SIDE
                ? (long)mode.width*mode.height : -cropSide;
    }

    /** Lower is better. A 4:2:0 stream needs at least twice the decoder side
     * in its square crop to preserve the required independent colour grid.
     * Colour adequacy deliberately precedes advertised frame duration because
     * several Samsung modes sustain 60 FPS despite publishing a 30 FPS
     * minimum-frame-duration entry. Measured FPS remains visible in the UI. */
    public static long rank(CameraMode mode, int requestedFps) {
        int cropSide=Math.min(mode.width,mode.height);
        int colourTier=cropSide>=Rgb3Yuv.MIN_420_CHROMA_SIDE?0:1;
        int fpsTier = requestedFps == 60 && mode.theoretically60Fps ? 0
                : requestedFps == 30 && theoreticallyCapable(mode.theoreticalMaxFps, 30) ? 1
                : mode.theoretically60Fps ? 2
                : theoreticallyCapable(mode.theoreticalMaxFps, 30) ? 3 : 4;
        return (long)colourTier*100+fpsTier;
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
