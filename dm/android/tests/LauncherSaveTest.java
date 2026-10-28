/* 仅用合成存档检验真实 ZIP、覆盖预览、提交故障、恢复和配置互通。
 * Synthetic-only tests of real ZIP transfers, conflict previews, commit faults, recovery and shared settings. */
package org.d2r.binaryprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.*;

public final class LauncherSaveTest {
    private static int checks,symlinks;
    private static final SaveArchive.Sync SYNC=dir->{};
    private static void check(boolean ok,String message){checks++;if(!ok)throw new AssertionError(message);}
    interface Action {void run()throws Exception;}
    private static void rejects(Action a,String message)throws Exception {try{a.run();throw new AssertionError(message);}catch(IOException expected){checks++;}}
    private static File dir(File parent,String path)throws IOException{File f=new File(parent,path);Files.createDirectories(f.toPath());return f;}
    private static File put(File parent,String name,String text)throws IOException {File f=new File(parent,name);dir(f.getParentFile(),"");Files.write(f.toPath(),text.getBytes(StandardCharsets.UTF_8));return f;}
    private static String read(File f)throws IOException{return new String(Files.readAllBytes(f.toPath()),StandardCharsets.UTF_8);}
    private static byte[] archive(String... paths)throws IOException {
        ByteArrayOutputStream b=new ByteArrayOutputStream();try(ZipOutputStream z=new ZipOutputStream(b)){
            for(String path:paths){z.putNextEntry(new ZipEntry(path));if(!path.endsWith("/"))z.write("synthetic-save".getBytes(StandardCharsets.UTF_8));z.closeEntry();}}
        return b.toByteArray();
    }
    private static byte[] character(String name,int version,boolean valid)throws IOException {
        byte[] b=new byte[400];b[0]=0x55;b[1]=(byte)0xaa;b[2]=0x55;b[3]=(byte)0xaa;b[4]=(byte)version;b[8]=(byte)(b.length&255);b[9]=(byte)(b.length>>8);
        byte[] n=name.getBytes(StandardCharsets.UTF_8);System.arraycopy(n,0,b,299,n.length);
        long sum=0;for(int i=0;i<b.length;i++){long v=(i>=12&&i<16)?0:b[i]&255;sum=((((sum<<1)|(sum>>>31))&0xffffffffL)+v)&0xffffffffL;}
        if(!valid)sum^=1;for(int i=0;i<4;i++)b[12+i]=(byte)(sum>>>(8*i));
        return b;
    }
    public static void main(String[] args)throws Exception {
        File root=Files.createTempDirectory("launcher-synthetic-").toFile();
        try{
            File original=dir(root,"original");put(original,"hero/hero.d2s","old-character");put(original,"Settings/Settings.json","old-settings");put(original,"hero/.guest-name","synthetic-name");put(original,"other/other.d2s","keep-other");
            ByteArrayOutputStream exported=new ByteArrayOutputStream();SaveArchive.zip(original,exported,"base");
            File unpack=new File(root,"unpack");SaveArchive.Summary round=SaveArchive.unzip(new ByteArrayInputStream(exported.toByteArray()),unpack,"base");
            check(round.files==4&&round.characters==2,"round trip counts");
            for(String path:SaveArchive.inspect(original).paths)check(read(new File(original,path)).equals(read(new File(unpack,path))),"round trip "+path);
            check(!new File(unpack,SaveArchive.MARKER).exists(),"marker is not save data");
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(exported.toByteArray()),new File(root,"wrong-mod"),"mod-demo"),"cross-MOD archive rejected");
            File incoming=dir(root,"incoming");put(incoming,"hero/hero.d2s","new-character");put(incoming,"new/new.d2s","new-hero");
            SaveArchive.Summary preview=SaveArchive.preview(original,incoming);
            check(preview.files==2&&preview.characters==2&&preview.conflicts==1,"accurate conflict preview");
            File backups=dir(root,"backups"),backup=SaveArchive.merge(original,incoming,backups,"base",SYNC);
            check(read(new File(original,"hero/hero.d2s")).equals("new-character"),"matching file imported");
            check(read(new File(original,"other/other.d2s")).equals("keep-other"),"other character retained");
            check(read(new File(original,"Settings/Settings.json")).equals("old-settings"),"unrelated settings retained");
            File before=new File(root,"before");SaveArchive.unzip(new FileInputStream(backup),before,"base");
            check(read(new File(before,"hero/hero.d2s")).equals("old-character"),"backup contains previous generation");
            check(!new File(before,"new/new.d2s").exists(),"backup is pre-import");
            for(int failAt=1;failAt<=7;failAt++){
                File live=dir(root,"failure"+failAt);put(live,"hero/hero.d2s","old");AtomicInteger calls=new AtomicInteger();final int fail=failAt;
                rejects(()->SaveArchive.merge(live,incoming,dir(root,"fault-backups"+fail),"base",directory->{if(calls.incrementAndGet()==fail)throw new IOException("injected directory sync");}),"fault must be reported "+fail);
                check(read(new File(live,"hero/hero.d2s")).equals("old"),"old generation restored at fault "+fail);
                check(!new File(live,"new/new.d2s").exists(),"no partial import at fault "+fail);
                SaveArchive.recover(live,SYNC);check(read(new File(live,"hero/hero.d2s")).equals("old"),"recovery idempotent "+fail);
            }
            File crashed=new File(root,"crashed");put(dir(root,"crashed.transfer-old"),"hero.d2s","committed");put(dir(root,"crashed.transfer-next"),"hero.d2s","unpublished");put(root,"crashed.transfer-pending","save-transfer-v1\n");
            SaveArchive.recover(crashed,SYNC);check(read(new File(crashed,"hero.d2s")).equals("committed"),"recover interruption between renames");
            put(dir(root,"crashed.transfer-old"),"hero.d2s","older");put(root,"crashed.transfer-pending","save-transfer-v1\n");
            SaveArchive.recover(crashed,SYNC);check(read(new File(crashed,"hero.d2s")).equals("committed"),"retain published generation after crash");
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(new byte[]{1,2,3}),new File(root,"bad"),"base"),"non-ZIP rejected");
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(archive()),new File(root,"empty"),"base"),"empty ZIP rejected");
            check(SaveArchive.target(root,"debug").getPath().replace(File.separatorChar,'/').endsWith("d2r-mobile/debug/save"),"debug scope maps to the debug save");
            check(SaveArchive.plainScope("debug-mod-demo").equals("mod-demo")&&SaveArchive.plainScope("debug").equals("base")&&SaveArchive.plainScope("base").equals("base"),"debug twin of scope");
            rejects(()->SaveArchive.target(root,"debug-demo"),"malformed debug scope rejected");
            check(SaveArchive.characterName("Mage-Frost",null).equals("Mage_Frost")&&SaveArchive.characterName("Ice__Bolt!!",null).equals("Ice_Bolt")
                &&SaveArchive.characterName("法师Ab",null).equals("Ab")&&SaveArchive.characterName("_x_",null).equals("Hero")&&SaveArchive.characterName("A_b_c",null).equals("A_bc"),"character name rules");
            byte[] cn=character("法师-春饼暴",0x69,true);
            File live=dir(root,"live-d2s");put(live,"Hero/Hero.d2s","existing");
            check(SaveArchive.characterName("法",live,1).equals("Sorceress")&&SaveArchive.characterName("法",null,9).equals("Hero")&&SaveArchive.characterName("法",dir(root,"taken"),7).equals("Warlock"),"class fallback");
            put(new File(root,"taken"),"Warlock/Warlock.d2s","x");check(SaveArchive.characterName("法",new File(root,"taken"),7).equals("WarlockA"),"class fallback made unique");
            File one=new File(root,"d2s-one");SaveArchive.Summary imported=SaveArchive.unzip(new ByteArrayInputStream(cn),one,"base",live);
            check(imported.files==2&&imported.characters==1,"bare d2s becomes one character");
            check(new File(one,"Amazon/Amazon.d2s").isFile()&&new File(one,"Amazon/Amazon.d2s.meta").isFile(),"invalid name replaced by the class name");
            byte[] renamed=Files.readAllBytes(new File(one,"Amazon/Amazon.d2s").toPath());
            check(new String(renamed,299,6,StandardCharsets.US_ASCII).equals("Amazon")&&renamed[305]==0,"name rewritten in header");
            check(SaveArchive.d2sChecksum(renamed)==((renamed[12]&255L)|(renamed[13]&255L)<<8|(renamed[14]&255L)<<16|(renamed[15]&255L)<<24),"checksum recomputed");
            File keep=new File(root,"d2s-keep");SaveArchive.unzip(new ByteArrayInputStream(character("Blizzard",0x69,true)),keep,"base",null);
            check(new File(keep,"Blizzard/Blizzard.d2s").isFile(),"valid name kept");
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(character("Blizzard",0x61,true)),new File(root,"d2s-version"),"base"),"other version rejected");
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(character("Blizzard",0x69,false)),new File(root,"d2s-sum"),"base"),"bad checksum rejected");
            String[] bad={"../escape","/absolute","a/../../escape","a\\b","C:/drive","a//b","./relative","bad\u0001name","../"};
            for(int i=0;i<bad.length;i++){final String path=bad[i];final File stage=new File(root,"path"+i);
                rejects(()->SaveArchive.unzip(new ByteArrayInputStream(archive(path)),stage,"base"),"path rejected "+path);check(!stage.exists(),"rejected archive cleaned");}
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(archive("A.d2s","a.d2s")),new File(root,"duplicate"),"base"),"case collision rejected");
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(archive("Hero/a.d2s","hero/b.d2s")),new File(root,"case-parent"),"base"),"case parent collision rejected");
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(archive("hero/a.d2s","hero")),new File(root,"parent-file"),"base"),"implicit directory cannot become file");
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(archive(SaveArchive.MARKER+"/","hero.d2s")),new File(root,"marker-dir"),"base"),"marker cannot be directory");
            File alias=dir(root,"alias");put(alias,"Hero/new.d2s","alias");rejects(()->SaveArchive.preview(original,alias),"live/import directory alias rejected");
            File conflict=dir(root,"conflict");put(conflict,"hero","file-in-place-of-directory");rejects(()->SaveArchive.preview(conflict,incoming),"file/directory conflict rejected");
            byte[] truncated=Arrays.copyOf(exported.toByteArray(),exported.size()/3);
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(truncated),new File(root,"truncated"),"base"),"truncated entry rejected");
            File tooBig=dir(root,"oversized");try(RandomAccessFile f=new RandomAccessFile(new File(tooBig,"huge"),"rw")){f.setLength(SaveArchive.MAX_FILE+1);}
            rejects(()->SaveArchive.zip(tooBig,new ByteArrayOutputStream(),"base"),"oversized source rejected");
            ByteArrayOutputStream bomb=new ByteArrayOutputStream();try(ZipOutputStream z=new ZipOutputStream(bomb)){
                z.putNextEntry(new ZipEntry("large.d2s"));byte[] zeros=new byte[65536];for(int i=0;i<512;i++)z.write(zeros);z.write(1);z.closeEntry();}
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(bomb.toByteArray()),new File(root,"bomb"),"base"),"expanded file limit enforced");
            byte[] content="synthetic-crc".getBytes(StandardCharsets.UTF_8);CRC32 crc=new CRC32();crc.update(content);ByteArrayOutputStream corrupt=new ByteArrayOutputStream();
            try(ZipOutputStream z=new ZipOutputStream(corrupt)){ZipEntry e=new ZipEntry("hero.d2s");e.setMethod(ZipEntry.STORED);e.setSize(content.length);e.setCompressedSize(content.length);e.setCrc(crc.getValue());z.putNextEntry(e);z.write(content);z.closeEntry();}
            byte[] damaged=corrupt.toByteArray();damaged[30+"hero.d2s".length()]^=1;
            rejects(()->SaveArchive.unzip(new ByteArrayInputStream(damaged),new File(root,"crc"),"base"),"CRC corruption rejected");
            File fresh=new File(root,"fresh");AtomicInteger freshSyncs=new AtomicInteger();
            rejects(()->SaveArchive.merge(fresh,incoming,backups,"base",directory->{if(freshSyncs.incrementAndGet()==5)throw new IOException("fresh publication sync");}),"fresh publication failure reported");
            check(!fresh.exists(),"fresh failed publication rolled back");SaveArchive.recover(fresh,SYNC);check(!fresh.exists(),"fresh recovery does not expose failed generation");
            File backupRoot=dir(root,"backup-app"),backupDir=dir(backupRoot,"d2r-mobile/save-backups");
            check(SaveArchive.latestBackup(backupRoot,"base")==null,"no initial backup");
            String backupName="base-1-"+UUID.randomUUID()+".zip";File retained=new File(backupDir,backupName);Files.copy(backup.toPath(),retained.toPath());
            put(backupDir,backupName+".partial","unfinished");put(backupDir,"mod-demo-2-"+UUID.randomUUID()+".zip","other scope");
            check(SaveArchive.latestBackup(backupRoot,"base").equals(retained),"only complete matching-scope backup selected");
            rejects(()->SaveArchive.latestBackup(backupRoot,"../base"),"invalid backup scope rejected");
            File settingsRoot=dir(root,"app");LauncherSettingsStore settings=new LauncherSettingsStore(settingsRoot);
            check(settings.scale().equals("auto")&&!settings.fps(),"defaults match game");
            put(settingsRoot,"d2r-mobile/sw_custom.txt","lang=2\nunknown_future=42\nshow_fps=0\nshow_fps=0\nmerc_pot_v2=1\n");
            settings.fps(true);String config=read(new File(settingsRoot,"d2r-mobile/sw_custom.txt"));
            check(config.contains("lang=2\n")&&config.contains("unknown_future=42\n")&&config.contains("merc_pot_v2=1\n"),"unowned keys preserved");
            check(settings.fps()&&config.indexOf("show_fps=")==config.lastIndexOf("show_fps="),"single shared FPS key");
            for(int i=0;i<4;i++){settings.scale(i);check(settings.scale().equals(LauncherSettingsStore.SCALES[i]),"shared scale preset "+i);}
            put(settingsRoot,"d2r-mobile/diagnostics/render-scale.txt","0.8\n");check(settings.scale().equals("0.8"),"custom existing scale preserved");
            put(settingsRoot,"d2r-mobile/diagnostics/render-scale.txt","NaN\n");rejects(settings::scale,"nonfinite scale rejected");
            rejects(()->settings.scale(4),"invalid preset rejected");
            File scopeRoot=dir(root,"scope");check(SaveArchive.target(scopeRoot,"base").equals(new File(scopeRoot,"d2r-mobile/save")),"base committed path");
            put(scopeRoot,"d2r-mobile/mods/demo/package.txt","synthetic MOD manifest");check(SaveArchive.target(scopeRoot,"mod-demo").equals(new File(scopeRoot,"d2r-mobile/mods/demo/save")),"MOD isolation path");
            rejects(()->SaveArchive.target(scopeRoot,"mod-../escape"),"invalid MOD scope");
            try{File outside=dir(root,"outside"),link=new File(root,"link");put(outside,"secret","keep");Files.createSymbolicLink(link.toPath(),outside.toPath());
                rejects(()->SaveArchive.zip(link,new ByteArrayOutputStream(),"base"),"symlink export refused");rejects(()->SaveArchive.delete(link),"symlink deletion refused");check(read(new File(outside,"secret")).equals("keep"),"outside files untouched");Files.delete(link.toPath());symlinks=1;
            }catch(FileSystemException|UnsupportedOperationException unavailable){System.out.println("SYMLINK_TEST unavailable="+unavailable.getClass().getSimpleName());}
            check(GameSessionGate.beginTransfer(),"reserve transfer");check(!GameSessionGate.beginGuest()&&!GameSessionGate.beginTransfer(),"mutual exclusion");GameSessionGate.endTransfer();
            check(GameSessionGate.beginGuest(),"start after completed transfer");check(!GameSessionGate.beginTransfer(),"no transfer while game exists");GameSessionGate.guestDestroyed();
            check(!GameSessionGate.guestRunning()&&GameSessionGate.guestStarted()&&!GameSessionGate.beginGuest(),"no native restart before process restart");
            System.out.println("LAUNCHER_SAVE checks="+checks+" symlink_test="+symlinks+" synthetic_only=1 passed=1");
        }finally{SaveArchive.delete(root);}
    }
}
