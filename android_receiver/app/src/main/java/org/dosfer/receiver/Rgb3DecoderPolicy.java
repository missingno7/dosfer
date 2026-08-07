package org.dosfer.receiver;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;

/** Protocol-aware de-duplication and automatic BW/RGB3 transport detection. */
final class Rgb3DecoderPolicy {
    enum Mode { UNKNOWN, BW, RGB3 }

    /**
     * A sender keeps one optical mode for a complete acknowledged window.  We
     * therefore spend three ZXing calls only while the mode is unknown.  Two
     * distinct DATA/CALIBRATION frames whose three channels carry the same ID
     * lock the window to BW.  Any physical frame containing two or three
     * different valid IDs locks it to RGB3 immediately.
     *
     * Requiring two distinct equal triples prevents the RGB3 focus frame
     * (frame zero repeated in R/G/B) from being mistaken for a BW transfer.
     */
    static final class Detector {
        private Mode mode = Mode.UNKNOWN;
        private long session;
        private int window = -1;
        private Protocol.FrameKey lastEqualKey;
        private int equalTripleStreak;

        synchronized Mode mode() { return mode; }

        synchronized void reset() {
            mode = Mode.UNKNOWN;
            session = 0;
            window = -1;
            lastEqualKey = null;
            equalTripleStreak = 0;
        }

        synchronized List<byte[]> accept(byte[][] candidates) {
            Parsed parsed = parseUnique(candidates);
            if (parsed.valid.isEmpty()) return new ArrayList<>();

            Protocol.Frame anchor = parsed.valid.get(0).frame;
            if (session != 0 && (anchor.session != session || anchor.window != window)) {
                mode = Mode.UNKNOWN;
                lastEqualKey = null;
                equalTripleStreak = 0;
            }
            session = anchor.session;
            window = anchor.window;

            if (mode == Mode.UNKNOWN) {
                if (parsed.unique.size() >= 2) {
                    mode = Mode.RGB3;
                    lastEqualKey = null;
                    equalTripleStreak = 0;
                } else if (parsed.valid.size() == Rgb3Yuv.CHANNELS &&
                        (anchor.kind == Protocol.DATA || anchor.kind == Protocol.CALIBRATION)) {
                    Protocol.FrameKey key = parsed.valid.get(0).key;
                    boolean allEqual = true;
                    for (ParsedFrame candidate : parsed.valid)
                        if (!key.equals(candidate.key)) { allEqual = false; break; }
                    if (allEqual) {
                        if (!key.equals(lastEqualKey)) {
                            lastEqualKey = key;
                            equalTripleStreak++;
                        }
                        if (equalTripleStreak >= 2) mode = Mode.BW;
                    }
                }
            }

            /* END_WINDOW is deliberately monochrome in both transports.  The
             * following window gets a fresh classification, allowing a sender
             * restart or a changed /RGB3-/BW setting without app restart. */
            for (ParsedFrame candidate : parsed.valid) if (candidate.frame.kind == Protocol.END_WINDOW) {
                mode = Mode.UNKNOWN;
                session = 0;
                window = -1;
                lastEqualKey = null;
                equalTripleStreak = 0;
                break;
            }
            return new ArrayList<>(parsed.unique.values());
        }
    }

    private static final class ParsedFrame {
        final byte[] raw;
        final Protocol.Frame frame;
        final Protocol.FrameKey key;
        ParsedFrame(byte[] raw, Protocol.Frame frame) {
            this.raw = raw;
            this.frame = frame;
            this.key = new Protocol.FrameKey(frame);
        }
    }

    private static final class Parsed {
        final List<ParsedFrame> valid = new ArrayList<>();
        final LinkedHashMap<Protocol.FrameKey, byte[]> unique = new LinkedHashMap<>();
    }

    private Rgb3DecoderPolicy() {}

    static List<byte[]> uniqueValidFrames(byte[][] candidates) {
        return new ArrayList<>(parseUnique(candidates).unique.values());
    }

    private static Parsed parseUnique(byte[][] candidates) {
        Parsed parsed = new Parsed();
        if (candidates == null) return parsed;
        for (byte[] candidate : candidates) {
            if (candidate == null) continue;
            try {
                Protocol.Frame frame = Protocol.parseFrame(candidate);
                ParsedFrame item = new ParsedFrame(candidate, frame);
                parsed.valid.add(item);
                parsed.unique.putIfAbsent(item.key, candidate);
            } catch (RuntimeException ignored) {
                // ZXing may return an unrelated QR; only DOSfer frames leave this layer.
            }
        }
        return parsed;
    }
}
