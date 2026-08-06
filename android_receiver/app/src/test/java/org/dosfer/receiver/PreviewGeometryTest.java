package org.dosfer.receiver;

import org.junit.Test;

import static org.junit.Assert.*;

public class PreviewGeometryTest {
    @Test public void canonicalCropExamplesUseExplicitRightAndBottom() {
        assertCrop(1088, 1088, 0, 0, 1088, 1088);
        assertCrop(1440, 1080, 180, 0, 1260, 1080);
        assertCrop(1920, 1080, 420, 0, 1500, 1080);
        assertCrop(1920, 1440, 240, 0, 1680, 1440);
        assertCrop(1080, 1920, 0, 420, 1080, 1500);
    }

    @Test public void relativeRotationHandlesBackAndFrontCameras() {
        assertEquals(90, PreviewGeometry.relativeRotation(90, 0, 1));
        assertEquals(180, PreviewGeometry.relativeRotation(90, 1, 1));
        assertEquals(270, PreviewGeometry.relativeRotation(90, 2, 1));
        assertEquals(90, PreviewGeometry.relativeRotation(90, 0, 0));
        assertEquals(0, PreviewGeometry.relativeRotation(90, 1, 0));
    }

    @Test public void squareMappingUsesUniformScaleAndCropOnly() {
        CameraCrop crop = CameraCrop.forCapture(1920, 1080);
        assertEquals(0.6666667f, PreviewGeometry.uniformScale(crop.side, 720, 720), 0.0001f);
        assertArrayEquals(new int[]{420, 0, 1500, 0, 1500, 1080, 420, 1080},
                PreviewGeometry.viewToBufferCorners(crop, 0, false));
        assertArrayEquals(new int[]{420, 1080, 420, 0, 1500, 0, 1500, 1080},
                PreviewGeometry.viewToBufferCorners(crop, 90, false));
    }


    @Test public void textureTransformExpandsCenteredSquareCropToFillView() {
        float[] fourByThree = PreviewGeometry.textureFillCenterScale(
                1440, 1080, 720, 720, 90, 90);
        assertArrayEquals(new float[]{1f, 4f / 3f}, fourByThree, 0.0001f);

        float[] sixteenByNine = PreviewGeometry.textureFillCenterScale(
                1920, 1080, 720, 720, 90, 90);
        assertArrayEquals(new float[]{1f, 16f / 9f}, sixteenByNine, 0.0001f);

        float[] square = PreviewGeometry.textureFillCenterScale(
                1088, 1088, 720, 720, 90, 90);
        assertArrayEquals(new float[]{1f, 1f}, square, 0.0001f);
    }

    @Test public void frontMirrorIsExplicitOnly() {
        CameraCrop crop = CameraCrop.forCapture(1088, 1088);
        assertArrayEquals(new int[]{0, 0, 1088, 0, 1088, 1088, 0, 1088},
                PreviewGeometry.viewToBufferCorners(crop, 0, false));
        assertArrayEquals(new int[]{0, 1088, 1088, 1088, 1088, 0, 0, 0},
                PreviewGeometry.viewToBufferCorners(crop, 0, true));
    }

    private static void assertCrop(int width, int height, int left, int top, int right, int bottom) {
        CameraCrop crop = CameraCrop.forCapture(width, height);
        assertEquals(left, crop.left); assertEquals(top, crop.top);
        assertEquals(right, crop.right); assertEquals(bottom, crop.bottom);
        assertEquals(right - left, crop.side); assertEquals(bottom - top, crop.side);
    }
}
