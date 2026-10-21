package org.d2r.binaryprobe;

public final class GraphicsDriverPolicyTest {
    public static void main(String[] args) {
        // Auto: bundled Turnip on Qualcomm (QTI) from Android 12, system driver everywhere else.
        for(String maker:new String[]{"QTI"})
            if(!GraphicsDriverPolicy.useBundledTurnip(31,maker)||!GraphicsDriverPolicy.useBundledTurnip(34,maker))
                throw new AssertionError("Qualcomm must default to the bundled driver: "+maker);
        String[] others={null,"","unknown","qti","Qualcomm","MediaTek","Samsung","HiSilicon","Google","UNISOC"};
        for(String maker:others)if(GraphicsDriverPolicy.useBundledTurnip(34,maker))
            throw new AssertionError("Non-Qualcomm SoC selected: "+maker);
        if(GraphicsDriverPolicy.useBundledTurnip(30,"QTI")||GraphicsDriverPolicy.useBundledTurnip(29,"QTI"))
            throw new AssertionError("SOC_MANUFACTURER unavailable before API 31");
        if(GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.SYSTEM,34,"QTI"))
            throw new AssertionError("Explicit system choice must override recommendation");
        if(!GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.TURNIP,29,"unknown"))
            throw new AssertionError("Explicit bundled choice must override recommendation");
        if(!GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.AUTO,34,"QTI")||
           GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.AUTO,34,"MediaTek"))
            throw new AssertionError("Auto must follow the Qualcomm default");
        // Unknown saved values recover to auto, which keeps the same default.
        if(!GraphicsDriverPolicy.useBundledTurnip("invalid",34,"QTI")||
           GraphicsDriverPolicy.useBundledTurnip("invalid",34,"Samsung")||
           !GraphicsDriverPolicy.AUTO.equals(GraphicsDriverPolicy.normalize(null)))
            throw new AssertionError("Unknown saved values must recover to auto");
        // Models whose Turnip performance was measured on a device.
        if(!GraphicsDriverPolicy.verifiedSoc("SM7325")||!GraphicsDriverPolicy.verifiedSoc("SM8750")||
           GraphicsDriverPolicy.verifiedSoc("SM8150")||GraphicsDriverPolicy.verifiedSoc(null)||GraphicsDriverPolicy.verifiedSoc("sm7325"))
            throw new AssertionError("Verified SoC list");
        // SM8750 presents transposed frames through the bundled Turnip swapchain: automatic keeps the system driver,
        // other Qualcomm models and explicit choices are unchanged.
        if(GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.AUTO,34,"QTI","SM8750")||
           GraphicsDriverPolicy.useBundledTurnip("invalid",37,"QTI","SM8750"))
            throw new AssertionError("Auto must keep the system driver on SM8750");
        if(!GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.TURNIP,37,"QTI","SM8750"))
            throw new AssertionError("Explicit Turnip must still win on SM8750");
        if(!GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.AUTO,34,"QTI","SM7325")||
           !GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.AUTO,34,"QTI",null)||
           GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.AUTO,34,"MediaTek","SM8750"))
            throw new AssertionError("Other models keep the old automatic choice");
        if(!GraphicsDriverPolicy.turnipPresentBug("SM8750")||GraphicsDriverPolicy.turnipPresentBug("SM7325")||GraphicsDriverPolicy.turnipPresentBug(null))
            throw new AssertionError("Present-bug list");
        System.out.println("ANDROID_GRAPHICS_DRIVER qualcomm_default=1 explicit_choices=1 api_guard=1 verified_list=1 present_bug_soc=1 passed=1");
    }
}
