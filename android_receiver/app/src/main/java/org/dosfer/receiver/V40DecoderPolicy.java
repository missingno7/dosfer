package org.dosfer.receiver;

import java.util.Arrays;

/** Pure, Android-independent policy helpers for the fixed DOSfer QR format. */
public final class V40DecoderPolicy {
    private static final int MAGIC_LENGTH = 4;

    private V40DecoderPolicy() {}

    /**
     * Returns the DOSfer frame from either a normal decoder result or the
     * complete QR byte stream. Some QR decoders expose the ECI/Byte-segment
     * header before the actual application bytes.
     */
    public static byte[] extractDosferFrame(byte[] bytes) {
        if (bytes == null) return null;
        int last = Math.min(8, bytes.length - MAGIC_LENGTH);
        for (int offset = 0; offset <= last; offset++) {
            if (bytes[offset] != 'D' || bytes[offset + 1] != 'Q'
                    || bytes[offset + 2] != 'R' || bytes[offset + 3] != '1') continue;
            if (bytes.length - offset < Protocol.FRAME_HEADER) continue;
            int payloadLength = ((bytes[offset + 32] & 255) << 8) | (bytes[offset + 33] & 255);
            int frameLength = Protocol.FRAME_HEADER + payloadLength;
            if (frameLength <= bytes.length - offset)
                return Arrays.copyOfRange(bytes, offset, offset + frameLength);
        }
        return null;
    }

    public static boolean hasDosferMagic(byte[] bytes) {
        return bytes != null && bytes.length >= MAGIC_LENGTH
                && bytes[0] == 'D' && bytes[1] == 'Q' && bytes[2] == 'R' && bytes[3] == '1';
    }

}
