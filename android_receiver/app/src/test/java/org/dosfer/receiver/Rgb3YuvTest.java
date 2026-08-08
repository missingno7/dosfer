package org.dosfer.receiver;

import org.junit.Test;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Random;
import static org.junit.Assert.*;

public class Rgb3YuvTest {
    @Test public void decoderNeverDownsamplesBelowRequiredSide() {
        assertEquals(3,Rgb3Yuv.decoderDownsampleFactorForCapture(2736,3648,2736,3));
        assertEquals(912,Rgb3Yuv.decoderSideForCapture(2736,3648,2736,3));
        assertEquals(3,Rgb3Yuv.decoderDownsampleFactorForCapture(2736,3648,2736,4));
        assertEquals(912,Rgb3Yuv.decoderSideForCapture(2736,3648,2736,4));
    }

    @Test public void factorAdaptsToCropRatherThanSourceClassification() {
        assertEquals(1,Rgb3Yuv.decoderDownsampleFactorForCapture(1080,1920,1080,3));
        assertEquals(1080,Rgb3Yuv.decoderSideForCapture(1080,1920,1080,3));
        assertEquals(2,Rgb3Yuv.decoderDownsampleFactorForCapture(1440,1920,1440,4));
        assertEquals(720,Rgb3Yuv.decoderSideForCapture(1440,1920,1440,4));
        assertEquals(3,Rgb3Yuv.decoderDownsampleFactorForCapture(2160,3840,2160,4));
        assertEquals(720,Rgb3Yuv.decoderSideForCapture(2160,3840,2160,4));
        assertEquals(4,Rgb3Yuv.decoderDownsampleFactorForCapture(2992,2992,2992,4));
        assertEquals(748,Rgb3Yuv.decoderSideForCapture(2992,2992,2992,4));
        assertEquals(4,Rgb3Yuv.decoderDownsampleFactorForCapture(3000,4000,3000,4));
        assertEquals(750,Rgb3Yuv.decoderSideForCapture(3000,4000,3000,4));
    }

    @Test public void convertsBt601RedToIndependentChannels() {
        ByteBuffer y=ByteBuffer.allocate(4),u=ByteBuffer.allocate(1),v=ByteBuffer.allocate(1);
        for(int i=0;i<4;i++)y.put(i,(byte)82);u.put(0,(byte)90);v.put(0,(byte)240);
        ByteBuffer[] out={ByteBuffer.allocate(4),ByteBuffer.allocate(4),ByteBuffer.allocate(4)};
        Rgb3Yuv.convert420(y,2,1,u,1,1,v,1,1,0,0,2,2,out);
        for(int i=0;i<4;i++){assertTrue((out[0].get(i)&255)>240);assertTrue((out[1].get(i)&255)<20);assertTrue((out[2].get(i)&255)<20);}
    }
    @Test public void respectsPlaneBufferPositionsAndPixelStride() {
        ByteBuffer y=ByteBuffer.allocate(12),u=ByteBuffer.allocate(6),v=ByteBuffer.allocate(6);
        y.position(4);u.position(2);v.position(2);
        int[] yValues={82,82,82,82};for(int i=0;i<4;i++)y.put(4+i*2,(byte)yValues[i]);
        u.put(2,(byte)90);v.put(2,(byte)240);
        ByteBuffer[] out={ByteBuffer.allocate(4),ByteBuffer.allocate(4),ByteBuffer.allocate(4)};
        Rgb3Yuv.convert420(y,4,2,u,2,2,v,2,2,0,0,2,2,out);
        for(int i=0;i<4;i++){assertTrue((out[0].get(i)&255)>240);assertTrue((out[1].get(i)&255)<20);assertTrue((out[2].get(i)&255)<20);}
    }


    @Test public void optimizedLoopMatchesScalarForOddCropAndStrides() {
        int sourceWidth=6,sourceHeight=5,left=1,top=1,width=4,height=3;
        int yOffset=3,yRowStride=8,uOffset=2,vOffset=4,uvRowStride=8,uvPixelStride=2;
        ByteBuffer y=ByteBuffer.allocate(yOffset+yRowStride*sourceHeight);
        ByteBuffer u=ByteBuffer.allocate(uOffset+uvRowStride*((sourceHeight+1)/2));
        ByteBuffer v=ByteBuffer.allocate(vOffset+uvRowStride*((sourceHeight+1)/2));
        y.position(yOffset);u.position(uOffset);v.position(vOffset);
        for(int row=0;row<sourceHeight;row++)for(int col=0;col<sourceWidth;col++)
            y.put(yOffset+row*yRowStride+col,(byte)(24+(row*37+col*19)%210));
        for(int row=0;row<(sourceHeight+1)/2;row++)for(int col=0;col<(sourceWidth+1)/2;col++){
            u.put(uOffset+row*uvRowStride+col*uvPixelStride,(byte)(32+(row*43+col*61)%192));
            v.put(vOffset+row*uvRowStride+col*uvPixelStride,(byte)(40+(row*71+col*29)%176));
        }
        int pixels=width*height;
        ByteBuffer[] out={ByteBuffer.allocateDirect(pixels).order(ByteOrder.LITTLE_ENDIAN),
                ByteBuffer.allocateDirect(pixels).order(ByteOrder.LITTLE_ENDIAN),
                ByteBuffer.allocateDirect(pixels).order(ByteOrder.LITTLE_ENDIAN)};
        Rgb3Yuv.convert420(y,yRowStride,1,u,uvRowStride,uvPixelStride,
                v,uvRowStride,uvPixelStride,left,top,width,height,out);
        byte[][] expected=scalar(y,yRowStride,1,u,uvRowStride,uvPixelStride,
                v,uvRowStride,uvPixelStride,left,top,width,height);
        for(int channel=0;channel<3;channel++){
            assertEquals(0,out[channel].position());assertEquals(pixels,out[channel].limit());
            for(int i=0;i<pixels;i++)assertEquals(expected[channel][i],out[channel].get(i));
        }
    }

    private static byte[][] scalar(ByteBuffer ySource,int yRowStride,int yPixelStride,
            ByteBuffer uSource,int uRowStride,int uPixelStride,
            ByteBuffer vSource,int vRowStride,int vPixelStride,
            int left,int top,int width,int height) {
        ByteBuffer y=ySource.duplicate(),u=uSource.duplicate(),v=vSource.duplicate();
        int yo=y.position(),uo=u.position(),vo=v.position();
        byte[][] out=new byte[3][width*height];int dst=0;
        for(int row=0;row<height;row++)for(int col=0;col<width;col++,dst++){
            int sy=top+row,sx=left+col;
            int yy=(y.get(yo+sy*yRowStride+sx*yPixelStride)&255)-16;
            int uu=(u.get(uo+(sy>>1)*uRowStride+(sx>>1)*uPixelStride)&255)-128;
            int vv=(v.get(vo+(sy>>1)*vRowStride+(sx>>1)*vPixelStride)&255)-128;
            if(yy<0)yy=0;
            out[0][dst]=(byte)clamp((298*yy+409*vv+128)>>8);
            out[1][dst]=(byte)clamp((298*yy-100*uu-208*vv+128)>>8);
            out[2][dst]=(byte)clamp((298*yy+516*uu+128)>>8);
        }
        return out;
    }

    private static int clamp(int value){return value<0?0:Math.min(value,255);}

    @Test public void optimizedLoopMatchesScalarAcrossAlignmentMatrix() {
        Random random=new Random(0x52474233L);
        for(int iteration=0;iteration<200;iteration++){
            int left=random.nextInt(4),top=random.nextInt(4);
            int width=1+random.nextInt(7),height=1+random.nextInt(7);
            int sourceWidth=left+width+2,sourceHeight=top+height+2;
            int yPixelStride=1+random.nextInt(2),uvPixelStride=1+random.nextInt(2);
            int yRowStride=sourceWidth*yPixelStride+random.nextInt(4);
            int uvWidth=(sourceWidth+1)/2,uvHeight=(sourceHeight+1)/2;
            int uvRowStride=uvWidth*uvPixelStride+random.nextInt(4);
            int yOffset=random.nextInt(5),uOffset=random.nextInt(5),vOffset=random.nextInt(5);
            ByteBuffer y=ByteBuffer.allocate(yOffset+yRowStride*sourceHeight);
            ByteBuffer u=ByteBuffer.allocate(uOffset+uvRowStride*uvHeight);
            ByteBuffer v=ByteBuffer.allocate(vOffset+uvRowStride*uvHeight);
            y.position(yOffset);u.position(uOffset);v.position(vOffset);
            for(int row=0;row<sourceHeight;row++)for(int col=0;col<sourceWidth;col++)
                y.put(yOffset+row*yRowStride+col*yPixelStride,(byte)random.nextInt(256));
            for(int row=0;row<uvHeight;row++)for(int col=0;col<uvWidth;col++){
                u.put(uOffset+row*uvRowStride+col*uvPixelStride,(byte)random.nextInt(256));
                v.put(vOffset+row*uvRowStride+col*uvPixelStride,(byte)random.nextInt(256));
            }
            int pixels=width*height;
            ByteBuffer[] out={ByteBuffer.allocateDirect(pixels),ByteBuffer.allocateDirect(pixels),
                    ByteBuffer.allocateDirect(pixels)};
            Rgb3Yuv.convert420(y,yRowStride,yPixelStride,u,uvRowStride,uvPixelStride,
                    v,uvRowStride,uvPixelStride,left,top,width,height,out);
            byte[][] expected=scalar(y,yRowStride,yPixelStride,u,uvRowStride,uvPixelStride,
                    v,uvRowStride,uvPixelStride,left,top,width,height);
            for(int channel=0;channel<3;channel++)for(int i=0;i<pixels;i++)
                assertEquals("iteration="+iteration+" channel="+channel+" pixel="+i,
                        expected[channel][i],out[channel].get(i));
        }
    }

    @Test public void separatesAllEightIdealRgbSymbols() {
        int[][] yuv={
                {16,128,128},{41,240,110},{145,54,34},{170,166,16},
                {82,90,240},{107,202,222},{210,16,146},{235,128,128}
        };
        for(int symbol=0;symbol<8;symbol++){
            ByteBuffer y=ByteBuffer.allocate(1),u=ByteBuffer.allocate(1),v=ByteBuffer.allocate(1);
            y.put(0,(byte)yuv[symbol][0]);u.put(0,(byte)yuv[symbol][1]);v.put(0,(byte)yuv[symbol][2]);
            ByteBuffer[] out={ByteBuffer.allocate(1),ByteBuffer.allocate(1),ByteBuffer.allocate(1)};
            Rgb3Yuv.convert420(y,1,1,u,1,1,v,1,1,0,0,1,1,out);
            int expectedRed=(symbol>>2)&1,expectedGreen=(symbol>>1)&1,expectedBlue=symbol&1;
            assertEquals(expectedRed,(out[0].get(0)&255)>127?1:0);
            assertEquals(expectedGreen,(out[1].get(0)&255)>127?1:0);
            assertEquals(expectedBlue,(out[2].get(0)&255)>127?1:0);
        }
    }

    @Test public void downsampledConversionAveragesConvertedRgbSamples() {
        ByteBuffer y=ByteBuffer.allocate(16),u=ByteBuffer.allocate(4),v=ByteBuffer.allocate(4);
        for(int row=0;row<4;row++)for(int col=0;col<4;col++)y.put(row*4+col,(byte)(16+row*40+col*10));
        for(int i=0;i<4;i++){u.put(i,(byte)128);v.put(i,(byte)128);}
        ByteBuffer[] out={ByteBuffer.allocate(4),ByteBuffer.allocate(4),ByteBuffer.allocate(4)};
        Rgb3Yuv.convert420Downsampled(y,4,1,u,2,1,v,2,1,0,0,2,2,2,out);
        for(int channel=0;channel<3;channel++) {
            assertEquals(29,out[channel].get(0)&255);
            assertEquals(146,out[channel].get(3)&255);
        }
    }

    @Test public void autoLevelsUsesRobustEndpoints() {
        ByteBuffer b=ByteBuffer.allocate(100);
        b.put(0,(byte)0);b.put(1,(byte)255);
        for(int i=2;i<51;i++)b.put(i,(byte)60);
        for(int i=51;i<100;i++)b.put(i,(byte)160);
        b.limit(100);b.position(0);
        Rgb3Yuv.autoLevels(b,100,new int[256]);
        assertEquals(0,b.get(2)&255);assertEquals(255,b.get(99)&255);
    }

}
