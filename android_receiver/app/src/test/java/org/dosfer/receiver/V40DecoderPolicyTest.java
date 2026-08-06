package org.dosfer.receiver;

import org.junit.Test;

import static org.junit.Assert.*;

public class V40DecoderPolicyTest {
    @Test public void acceptsOnlyDosferMagic() {
        assertTrue(V40DecoderPolicy.hasDosferMagic(new byte[]{'D', 'Q', 'R', '1', 0}));
        assertFalse(V40DecoderPolicy.hasDosferMagic(new byte[]{'D', 'Q', 'R', '0'}));
        assertFalse(V40DecoderPolicy.hasDosferMagic(null));
    }

    @Test public void extractsFrameAfterQrByteSegmentHeader() {
        byte[] frame = Protocol.encodeFrame(Protocol.DATA, 0, 0x6A67C69DL,
                0, 0, 0, 1, 0, 0, new byte[0]);
        byte[] qrBytes = new byte[4 + frame.length + 12];
        qrBytes[0] = 0x70; qrBytes[1] = 0x34; qrBytes[2] = 0; qrBytes[3] = 0x54;
        System.arraycopy(frame, 0, qrBytes, 4, frame.length);
        assertArrayEquals(frame, V40DecoderPolicy.extractDosferFrame(qrBytes));
        assertArrayEquals(frame, V40DecoderPolicy.extractDosferFrame(frame));
    }

}
