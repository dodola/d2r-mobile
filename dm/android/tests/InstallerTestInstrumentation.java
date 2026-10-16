package org.d2r.binaryprobe;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewGroup;
import android.widget.RadioButton;
import java.io.*;
import java.nio.file.Files;

/** Run the shared importer failure/recovery tests on Android's real filesystem/runtime. */
public final class InstallerTestInstrumentation extends Instrumentation {
    private static RadioButton radio(View view,String tag) {
        if(view instanceof RadioButton&&tag.equals(view.getTag()))return (RadioButton)view;
        if(view instanceof ViewGroup)for(int i=0;i<((ViewGroup)view).getChildCount();i++) {
            RadioButton found=radio(((ViewGroup)view).getChildAt(i),tag);if(found!=null)return found;
        }
        return null;
    }
    private void verifyDriverEntry(Intent launcher)throws Exception {
        Activity entry=startActivitySync(launcher);waitForIdleSync();
        runOnMainSync(()->{RadioButton option=radio(entry.getWindow().getDecorView(),"system");
            if(option==null)throw new AssertionError("Driver choices missing");option.performClick();});
        if(!"system".equals(getTargetContext().getSharedPreferences("graphics-driver",0).getString("selection","")))throw new AssertionError("Choice not saved");
        runOnMainSync(entry::finish);waitForIdleSync();
        Activity reopened=startActivitySync(launcher);waitForIdleSync();
        runOnMainSync(()->{if(!radio(reopened.getWindow().getDecorView(),"system").isChecked())throw new AssertionError("Choice not restored");});
        Class<?> settings=getTargetContext().getClassLoader().loadClass("org.d2r.binaryprobe.GraphicsDriverSettings");
        java.lang.reflect.Method configure=settings.getDeclaredMethod("configure",android.content.Context.class);configure.setAccessible(true);
        File diagnostics=new File(getTargetContext().getFilesDir(),"d2r-mobile/diagnostics");diagnostics.mkdirs();
        File marker=new File(diagnostics,"private-vulkan");marker.createNewFile();
        configure.invoke(null,getTargetContext());
        if(marker.exists()||android.system.Os.getenv("DM_PRIVATE_VULKAN")!=null)throw new AssertionError("System choice ignored");
        runOnMainSync(()->radio(reopened.getWindow().getDecorView(),"turnip").performClick());
        configure.invoke(null,getTargetContext());
        if(!"turnip".equals(android.system.Os.getenv("DM_PRIVATE_VULKAN")))throw new AssertionError("Bundled choice ignored");
        runOnMainSync(()->{radio(reopened.getWindow().getDecorView(),"auto").performClick();reopened.finish();});waitForIdleSync();
    }
    @Override public void onCreate(Bundle arguments){super.onCreate(arguments);start();}
    @Override public void onStart(){
        Bundle result=new Bundle();
        try {
            File root=Files.createTempDirectory(getTargetContext().getCacheDir().toPath(),"offline-tests-").toFile();
            String tests=OfflinePackageInstallerTest.runAll(root);
            PackageManager pm=getTargetContext().getPackageManager();String name=getTargetContext().getPackageName();
            Intent launcher=pm.getLaunchIntentForPackage(name);
            if(launcher==null||!launcher.getComponent().getClassName().equals("org.d2r.binaryprobe.InstallerActivity"))throw new AssertionError("Wrong launcher");
            if(pm.getPackageInfo(name,PackageManager.GET_PERMISSIONS).requestedPermissions!=null)throw new AssertionError("Unexpected permission");
            Drawable icon=pm.getApplicationIcon(name);
            if(!getTargetContext().getResources().getResourceName(getTargetContext().getApplicationInfo().icon).endsWith(":mipmap/ic_launcher"))throw new AssertionError("Wrong launcher resource");
            Bitmap image=Bitmap.createBitmap(512,512,Bitmap.Config.ARGB_8888);icon.setBounds(0,0,512,512);icon.draw(new Canvas(image));
            try(FileOutputStream out=new FileOutputStream(new File(getTargetContext().getCacheDir(),"qa-launcher-icon.png"))){image.compress(Bitmap.CompressFormat.PNG,100,out);}
            // OEM theme engines may flatten adaptive icons; verify the actual visible artwork.
            int warm=0;for(int y=0;y<512;y++)for(int x=0;x<512;x++){int c=image.getPixel(x,y),r=(c>>16)&255,g=(c>>8)&255,b=c&255;if(r>100&&r>g*1.15&&r>b*1.25)warm++;}
            if(warm<256)throw new AssertionError("Launcher artwork is blank or generic; warm pixels="+warm);
            verifyDriverEntry(launcher);
            result.putString("stream",tests+" launcher=InstallerActivity launcher_artwork=1 platform_icon="+icon.getClass().getSimpleName()+" permissions=0 driver_entry=1 preference_reopen=1 explicit_driver_env=1");finish(Activity.RESULT_OK,result);
        }catch(Throwable error){StringWriter text=new StringWriter();error.printStackTrace(new PrintWriter(text));result.putString("stream",text.toString());finish(Activity.RESULT_CANCELED,result);}
    }
}
