package org.d2r.binaryprobe;

import android.content.Context;
import android.os.Build;
import android.system.Os;
import java.io.File;
import java.io.IOException;

/** Shared owner of the launcher preference and pre-Vulkan driver selection. */
final class GraphicsDriverSettings {
    private static final String PREFS="graphics-driver", KEY="selection";
    static String read(Context context) {
        return GraphicsDriverPolicy.normalize(context.getSharedPreferences(PREFS,Context.MODE_PRIVATE).getString(KEY,GraphicsDriverPolicy.AUTO));
    }
    static boolean save(Context context,String mode) {
        return context.getSharedPreferences(PREFS,Context.MODE_PRIVATE).edit().putString(KEY,GraphicsDriverPolicy.normalize(mode)).commit();
    }
    static boolean recommendedTurnip() {
        return GraphicsDriverPolicy.useBundledTurnip(Build.VERSION.SDK_INT,Build.VERSION.SDK_INT>=31?Build.SOC_MODEL:"");
    }
    static void configure(Context context)throws Exception {
        String mode=read(context);
        // An explicit launcher choice supersedes old ADB diagnostic overrides.
        // Auto retains the established diagnostic workflow on other hardware.
        if(!GraphicsDriverPolicy.AUTO.equals(mode)) {
            File diagnostics=new File(context.getFilesDir(),"d2r-mobile/diagnostics");
            for(String name:new String[]{"private-vulkan","private-vulkan-sysmem"}) {
                File marker=new File(diagnostics,name);
                if(marker.exists()&&!marker.delete())throw new IOException("无法应用驱动选择，请重新启动应用后重试");
            }
        }
        boolean turnip=GraphicsDriverPolicy.useBundledTurnip(mode,Build.VERSION.SDK_INT,Build.VERSION.SDK_INT>=31?Build.SOC_MODEL:"");
        if(turnip)Os.setenv("DM_PRIVATE_VULKAN","turnip",true);
        else Os.unsetenv("DM_PRIVATE_VULKAN");
        android.util.Log.i("D2RDriver","selection="+mode+" resolved="+(turnip?"turnip":"system"));
    }
}
