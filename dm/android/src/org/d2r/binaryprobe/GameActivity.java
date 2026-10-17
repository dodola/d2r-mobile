package org.d2r.binaryprobe;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Button;
import android.content.res.ColorStateList;
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
    private static native int nativeTouch(int[] ids,float[] xs,float[] ys,boolean cancelled);
    private static native int nativeActions(long buttons,int lx,int ly,int rx,int ry);
    private static native int nativeControllerButtons(long buttons);
    private static native void nativeControllerCancel(long remainingButtons);
    private static native int nativeSurface(Surface surface,int width,int height,float density,float hz,int focus);
    private static native int nativeStart();
    private static native String nativeStatus();
    private static native int nativeRenderScale(float scale);
    private static native String nativeKeyboardRequest();
    private static native void nativeKeyboardResult(long id,boolean accepted,String text);
    private static native void nativeKeyboardReady(boolean ready);
    private android.app.AlertDialog keyboardDialog;
    private long keyboardId;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final HandlerThread surfaceThread=new HandlerThread("game-surface");
    private Handler surfaceWorker;
    private boolean nativeLaunched, destroyed;
    private volatile boolean inputReady;
    private volatile int surfaceGeneration;
    private SurfaceView game;
    private TextView status,fps;
    private final GameDisplayMetrics.FpsSampler fpsSampler=new GameDisplayMetrics.FpsSampler();
    private double displayedFps=Double.NaN;
    private RendererSettingsDialog rendererSettings;
    private int appliedRendererFlags=RendererOptions.DEFAULT_FLAGS;
    private float appliedRenderScale=RenderQuality.ORIGINAL;
    private TouchControlsView touchControls;
    private final java.util.ArrayList<View> menuControls=new java.util.ArrayList<>();
    private boolean prepared, configured, surfaceReady, launched, resumed, failed;
    private int width,height;
    private long actionButtons;
    private final BackButtonState backButtons=new BackButtonState();
    private android.hardware.input.InputManager inputManager;
    private final android.hardware.input.InputManager.InputDeviceListener deviceListener=new android.hardware.input.InputManager.InputDeviceListener(){
        @Override public void onInputDeviceAdded(int id){}
        @Override public void onInputDeviceChanged(int id){}
        @Override public void onInputDeviceRemoved(int id){if(backButtons.removeDevice(id)&&configured)nativeControllerCancel(canSendBack()?backButtons.buttons():0);}
    };
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        inputManager=(android.hardware.input.InputManager)getSystemService(INPUT_SERVICE);
        if(inputManager!=null)inputManager.registerInputDeviceListener(deviceListener,ui);
        surfaceThread.start();surfaceWorker=new Handler(surfaceThread.getLooper());
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        FrameLayout root=new FrameLayout(this);root.setBackgroundColor(0xff0d0d0f);
        GameViewport viewport=new GameViewport(this);
        root.addView(viewport,new FrameLayout.LayoutParams(-1,-1,Gravity.CENTER));
        game=new SurfaceView(this);game.setContentDescription("游戏画面");game.getHolder().addCallback(this);
        game.setOnTouchListener((view,event) -> {
            if(!configured||!surfaceReady||!inputReady||width<=0||height<=0)return true;
            int action=event.getActionMasked(),skip=action==MotionEvent.ACTION_POINTER_UP?event.getActionIndex():-1;
            int count=(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL)?0:event.getPointerCount()-(skip>=0?1:0);
            if(count>16){nativeTouch(new int[0],new float[0],new float[0],true);return true;}
            int[] ids=new int[count];float[] xs=new float[count],ys=new float[count];int k=0;
            for(int i=0;i<event.getPointerCount()&&k<count;i++)if(i!=skip){
                ids[k]=event.getPointerId(i);xs[k]=Math.max(0,Math.min(0.999999f,event.getX(i)/width));
                ys[k]=Math.max(0,Math.min(0.999999f,event.getY(i)/height));k++;
            }
            int rc=nativeTouch(ids,xs,ys,action==MotionEvent.ACTION_CANCEL);
            if(rc!=0)fail("触摸输入不可用",new Exception("错误 "+rc));return true;
        });
        viewport.addView(game,new FrameLayout.LayoutParams(-1,-1));
        addAction(viewport,"确认",1,Gravity.RIGHT|Gravity.BOTTOM,18,18);
        addAction(viewport,"返回",2,Gravity.RIGHT|Gravity.BOTTOM,104,18);
        addAction(viewport,"开始",1L<<10,Gravity.RIGHT|Gravity.TOP,18,18);
        addAction(viewport,"↑",1L<<13,Gravity.LEFT|Gravity.BOTTOM,92,92);
        addAction(viewport,"↓",1L<<15,Gravity.LEFT|Gravity.BOTTOM,92,18);
        addAction(viewport,"←",1L<<12,Gravity.LEFT|Gravity.BOTTOM,18,55);
        addAction(viewport,"→",1L<<14,Gravity.LEFT|Gravity.BOTTOM,166,55);
        touchControls=new TouchControlsView(this,() -> inputReady&&resumed&&!failed);
        viewport.addView(touchControls,new FrameLayout.LayoutParams(-1,-1));
        TextView mode=new TextView(this);mode.setText("战斗操控");mode.setTextColor(0xffe9d9ba);mode.setTextSize(16);
        mode.setGravity(Gravity.CENTER);mode.setBackgroundColor(0x8030241b);mode.setContentDescription("切换到战斗触摸操控");mode.setClickable(true);mode.setFocusable(true);
        FrameLayout.LayoutParams modeParams=new FrameLayout.LayoutParams(dp(100),dp(48),Gravity.LEFT|Gravity.TOP);modeParams.leftMargin=dp(18);modeParams.topMargin=dp(18);
        mode.setOnClickListener(v -> {boolean combat=touchControls.getVisibility()!=View.VISIBLE;actionButtons=0;if(inputReady)nativeActions(0,0,0,0,0);
            touchControls.setActive(combat);for(View control:menuControls)control.setVisibility(combat?View.GONE:View.VISIBLE);
            mode.setText(combat?"菜单操控":"战斗操控");mode.setContentDescription(combat?"切换到菜单操控":"切换到战斗触摸操控");});
        viewport.addView(mode,modeParams);
        Button system=new Button(this);system.setText("系统");system.setContentDescription("系统菜单");system.setTextColor(TouchControlsView.TEXT);system.setTextSize(16);
        system.setBackgroundTintList(ColorStateList.valueOf(TouchControlsView.INK));
        FrameLayout.LayoutParams systemParams=new FrameLayout.LayoutParams(dp(80),dp(48),Gravity.LEFT|Gravity.TOP);
        systemParams.leftMargin=dp(134);systemParams.topMargin=dp(18);
        system.setOnClickListener(v->{if(!configured||failed||rendererSettings!=null||keyboardDialog!=null)return;
            actionButtons=0;if(inputReady)nativeActions(0,0,0,0,0);touchControls.cancel();
            rendererSettings=RendererSettingsDialog.show(this,appliedRendererFlags,appliedRenderScale,GameActivity::nativeRenderScale,system,()->rendererSettings=null);});
        viewport.addView(system,systemParams);
        fps=new TextView(this);fps.setText("FPS —");fps.setTextColor(TouchControlsView.TEXT);
        fps.setTextSize(12);fps.setTypeface(Typeface.MONOSPACE);fps.setGravity(Gravity.CENTER);
        fps.setMinWidth(dp(84));fps.setPadding(dp(8),dp(4),dp(8),dp(4));fps.setBackgroundColor(TouchControlsView.INK);
        fps.setClickable(false);fps.setFocusable(false);
        FrameLayout.LayoutParams fpsParams=new FrameLayout.LayoutParams(-2,-2,Gravity.TOP|Gravity.CENTER_HORIZONTAL);
        fpsParams.topMargin=dp(10);viewport.addView(fps,fpsParams);
        status=new TextView(this);status.setTextColor(0xffe9d9ba);status.setTextSize(18);
        status.setGravity(Gravity.CENTER);status.setBackgroundColor(0xe60d0d0f);
        status.setPadding(dp(28),dp(18),dp(28),dp(18));status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        status.setText("正在准备游戏文件");root.addView(status,new FrameLayout.LayoutParams(-1,-1));setContentView(root);
        new Thread(() -> {
            try { prepareModules();GraphicsDriverSettings.configure(this);ui.post(() -> {prepared=true;configureAndStart();}); }
            catch(Exception e){ui.post(() -> fail("游戏文件准备失败",e));}
        },"game-files").start();
    }
    private int dp(int value){return Math.round(value*getResources().getDisplayMetrics().density);}
    private void addAction(FrameLayout root,String label,long mask,int gravity,int horizontal,int vertical){
        TextView button=new TextView(this);button.setText(label);button.setContentDescription(label);button.setTextColor(0xffeadfcd);
        button.setGravity(Gravity.CENTER);button.setTextSize(17);button.setBackgroundColor(0x70443930);
        FrameLayout.LayoutParams p=new FrameLayout.LayoutParams(dp(68),dp(58),gravity);
        if((gravity&Gravity.RIGHT)==Gravity.RIGHT)p.rightMargin=dp(horizontal);else p.leftMargin=dp(horizontal);
        if((gravity&Gravity.BOTTOM)==Gravity.BOTTOM)p.bottomMargin=dp(vertical);else p.topMargin=dp(vertical);
        button.setOnTouchListener((v,e)->{if(!configured||!surfaceReady||!inputReady)return true;
            int a=e.getActionMasked();if(a==MotionEvent.ACTION_DOWN)actionButtons|=mask;
            else if(a==MotionEvent.ACTION_UP||a==MotionEvent.ACTION_CANCEL)actionButtons&=~mask;
            int rc=nativeActions(actionButtons,0,0,0,0);if(rc!=0)fail("操作输入不可用",new Exception("错误 "+rc));return true;});
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
        JSONArray list=new JSONObject(json).getJSONArray("files");File directory=new File(getFilesDir(),"d2r-mobile/modules");
        if(!directory.isDirectory()&&!directory.mkdirs())throw new java.io.IOException("不能创建游戏目录");
        for(int i=0;i<list.length();i++){
            JSONObject entry=list.getJSONObject(i);String name=entry.getString("name"),hash=entry.getString("sha256");
            if(!name.matches("(main|sdk|subsdk0)\\.(patched\\.bin|fixups\\.tsv|gates\\.tsv)"))throw new java.io.IOException("模块清单无效");
            File target=new File(directory,name);
            if(target.isFile()&&target.length()==entry.getLong("bytes")&&digest(target).equals(hash))continue;
            File temporary=new File(directory,name+".new");
            try(InputStream in=getAssets().open("modules/"+name);FileOutputStream out=new FileOutputStream(temporary)){
                byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)out.write(b,0,n);out.getFD().sync();
            }
            if(temporary.length()!=entry.getLong("bytes")||!digest(temporary).equals(hash))throw new java.io.IOException("游戏模块校验失败");
            if(!temporary.renameTo(target))throw new java.io.IOException("不能保存游戏模块");
        }
        if(!new File(getFilesDir(),"game.romfs").isFile())throw new java.io.IOException("缺少原游戏资源");
        if(!new File(getFilesDir(),"d2r-mobile/shaders/shader_index.bin").isFile())throw new java.io.IOException("缺少游戏图形缓存");
    }
    private void configureAndStart(){
        if(!prepared||failed||destroyed)return;
        if(!configured){
            // Recover an interrupted AtomicFile write before native startup reads it.
            try{new RenderQualityStore(this).read();}catch(java.io.IOException e){fail("无法读取画质设置",e);return;}
            int rc=nativeConfigure(getFilesDir().getAbsolutePath(),getResources().getConfiguration().getLocales().get(0).toLanguageTag(),
            getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_TOUCHSCREEN));
            if(rc!=0){fail("游戏初始化失败",new Exception("错误 "+rc));return;}configured=true;}
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
            if(rc!=0){final int error=rc;ui.post(() -> {if(!destroyed&&generation==surfaceGeneration)fail("游戏窗口不可用",new Exception("错误 "+error));});return;}
            inputReady=start;
            ui.post(() -> {if(!destroyed)touchControls.postInvalidateOnAnimation();});
            if(start&&!nativeLaunched){rc=nativeStart();
                if(rc!=0){final int error=rc;ui.post(() -> {if(!destroyed)fail("游戏启动失败",new Exception("错误 "+error));});return;}
                nativeLaunched=true;ui.post(() -> {if(!destroyed&&!launched){launched=true;ui.post(poll);}});
            }
        });
    }
    private void closeRendererSettings(){if(rendererSettings!=null){rendererSettings.dismiss();rendererSettings=null;}}
    static void installDialogBack(android.app.Dialog dialog){
        dialog.setOnKeyListener((d,key,event)->{
            if(key!=KeyEvent.KEYCODE_BUTTON_B)return false;
            if(event.getAction()==KeyEvent.ACTION_UP&&!event.isCanceled())dialog.dismiss();
            return true;
        });
    }
    private boolean canSendBack(){return configured&&resumed&&inputReady&&!failed&&!destroyed;}
    private void clearBackButtons(){backButtons.clear();if(configured)nativeControllerCancel(0);}
    private void publishBackButtons(){
        if(configured){int rc=nativeControllerButtons(canSendBack()?backButtons.buttons():0);
            if(rc!=0)android.util.Log.w("D2RInput","Back button publication unavailable: "+rc);}
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        int key=event.getKeyCode();
        if(!BackButtonState.handles(key))return super.dispatchKeyEvent(event);
        if(rendererSettings!=null||keyboardDialog!=null){
            if(event.getAction()==KeyEvent.ACTION_UP&&!event.isCanceled())requestGameBack();
            return true;
        }
        if(event.getAction()==KeyEvent.ACTION_DOWN||event.getAction()==KeyEvent.ACTION_UP){
            if(event.isCanceled()){
                if(backButtons.cancel(event.getDeviceId(),key)&&configured)nativeControllerCancel(canSendBack()?backButtons.buttons():0);
                return true;
            }
            boolean down=event.getAction()==KeyEvent.ACTION_DOWN&&canSendBack();
            backButtons.update(event.getDeviceId(),key,down);
            publishBackButtons();
        }
        return true;
    }
    private void requestGameBack(){
        if(rendererSettings!=null){closeRendererSettings();return;}
        if(keyboardDialog!=null){keyboardDialog.dismiss();return;}
        if(canSendBack()){
            // Native latches a gesture shorter than one guest input sample;
            // releasing the gesture preserves a physically held B button.
            nativeControllerButtons(2);nativeControllerButtons(backButtons.buttons());
        }
    }
    // The target-33 shell uses legacy Back dispatch (including system gestures).
    @Override public void onBackPressed(){requestGameBack();}
    @Override public void onWindowFocusChanged(boolean focused){
        super.onWindowFocusChanged(focused);
        if(!focused)clearBackButtons();
    }
    private void fail(String title,Exception error){closeRendererSettings();nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}failed=true;status.setVisibility(View.VISIBLE);status.setText(title+"\n"+error.getMessage());}
    private final Runnable poll=new Runnable(){public void run(){
        if(!launched||failed)return;
        try{String raw=nativeStatus();JSONObject s=new JSONObject(raw);
            appliedRendererFlags=s.optInt("renderer_flags",RendererOptions.DEFAULT_FLAGS);
            appliedRenderScale=(float)s.optDouble("render_scale",RenderQuality.ORIGINAL);
            if(rendererSettings!=null)rendererSettings.updateApplied(appliedRendererFlags,appliedRenderScale);
            long frame=s.getLong("presented_frames");
            if(frame>0)status.setVisibility(View.GONE);else status.setText(s.getString("phase"));
            if(resumed){double current=fpsSampler.sample(frame,SystemClock.elapsedRealtime());
                if(!Double.isNaN(current)&&Double.compare(current,displayedFps)!=0){
                    fps.setText(String.format(getResources().getConfiguration().getLocales().get(0),"FPS %.1f",current));
                    displayedFps=current;
                }}
            String request=nativeKeyboardRequest();if(request!=null&&resumed&&!destroyed){JSONObject k=new JSONObject(request);long id=k.getLong("id");
                if(keyboardDialog==null||keyboardId!=id){closeRendererSettings();if(keyboardDialog!=null)keyboardDialog.dismiss();keyboardId=id;actionButtons=0;nativeActions(0,0,0,0,0);touchControls.cancel();
                    keyboardDialog=ShellTextInputDialog.show(GameActivity.this,k.getString("title"),k.getString("subtitle"),k.getString("hint"),k.getString("initial"),k.getInt("min"),k.getInt("max"),(ok,text)->{nativeKeyboardResult(id,ok,text);keyboardDialog=null;});}}
        }catch(Exception e){fail("无法读取游戏状态",e);}ui.postDelayed(this,500);
    }};
    @Override public void onResume(){super.onResume();resumed=true;fpsSampler.reset();nativeKeyboardReady(true);configureAndStart();}
    @Override public void onPause(){clearBackButtons();closeRendererSettings();nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}resumed=false;fpsSampler.reset();displayedFps=Double.NaN;if(fps!=null)fps.setText("FPS —");inputReady=false;actionButtons=0;if(touchControls!=null)touchControls.cancel();if(configured)nativeActions(0,0,0,0,0);if(configured&&surfaceReady)publishSurface();super.onPause();}
    @Override public void surfaceCreated(SurfaceHolder holder){if(!destroyed)surfaceReady=true;}
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int w,int h){if(destroyed)return;width=w;height=h;surfaceReady=true;configureAndStart();}
    @Override public void surfaceDestroyed(SurfaceHolder holder){if(destroyed)return;surfaceReady=false;if(configured)publishSurface();}
    @Override public void onDestroy(){if(inputManager!=null)inputManager.unregisterInputDeviceListener(deviceListener);clearBackButtons();closeRendererSettings();nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}destroyed=true;inputReady=false;surfaceReady=false;if(configured)publishSurface();ui.removeCallbacks(poll);surfaceThread.quitSafely();super.onDestroy();}
}
