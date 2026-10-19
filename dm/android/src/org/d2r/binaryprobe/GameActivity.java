package org.d2r.binaryprobe;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.view.KeyEvent;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import org.json.JSONArray;
import org.json.JSONObject;

public final class GameActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("d2r_mobile_android"); }
    private static native int nativeConfigure(String files, String locale,boolean touchRoute);
    private static native int nativeContentProfile(long romfsBytes,int[] addOnIndices);
    private static native int nativeTouch(int[] ids,float[] xs,float[] ys,boolean cancelled);
    private static native int[] nativeTouchGameState();
    private static native int nativeActions(long buttons,int lx,int ly,int rx,int ry);
    private static native int nativeControllerButtons(long buttons);
    private static native int nativeControllerActions(long buttons,int lx,int ly,int rx,int ry);
    private static native boolean nativeControllerMode();
    private static native void nativeTouchActivity();
    private static native void nativeControllerCancel(long buttons);
    private static native int nativeSurface(Surface surface,int width,int height,float density,float hz,int focus);
    private static native int nativeLifecycle(int event);
    private static native int nativeAudioInterruption(boolean began);
    private static native void nativeShutdown();
    private static native int nativeStart();
    private static native String nativeStatus();
    static native int nativeCloudProvider(Object store,String providerId,String cacheDirectory);
    static native String nativeCloudStatus();
    private static native String nativeSaveFailure();
    private static native String nativeKeyboardRequest();
    private static native void nativeKeyboardResult(long id,boolean accepted,String text);
    private static native void nativeKeyboardReady(boolean ready);
    private android.app.AlertDialog keyboardDialog;
    private android.app.AlertDialog saveFailureDialog;
    private long keyboardId;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final HandlerThread surfaceThread=new HandlerThread("game-surface");
    private Handler surfaceWorker;
    private boolean nativeLaunched, destroyed;
    private volatile boolean inputReady;
    private volatile int surfaceGeneration;
    private SurfaceView game;
    private TextView status,fps;
    private TextView modeControl;
    private GameUiStrings strings;
    private SafCloudSettings cloudSettings;
    private final GameDisplayMetrics.FpsSampler fpsSampler=new GameDisplayMetrics.FpsSampler();
    private double displayedFps=Double.NaN;
    private TouchControlsView touchControls;
    private final java.util.ArrayList<View> menuControls=new java.util.ArrayList<>();
    private boolean prepared, configured, surfaceReady, launched, resumed, failed;
    private long romfsBytes=8973693207L;
    private int[] addOnIndices=new int[0];
    private boolean touchMode;
    private boolean inGame;
    private int width,height;
    private long actionButtons;
    private long controllerButtons;
    private final GameControllerState controllers=new GameControllerState();
    private android.hardware.input.InputManager inputManager;
    private final android.hardware.input.InputManager.InputDeviceListener deviceListener=new android.hardware.input.InputManager.InputDeviceListener(){
        @Override public void onInputDeviceAdded(int id){}
        @Override public void onInputDeviceChanged(int id){}
        @Override public void onInputDeviceRemoved(int id){controllers.remove(id);if(gameInputReady())publishControllers();}
    };
    private static final long BTN_A=1L, BTN_B=2L, BTN_X=4L, BTN_Y=8L;
    private static final long BTN_LS=16L, BTN_RS=32L;
    private static final long BTN_L1=64L, BTN_R1=128L, BTN_L2=256L, BTN_R2=512L;
    private static final long BTN_START=1024L, BTN_SELECT=2048L;
    private static final long BTN_LEFT=4096L, BTN_UP=8192L, BTN_RIGHT=16384L, BTN_DOWN=32768L;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        strings=new GameUiStrings(getResources().getConfiguration().getLocales().get(0));
        cloudSettings=new SafCloudSettings(this,strings);
        new GameHaptics(this);
        inputManager=(android.hardware.input.InputManager)getSystemService(INPUT_SERVICE);
        if(inputManager!=null)inputManager.registerInputDeviceListener(deviceListener,ui);
        // Match iOS MixWithOthers: AAudio uses a shared GAME stream. Taking
        // AUDIOFOCUS_GAIN here would stop other apps even at zero game volume.
        // Activity lifecycle still suspends our stream in the background.
        android.util.Log.i("D2RAudio", "shared output; other apps may keep playing");
        surfaceThread.start();surfaceWorker=new Handler(surfaceThread.getLooper());
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        FrameLayout root=new FrameLayout(this);root.setBackgroundColor(0xff0d0d0f);
        GameViewport viewport=new GameViewport(this);
        root.addView(viewport,new FrameLayout.LayoutParams(-1,-1,Gravity.CENTER));
        game=new SurfaceView(this);game.setContentDescription(t("游戏画面"));game.getHolder().addCallback(this);
        game.setOnTouchListener((view,event) -> {
            if(!configured||!surfaceReady||!inputReady||width<=0||height<=0)return true;
            if(!touchMode) setTouchMode(true);
            // The SurfaceView owns this contact after DOWN even when the
            // overlay becomes visible. Forward the whole first gesture too.
            if(touchControls!=null)return touchControls.onTouchEvent(event);
            int action=event.getActionMasked(),skip=action==MotionEvent.ACTION_POINTER_UP?event.getActionIndex():-1;
            int count=(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL)?0:event.getPointerCount()-(skip>=0?1:0);
            if(count>16){nativeTouch(new int[0],new float[0],new float[0],true);return true;}
            int[] ids=new int[count];float[] xs=new float[count],ys=new float[count];int k=0;
            for(int i=0;i<event.getPointerCount()&&k<count;i++)if(i!=skip){
                ids[k]=event.getPointerId(i);xs[k]=Math.max(0,Math.min(0.999999f,event.getX(i)/width));
                ys[k]=Math.max(0,Math.min(0.999999f,event.getY(i)/height));k++;
            }
            int rc=nativeTouch(ids,xs,ys,action==MotionEvent.ACTION_CANCEL);
            if(rc!=0)fail(t("触摸输入不可用"),new Exception(t("错误 ")+rc));return true;
        });
        viewport.addView(game,new FrameLayout.LayoutParams(-1,-1));
        addAction(viewport,"确认",1,Gravity.RIGHT|Gravity.BOTTOM,18,18);
        addAction(viewport,"返回",2,Gravity.RIGHT|Gravity.BOTTOM,104,18);
        addAction(viewport,"开始",1L<<10,Gravity.RIGHT|Gravity.TOP,18,18);
        addAction(viewport,"↑",1L<<13,Gravity.LEFT|Gravity.BOTTOM,92,92);
        addAction(viewport,"↓",1L<<15,Gravity.LEFT|Gravity.BOTTOM,92,18);
        addAction(viewport,"←",1L<<12,Gravity.LEFT|Gravity.BOTTOM,18,55);
        addAction(viewport,"→",1L<<14,Gravity.LEFT|Gravity.BOTTOM,166,55);
        touchControls=new TouchControlsView(this,() -> inputReady&&resumed&&!failed,strings);
        touchControls.setTouchActivation(() -> {
            if(inGame&&gameInputReady()){setTouchMode(true);return true;}
            return false;
        });
        viewport.addView(touchControls,new FrameLayout.LayoutParams(-1,-1));
        TextView mode=new TextView(this);modeControl=mode;mode.setText(strings.modeLabel(touchMode));mode.setTextColor(0xffe9d9ba);mode.setTextSize(16);
        mode.setGravity(Gravity.CENTER);mode.setBackgroundColor(0x8030241b);mode.setContentDescription(strings.modeDescription(touchMode));mode.setClickable(true);mode.setFocusable(true);
        FrameLayout.LayoutParams modeParams=new FrameLayout.LayoutParams(dp(100),dp(48),Gravity.LEFT|Gravity.TOP);modeParams.leftMargin=dp(18);modeParams.topMargin=dp(18);
        mode.setOnClickListener(v -> setTouchMode(!touchMode));
        mode.setOnLongClickListener(v -> {cloudSettings.showMenu();return true;});
        viewport.addView(mode,modeParams);
        fps=new TextView(this);fps.setText("FPS —");fps.setTextColor(TouchControlsView.TEXT);
        fps.setTextSize(12);fps.setTypeface(Typeface.MONOSPACE);fps.setGravity(Gravity.CENTER);
        fps.setMinWidth(dp(84));fps.setPadding(dp(8),dp(4),dp(8),dp(4));fps.setBackgroundColor(TouchControlsView.INK);
        fps.setClickable(false);fps.setFocusable(false);
        FrameLayout.LayoutParams fpsParams=new FrameLayout.LayoutParams(-2,-2,Gravity.TOP|Gravity.CENTER_HORIZONTAL);
        fpsParams.topMargin=dp(10);viewport.addView(fps,fpsParams);
        status=new TextView(this);status.setTextColor(0xffe9d9ba);status.setTextSize(18);
        status.setGravity(Gravity.CENTER);status.setBackgroundColor(0xe60d0d0f);
        status.setPadding(dp(28),dp(18),dp(28),dp(18));status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        status.setText(t("正在准备游戏文件"));root.addView(status,new FrameLayout.LayoutParams(-1,-1));setContentView(root);
        new Thread(() -> {
            try { prepareModules();cloudSettings.prepareProvider();ui.post(() -> {prepared=true;configureAndStart();}); }
            catch(Exception e){ui.post(() -> fail(t("游戏文件准备失败"),e));}
        },"game-files").start();
    }
    private String t(String text){return strings.text(text);}
    private void refreshFrontendStrings(){
        game.setContentDescription(t("游戏画面"));
        if(modeControl!=null){modeControl.setText(strings.modeLabel(touchMode));modeControl.setContentDescription(strings.modeDescription(touchMode));}
        for(View view:menuControls){TextView button=(TextView)view;String label=(String)button.getTag();button.setText(t(label));button.setContentDescription(strings.menuDescription(label));}
        if(cloudSettings!=null)cloudSettings.refreshText();
    }
    private int dp(int value){return Math.round(value*getResources().getDisplayMetrics().density);}
    private void setTouchMode(boolean on){
        touchMode=on; actionButtons=0; if(inputReady)nativeActions(0,0,0,0,0);
        if(touchControls!=null)touchControls.setActive(on);
        for(View control:menuControls)control.setVisibility(on?View.GONE:View.VISIBLE);
        if(modeControl!=null){modeControl.setText(strings.modeLabel(on));modeControl.setContentDescription(strings.modeDescription(on));}
    }
    private void physicalInputSeen(){if(touchMode)setTouchMode(false);}
    private static long keyButton(int key) {
        switch (key) {
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_ENTER: return BTN_A;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_SPACE: return BTN_B;
            case KeyEvent.KEYCODE_BUTTON_X: return BTN_X; case KeyEvent.KEYCODE_BUTTON_Y: return BTN_Y;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return BTN_LS; case KeyEvent.KEYCODE_BUTTON_THUMBR: return BTN_RS;
            case KeyEvent.KEYCODE_BUTTON_L1: return BTN_L1; case KeyEvent.KEYCODE_BUTTON_R1: return BTN_R1;
            case KeyEvent.KEYCODE_BUTTON_L2: return BTN_L2; case KeyEvent.KEYCODE_BUTTON_R2: return BTN_R2;
            case KeyEvent.KEYCODE_BUTTON_START: return BTN_START; case KeyEvent.KEYCODE_BUTTON_SELECT: return BTN_SELECT;
            case KeyEvent.KEYCODE_DPAD_LEFT: return BTN_LEFT; case KeyEvent.KEYCODE_DPAD_UP: return BTN_UP;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return BTN_RIGHT; case KeyEvent.KEYCODE_DPAD_DOWN: return BTN_DOWN;
            default: return 0;
        }
    }
    private static int axis(float v) { return Math.round(Math.max(-1f, Math.min(1f, v)) * 32767f); }
    private static float centredAxis(MotionEvent event,int axis){
        android.view.InputDevice device=event.getDevice();
        android.view.InputDevice.MotionRange range=device==null?null:device.getMotionRange(axis,event.getSource());
        float value=event.getAxisValue(axis);return range!=null&&Math.abs(value)<=range.getFlat()?0:value;
    }
    private static boolean hasAxis(MotionEvent event,int axis){android.view.InputDevice d=event.getDevice();return d!=null&&d.getMotionRange(axis,event.getSource())!=null;}
    private void publishControllers(){
        GameControllerState.Snapshot s=controllers.snapshot();controllerButtons=s.buttons;
        int rc=nativeControllerActions(s.buttons,s.lx,s.ly,s.rx,s.ry);
        if(rc==0&&nativeControllerMode())physicalInputSeen();
    }
    private boolean gameInputReady() { return configured && surfaceReady && inputReady && resumed && hasWindowFocus() && !failed; }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        long bit=keyButton(event.getKeyCode());
        boolean physical=(event.getSource() & (android.view.InputDevice.SOURCE_GAMEPAD|android.view.InputDevice.SOURCE_JOYSTICK))!=0;
        if(bit!=0 && physical) {
            if(gameInputReady()) {
                if(event.getAction()==KeyEvent.ACTION_DOWN||event.getAction()==KeyEvent.ACTION_UP){
                    controllers.key(event.getDeviceId(),bit,event.getAction()==KeyEvent.ACTION_DOWN);publishControllers();}
            }
            return true;
        }
        if(event.getKeyCode()==KeyEvent.KEYCODE_BACK &&
            (event.getAction()==KeyEvent.ACTION_DOWN || event.getAction()==KeyEvent.ACTION_UP)) {
            if(gameInputReady()) {
                controllers.key(event.getDeviceId(),BTN_B,event.getAction()==KeyEvent.ACTION_DOWN);publishControllers();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }
    @Override public void onBackPressed() {
        if(gameInputReady()) {
            controllers.key(-1,BTN_B,true);publishControllers();controllers.key(-1,BTN_B,false);publishControllers();
        } else super.onBackPressed();
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if((event.getSource() & android.view.InputDevice.SOURCE_JOYSTICK)!=0 && gameInputReady() &&
            (event.getAction()==MotionEvent.ACTION_MOVE || event.getAction()==MotionEvent.ACTION_HOVER_MOVE)) {
            long buttons=0;
            float hx=event.getAxisValue(MotionEvent.AXIS_HAT_X),hy=event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            if(hx<-.5f)buttons|=BTN_LEFT;if(hx>.5f)buttons|=BTN_RIGHT;
            if(hy<-.5f)buttons|=BTN_UP;if(hy>.5f)buttons|=BTN_DOWN;
            if(Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER),event.getAxisValue(MotionEvent.AXIS_BRAKE))>.5f)buttons|=BTN_L2;
            if(Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER),event.getAxisValue(MotionEvent.AXIS_GAS))>.5f)buttons|=BTN_R2;
            int rightX=hasAxis(event,MotionEvent.AXIS_Z)?MotionEvent.AXIS_Z:MotionEvent.AXIS_RX;
            int rightY=hasAxis(event,MotionEvent.AXIS_RZ)?MotionEvent.AXIS_RZ:MotionEvent.AXIS_RY;
            controllers.motion(event.getDeviceId(),buttons,axis(centredAxis(event,MotionEvent.AXIS_X)),
                axis(-centredAxis(event,MotionEvent.AXIS_Y)),axis(centredAxis(event,rightX)),axis(-centredAxis(event,rightY)));
            publishControllers();
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }
    private void addAction(FrameLayout root,String label,long mask,int gravity,int horizontal,int vertical){
        TextView button=new TextView(this);button.setTag(label);button.setText(t(label));button.setContentDescription(strings.menuDescription(label));button.setTextColor(0xffeadfcd);
        button.setGravity(Gravity.CENTER);button.setTextSize(17);button.setBackgroundColor(0x70443930);
        FrameLayout.LayoutParams p=new FrameLayout.LayoutParams(dp(68),dp(58),gravity);
        if((gravity&Gravity.RIGHT)==Gravity.RIGHT)p.rightMargin=dp(horizontal);else p.leftMargin=dp(horizontal);
        if((gravity&Gravity.BOTTOM)==Gravity.BOTTOM)p.bottomMargin=dp(vertical);else p.topMargin=dp(vertical);
        button.setOnTouchListener((v,e)->{if(!configured||!surfaceReady||!inputReady)return true;
            int a=e.getActionMasked();if(a==MotionEvent.ACTION_DOWN){nativeTouchActivity();actionButtons|=mask;}
            else if(a==MotionEvent.ACTION_UP||a==MotionEvent.ACTION_CANCEL)actionButtons&=~mask;
            int rc=nativeActions(actionButtons,0,0,0,0);if(rc!=0)fail(t("操作输入不可用"),new Exception(t("错误 ")+rc));return true;});
        root.addView(button,p);
        menuControls.add(button);
    }
    private static String digest(File file)throws Exception {
        MessageDigest md=MessageDigest.getInstance("SHA-256");byte[] bytes=new byte[65536];
        try(InputStream in=new FileInputStream(file)){int n;while((n=in.read(bytes))!=-1)md.update(bytes,0,n);}
        StringBuilder text=new StringBuilder();for(byte b:md.digest())text.append(String.format("%02x",b&255));return text.toString();
    }
    private void prepareModules()throws Exception {
        String json;
        try(InputStream in=getAssets().open("module-manifest.json")){
            java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream();byte[] b=new byte[4096];int n;
            while((n=in.read(b))!=-1)out.write(b,0,n);json=new String(out.toByteArray(),StandardCharsets.UTF_8);
        }
        JSONObject manifest=new JSONObject(json);
        JSONArray list=manifest.getJSONArray("files");File directory=new File(getFilesDir(),"d2r-mobile/modules");
        if(!directory.isDirectory()&&!directory.mkdirs())throw new java.io.IOException(t("不能创建游戏目录"));
        java.util.HashSet<String> remaining=new java.util.HashSet<>();
        for(String module:new String[]{"main","sdk","subsdk0"})
            for(String suffix:new String[]{"patched.bin","fixups.tsv","gates.tsv"})remaining.add(module+"."+suffix);
        for(int i=0;i<list.length();i++){
            JSONObject entry=list.getJSONObject(i);String name=entry.getString("name"),hash=entry.getString("sha256");
            if(!name.matches("(main|sdk|subsdk0)\\.(patched\\.bin|fixups\\.tsv|gates\\.tsv)"))throw new java.io.IOException(t("模块清单无效"));
            if(!remaining.remove(name)||!hash.matches("[0-9a-f]{64}"))throw new java.io.IOException(t("模块清单重复或校验值无效"));
            File target=new File(directory,name);
            if(target.isFile()&&target.length()==entry.getLong("bytes")&&digest(target).equals(hash))continue;
            File temporary=new File(directory,name+".new");
            try(InputStream in=getAssets().open("modules/"+name);FileOutputStream out=new FileOutputStream(temporary)){
                byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)out.write(b,0,n);out.getFD().sync();
            }
            if(temporary.length()!=entry.getLong("bytes")||!digest(temporary).equals(hash))throw new java.io.IOException(t("游戏模块校验失败"));
            if(!temporary.renameTo(target))throw new java.io.IOException(t("不能保存游戏模块"));
        }
        if(!remaining.isEmpty())throw new java.io.IOException(t("游戏模块清单不完整"));
        if(!new File(getFilesDir(),"game.romfs").isFile())throw new java.io.IOException(t("缺少原游戏资源"));
        if(manifest.has("romfs")){
            if(!"0100726014352000".equals(manifest.getString("title_id"))||
                !"db0507f36b84200ac05f75d76ed5026f".equals(manifest.getString("build_id")))
                throw new java.io.IOException(t("游戏资源版本不受支持"));
            String expected="# BUILD_ID\t"+manifest.getString("build_id");
            String fixups=new String(java.nio.file.Files.readAllBytes(new File(directory,"main.fixups.tsv").toPath()),StandardCharsets.UTF_8);
            boolean matched=false;for(String line:fixups.split("\\r?\\n"))if(line.equals(expected))matched=true;
            if(!matched)throw new java.io.IOException(t("游戏模块与资源版本不匹配"));
            JSONObject resource=manifest.getJSONObject("romfs");romfsBytes=resource.getLong("bytes");
            String hash=resource.getString("sha256");File romfs=new File(getFilesDir(),"game.romfs");
            if(romfsBytes<0x50||romfs.length()!=romfsBytes||!hash.matches("[0-9a-f]{64}")||!digest(romfs).equals(hash))
                throw new java.io.IOException(t("游戏资源校验失败"));
            JSONArray addOns=manifest.optJSONArray("add_on_content_indices");
            if(addOns!=null){
                if(addOns.length()>64)throw new java.io.IOException(t("追加内容清单无效"));
                addOnIndices=new int[addOns.length()];java.util.HashSet<Integer> seen=new java.util.HashSet<>();
                for(int i=0;i<addOnIndices.length;i++){
                    int value=addOns.getInt(i);if(value<0||!seen.add(value))throw new java.io.IOException(t("追加内容清单无效"));
                    addOnIndices[i]=value;
                }
            }
        }
        if(!new File(getFilesDir(),"d2r-mobile/shaders/shader_index.bin").isFile())throw new java.io.IOException(t("缺少游戏图形缓存"));
    }
    private void configureAndStart(){
        if(!prepared||failed||destroyed)return;
        if(!configured){int rc=nativeConfigure(getFilesDir().getAbsolutePath(),getResources().getConfiguration().getLocales().get(0).toLanguageTag(),
            getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_TOUCHSCREEN));
            if(rc==0)rc=nativeContentProfile(romfsBytes,addOnIndices);
            if(rc!=0){fail(t("游戏初始化失败"),new Exception(t("错误 ")+rc));return;}configured=true;}
        if(surfaceReady)publishSurface();
    }
    private void publishSurface(){
        if(!configured)return;
        final Surface surface=surfaceReady?game.getHolder().getSurface():null;
        final int w=width,h=height,focus=resumed?1:3;
        final float density=getResources().getDisplayMetrics().density,hz=getWindowManager().getDefaultDisplay().getRefreshRate();
        final boolean start=surfaceReady&&resumed&&!destroyed;
        final int generation=++surfaceGeneration;
        inputReady=false;
        // Draining an in-flight GPU frame can take seconds. Serialize Surface
        // replacement and startup off the Android lifecycle/UI thread.
        surfaceWorker.post(() -> {
            if(generation!=surfaceGeneration)return;
            int rc=nativeSurface(surface,w,h,density,hz,focus);
            if(generation!=surfaceGeneration)return;
            if(rc!=0){final int error=rc;ui.post(() -> {if(!destroyed&&generation==surfaceGeneration)fail(t("游戏窗口不可用"),new Exception(t("错误 ")+error));});return;}
            inputReady=start;
            ui.post(() -> {if(!destroyed)touchControls.postInvalidateOnAnimation();});
            if(start&&!nativeLaunched){rc=nativeStart();
                if(rc!=0){final int error=rc;ui.post(() -> {if(!destroyed&&generation==surfaceGeneration)fail(t("游戏启动失败"),new Exception(t("错误 ")+error));});return;}
                nativeLaunched=true;ui.post(() -> {if(!destroyed&&!launched){launched=true;ui.post(poll);}});
            }
        });
    }
    private void fail(String title,Exception error){nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}failed=true;status.setVisibility(View.VISIBLE);status.setText(title+"\n"+error.getMessage());}
    private final Runnable poll=new Runnable(){public void run(){
        if(!launched||failed)return;
        try{String raw=nativeStatus();JSONObject s=new JSONObject(raw);
            JSONObject scene=s.optJSONObject("scene");inGame=scene!=null&&scene.optInt("game",0)!=0;
            long frame=s.getLong("presented_frames");
            if(frame>0&&touchControls!=null&&touchControls.refreshLanguage())refreshFrontendStrings();
            if(cloudSettings!=null)cloudSettings.updateNativeStatus(nativeCloudStatus());
            if(frame>0)status.setVisibility(View.GONE);else status.setText(t(s.getString("phase")));
            if(resumed){double current=fpsSampler.sample(frame,SystemClock.elapsedRealtime());
                if(!Double.isNaN(current)&&Double.compare(current,displayedFps)!=0){
                    fps.setText(String.format(getResources().getConfiguration().getLocales().get(0),"FPS %.1f",current));
                    displayedFps=current;
                }}
            /* The native title adapter publishes the same state contract used
             * by the iOS touch overlay.  Pull it at the UI cadence so target
             * visibility (navigation/panels/belt/held item) follows the game
             * without making JNI calls from MotionEvent dispatch. */
            if(touchControls!=null){int[] ts=nativeTouchGameState();if(ts!=null&&ts.length>=10)
                touchControls.setGameState(ts[0]!=0,ts[1],ts[2],ts[3]!=0,ts[4]!=0,ts[5],ts[6]!=0,ts[7],ts[8],ts[9]);}
            String request=nativeKeyboardRequest();if(request!=null&&resumed&&!destroyed){JSONObject k=new JSONObject(request);long id=k.getLong("id");
                if(keyboardDialog==null||keyboardId!=id){if(keyboardDialog!=null)keyboardDialog.dismiss();keyboardId=id;actionButtons=0;nativeActions(0,0,0,0,0);touchControls.cancel();
                    keyboardDialog=ShellTextInputDialog.show(GameActivity.this,k.getString("title"),k.getString("subtitle"),k.getString("hint"),k.getString("initial"),k.getInt("min"),k.getInt("max"),strings,(ok,text)->{nativeKeyboardResult(id,ok,text);keyboardDialog=null;});}}
            if(resumed&&!destroyed&&keyboardDialog==null&&saveFailureDialog==null){String message=nativeSaveFailure();if(message!=null){
                saveFailureDialog=new android.app.AlertDialog.Builder(GameActivity.this).setTitle(message)
                    .setPositiveButton(t("确认"),null).create();
                saveFailureDialog.setOnDismissListener(dialog -> saveFailureDialog=null);saveFailureDialog.show();}}
        }catch(Exception e){fail(t("无法读取游戏状态"),e);}ui.postDelayed(this,500);
    }};
    @Override public void onWindowFocusChanged(boolean hasFocus){super.onWindowFocusChanged(hasFocus);if(!hasFocus){
        controllers.clear();controllerButtons=0;actionButtons=0;
        if(configured){nativeControllerCancel(0);nativeActions(0,0,0,0,0);}
        if(touchControls!=null)touchControls.cancel();
    }}
    @Override public void onResume(){super.onResume();resumed=true;if(cloudSettings!=null)cloudSettings.onResume();fpsSampler.reset();nativeKeyboardReady(true);if(configured)nativeAudioInterruption(false);if(configured)nativeLifecycle(2);configureAndStart();}
    @Override public void onPause(){if(cloudSettings!=null)cloudSettings.onPause();controllers.clear();if(configured){nativeLifecycle(0);nativeLifecycle(1);}nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}resumed=false;fpsSampler.reset();displayedFps=Double.NaN;if(fps!=null)fps.setText("FPS —");inputReady=false;actionButtons=0;controllerButtons=0;if(touchControls!=null)touchControls.cancel();if(configured){nativeActions(0,0,0,0,0);nativeControllerCancel(0);}if(configured&&surfaceReady)publishSurface();super.onPause();}
    @Override protected void onActivityResult(int request,int result,android.content.Intent data){super.onActivityResult(request,result,data);if(cloudSettings!=null)cloudSettings.onActivityResult(request,result,data);}
    @Override public void surfaceCreated(SurfaceHolder holder){if(!destroyed)surfaceReady=true;}
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int w,int h){if(destroyed)return;width=w;height=h;surfaceReady=true;configureAndStart();}
    @Override public void surfaceDestroyed(SurfaceHolder holder){if(destroyed)return;surfaceReady=false;if(configured)publishSurface();}
    @Override public void onDestroy(){if(cloudSettings!=null)cloudSettings.close();if(inputManager!=null)inputManager.unregisterInputDeviceListener(deviceListener);controllers.clear();if(configured){nativeLifecycle(1);nativeControllerCancel(0);}nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}if(saveFailureDialog!=null){saveFailureDialog.dismiss();saveFailureDialog=null;}destroyed=true;inputReady=false;surfaceReady=false;if(configured)publishSurface();ui.removeCallbacks(poll);nativeShutdown();surfaceThread.quitSafely();super.onDestroy();}
}
