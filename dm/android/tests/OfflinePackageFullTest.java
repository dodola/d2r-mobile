package org.d2r.binaryprobe;

import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Import the real >8 GiB release with a small JVM heap, not a synthetic large-size stub. */
public final class OfflinePackageFullTest {
    public static void main(String[] args)throws Exception {
        List<OfflinePackageInstaller.Entry> entries=new ArrayList<>();
        for(String line:Files.readAllLines(Paths.get(args[1]))) {
            String[] fields=line.split("\t");entries.add(new OfflinePackageInstaller.Entry(fields[0],Long.parseLong(fields[1]),fields[2]));
        }
        OfflinePackageInstaller.Manifest manifest=new OfflinePackageInstaller.Manifest(Files.readAllBytes(Paths.get(args[0])),entries);
        File root=new File(args[3]);root.mkdirs();final long[] printed={0};
        OfflinePackageInstaller installer=new OfflinePackageInstaller(root,manifest,(phase,done,total)->{
            if(done-printed[0]>=1024L*1024*1024||done==total){System.out.println(phase+" "+done+"/"+total);printed[0]=done;}
        },()->false,root::getUsableSpace);
        try(InputStream in=new FileInputStream(args[2])){installer.install(in);}
        if(!installer.prepareExisting())throw new AssertionError("Full installation is not ready");
        if(new File(root,"game.romfs").length()<=Integer.MAX_VALUE)throw new AssertionError("Not a full size test");
        System.out.println("OFFLINE_FULL_IMPORT bytes="+manifest.total+" files="+entries.size()+" pinned_manifest="+manifest.tag+" ready=1 passed=1");
    }
}
