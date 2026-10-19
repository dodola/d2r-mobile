package org.d2r.binaryprobe;

public final class GameControllerStateTest {
    private static int checks;
    private static void check(boolean value){checks++;if(!value)throw new AssertionError("check "+checks);}
    public static void main(String[] args){GameControllerState c=new GameControllerState();
        c.key(1,1,true);c.key(2,1,true);c.key(2,2,true);check(c.snapshot().buttons==3);
        c.key(1,1,false);check(c.snapshot().buttons==3);
        c.motion(1,4096,20000,0,0,0);c.motion(2,8192,0,10000,0,-25000);
        GameControllerState.Snapshot s=c.snapshot();check(s.buttons==12291&&s.lx==20000&&s.ly==0&&s.rx==0&&s.ry==-25000);
        c.remove(2);s=c.snapshot();check(s.buttons==4096&&s.lx==20000&&s.ry==0);
        c.key(1,256,true);c.motion(1,256,0,0,0,0);c.key(1,256,false);check((c.snapshot().buttons&256)!=0);
        c.motion(1,0,0,0,0,0);check(c.snapshot().buttons==0);c.clear();check(c.snapshot().lx==0);
        System.out.println("ANDROID_GAME_CONTROLLER checks="+checks+" per_device_merge=1 disconnect=1 digital_analog_overlap=1 passed=1");
    }
}
