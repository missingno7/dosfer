package org.dosfer.receiver;

import android.graphics.Rect;
import android.util.Range;
import org.junit.Test;
import java.util.Arrays;
import java.util.List;
import static org.junit.Assert.*;

public class CameraModeTest {
    private static CameraMode mode(int w, int h, double fps) {
        return new CameraMode(w, h, fps <= 0 ? 0 : (long) (1_000_000_000.0 / fps));
    }

    @Test public void automaticRankingPrefersSmallestColourAdequate60Mode() {
        List<CameraMode> modes = Arrays.asList(mode(1088, 1088, 60), mode(1920, 1080, 60),
                mode(1440, 1080, 60), mode(1920, 1440, 60), mode(2992,2992,30));
        assertEquals("1920x1440", CameraMode.chooseAutomatic(modes, 60).key());
    }

    @Test public void colourAdequatePublished30ModeBeatsInsufficient60Mode() {
        List<CameraMode> modes=Arrays.asList(mode(1440,1080,60),mode(2992,2992,30));
        assertEquals("2992x2992",CameraMode.chooseAutomatic(modes,60).key());
    }

    @Test public void largestCropWinsWhenNoModeHasEnoughChroma() {
        List<CameraMode> modes=Arrays.asList(mode(640,480,60),mode(1440,1080,60),
                mode(1280,720,60));
        assertEquals("1440x1080",CameraMode.chooseAutomatic(modes,60).key());
    }

    @Test public void centeredCropDoesNotScale() {
        assertArrayEquals(new int[]{180, 0, 1260, 1080}, CameraMode.centeredCropValues(1440, 1080));
        assertArrayEquals(new int[]{420, 0, 1500, 1080}, CameraMode.centeredCropValues(1920, 1080));
        assertArrayEquals(new int[]{0, 0, 1088, 1088}, CameraMode.centeredCropValues(1088, 1088));
    }

    @Test public void theoreticalCapabilityClassifies60And30() {
        assertTrue(CameraMode.theoreticallyCapable(60.0, 60));
        assertTrue(CameraMode.theoreticallyCapable(30.0, 30));
        assertFalse(CameraMode.theoreticallyCapable(59.0, 60));
        assertFalse(CameraMode.theoreticallyCapable(29.9, 30));
    }

    @Test public void missingManualModeFallsBackToAutomatic() {
        List<CameraMode> modes = Arrays.asList(mode(640, 480, 30), mode(1440, 1080, 60));
        CameraSettings.Selection selection = new CameraSettings.Selection("0", 60, "does-not-exist", false);
        assertEquals("1440x1080", CameraScanner.selectMode(modes, selection).key());
    }

    @Test public void persistedConfigurationRoundTripsAndInvalidFpsFallsBackToAuto() {
        CameraSettings.Selection original = new CameraSettings.Selection("logical|0", 60, "1440x1080", false, 3);
        assertEquals(original.serialize(), CameraSettings.Selection.parse(original.serialize()).serialize());
        assertEquals(0, CameraSettings.Selection.parse("0|120|auto|true").targetFps);
        assertTrue(CameraSettings.Selection.parse("0|120|auto|true").automatic);
        assertEquals(2, CameraSettings.Selection.parse("0|60|auto|true").decodeWorkers);
        assertEquals(4, CameraSettings.Selection.parse("0|60|auto|true|99").decodeWorkers);
        assertEquals(1, CameraSettings.Selection.parse("0|60|auto|true|0").decodeWorkers);
        assertEquals(4, CameraScanner.imageReaderBufferCount(1));
        assertEquals(6, CameraScanner.imageReaderBufferCount(4));
    }
}
