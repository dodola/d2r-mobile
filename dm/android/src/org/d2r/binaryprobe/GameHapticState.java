package org.d2r.binaryprobe;

/** Lifecycle and exactly-once delivery, independent of Android's vibrator. */
final class GameHapticState {
    interface Source { int take(boolean active); }
    interface Output { void play(int tier); void cancel(); }
    private final Source source;
    private final Output output;
    private boolean active, playing;

    GameHapticState(Source source,Output output){this.source=source;this.output=output;}
    void active(boolean value){
        if(active==value)return;
        // Drain on both edges, including events accumulated while backgrounded.
        source.take(false);active=value;cancel();
    }
    void tick(boolean systemEnabled){
        int tier=source.take(active&&systemEnabled);
        if(!active||!systemEnabled||tier<0){cancel();return;}
        if(tier>=1&&tier<=3){output.play(tier);playing=true;}
    }
    void stop(){source.take(false);active=false;output.cancel();playing=false;}
    private void cancel(){if(playing){output.cancel();playing=false;}}
}
