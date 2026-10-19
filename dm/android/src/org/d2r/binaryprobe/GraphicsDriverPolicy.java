package org.d2r.binaryprobe;

/** Auto uses the bundled Turnip driver only on SoCs listed in VERIFIED_TURNIP_SOCS; every other device keeps the
 * system driver. Explicit launcher choices always win. Shared with the optional offline installer.
 * The list is empty: the Adreno 642L (SM7325) fog artifacts that once justified Turnip were a Qualcomm compiler
 * miscompile of dynamic uniform component indexing, now lowered in vertex and fragment shaders, and the system
 * driver is both correct and faster there. */
final class GraphicsDriverPolicy {
    static final String AUTO="auto", SYSTEM="system", TURNIP="turnip";
    private static final String[] VERIFIED_TURNIP_SOCS={};
    private GraphicsDriverPolicy() {}
    static String normalize(String mode){return SYSTEM.equals(mode)||TURNIP.equals(mode)?mode:AUTO;}
    /** SOC_MODEL exists from API 31; before that nothing can be verified. */
    static boolean useBundledTurnip(int sdk,String socModel){
        if(sdk<31||socModel==null)return false;
        for(String soc:VERIFIED_TURNIP_SOCS)if(soc.equals(socModel))return true;
        return false;
    }
    static boolean useBundledTurnip(String mode,int sdk,String socModel){
        String m=normalize(mode);
        if(TURNIP.equals(m))return true;
        if(SYSTEM.equals(m))return false;
        return useBundledTurnip(sdk,socModel);
    }
}
