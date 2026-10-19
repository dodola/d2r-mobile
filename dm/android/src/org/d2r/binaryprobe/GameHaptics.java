package org.d2r.binaryprobe;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.database.ContentObserver;
import android.media.AudioAttributes;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationAttributes;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.provider.Settings;
import android.view.View;
import android.view.ViewTreeObserver;

/** Consumes only the shared drop-haptic queue; this is not nn::hid rumble. */
final class GameHaptics implements Application.ActivityLifecycleCallbacks,
        ViewTreeObserver.OnWindowFocusChangeListener {
    private static native int nativePoll(boolean active);
    private final Activity owner;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final View decor;
    private final Vibrator vibrator;
    private final GameHapticState state;
    private boolean resumed,destroyed,systemEnabled=true,nativeAvailable=true;
    private final ContentObserver settings=new ContentObserver(handler){
        @Override public void onChange(boolean self){readSystemSetting();}
    };
    private final Runnable frame=new Runnable(){@Override public void run(){
        if(destroyed||!resumed)return;
        state.tick(systemEnabled);handler.postDelayed(this,16);
    }};

    GameHaptics(Activity activity){
        owner=activity;decor=activity.getWindow().getDecorView();
        vibrator=Build.VERSION.SDK_INT>=31
            ? defaultVibrator(activity)
            : (Vibrator)activity.getSystemService(Context.VIBRATOR_SERVICE);
        state=new GameHapticState(this::take,new GameHapticState.Output(){
            @Override public void play(int tier){vibrate(tier);}
            @Override public void cancel(){cancelVibration();}
        });
        readSystemSetting();
        owner.getContentResolver().registerContentObserver(Settings.System.getUriFor(
            Settings.System.HAPTIC_FEEDBACK_ENABLED),false,settings);
        decor.getViewTreeObserver().addOnWindowFocusChangeListener(this);
        owner.getApplication().registerActivityLifecycleCallbacks(this);
    }
    private static Vibrator defaultVibrator(Context context){
        VibratorManager manager=(VibratorManager)context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
        return manager==null?null:manager.getDefaultVibrator();
    }
    private int take(boolean active){
        if(!nativeAvailable)return -1;
        try{return nativePoll(active);}catch(UnsatisfiedLinkError unavailable){nativeAvailable=false;return -1;}
    }
    private void readSystemSetting(){
        systemEnabled=Settings.System.getInt(owner.getContentResolver(),
            Settings.System.HAPTIC_FEEDBACK_ENABLED,1)!=0;
        // Flush at either setting edge; enabling must not replay a suppressed drop.
        if(state!=null)state.tick(false);
    }
    private void vibrate(int tier){
        if(vibrator==null||!vibrator.hasVibrator())return;
        int predefined=tier==1?VibrationEffect.EFFECT_HEAVY_CLICK
            :tier==2?VibrationEffect.EFFECT_CLICK:VibrationEffect.EFFECT_TICK;
        VibrationEffect effect;
        if(Build.VERSION.SDK_INT>=30&&vibrator.areEffectsSupported(predefined)[0]==Vibrator.VIBRATION_EFFECT_SUPPORT_YES)
            effect=VibrationEffect.createPredefined(predefined);
        else effect=VibrationEffect.createOneShot(tier==1?45:tier==2?28:14,
            vibrator.hasAmplitudeControl()?(tier==1?255:tier==2?160:80):VibrationEffect.DEFAULT_AMPLITUDE);
        // Touch feedback attributes respect system suppression. Never request
        // FLAG_BYPASS_* or IGNORE_GLOBAL_SETTING, even for a valuable drop.
        try{
            if(Build.VERSION.SDK_INT>=33)vibrator.vibrate(effect,new VibrationAttributes.Builder()
                .setUsage(VibrationAttributes.USAGE_TOUCH).build());
            else vibrator.vibrate(effect,new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build());
        }catch(SecurityException unavailable){android.util.Log.w("D2RHaptics","Vibration permission unavailable");}
    }
    private void cancelVibration(){
        if(vibrator!=null)try{vibrator.cancel();}catch(SecurityException unavailable){/* No active output. */}
    }
    private void foreground(){state.active(resumed&&!destroyed&&decor.hasWindowFocus());}
    @Override public void onWindowFocusChanged(boolean focus){foreground();}
    @Override public void onActivityResumed(Activity activity){if(activity==owner){
        resumed=true;readSystemSetting();foreground();handler.removeCallbacks(frame);handler.post(frame);}}
    @Override public void onActivityPaused(Activity activity){if(activity==owner){
        resumed=false;handler.removeCallbacks(frame);state.stop();}}
    @Override public void onActivityDestroyed(Activity activity){if(activity==owner){
        destroyed=true;resumed=false;handler.removeCallbacks(frame);state.stop();
        owner.getContentResolver().unregisterContentObserver(settings);
        if(decor.getViewTreeObserver().isAlive())decor.getViewTreeObserver().removeOnWindowFocusChangeListener(this);
        owner.getApplication().unregisterActivityLifecycleCallbacks(this);}}
    @Override public void onActivityCreated(Activity activity,Bundle state){}
    @Override public void onActivityStarted(Activity activity){}
    @Override public void onActivityStopped(Activity activity){}
    @Override public void onActivitySaveInstanceState(Activity activity,Bundle state){}
}
