import java.awt.image.BufferedImage;
import java.io.File;
import java.util.EnumMap;
import java.util.Map;
import javax.imageio.ImageIO;
import com.google.zxing.BinaryBitmap;
import com.google.zxing.DecodeHintType;
import com.google.zxing.MultiFormatReader;
import com.google.zxing.RGBLuminanceSource;
import com.google.zxing.Result;
import com.google.zxing.ResultMetadataType;
import com.google.zxing.common.HybridBinarizer;
import java.util.List;
import java.nio.file.Files;
import java.nio.file.Path;

public final class DecodeQrImage {
    public static void main(String[] args) throws Exception {
        BufferedImage image = ImageIO.read(new File(args[0]));
        int width = image.getWidth(), height = image.getHeight();
        int[] pixels = image.getRGB(0, 0, width, height, null, 0, width);
        BinaryBitmap bitmap = new BinaryBitmap(new HybridBinarizer(
            new RGBLuminanceSource(width, height, pixels)));
        Map<DecodeHintType,Object> hints = new EnumMap<>(DecodeHintType.class);
        hints.put(DecodeHintType.TRY_HARDER, Boolean.TRUE);
        Result result = new MultiFormatReader().decode(bitmap, hints);
        byte[] raw = result.getRawBytes();
        System.out.printf("format=%s rawBytes=%d prefix=", result.getBarcodeFormat(), raw.length);
        for (int i = 0; i < Math.min(raw.length, 16); i++) System.out.printf("%02X", raw[i] & 255);
        System.out.println();
        Object segments=result.getResultMetadata()==null?null:result.getResultMetadata().get(ResultMetadataType.BYTE_SEGMENTS);
        if(segments instanceof List)for(Object item:(List<?>)segments)if(item instanceof byte[]){
            byte[] b=(byte[])item;int zeros=0,ones=0,maxRun=0,run=0,last=-1;
            for(byte value:b){int v=value&255;if(v==0)zeros++;for(int bit=7;bit>=0;bit--){int x=(v>>bit)&1;ones+=x;if(x==last)run++;else{last=x;run=1;}if(run>maxRun)maxRun=run;}}
            System.out.printf("byteSegment=%d zeroBytes=%d (%.1f%%) oneBits=%.1f%% maxBitRun=%d prefix=",b.length,zeros,zeros*100.0/b.length,ones*100.0/(b.length*8),maxRun);
            for(int i=0;i<Math.min(b.length,24);i++)System.out.printf("%02X",b[i]&255);System.out.println();
            if(args.length>1){Files.write(Path.of(args[1]),b);System.out.println("saved="+args[1]);}
            break;
        }
    }
}
