package org.d2r.binaryprobe;

/** Process-owned job state shared by the worker and whichever launcher is visible. */
final class OfflineInstallState {
    enum Phase { CHECKING, WAITING, IMPORTING, CANCELLING, COMMITTING, READY, FAILED, STOPPED }
    private volatile Phase phase=Phase.CHECKING;
    Phase phase(){return phase;}
    boolean busy(){
        Phase p=phase;
        return p==Phase.CHECKING||p==Phase.IMPORTING||p==Phase.CANCELLING||p==Phase.COMMITTING;
    }
    boolean ready(){return phase==Phase.READY;}
    boolean canSelect(){return phase==Phase.WAITING||phase==Phase.FAILED||phase==Phase.STOPPED;}
    boolean canCancel(){return phase==Phase.IMPORTING;}
    boolean cancelled(){return phase==Phase.CANCELLING;}
    synchronized boolean beginImport(){
        if(!canSelect())return false;
        phase=Phase.IMPORTING;return true;
    }
    synchronized boolean requestCancel(){
        if(!canCancel())return false;
        phase=Phase.CANCELLING;return true;
    }
    // The installer has already written its durable recovery marker at this point.
    // Completion wins even if cancellation raced with the final input read.
    synchronized void beginCommit(){phase=Phase.COMMITTING;}
    synchronized void finishReady(){phase=Phase.READY;}
    synchronized void finishWaiting(){phase=Phase.WAITING;}
    synchronized void finishFailed(){phase=cancelled()?Phase.STOPPED:Phase.FAILED;}
}
