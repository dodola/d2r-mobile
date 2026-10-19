package org.d2r.binaryprobe;

public final class GameHapticStateTest {
    private static int checks;
    private static void check(boolean value){checks++;if(!value)throw new AssertionError("check "+checks);}
    private static final class Queue implements GameHapticState.Source {
        int pending,takes;boolean enabled=true;
        void drop(int tier){if(enabled&&(pending==0||tier<pending))pending=tier;}
        @Override public int take(boolean active){takes++;int tier=pending;pending=0;return active&&enabled?tier:-1;}
    }
    private static final class Output implements GameHapticState.Output {
        int plays,cancels,last;
        @Override public void play(int tier){plays++;last=tier;}
        @Override public void cancel(){cancels++;}
    }
    public static void main(String[] args){
        Queue q=new Queue();Output out=new Output();GameHapticState state=new GameHapticState(q,out);
        q.drop(1);state.tick(true);check(out.plays==0&&q.pending==0);
        q.drop(2);state.active(true);state.tick(true);check(out.plays==0&&q.pending==0);
        for(int tier=1;tier<=3;tier++){q.drop(tier);state.tick(true);check(out.last==tier&&out.plays==tier);
            state.tick(true);state.tick(true);check(out.plays==tier);}
        q.drop(3);q.drop(1);q.drop(2);state.tick(true);check(out.plays==4&&out.last==1);
        state.active(false);check(out.cancels==1);q.drop(1);state.active(true);state.tick(true);check(out.plays==4);
        q.drop(2);state.tick(true);check(out.plays==5);q.enabled=false;state.tick(true);
        check(out.cancels==2);q.drop(1);state.tick(true);q.enabled=true;state.tick(true);check(out.plays==5);
        q.drop(3);state.tick(false);check(out.plays==5&&q.pending==0);state.tick(true);check(out.plays==5);
        q.drop(3);state.tick(true);check(out.plays==6);state.stop();check(out.cancels==3);
        q.drop(1);state.active(true);state.tick(true);check(out.plays==6);
        q.pending=7;state.tick(true);check(out.plays==6);
        q.drop(1);state.tick(true);state.stop();check(out.plays==7&&out.cancels==4);
        state.stop();check(out.cancels==5);state.active(true);state.tick(true);check(out.plays==7);
        System.out.println("ANDROID_GAME_HAPTICS checks="+checks+" strongest=1 exactly_once=1 tiers=3 lifecycle_cancel=1 settings=1 system_suppression=1 passed=1");
    }
}
