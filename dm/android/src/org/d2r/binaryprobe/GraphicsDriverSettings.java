package org.d2r.binaryprobe;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.system.Os;
import java.io.File;
import java.io.IOException;

/** Shared owner of the launcher preference and pre-Vulkan driver selection. */
final class GraphicsDriverSettings {
    private static final String PREFS="graphics-driver", KEY="selection", AUTO_FAILED="auto_turnip_failed", AUTO_STRIKES="auto_turnip_strikes";
    static String read(Context context) {
        return GraphicsDriverPolicy.normalize(context.getSharedPreferences(PREFS,Context.MODE_PRIVATE).getString(KEY,GraphicsDriverPolicy.AUTO));
    }
    static boolean save(Context context,String mode) {
        // Choosing a mode again (including auto) clears an earlier automatic fallback so the recommendation is retried.
        return context.getSharedPreferences(PREFS,Context.MODE_PRIVATE).edit().putString(KEY,GraphicsDriverPolicy.normalize(mode))
            .putBoolean(AUTO_FAILED,false).commit();
    }
    private static File pendingMarker(Context context){return new File(context.getFilesDir(),"d2r-mobile/driver-auto-pending");}
    /** Called once the game presented a frame or the activity ended normally; a leftover marker means a crash. */
    static void confirmStarted(Context context){
        pendingMarker(context).delete();
        SharedPreferences prefs=context.getSharedPreferences(PREFS,Context.MODE_PRIVATE);
        if(prefs.getInt(AUTO_STRIKES,0)!=0)prefs.edit().putInt(AUTO_STRIKES,0).apply();
    }
    static boolean recommendedTurnip() {
        return GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.AUTO,Build.VERSION.SDK_INT,Build.VERSION.SDK_INT>=31?Build.SOC_MANUFACTURER:"",
            Build.VERSION.SDK_INT>=31?Build.SOC_MODEL:null);
    }
    static void configure(Context context)throws Exception {
        String mode=read(context);
        boolean auto=GraphicsDriverPolicy.AUTO.equals(mode);
        File diagnostics=new File(context.getFilesDir(),"d2r-mobile/diagnostics");
        // Auto retains explicit ADB diagnostic overrides (private-vulkan markers).
        if(auto&&(new File(diagnostics,"private-vulkan").exists()||new File(diagnostics,"private-vulkan-sysmem").exists()))return;
        // An explicit launcher choice supersedes old ADB diagnostic overrides.
        if(!auto) {
            for(String name:new String[]{"private-vulkan","private-vulkan-sysmem"}) {
                File marker=new File(diagnostics,name);
                if(marker.exists()&&!marker.delete())throw new IOException("无法应用驱动选择，请重新启动应用后重试");
            }
        }
        boolean turnip=GraphicsDriverPolicy.useBundledTurnip(mode,Build.VERSION.SDK_INT,Build.VERSION.SDK_INT>=31?Build.SOC_MANUFACTURER:"",
            Build.VERSION.SDK_INT>=31?Build.SOC_MODEL:null);
        String reason="";
        if(auto&&turnip) {
            // The recommended driver must never trap the user: a previous automatic start that never reached a frame
            // (marker still present) means it crashed, so fall back to the system driver until the user chooses again.
            SharedPreferences prefs=context.getSharedPreferences(PREFS,Context.MODE_PRIVATE);
            File pending=pendingMarker(context);
            // Two consecutive starts that never reached a frame count as a crash; one may be a user swiping the app away.
            if(pending.exists()){
                int strikes=prefs.getInt(AUTO_STRIKES,0)+1;
                prefs.edit().putInt(AUTO_STRIKES,strikes).putBoolean(AUTO_FAILED,strikes>=2).commit();pending.delete();
            }
            if(prefs.getBoolean(AUTO_FAILED,false)){turnip=false;reason=" auto_fallback=1";}
            else{File dir=pending.getParentFile();if(dir!=null)dir.mkdirs();if(!pending.createNewFile()&&!pending.exists())throw new IOException("无法记录驱动状态");}
        }
        if(turnip){
            Os.setenv("DM_PRIVATE_VULKAN","turnip",true);
            // The bundled Turnip build is validated on the sysmem path. It
            // avoids the tiled-cache transition stalls seen on Adreno 642L.
            Os.setenv("TU_DEBUG","sysmem",true);
        }else{
            Os.unsetenv("DM_PRIVATE_VULKAN");
            Os.unsetenv("TU_DEBUG");
        }
        android.util.Log.i("D2RDriver","selection="+mode+" resolved="+(turnip?"turnip":"system")+reason);
    }
}
