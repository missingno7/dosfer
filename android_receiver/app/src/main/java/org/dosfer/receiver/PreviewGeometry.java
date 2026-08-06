package org.dosfer.receiver;

/** Pure geometry used to keep the square preview aligned with the raw crop. */
public final class PreviewGeometry {
    private PreviewGeometry() {}

    public static int displayDegrees(int displayRotation) {
        switch (displayRotation) {
            case 1: return 90;
            case 2: return 180;
            case 3: return 270;
            default: return 0;
        }
    }

    /** Camera2's clockwise buffer-to-display rotation, including front-camera convention. */
    public static int relativeRotation(int sensorOrientation, int displayRotation, int lensFacing) {
        int sensor = ((sensorOrientation % 360) + 360) % 360;
        int display = displayDegrees(displayRotation);
        int result = lensFacing == 0 /* LENS_FACING_FRONT */
                ? sensor - display : sensor + display;
        return (result % 360 + 360) % 360;
    }

    public static float uniformScale(int cropSide, int previewWidth, int previewHeight) {
        if (cropSide <= 0 || previewWidth <= 0 || previewHeight <= 0) return 0;
        return Math.min(previewWidth, previewHeight) / (float) cropSide;
    }

    /**
     * TextureView stretches the camera buffer to the view bounds after it has
     * applied the sensor orientation. Return the extra x/y scale required to
     * undo that stretch and use centered FILL_CENTER cropping.
     */
    public static float[] textureFillCenterScale(int captureWidth, int captureHeight,
                                                  int viewWidth, int viewHeight,
                                                  int sensorOrientation, int relativeRotation) {
        if (captureWidth <= 0 || captureHeight <= 0 || viewWidth <= 0 || viewHeight <= 0) {
            return new float[]{1f, 1f};
        }
        boolean rotationRequired = ((relativeRotation % 360 + 360) % 360) % 180 != 0;
        float scaleX;
        float scaleY;
        int normalizedSensor = ((sensorOrientation % 360) + 360) % 360;
        if (normalizedSensor % 180 == 0) {
            scaleX = rotationRequired
                    ? viewWidth / (float) captureWidth
                    : viewWidth / (float) captureHeight;
            scaleY = rotationRequired
                    ? viewHeight / (float) captureHeight
                    : viewHeight / (float) captureWidth;
        } else {
            scaleX = rotationRequired
                    ? viewWidth / (float) captureHeight
                    : viewWidth / (float) captureWidth;
            scaleY = rotationRequired
                    ? viewHeight / (float) captureWidth
                    : viewHeight / (float) captureHeight;
        }
        float finalScale = Math.max(scaleX, scaleY);
        if (rotationRequired) {
            return new float[]{finalScale / scaleX, finalScale / scaleY};
        }
        return new float[]{
                viewHeight / (float) viewWidth * finalScale / scaleY,
                viewWidth / (float) viewHeight * finalScale / scaleX
        };
    }

    /**
     * View-corner order is top-left, top-right, bottom-right, bottom-left.
     * These are the raw buffer points that TextureView should sample for the
     * corresponding view corners. No mirroring is applied to rear cameras.
     */
    public static int[] viewToBufferCorners(CameraCrop crop, int relativeRotation, boolean mirror) {
        int left = crop.left, top = crop.top, right = crop.right, bottom = crop.bottom;
        int[] points;
        switch ((relativeRotation % 360 + 360) % 360) {
            case 90: points = new int[]{left, bottom, left, top, right, top, right, bottom}; break;
            case 180: points = new int[]{right, bottom, left, bottom, left, top, right, top}; break;
            case 270: points = new int[]{right, top, right, bottom, left, bottom, left, top}; break;
            default: points = new int[]{left, top, right, top, right, bottom, left, bottom}; break;
        }
        if (!mirror) return points;
        int[] mirrored = new int[8];
        for (int corner = 0; corner < 4; corner++) {
            int sourceCorner = 3 - corner;
            mirrored[corner * 2] = points[sourceCorner * 2];
            mirrored[corner * 2 + 1] = points[sourceCorner * 2 + 1];
        }
        return mirrored;
    }
}
