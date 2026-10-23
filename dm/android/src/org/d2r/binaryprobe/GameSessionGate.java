/* 一个进程只启动一次客体，存档传输与客体启动互斥。
 * A process starts one guest only; save transfers and guest startup are mutually exclusive.
 * 独立于 GameActivity，查询不会提前加载 native 库。
 * Independent of GameActivity so queries never load the native library. */
package org.d2r.binaryprobe;

final class GameSessionGate {
    private static boolean guest,running,transfer;
    static synchronized boolean beginGuest(){if(guest||transfer)return false;guest=running=true;return true;}
    static synchronized boolean guestStarted(){return guest;}
    static synchronized boolean guestRunning(){return running;}
    static synchronized void guestDestroyed(){running=false;}
    static synchronized boolean beginTransfer(){if(guest||transfer)return false;transfer=true;return true;}
    static synchronized void endTransfer(){transfer=false;}
}
