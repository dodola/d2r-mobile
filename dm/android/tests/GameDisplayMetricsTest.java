package org.d2r.binaryprobe;

public final class GameDisplayMetricsTest {
    private static int checks;
    private static void check(boolean value){checks++;if(!value)throw new AssertionError("check "+checks);}
    public static void main(String[] args) {
        int[][] screens={{2800,1264},{2665,1264},{1920,1080},{2560,1600},{1280,800},{1264,2800},{1,1},{0,720}};
        for(int[] screen:screens){int[] fit=GameDisplayMetrics.fitGame(screen[0],screen[1]);
            check(fit[0]<=screen[0]&&fit[1]<=screen[1]);
            if(screen[0]>0&&screen[1]>0){check(fit[0]>0&&fit[1]>0);check(Math.abs((long)fit[0]*9-(long)fit[1]*16)<=16);}}
        int[] wide=GameDisplayMetrics.fitGame(2665,1264);check(wide[0]==2247&&wide[1]==1264);
        int[] standard=GameDisplayMetrics.fitGame(1920,1080);check(standard[0]==1920&&standard[1]==1080);
        GameDisplayMetrics.FpsSampler fps=new GameDisplayMetrics.FpsSampler();
        check(Double.isNaN(fps.sample(100,1000)));
        check(Double.isNaN(fps.sample(115,1500)));
        check(fps.sample(130,2000)==30);
        check(fps.sample(190,3000)==60);
        check(fps.sample(190,4000)==0);
        fps.reset();check(Double.isNaN(fps.sample(900,10000)));
        check(fps.sample(930,11000)==30);
        check(Double.isNaN(fps.sample(1,12000)));
        check(fps.sample(61,13000)==60);
        check(Double.isNaN(fps.sample(62,12000)));
        System.out.println("ANDROID_GAME_DISPLAY checks="+checks+" aspect_fit=1 fps_sampling=1 pause_reset=1 passed=1");
    }
}
