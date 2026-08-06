package org.dosfer.receiver;

import android.graphics.Rect;

/** Canonical raw Image-buffer crop shared by preview and ZXing. */
public final class CameraCrop {
    public final int captureWidth;
    public final int captureHeight;
    public final int side;
    public final int left;
    public final int top;
    public final int right;
    public final int bottom;

    private CameraCrop(int width, int height) {
        captureWidth = width;
        captureHeight = height;
        side = Math.min(width, height);
        left = (width - side) / 2;
        top = (height - side) / 2;
        right = left + side;
        bottom = top + side;
    }

    public static CameraCrop forCapture(int width, int height) {
        if (width <= 0 || height <= 0) throw new IllegalArgumentException("capture dimensions must be positive");
        return new CameraCrop(width, height);
    }

    public Rect toRect() { return new Rect(left, top, right, bottom); }
}
