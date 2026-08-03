package org.dosfer.receiver;

import android.Manifest;
import android.app.Activity;
import android.content.*;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.*;
import android.provider.Settings;
import android.view.*;
import android.widget.*;
import java.util.Locale;

public final class MainActivity extends Activity {
    private static final int CAMERA_PERMISSION=10,DESTINATION=20;
    private TextureView preview;private TextView status,result;private Button reconstruct;private SessionStore store;private CameraScanner scanner;private Uri destination;private final Handler ui=new Handler(Looper.getMainLooper());private String lastMessage="";
    private final Runnable refresh=new Runnable(){public void run(){showStats();ui.postDelayed(this,250);}};
    private static final class SquareTextureView extends TextureView {
        SquareTextureView(Context context){super(context);}
        @Override protected void onMeasure(int widthSpec,int heightSpec){int side=MeasureSpec.getSize(widthSpec);setMeasuredDimension(side,side);}
    }
    @Override public void onCreate(Bundle b){super.onCreate(b);store=new SessionStore(this);buildUi();String saved=getPreferences(0).getString("destination",null);if(saved!=null)destination=Uri.parse(saved);ui.post(refresh);ensureCamera();}
    private void buildUi(){getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);LinearLayout root=new LinearLayout(this);root.setOrientation(LinearLayout.VERTICAL);root.setPadding(16,16,16,16);root.setBackgroundColor(Color.rgb(16,24,32));
        preview=new SquareTextureView(this);root.addView(preview,new LinearLayout.LayoutParams(-1,-2));
        status=text(15,Color.WHITE);status.setTypeface(android.graphics.Typeface.MONOSPACE);root.addView(status,new LinearLayout.LayoutParams(-1,0,1));
        result=text(22,Color.rgb(100,255,140));result.setGravity(Gravity.CENTER);root.addView(result,new LinearLayout.LayoutParams(-1,-2));
        LinearLayout row=new LinearLayout(this);String[] labels={"Choose destination","Lock focus/exposure","Reconstruct","Reset session"};for(String l:labels){Button v=new Button(this);v.setText(l);row.addView(v,new LinearLayout.LayoutParams(0,-2,1));if(l.startsWith("Choose"))v.setOnClickListener(x->choose());else if(l.startsWith("Lock"))v.setOnClickListener(x->{if(scanner!=null)scanner.lockStability();});else if(l.startsWith("Reconstruct")){reconstruct=v;v.setOnClickListener(x->reconstruct());}else v.setOnClickListener(x->{store.reset();if(scanner!=null)scanner.clearLastPayload();lastMessage="";});}root.addView(row);setContentView(root);}
    private TextView text(int sp,int color){TextView v=new TextView(this);v.setTextSize(sp);v.setTextColor(color);v.setPadding(8,8,8,8);return v;}
    private void ensureCamera(){if(checkSelfPermission(Manifest.permission.CAMERA)==PackageManager.PERMISSION_GRANTED)startCamera();else requestPermissions(new String[]{Manifest.permission.CAMERA},CAMERA_PERMISSION);}
    private void startCamera(){if(scanner!=null)return;scanner=new CameraScanner(this,preview,new CameraScanner.Listener(){public void payload(byte[] b,long latency){SessionStore.Result r=store.accept(b,latency);runOnUiThread(()->{if(r==SessionStore.Result.INVALID)lastMessage="INVALID FRAME";});}public void error(String m){runOnUiThread(()->lastMessage="Camera: "+m);}});scanner.start();}
    @Override public void onRequestPermissionsResult(int r,String[] p,int[] g){super.onRequestPermissionsResult(r,p,g);if(r==CAMERA_PERMISSION&&g.length>0&&g[0]==PackageManager.PERMISSION_GRANTED)startCamera();else result.setText("Camera permission is required. Enable it in Android settings.");}
    private void choose(){Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);startActivityForResult(i,DESTINATION);}
    @Override protected void onActivityResult(int r,int c,Intent data){super.onActivityResult(r,c,data);if(r==DESTINATION&&c==RESULT_OK&&data!=null){destination=data.getData();if(destination!=null){getContentResolver().takePersistableUriPermission(destination,Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION);getPreferences(0).edit().putString("destination",destination.toString()).apply();}}}
    private void showStats(){SessionStore.Stats s=store.stats();CameraScanner.Stats c=scanner==null?new CameraScanner.Stats():scanner.stats();status.setText(String.format(Locale.US,"Session: %08X    DOS window: %d\nUnique: %d / %d in window    Missing: %s\nDuplicate: %d    Invalid: %d    Other session: %d\nDecoded: %.2f fps    Useful: %.0f B/s    Latency: %.1f ms\nCalibration: %d unique    %d missed\nCamera: %dx%d capture -> %dx%d decode\nRate: %d target, %.1f actual fps    Attempts: %.1f/s\nQR reads: %d    No QR: %d    Latest replacements: %d\nAttempt time: %.1f ms avg / %.1f ms max\nPayload received: %d bytes    Total frames: %d\nDestination: %s\nIntegrity: %s",s.session,s.window+1,s.uniqueWindow,s.expected,s.missing,s.duplicates,s.invalid,s.other,s.decodedFps,s.usefulBps,s.avgLatencyMs,s.calibrationUnique,s.calibrationMissed,c.width,c.height,c.decodeWidth,c.decodeHeight,c.targetFps,c.cameraFps,c.attemptFps,c.successes,c.failures,c.busyDrops,c.avgAttemptMs,c.maxAttemptMs,s.totalPayload,s.uniqueTotal,destination==null?"not selected":"selected",s.complete?"all frames present; ready to reconstruct":"INCOMPLETE"));if(!lastMessage.isEmpty())result.setText(lastMessage);else if(s.session==0)result.setText(s.calibration);else result.setText(s.complete?"ALL FRAMES PRESENT":"SCANNING");reconstruct.setEnabled(s.complete&&destination!=null);}
    private void reconstruct(){reconstruct.setEnabled(false);lastMessage="Reconstructing…";new Thread(()->{Reconstructor.Result r=new Reconstructor(getContentResolver(),destination).reconstruct(store.orderedFrames(),m->runOnUiThread(()->lastMessage=m));runOnUiThread(()->{lastMessage=(r.ok?"VALID: ":"NOT COMPLETE: ")+r.message;reconstruct.setEnabled(true);});},"dosfer-reconstruct").start();}
    @Override protected void onPause(){if(scanner!=null){scanner.stop();scanner=null;}super.onPause();}
    @Override protected void onResume(){super.onResume();if(checkSelfPermission(Manifest.permission.CAMERA)==PackageManager.PERMISSION_GRANTED)preview.post(this::startCamera);}
    @Override protected void onDestroy(){ui.removeCallbacks(refresh);if(scanner!=null)scanner.stop();super.onDestroy();}
}
