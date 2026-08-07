package org.dosfer.receiver;

import org.junit.Test;
import java.util.List;
import static org.junit.Assert.*;

public class Rgb3DecoderPolicyTest {
    private static byte[] frame(long index) {
        return frame(Protocol.DATA, 0, index, (int) index);
    }
    private static byte[] frame(int kind, int window, long index, int wi) {
        return Protocol.encodeFrame(kind, kind == Protocol.DATA ? Protocol.FLAG_WHITENED : 0,
                0x12345678L, window, index, wi, 66, 0, 0, new byte[0]);
    }

    @Test public void monochromeTripleCollapsesToOneLogicalFrame() {
        byte[] a = frame(1);
        List<byte[]> out = Rgb3DecoderPolicy.uniqueValidFrames(new byte[][]{a, a.clone(), a.clone()});
        assertEquals(1, out.size());
        assertArrayEquals(a, out.get(0));
    }

    @Test public void threeChannelsRemainThreeFramesAndInvalidQrIsIgnored() {
        byte[] a=frame(1), b=frame(2), c=frame(3);
        List<byte[]> out=Rgb3DecoderPolicy.uniqueValidFrames(new byte[][]{a,b,new byte[]{1,2},c});
        assertEquals(3,out.size());assertArrayEquals(a,out.get(0));assertArrayEquals(b,out.get(1));assertArrayEquals(c,out.get(2));
    }

    @Test public void focusFrameDoesNotMisclassifyRgb3AsBw() {
        Rgb3DecoderPolicy.Detector detector = new Rgb3DecoderPolicy.Detector();
        byte[] focus=frame(0),a=frame(0),b=frame(1),c=frame(2);
        detector.accept(new byte[][]{focus,focus,focus});
        assertEquals(Rgb3DecoderPolicy.Mode.UNKNOWN,detector.mode());
        detector.accept(new byte[][]{a,b,c});
        assertEquals(Rgb3DecoderPolicy.Mode.RGB3,detector.mode());
    }

    @Test public void twoDistinctEqualTriplesLockBwUntilEndWindow() {
        Rgb3DecoderPolicy.Detector detector = new Rgb3DecoderPolicy.Detector();
        byte[] a=frame(0),b=frame(1);
        detector.accept(new byte[][]{a,a.clone(),a.clone()});
        assertEquals(Rgb3DecoderPolicy.Mode.UNKNOWN,detector.mode());
        detector.accept(new byte[][]{b,b.clone(),b.clone()});
        assertEquals(Rgb3DecoderPolicy.Mode.BW,detector.mode());
        byte[] eow=frame(Protocol.END_WINDOW,0,65,0);
        detector.accept(new byte[][]{eow,null,null});
        assertEquals(Rgb3DecoderPolicy.Mode.UNKNOWN,detector.mode());
    }
    @Test public void partialRgbDecodeKeepsValidChannelsAndLocksRgb3() {
        Rgb3DecoderPolicy.Detector detector = new Rgb3DecoderPolicy.Detector();
        byte[] a=frame(4),c=frame(6);
        List<byte[]> out=detector.accept(new byte[][]{a,null,c});
        assertEquals(2,out.size());
        assertEquals(Rgb3DecoderPolicy.Mode.RGB3,detector.mode());
    }

}
