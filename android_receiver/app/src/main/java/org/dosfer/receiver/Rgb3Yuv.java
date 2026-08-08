package org.dosfer.receiver;

import android.graphics.Rect;
import android.media.Image;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Converts a Camera2 YUV_420_888 crop directly into three packed luminance
 * buffers. No Bitmap or intermediate ARGB frame is allocated. */
final class Rgb3Yuv {
    static final int RED=0, GREEN=1, BLUE=2, CHANNELS=3;
    /** Keep every ZXing channel above the largest RGB3 symbol grid in use. */
    static final int MIN_DECODER_SIDE=708;
    static final int MIN_420_CHROMA_SIDE=MIN_DECODER_SIDE*2;
    private static final int DEFAULT_MAX_DOWNSAMPLE=4;
    private static final int MAX_GPU_DOWNSAMPLE=4;

    /* BT.601 limited-range terms. Chroma contribution is shared by a 2x2 YUV
     * block, so the hot loop reads U/V and evaluates those products only once
     * for up to four output pixels. */
    private static final int[] Y_TERM=new int[256];
    private static final int[] RED_V=new int[256];
    private static final int[] GREEN_U=new int[256];
    private static final int[] GREEN_V=new int[256];
    private static final int[] BLUE_U=new int[256];

    static {
        for (int value=0; value<256; value++) {
            int y=value-16;
            if (y<0) y=0;
            Y_TERM[value]=298*y+128;
            int chroma=value-128;
            RED_V[value]=409*chroma;
            GREEN_U[value]=-100*chroma;
            GREEN_V[value]=-208*chroma;
            BLUE_U[value]=516*chroma;
        }
    }

    private Rgb3Yuv() {}

    private static int normalizedMaximumFactor(int maximumFactor) {
        return Math.max(1,Math.min(maximumFactor,MAX_GPU_DOWNSAMPLE));
    }

    static int decoderSide(int cropSide) {
        return decoderSide(cropSide,DEFAULT_MAX_DOWNSAMPLE);
    }

    static int decoderSide(int cropSide, int maximumFactor) {
        return cropSide/decoderDownsampleFactor(cropSide,maximumFactor);
    }

    static int decoderSideForCapture(int cropSide, int sourceWidth, int sourceHeight,
            int maximumFactor) {
        return decoderSide(cropSide,maximumFactor);
    }

    static int decoderDownsampleFactor(int cropSide) {
        return decoderDownsampleFactor(cropSide,DEFAULT_MAX_DOWNSAMPLE);
    }

    /** Selects the largest cheap integer reduction that cannot take the
     * decoder below its required input side. Small captures remain at 1x. */
    static int decoderDownsampleFactor(int cropSide, int maximumFactor) {
        if (cropSide <= 0) throw new IllegalArgumentException("invalid crop side");
        int permitted=Math.max(1,cropSide/MIN_DECODER_SIDE);
        return Math.min(normalizedMaximumFactor(maximumFactor),permitted);
    }

    static int decoderDownsampleFactorForCapture(int cropSide, int sourceWidth, int sourceHeight,
            int maximumFactor) {
        return decoderDownsampleFactor(cropSide,maximumFactor);
    }

    /**
     * Produces the exact three luma buffers given to ZXing.  For the high
     * resolution camera mode, convert each source sample before box filtering
     * it: this preserves the extra chroma samples that would be lost by
     * downsampling YUV first.  Box filtering is deliberately used instead of
     * bilinear sampling because it anti-aliases the CRT's one-pixel modules.
     */
    static int convertForDecode(Image image, Rect crop, ByteBuffer[] output, int[] histogram) {
        return convertForDecode(image,crop,output,histogram,DEFAULT_MAX_DOWNSAMPLE);
    }

    static int convertForDecode(Image image, Rect crop, ByteBuffer[] output, int[] histogram,
            int highResolutionFactor) {
        if (image == null) throw new IllegalArgumentException("RGB3 image required");
        return convertForDecode(image, crop, output, histogram,
                image.getWidth(), image.getHeight(), highResolutionFactor);
    }

    static int convertForDecode(Image image, Rect crop, ByteBuffer[] output, int[] histogram,
            int sourceWidth, int sourceHeight, int highResolutionFactor) {
        if (image == null || crop == null || crop.width() != crop.height())
            throw new IllegalArgumentException("RGB3 square crop required");
        Image.Plane[] planes = image.getPlanes();
        if (planes.length < 3) throw new IllegalArgumentException("YUV_420_888 requires three planes");
        int sourceSide=crop.width(), factor=decoderDownsampleFactorForCapture(
                sourceSide, sourceWidth, sourceHeight, highResolutionFactor),
                side=decoderSideForCapture(sourceSide, sourceWidth, sourceHeight, highResolutionFactor);
        convert420Downsampled(planes[0].getBuffer(), planes[0].getRowStride(), planes[0].getPixelStride(),
                planes[1].getBuffer(), planes[1].getRowStride(), planes[1].getPixelStride(),
                planes[2].getBuffer(), planes[2].getRowStride(), planes[2].getPixelStride(),
                crop.left, crop.top, side, side, factor, output);
        for (int channel=0; channel<CHANNELS; channel++) autoLevels(output[channel], side*side, histogram);
        return side;
    }

    static void convert(Image image, Rect crop, ByteBuffer[] output) {
        if (image == null || crop == null || output == null || output.length < CHANNELS)
            throw new IllegalArgumentException("RGB3 conversion arguments");
        Image.Plane[] planes = image.getPlanes();
        if (planes.length < 3) throw new IllegalArgumentException("YUV_420_888 requires three planes");
        convert420(planes[0].getBuffer(), planes[0].getRowStride(), planes[0].getPixelStride(),
                planes[1].getBuffer(), planes[1].getRowStride(), planes[1].getPixelStride(),
                planes[2].getBuffer(), planes[2].getRowStride(), planes[2].getPixelStride(),
                crop.left, crop.top, crop.width(), crop.height(), output);
    }

    static void convert420(ByteBuffer ySource, int yRowStride, int yPixelStride,
            ByteBuffer uSource, int uRowStride, int uPixelStride,
            ByteBuffer vSource, int vRowStride, int vPixelStride,
            int left, int top, int width, int height, ByteBuffer[] output) {
        if (ySource == null || uSource == null || vSource == null ||
                width <= 0 || height <= 0 || left < 0 || top < 0 ||
                yRowStride <= 0 || uRowStride <= 0 || vRowStride <= 0 ||
                yPixelStride <= 0 || uPixelStride <= 0 || vPixelStride <= 0 ||
                output == null || output.length < CHANNELS)
            throw new IllegalArgumentException("invalid YUV geometry");

        int pixels = Math.multiplyExact(width, height);
        for (int channel=0; channel<CHANNELS; channel++) {
            if (output[channel] == null || output[channel].capacity() < pixels)
                throw new IllegalArgumentException("RGB3 output buffer too small");
            /* putShort() below is used only as two adjacent byte stores. Force
             * a deterministic order even if a caller reused a little-endian
             * buffer. */
            output[channel].clear();
            output[channel].order(ByteOrder.BIG_ENDIAN);
        }

        /* Absolute reads do not disturb Camera2's buffer positions, so avoid
         * allocating three duplicate ByteBuffer views for every image. */
        ByteBuffer y=ySource, u=uSource, v=vSource;
        int yOffset=y.position(), uOffset=u.position(), vOffset=v.position();
        ByteBuffer red=output[RED], green=output[GREEN], blue=output[BLUE];

        int row=0;
        while (row<height) {
            int sy=top+row;
            boolean twoRows=(sy&1)==0 && row+1<height;
            int rows=twoRows?2:1;
            int yBase0=sy*yRowStride+left*yPixelStride;
            int yBase1=twoRows?(sy+1)*yRowStride+left*yPixelStride:0;
            int uvY=sy>>1;
            int uBase=uvY*uRowStride;
            int vBase=uvY*vRowStride;
            int outBase0=row*width;
            int outBase1=twoRows?outBase0+width:0;
            int col=0;

            /* An odd source X belongs to the previous chroma pair. Consume it
             * alone so every following two-pixel iteration is chroma aligned. */
            if ((left&1)!=0 && col<width) {
                int uvX=left>>1;
                int uu=u.get(uOffset+uBase+uvX*uPixelStride)&0xff;
                int vv=v.get(vOffset+vBase+uvX*vPixelStride)&0xff;
                int rv=RED_V[vv], gv=GREEN_U[uu]+GREEN_V[vv], bu=BLUE_U[uu];
                int yt=Y_TERM[y.get(yOffset+yBase0)&0xff];
                putPixel(red,green,blue,outBase0,yt,rv,gv,bu);
                if (twoRows) {
                    yt=Y_TERM[y.get(yOffset+yBase1)&0xff];
                    putPixel(red,green,blue,outBase1,yt,rv,gv,bu);
                }
                col=1;
            }

            /* Two horizontally adjacent pixels share one chroma sample; when
             * sy is even the next row shares it as well. This is the common
             * centered 1080x1080 camera-crop path. */
            for (; col+1<width; col+=2) {
                int sx=left+col;
                int uvX=sx>>1;
                int uu=u.get(uOffset+uBase+uvX*uPixelStride)&0xff;
                int vv=v.get(vOffset+vBase+uvX*vPixelStride)&0xff;
                int rv=RED_V[vv], gv=GREEN_U[uu]+GREEN_V[vv], bu=BLUE_U[uu];
                int yIndex0=yOffset+yBase0+col*yPixelStride;
                int yt00=Y_TERM[y.get(yIndex0)&0xff];
                int yt01=Y_TERM[y.get(yIndex0+yPixelStride)&0xff];
                putPair(red,green,blue,outBase0+col,yt00,yt01,rv,gv,bu);
                if (twoRows) {
                    int yIndex1=yOffset+yBase1+col*yPixelStride;
                    int yt10=Y_TERM[y.get(yIndex1)&0xff];
                    int yt11=Y_TERM[y.get(yIndex1+yPixelStride)&0xff];
                    putPair(red,green,blue,outBase1+col,yt10,yt11,rv,gv,bu);
                }
            }

            if (col<width) {
                int sx=left+col;
                int uvX=sx>>1;
                int uu=u.get(uOffset+uBase+uvX*uPixelStride)&0xff;
                int vv=v.get(vOffset+vBase+uvX*vPixelStride)&0xff;
                int rv=RED_V[vv], gv=GREEN_U[uu]+GREEN_V[vv], bu=BLUE_U[uu];
                int yt=Y_TERM[y.get(yOffset+yBase0+col*yPixelStride)&0xff];
                putPixel(red,green,blue,outBase0+col,yt,rv,gv,bu);
                if (twoRows) {
                    yt=Y_TERM[y.get(yOffset+yBase1+col*yPixelStride)&0xff];
                    putPixel(red,green,blue,outBase1+col,yt,rv,gv,bu);
                }
            }
            row+=rows;
        }

        for (int channel=0; channel<CHANNELS; channel++) {
            output[channel].limit(pixels);
            output[channel].position(0);
        }
    }

    static void convert420Downsampled(ByteBuffer ySource, int yRowStride, int yPixelStride,
            ByteBuffer uSource, int uRowStride, int uPixelStride,
            ByteBuffer vSource, int vRowStride, int vPixelStride,
            int left, int top, int width, int height, int factor, ByteBuffer[] output) {
        if (factor <= 0 || width <= 0 || height <= 0 || left < 0 || top < 0 ||
                ySource == null || uSource == null || vSource == null || output == null || output.length < CHANNELS)
            throw new IllegalArgumentException("invalid downsample geometry");
        if (factor == 1) {
            convert420(ySource,yRowStride,yPixelStride,uSource,uRowStride,uPixelStride,
                    vSource,vRowStride,vPixelStride,left,top,width,height,output);
            return;
        }
        int pixels=Math.multiplyExact(width,height);
        for (int channel=0;channel<CHANNELS;channel++) {
            if (output[channel] == null || output[channel].capacity() < pixels)
                throw new IllegalArgumentException("RGB3 output buffer too small");
            output[channel].clear();
        }
        int yOffset=ySource.position(),uOffset=uSource.position(),vOffset=vSource.position();
        int area=factor*factor, destination=0;
        for (int row=0;row<height;row++) for (int col=0;col<width;col++,destination++) {
            int red=0,green=0,blue=0;
            int startY=top+row*factor,startX=left+col*factor;
            for (int dy=0;dy<factor;dy++) for (int dx=0;dx<factor;dx++) {
                int sy=startY+dy,sx=startX+dx;
                int yy=Y_TERM[ySource.get(yOffset+sy*yRowStride+sx*yPixelStride)&0xff];
                int uu=uSource.get(uOffset+(sy>>1)*uRowStride+(sx>>1)*uPixelStride)&0xff;
                int vv=vSource.get(vOffset+(sy>>1)*vRowStride+(sx>>1)*vPixelStride)&0xff;
                red+=clamp((yy+RED_V[vv])>>8);
                green+=clamp((yy+GREEN_U[uu]+GREEN_V[vv])>>8);
                blue+=clamp((yy+BLUE_U[uu])>>8);
            }
            output[RED].put(destination,(byte)((red+area/2)/area));
            output[GREEN].put(destination,(byte)((green+area/2)/area));
            output[BLUE].put(destination,(byte)((blue+area/2)/area));
        }
        for (int channel=0;channel<CHANNELS;channel++) { output[channel].limit(pixels);output[channel].position(0); }
    }

    /** Stretches the robust 2nd--98th percentile range to the full decoder
     * range.  This compensates for camera exposure/white-balance transforms
     * without allowing a handful of sensor outliers to choose the endpoints. */
    static void autoLevels(ByteBuffer buffer, int pixels, int[] histogram) {
        if (buffer == null || histogram == null || histogram.length < 256 || pixels <= 0 || buffer.limit() < pixels)
            throw new IllegalArgumentException("invalid auto-levels buffer");
        java.util.Arrays.fill(histogram,0);
        for (int i=0;i<pixels;i++) histogram[buffer.get(i)&0xff]++;
        int lowRank=Math.max(0,pixels/50), highRank=Math.max(lowRank+1,pixels-lowRank-1);
        int low=percentile(histogram,lowRank),high=percentile(histogram,highRank);
        if (high<=low+8) return;
        int[] lut=new int[256];
        for (int value=0;value<256;value++) lut[value]=value<=low?0:value>=high?255:(value-low)*255/(high-low);
        for (int i=0;i<pixels;i++) buffer.put(i,(byte)lut[buffer.get(i)&0xff]);
        buffer.position(0);buffer.limit(pixels);
    }

    private static int percentile(int[] histogram, int rank) {
        int cumulative=0;
        for (int value=0;value<256;value++) { cumulative+=histogram[value];if(cumulative>rank)return value; }
        return 255;
    }

    private static void putPixel(ByteBuffer red, ByteBuffer green, ByteBuffer blue,
            int index, int y, int redV, int greenUv, int blueU) {
        red.put(index,(byte)clamp((y+redV)>>8));
        green.put(index,(byte)clamp((y+greenUv)>>8));
        blue.put(index,(byte)clamp((y+blueU)>>8));
    }

    private static void putPair(ByteBuffer red, ByteBuffer green, ByteBuffer blue,
            int index, int y0, int y1, int redV, int greenUv, int blueU) {
        red.putShort(index,pack(clamp((y0+redV)>>8),clamp((y1+redV)>>8)));
        green.putShort(index,pack(clamp((y0+greenUv)>>8),clamp((y1+greenUv)>>8)));
        blue.putShort(index,pack(clamp((y0+blueU)>>8),clamp((y1+blueU)>>8)));
    }

    private static short pack(int first, int second) {
        return (short)((first<<8)|second);
    }

    private static int clamp(int value) {
        return value<0?0:Math.min(value,255);
    }
}
