package org.d2r.binaryprobe;

/** The main renderer defaults to the system driver; bundled drivers remain
 * explicit launcher choices. Shared with the optional offline installer. */
final class GraphicsDriverPolicy {
    static final String AUTO="auto", SYSTEM="system", TURNIP="turnip";
    private GraphicsDriverPolicy() {}
    static String normalize(String mode){return SYSTEM.equals(mode)||TURNIP.equals(mode)?mode:AUTO;}
    static boolean useBundledTurnip(int sdk,String socModel){return false;}
    static boolean useBundledTurnip(String mode,int sdk,String socModel){return TURNIP.equals(normalize(mode));}
}
