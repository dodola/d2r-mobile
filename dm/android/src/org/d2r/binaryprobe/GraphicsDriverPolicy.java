package org.d2r.binaryprobe;

/** Auto uses the bundled Turnip driver (sysmem path) on Qualcomm Adreno devices (Android 12+, SOC_MANUFACTURER "QTI");
 * every other device keeps the system driver, since Turnip only drives Adreno. Explicit launcher choices always win,
 * and GraphicsDriverSettings falls back to the system driver after two automatic starts that never reached a frame.
 * Measured on device (character select, 100% scale): SM7325 / Adreno 642L 12.4 FPS Turnip vs 11.6 system, and
 * SM8750 / Adreno 830 about 20 FPS Turnip vs 10-13 system. VERIFIED_TURNIP_SOCS lists the models actually measured;
 * other Qualcomm models get the same default and rely on the fallback. */
final class GraphicsDriverPolicy {
    static final String AUTO="auto", SYSTEM="system", TURNIP="turnip";
    static final String[] VERIFIED_TURNIP_SOCS={"SM7325","SM8750"};
    private static final String QUALCOMM="QTI";
    private GraphicsDriverPolicy() {}
    static String normalize(String mode){return SYSTEM.equals(mode)||TURNIP.equals(mode)?mode:AUTO;}
    /** SOC_MANUFACTURER and SOC_MODEL exist from API 31; before that the SoC cannot be identified. */
    static boolean useBundledTurnip(int sdk,String socManufacturer){
        return sdk>=31&&QUALCOMM.equals(socManufacturer);
    }
    static boolean verifiedSoc(String socModel){
        if(socModel==null)return false;
        for(String soc:VERIFIED_TURNIP_SOCS)if(soc.equals(socModel))return true;
        return false;
    }
    static boolean useBundledTurnip(String mode,int sdk,String socManufacturer){
        String m=normalize(mode);
        if(TURNIP.equals(m))return true;
        if(SYSTEM.equals(m))return false;
        return useBundledTurnip(sdk,socManufacturer);
    }
}
