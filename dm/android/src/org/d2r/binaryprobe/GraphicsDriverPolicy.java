package org.d2r.binaryprobe;

/** Only select the bundled driver on hardware verified with this exact build. */
final class GraphicsDriverPolicy {
    static final String AUTO="auto", SYSTEM="system", TURNIP="turnip";
    private GraphicsDriverPolicy() {}

    static boolean useBundledTurnip(int sdk, String socModel) {
        return useBundledTurnip(AUTO,sdk,socModel);
    }

    static String normalize(String mode) {
        return SYSTEM.equals(mode)||TURNIP.equals(mode)?mode:AUTO;
    }

    static boolean useBundledTurnip(String mode,int sdk,String socModel) {
        mode=normalize(mode);
        if(SYSTEM.equals(mode))return false;
        if(TURNIP.equals(mode))return true;
        // Build.SOC_MODEL is public from API 31. On SM7325 (Adreno 642L),
        // the system driver fails vkCreateComputePipelines; bundled Turnip
        // passes the same startup and renders the game on TNA-AN00.
        return sdk >= 31 && "SM7325".equals(socModel);
    }
}
