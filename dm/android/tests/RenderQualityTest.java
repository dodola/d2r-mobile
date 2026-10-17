package org.d2r.binaryprobe;

public final class RenderQualityTest {
    private static int checks;
    private static void check(boolean ok){checks++;if(!ok)throw new AssertionError("check "+checks);}
    private static void invalid(String text){try{RenderQuality.parse(text);throw new AssertionError("accepted "+text);}catch(IllegalArgumentException expected){checks++;}}
    public static void main(String[] args){
        // Existing diagnostic files remain readable; new modes round-trip as
        // the exact scale consumed by native startup and runtime requests.
        check(RenderQuality.parse("1.0\n")==1.0f);
        check(RenderQuality.parse("0.75\r\n")==0.75f);
        check(RenderQuality.parse(" \t7.0e-1\r\n")==0.70f);
        check(RenderQuality.parse(".72")==0.72f);
        check(RenderQuality.parse("+1.")==1.0f);
        for(float scale:new float[]{RenderQuality.ORIGINAL,RenderQuality.PERFORMANCE,0.70f,0.72f})
            check(RenderQuality.parse(RenderQuality.encode(scale))==scale);
        for(String bad:new String[]{""," ","NaN","Infinity","-Infinity","0.69","1.01","0.75 extra","0.75\n1.0","0.75f","0x1.8p-1","1e999","1e-999","1e","\u00000.75","0.75\u0000",new String(new char[64]).replace('\u0000','1')})invalid(bad);
        check(!RenderQuality.description(1.0f).equals(RenderQuality.description(0.75f)));
        System.out.println("RENDER_QUALITY checks="+checks+" legacy_format=1 persisted_modes=1 invalid_input=1 passed=1");
    }
}
