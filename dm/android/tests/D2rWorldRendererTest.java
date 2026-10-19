package org.d2r.binaryprobe;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Paths;
public final class D2rWorldRendererTest {
    private static int checks;
    private static void check(boolean condition){checks++;if(!condition)throw new AssertionError("world renderer check="+checks);}
    private static boolean close(float a,float b){return Math.abs(a-b)<.01;}
    public static void main(String[] args)throws Exception {
        check(D2rWorldRenderer.decode(null)==null);check(D2rWorldRenderer.decode(new byte[1567])==null);
        byte[] bytes=Files.readAllBytes(Paths.get(args[0]));D2rWorldRenderer.Frame frame=D2rWorldRenderer.decode(bytes);check(frame!=null);check(frame.text[0].equals("Rift")&&frame.text[2].equals("5:59"));
        check(frame.w[1]==3&&frame.w[34]==64);D2rWorldRenderer.Goal goal=D2rWorldRenderer.place(frame,0,1280,720);
        check(goal.inside&&goal.symbol==0&&goal.label==-1);check(close(goal.x,704)&&close(goal.y,392)&&close(goal.symbolHeight,61.2f));
        frame.w[34]=1;goal=D2rWorldRenderer.place(frame,0,1280,720);check(goal.inside&&goal.symbol==7&&goal.label==1&&close(goal.labelHeight,16.992f));
        frame.w[32]=10000;frame.w[34]=70;goal=D2rWorldRenderer.place(frame,0,1280,720);check(!goal.inside&&goal.symbol==-1&&goal.label==0);check(goal.x<=1228.8f&&goal.y<=324.01f);
        frame.w[32]=150;frame.w[33]=100;goal=D2rWorldRenderer.place(frame,0,1280,720);check(!goal.inside&&goal.label==0&&goal.y<=324.01f);
        frame.w[32]=110;frame.w[7]=1;frame.w[16]=700000;frame.w[17]=50000;frame.w[18]=950000;frame.w[19]=300000;goal=D2rWorldRenderer.place(frame,0,1280,720);check(goal.inside&&goal.symbol==6&&close(goal.symbolHeight,36.72f));check(close(goal.x,1120)&&close(goal.y,158));
        frame.w[32]=-10000;goal=D2rWorldRenderer.place(frame,0,1280,720);check(!goal.inside&&goal.label==0&&goal.symbol==-1);check(goal.x>=915.19f&&goal.x<=1196.81f&&goal.y>=50.39f&&goal.y<=201.61f);
        frame.w[34]=63;check(D2rWorldRenderer.place(frame,0,1280,720)==null);check(D2rWorldRenderer.place(frame,1,1280,720)==null);
        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putLong(0,2);check(D2rWorldRenderer.decode(bytes)==null);
        System.out.println("ANDROID_D2R_WORLD_RENDERER_OK checks="+checks+" actual_native_bytes=1 full_mini_projection=1 skill_avoidance=1 original_symbols=1 no_canvas_claim=1");
    }
}
