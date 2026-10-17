package org.d2r.binaryprobe;

import java.util.HashSet;

/** Back and controller B mean guest Npad B, never Activity exit. */
final class BackButtonState {
    // android.view.KeyEvent.KEYCODE_BACK / KEYCODE_BUTTON_B.
    static final int BACK=4, BUTTON_B=97;
    private final HashSet<Long> held=new HashSet<>();
    static boolean handles(int key){return key==BACK||key==BUTTON_B;}
    long update(int device,int key,boolean down){
        if(!handles(key))throw new IllegalArgumentException("Not a back button");
        long token=((long)device<<32)|(key&0xffffffffL);
        if(down)held.add(token);else held.remove(token);
        return buttons();
    }
    long buttons(){return held.isEmpty()?0:2;}
    void clear(){held.clear();}
    boolean cancel(int device,int key){return held.remove(((long)device<<32)|(key&0xffffffffL));}
    boolean removeDevice(int device){return held.removeIf(token->(int)(token>>32)==device);}
}
