package org.d2r.binaryprobe;

import java.util.HashMap;

/** Per-device controller state; disconnecting one controller preserves the rest. */
final class GameControllerState {
    static final class Snapshot {
        long buttons;
        int lx, ly, rx, ry;
    }
    private static final class Device {
        long keys, motion;
        int lx, ly, rx, ry;
    }
    private final HashMap<Integer,Device> devices=new HashMap<>();
    private Device device(int id){return devices.computeIfAbsent(id,key -> new Device());}
    void key(int id,long bit,boolean down){Device d=device(id);if(down)d.keys|=bit;else d.keys&=~bit;}
    void motion(int id,long buttons,int lx,int ly,int rx,int ry){Device d=device(id);d.motion=buttons;d.lx=lx;d.ly=ly;d.rx=rx;d.ry=ry;}
    void remove(int id){devices.remove(id);}
    void clear(){devices.clear();}
    Snapshot snapshot(){Snapshot s=new Snapshot();long left=-1,right=-1;
        for(Device d:devices.values()){s.buttons|=d.keys|d.motion;
            long l=(long)d.lx*d.lx+(long)d.ly*d.ly,r=(long)d.rx*d.rx+(long)d.ry*d.ry;
            if(l>left){left=l;s.lx=d.lx;s.ly=d.ly;}if(r>right){right=r;s.rx=d.rx;s.ry=d.ry;}}
        return s;
    }
}
