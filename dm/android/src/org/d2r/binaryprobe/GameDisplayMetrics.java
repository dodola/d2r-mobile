package org.d2r.binaryprobe;

/** Geometry and sampling shared by the native Android shell and its JVM checks. */
final class GameDisplayMetrics {
    static int[] fitGame(int availableWidth,int availableHeight) {
        if(availableWidth<=0||availableHeight<=0)return new int[]{0,0};
        int width=availableWidth,height=availableHeight;
        if((long)width*9>(long)height*16)width=(int)((long)height*16/9);
        else height=(int)((long)width*9/16);
        return new int[]{Math.max(1,width),Math.max(1,height)};
    }

    static final class FpsSampler {
        private long firstFrame=-1,firstMs;
        private double last=Double.NaN;
        void reset(){firstFrame=-1;last=Double.NaN;}
        double sample(long frame,long monotonicMs) {
            if(firstFrame<0||frame<firstFrame||monotonicMs<firstMs){firstFrame=frame;firstMs=monotonicMs;last=Double.NaN;}
            long elapsed=monotonicMs-firstMs;
            if(elapsed>=1000){last=(frame-firstFrame)*1000.0/elapsed;firstFrame=frame;firstMs=monotonicMs;}
            return last;
        }
    }
}
