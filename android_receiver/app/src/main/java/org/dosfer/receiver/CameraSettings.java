package org.dosfer.receiver;

import android.content.Context;
import android.content.SharedPreferences;

/** Persisted camera choices. The wire format is also used by unit tests. */
public final class CameraSettings {
    private static final String PREFS = "camera-settings";
    private static final String KEY_CAMERA = "camera";
    private static final String KEY_FPS = "fps";
    private static final String KEY_MODE = "mode";
    private static final String KEY_AUTO = "auto";
    private static final String KEY_WORKERS = "workers";

    public static final class Selection {
        public final String cameraId;
        public final int targetFps; // 0 = automatic, otherwise 30 or 60
        public final String modeKey;
        public final boolean automatic;
        /** Number of independent native ZXing decode lanes (1 through 4). */
        public final int decodeWorkers;

        public Selection(String cameraId, int targetFps, String modeKey, boolean automatic) {
            this(cameraId, targetFps, modeKey, automatic, 2);
        }

        public Selection(String cameraId, int targetFps, String modeKey, boolean automatic,
                int decodeWorkers) {
            this.cameraId = cameraId;
            this.targetFps = targetFps == 30 || targetFps == 60 ? targetFps : 0;
            this.modeKey = modeKey == null ? "auto" : modeKey;
            this.automatic = automatic;
            this.decodeWorkers = decodeWorkers < 1 ? 1 : Math.min(decodeWorkers, 4);
        }

        public String serialize() {
            return escape(cameraId) + "|" + targetFps + "|" + escape(modeKey) + "|" + automatic
                    + "|" + decodeWorkers;
        }

        public static Selection parse(String value) {
            if (value == null || value.isEmpty()) return new Selection(null, 0, "auto", true);
            String[] parts = value.split("\\|", -1);
            if (parts.length != 4 && parts.length != 5) return new Selection(null, 0, "auto", true);
            boolean auto = Boolean.parseBoolean(parts[3]);
            int fps, workers = 2;
            try { fps = Integer.parseInt(parts[1]); } catch (NumberFormatException e) { fps = 0; }
            if (parts.length == 5) {
                try { workers = Integer.parseInt(parts[4]); } catch (NumberFormatException ignored) {}
            }
            return new Selection(unescape(parts[0]), fps, unescape(parts[2]), auto, workers);
        }

        private static String escape(String value) {
            return value == null ? "" : value.replace("%", "%25").replace("|", "%7C");
        }

        private static String unescape(String value) {
            return value.replace("%7C", "|").replace("%25", "%");
        }
    }

    private CameraSettings() {}

    public static Selection load(Context context) {
        SharedPreferences p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String packed = p.getString("selection", null);
        if (packed != null) return Selection.parse(packed);
        return new Selection(p.getString(KEY_CAMERA, null), p.getInt(KEY_FPS, 0),
                p.getString(KEY_MODE, "auto"), p.getBoolean(KEY_AUTO, true),
                p.getInt(KEY_WORKERS, 2));
    }

    public static void save(Context context, Selection selection) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString("selection", selection.serialize())
                .putString(KEY_CAMERA, selection.cameraId)
                .putInt(KEY_FPS, selection.targetFps)
                .putString(KEY_MODE, selection.modeKey)
                .putBoolean(KEY_AUTO, selection.automatic)
                .putInt(KEY_WORKERS, selection.decodeWorkers)
                .apply();
    }
}
