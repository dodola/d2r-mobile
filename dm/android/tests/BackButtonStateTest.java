package org.d2r.binaryprobe;

import android.view.KeyEvent;

public final class BackButtonStateTest {
    private static int checks;
    private static void check(boolean ok){checks++;if(!ok)throw new AssertionError("check "+checks);}
    public static void main(String[] args){
        BackButtonState s=new BackButtonState();
        check(BackButtonState.handles(KeyEvent.KEYCODE_BUTTON_B));
        check(BackButtonState.handles(KeyEvent.KEYCODE_BACK));
        check(!BackButtonState.handles(KeyEvent.KEYCODE_VOLUME_UP));
        check(s.buttons()==0);
        check(s.update(10,KeyEvent.KEYCODE_BUTTON_B,true)==2);
        check(s.update(10,KeyEvent.KEYCODE_BUTTON_B,true)==2); // repeat
        check(s.update(10,KeyEvent.KEYCODE_BUTTON_B,false)==0);
        check(s.update(10,KeyEvent.KEYCODE_BUTTON_B,false)==0); // stale release
        s.update(10,KeyEvent.KEYCODE_BUTTON_B,true);s.update(11,KeyEvent.KEYCODE_BUTTON_B,true);
        check(s.update(10,KeyEvent.KEYCODE_BUTTON_B,false)==2);
        check(s.update(11,KeyEvent.KEYCODE_BUTTON_B,false)==0);
        s.update(10,KeyEvent.KEYCODE_BUTTON_B,true);s.update(10,KeyEvent.KEYCODE_BACK,true);
        check(s.update(10,KeyEvent.KEYCODE_BUTTON_B,false)==2);
        check(s.update(10,KeyEvent.KEYCODE_BACK,false)==0);
        s.update(-1,KeyEvent.KEYCODE_BACK,true);s.update(10,KeyEvent.KEYCODE_BUTTON_B,true);
        check(!s.removeDevice(99));check(s.buttons()==2);
        check(s.removeDevice(10));check(s.buttons()==2);check(s.removeDevice(-1));check(s.buttons()==0);
        s.update(10,KeyEvent.KEYCODE_BUTTON_B,true);s.update(11,KeyEvent.KEYCODE_BUTTON_B,true);
        check(!s.cancel(99,KeyEvent.KEYCODE_BUTTON_B));check(s.buttons()==2);
        check(s.cancel(10,KeyEvent.KEYCODE_BUTTON_B));check(s.buttons()==2);
        check(!s.cancel(10,KeyEvent.KEYCODE_BUTTON_B));check(s.cancel(11,KeyEvent.KEYCODE_BUTTON_B));check(s.buttons()==0);
        s.update(10,KeyEvent.KEYCODE_BUTTON_B,true);s.clear();check(s.buttons()==0);
        check(s.update(10,KeyEvent.KEYCODE_BUTTON_B,false)==0);
        try{s.update(10,KeyEvent.KEYCODE_A,true);throw new AssertionError("unhandled key");}catch(IllegalArgumentException expected){checks++;}
        System.out.println("BACK_BUTTON checks="+checks+" repeat=1 multiple_devices=1 mixed_keys=1 cancellation=1 passed=1");
    }
}
