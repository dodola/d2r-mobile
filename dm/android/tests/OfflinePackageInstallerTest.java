package org.d2r.binaryprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** Run on the host JVM and unchanged on Android via InstallerTestInstrumentation. */
public final class OfflinePackageInstallerTest {
    private static int checks;
    private static final byte[] IMAGE=new byte[3*1024*1024+31],INDEX="shader-index-fixture".getBytes(StandardCharsets.UTF_8);
    private static final String INDEX_PATH="d2r-mobile/shaders/shader_index.bin";
    private static void check(boolean ok,String message){checks++;if(!ok)throw new AssertionError(message);}
    private static String hash(byte[] b)throws Exception{return OfflinePackageInstaller.hex(MessageDigest.getInstance("SHA-256").digest(b));}
    private static void write(File root,String path,byte[] bytes)throws Exception {File f=new File(root,path);f.getParentFile().mkdirs();Files.write(f.toPath(),bytes);}
    private static byte[] read(File root,String path)throws Exception{return Files.readAllBytes(new File(root,path).toPath());}
    private static OfflinePackageInstaller.Manifest manifest()throws Exception {
        return new OfflinePackageInstaller.Manifest("{\"fixture\":1}".getBytes(StandardCharsets.UTF_8),Arrays.asList(
            new OfflinePackageInstaller.Entry("game.romfs",IMAGE.length,hash(IMAGE)),new OfflinePackageInstaller.Entry(INDEX_PATH,INDEX.length,hash(INDEX))));
    }
    private static byte[] pack(OfflinePackageInstaller.Manifest m)throws Exception {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
        out.write(OfflinePackageInstaller.MAGIC);out.writeInt(m.json.length);out.write(m.json);out.write(IMAGE);out.write(INDEX);return bytes.toByteArray();
    }
    private interface Action {void run()throws Exception;}
    private static void fails(String reason,Action action)throws Exception {
        try{action.run();throw new AssertionError("Accepted "+reason);}catch(OfflinePackageInstaller.Failure e){check(reason.equals(e.reason),"Expected "+reason+", got "+e.reason);}
    }
    private static OfflinePackageInstaller installer(File root,OfflinePackageInstaller.Manifest m)throws Exception {
        root.mkdirs();return new OfflinePackageInstaller(root,m,(phase,done,total)->{},()->false,()->Long.MAX_VALUE);
    }
    public static String runAll(File base)throws Exception {
        checks=0;new Random(94012).nextBytes(IMAGE);OfflinePackageInstaller.Manifest m=manifest();byte[] pack=pack(m);
        File clean=new File(base,"success");OfflinePackageInstaller i=installer(clean,m);
        check(!i.prepareExisting(),"Fresh install was ready");write(clean,"d2r-mobile/save/hero.d2s",INDEX);
        i.install(new ByteArrayInputStream(pack));check(i.prepareExisting(),"Successful install not ready");
        check(Arrays.equals(read(clean,"game.romfs"),IMAGE),"Image changed");check(Arrays.equals(read(clean,INDEX_PATH),INDEX),"Index changed");
        check(Arrays.equals(read(clean,"d2r-mobile/save/hero.d2s"),INDEX),"Save changed");
        i.install(new ByteArrayInputStream(pack));check(i.prepareExisting(),"Repeated import failed");
        check(i.receiptCovers("game.romfs",IMAGE.length,hash(IMAGE)),"Receipt did not cover the installed image");
        check(!i.receiptCovers("game.romfs",IMAGE.length,hash(INDEX)),"Receipt covered a different hash");
        check(!i.receiptCovers("game.romfs",IMAGE.length-1,hash(IMAGE)),"Receipt covered a different size");
        check(!i.receiptCovers("other.bin",1,hash(INDEX)),"Receipt covered an unlisted path");
        File receiptImage=new File(clean,"game.romfs");long receiptTime=receiptImage.lastModified();receiptImage.setLastModified(receiptTime+5000);
        check(!i.receiptCovers("game.romfs",IMAGE.length,hash(IMAGE)),"Receipt covered a modified image");receiptImage.setLastModified(receiptTime);
        check(i.receiptCovers("game.romfs",IMAGE.length,hash(IMAGE)),"Receipt did not cover the restored image");
        write(clean,"d2r-offline-ready",new byte[]{0,2,(byte)255,(byte)255});check(i.prepareExisting(),"Damaged receipt did not revalidate existing resources");
        byte[] corrupt=pack.clone();corrupt[corrupt.length-1]^=1;
        File protectedRoot=new File(base,"corrupt");OfflinePackageInstaller protectedInstall=installer(protectedRoot,m);
        byte[] old="existing-installed-image".getBytes(StandardCharsets.UTF_8);write(protectedRoot,"game.romfs",old);
        fails("checksum",()->protectedInstall.install(new ByteArrayInputStream(corrupt)));
        check(Arrays.equals(read(protectedRoot,"game.romfs"),old),"Failure overwrote existing game");
        check(!new File(protectedRoot,"d2r-offline-ready").exists(),"Failed import wrote ready receipt");
        check(new File(protectedRoot,"d2r-offline-staging/game.romfs").isFile(),"Valid completed file not retained");
        protectedInstall.install(new ByteArrayInputStream(pack));check(protectedInstall.prepareExisting(),"Retry did not recover");
        byte[] wrong=pack.clone();wrong[0]=0;fails("format",()->installer(new File(base,"bad-magic"),m).install(new ByteArrayInputStream(wrong)));
        byte[] version=pack.clone();version[12]^=1;fails("version",()->installer(new File(base,"bad-version"),m).install(new ByteArrayInputStream(version)));
        for(int length:new int[]{0,7,12,pack.length-1}) {
            byte[] shortPack=Arrays.copyOf(pack,length);fails("truncated",()->installer(new File(base,"short-"+length),m).install(new ByteArrayInputStream(shortPack)));
        }
        byte[] trailing=Arrays.copyOf(pack,pack.length+1);fails("format",()->installer(new File(base,"trailing"),m).install(new ByteArrayInputStream(trailing)));
        fails("space",()->new OfflinePackageInstaller(new File(base,"space"),m,(p,d,t)->{},()->false,()->0).install(new ByteArrayInputStream(pack)));
        check(!new File(base,"space/game.romfs").exists(),"Space failure installed data");
        AtomicBoolean stop=new AtomicBoolean();File cancelled=new File(base,"cancel");
        OfflinePackageInstaller c=new OfflinePackageInstaller(cancelled,m,(p,d,t)->{if(p.equals("import")&&d>=IMAGE.length)stop.set(true);},stop::get,()->Long.MAX_VALUE);
        fails("cancelled",()->c.install(new ByteArrayInputStream(pack)));
        check(!new File(cancelled,"game.romfs").exists(),"Cancelled import committed");
        check(!new File(cancelled,"d2r-offline-staging/game.romfs.part").exists(),"Cancellation retained unverified partial file");
        OfflinePackageInstaller retry=installer(cancelled,m);check(!retry.prepareExisting(),"Incomplete import was launched");
        retry.install(new ByteArrayInputStream(pack));check(retry.prepareExisting(),"Cancel retry failed");
        File recovery=new File(base,"recovery");OfflinePackageInstaller r=installer(recovery,m);
        write(recovery,"game.romfs",IMAGE);write(recovery,"d2r-offline-staging/"+INDEX_PATH,INDEX);
        write(recovery,"d2r-offline-staging/commit",m.tag.getBytes(StandardCharsets.US_ASCII));
        check(r.prepareExisting(),"Interrupted commit did not recover");check(Arrays.equals(read(recovery,INDEX_PATH),INDEX),"Recovery index changed");
        File stale=new File(base,"stale-stage");OfflinePackageInstaller s=installer(stale,m);
        write(stale,"game.romfs",IMAGE);write(stale,INDEX_PATH,INDEX);write(stale,"d2r-offline-staging/game.romfs",old);
        check(s.prepareExisting(),"Existing files not adopted");check(Arrays.equals(read(stale,"game.romfs"),IMAGE),"Stale pending file overwrote valid image");
        File changed=new File(clean,INDEX_PATH);long modified=changed.lastModified();byte[] bad=INDEX.clone();bad[0]^=1;
        write(clean,INDEX_PATH,bad);changed.setLastModified(modified+2000);check(!i.prepareExisting(),"Changed file bypassed revalidation");
        List<OfflinePackageInstaller.Entry> unsafe=new ArrayList<>(m.entries);unsafe.set(0,new OfflinePackageInstaller.Entry("../game.romfs",1,hash(INDEX)));
        fails("manifest",()->new OfflinePackageInstaller.Manifest(m.json,unsafe));
        List<OfflinePackageInstaller.Entry> duplicate=new ArrayList<>(m.entries);duplicate.add(m.entries.get(0));
        fails("manifest",()->new OfflinePackageInstaller.Manifest(m.json,duplicate));
        List<OfflinePackageInstaller.Entry> large=new ArrayList<>(m.entries);large.set(0,new OfflinePackageInstaller.Entry("game.romfs",5L*1024*1024*1024,hash(IMAGE)));
        OfflinePackageInstaller.Manifest big=new OfflinePackageInstaller.Manifest(m.json,large);
        try {new OfflinePackageInstaller(new File(base,"long-space"),big,(p,d,t)->{},()->false,()->1).install(new ByteArrayInputStream(pack));throw new AssertionError("Long size truncated");}
        catch(OfflinePackageInstaller.Failure e){check(e.reason.equals("space")&&e.needed>Integer.MAX_VALUE,"64-bit space planning failed");}
        return "OFFLINE_INSTALL checks="+checks+" streaming=1 checksum=1 corruption=1 cancellation=1 retry=1 commit_recovery=1 saves_preserved=1 long_sizes=1 passed=1";
    }
    public static void main(String[] args)throws Exception {System.out.println(runAll(Files.createTempDirectory("d2r-installer-tests-").toFile()));}
}
