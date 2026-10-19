package org.d2r.binaryprobe;

/** Auto uses the bundled Turnip driver only on SoCs where it has been verified to render correctly; every other
 * device keeps the system driver. Explicit launcher choices always win. Shared with the optional offline installer.
 * Adreno 642L (SM7325): the system driver draws blocky white fog and shadow artifacts that Turnip does not. */
final class GraphicsDriverPolicy {
    static final String AUTO="auto", SYSTEM="system", TURNIP="turnip";
    private static final String[] VERIFIED_TURNIP_SOCS={"SM7325"};
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
