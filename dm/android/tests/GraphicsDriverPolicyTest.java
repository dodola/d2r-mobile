package org.d2r.binaryprobe;

public final class GraphicsDriverPolicyTest {
    public static void main(String[] args) {
        String[] others={null,"","unknown","SM7325","SM7325-extra","sm7325","SM8750","SM8150","Mali-G610","MT6895"};
        for(String soc:others)if(GraphicsDriverPolicy.useBundledTurnip(34,soc))
            throw new AssertionError("Unverified SoC selected: "+soc);
        if(GraphicsDriverPolicy.useBundledTurnip(30,"SM7325") ||
           GraphicsDriverPolicy.useBundledTurnip(29,"SM7325"))
            throw new AssertionError("SOC_MODEL unavailable before API 31");
        if(GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.SYSTEM,34,"SM7325"))
            throw new AssertionError("Explicit system choice must override recommendation");
        if(!GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.TURNIP,29,"unknown"))
            throw new AssertionError("Explicit bundled choice must override recommendation");
        if(GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.AUTO,34,"SM7325")||
           GraphicsDriverPolicy.useBundledTurnip(GraphicsDriverPolicy.AUTO,34,"SM8750"))
            throw new AssertionError("Auto must keep the system driver while the verified list is empty");
        if(GraphicsDriverPolicy.useBundledTurnip("invalid",34,"SM7325") ||
           GraphicsDriverPolicy.useBundledTurnip("invalid",34,"SM8750") ||
           !GraphicsDriverPolicy.AUTO.equals(GraphicsDriverPolicy.normalize(null)))
            throw new AssertionError("Unknown saved values must recover to auto");
        System.out.println("ANDROID_GRAPHICS_DRIVER system_default=1 explicit_choices=1 api_guard=1 passed=1");
    }
}
