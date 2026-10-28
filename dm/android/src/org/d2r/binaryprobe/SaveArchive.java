/* ZIP 存档往返、导入预览、备份及可恢复目录提交；只处理已提交的存档树。
 * ZIP save transfers, preview, backup and recoverable directory commit; committed saves only.
 * 交互参考 sydeEvans/d2r-android 5d8e322（MIT），实现适配本地事务目录与 MOD 隔离。
 * Interaction inspired by sydeEvans/d2r-android 5d8e322 (MIT), adapted to local transactions and MOD isolation. */
package org.d2r.binaryprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;
import java.util.zip.*;

final class SaveArchive {
    static final String MARKER="d2r-save.json";
    static final int MAX_FILES=4096,MAX_DEPTH=12;
    static final long MAX_FILE=32L<<20,MAX_TOTAL=256L<<20;
    interface Sync {void directory(File dir)throws IOException;}
    static final class Summary {
        int files,characters,conflicts;long bytes;
        final ArrayList<String> paths=new ArrayList<>();
    }
    private static final class Paths {
        final Map<String,String> spellings=new HashMap<>();
        final Set<String> files=new HashSet<>();
        void add(String name,boolean directory)throws IOException {
            String prefix="";String[] parts=name.split("/");
            for(int i=0;i<parts.length;i++){
                prefix+= (i==0?"":"/")+parts[i];String key=prefix.toLowerCase(Locale.ROOT);
                boolean dir=i<parts.length-1||directory;String previous=spellings.get(key);
                if(previous!=null&&(!previous.equals(prefix)||files.contains(key)||!dir))throw new IOException("Conflicting archive path");
                spellings.put(key,prefix);if(!dir)files.add(key);
            }
        }
    }
    /* Scopes: base, mod-<id>, and their Debug twins debug, debug-mod-<id> (the game uses separate Debug saves). */
    static File target(File files,String scope)throws IOException {
        File root=new File(files.getCanonicalFile(),"d2r-mobile");
        boolean debug=scope.equals("debug")||scope.startsWith("debug-mod-");
        String plain=debug?(scope.equals("debug")?"base":scope.substring(6)):scope;
        if("base".equals(plain))return debug?new File(root,"debug/save"):new File(root,"save");
        if(!plain.startsWith("mod-")||!ModSelectionStore.valid(plain.substring(4)))throw new IOException("Invalid save scope");
        File mod=new File(root,"mods/"+plain.substring(4));
        regular(new File(mod,"package.txt"));return new File(mod,debug?"debug-save":"save");
    }
    /* A Debug scope may exchange archives with its normal twin: the content is the same game data. */
    static String plainScope(String scope){return scope.equals("debug")?"base":scope.startsWith("debug-")?scope.substring(6):scope;}
    static void plainPath(File file)throws IOException {
        for(Path p=file.toPath().toAbsolutePath();p!=null;p=p.getParent())
            if(Files.isSymbolicLink(p))throw new IOException("Symbolic link in save path: "+p);
    }
    private static void regular(File file)throws IOException {
        plainPath(file);if(!Files.isRegularFile(file.toPath(),LinkOption.NOFOLLOW_LINKS))throw new IOException("Non-regular save file");
    }
    private static String valid(String name,boolean dir)throws IOException {
        if(dir&&name.endsWith("/"))name=name.substring(0,name.length()-1);
        if(name.isEmpty()||name.length()>1024||name.startsWith("/")||name.indexOf('\\')>=0||name.indexOf(':')>=0)throw new IOException("Invalid archive path");
        String[] parts=name.split("/",-1);if(parts.length>MAX_DEPTH)throw new IOException("Save tree too deep");
        for(String part:parts){if(part.isEmpty()||part.equals(".")||part.equals("..")||part.length()>240)throw new IOException("Invalid archive path");
            for(int i=0;i<part.length();i++)if(Character.isISOControl(part.charAt(i)))throw new IOException("Invalid archive path");}
        return name;
    }
    static Summary inspect(File dir)throws IOException {
        Summary s=new Summary();plainPath(dir);if(!dir.exists())return s;
        if(!dir.isDirectory())throw new IOException("Invalid save directory");walk(dir,"",s,new Paths());return s;
    }
    private static void walk(File dir,String prefix,Summary s,Paths paths)throws IOException {
        plainPath(dir);File[] files=dir.listFiles();if(files==null)throw new IOException("Cannot list saves");
        Arrays.sort(files,Comparator.comparing(File::getName));
        for(File file:files){plainPath(file);String name=valid(prefix+file.getName(),false);
            if(name.equalsIgnoreCase(MARKER))throw new IOException("Reserved archive marker in save tree");
            paths.add(name,file.isDirectory());
            if(file.isDirectory())walk(file,name+"/",s,paths);
            else{regular(file);long size=file.length();if(size>MAX_FILE||s.bytes+size>MAX_TOTAL||++s.files>MAX_FILES)throw new IOException("Save archive limits exceeded");
                s.bytes+=size;s.paths.add(name);if(name.toLowerCase(Locale.ROOT).endsWith(".d2s"))s.characters++;}
        }
    }
    static void zip(File dir,OutputStream raw,String scope)throws IOException {
        Summary s=inspect(dir);if(s.files==0)throw new IOException("No saves to export");
        try(ZipOutputStream zip=new ZipOutputStream(new BufferedOutputStream(raw))){
            zip.putNextEntry(new ZipEntry(MARKER));
            zip.write(("{\"format\":\"d2r-save\",\"version\":1,\"game_build\":94012,\"scope\":\""+scope+"\"}\n").getBytes(StandardCharsets.UTF_8));zip.closeEntry();
            byte[] buffer=new byte[65536];long total=0;
            for(String name:s.paths){File file=new File(dir,name);regular(file);zip.putNextEntry(new ZipEntry(name));long size=0;
                try(InputStream in=Files.newInputStream(file.toPath(),LinkOption.NOFOLLOW_LINKS)){int n;while((n=in.read(buffer))!=-1){size+=n;total+=n;if(size>MAX_FILE||total>MAX_TOTAL)throw new IOException("Save changed during export");zip.write(buffer,0,n);}}
                zip.closeEntry();}
        }
    }
    static Summary unzip(InputStream raw,File staging,String scope)throws IOException {return unzip(raw,staging,scope,null);}
    /* A bare .d2s (magic 55 AA 55 AA) is accepted next to ZIP archives: it is laid out as <name>/<name>.d2s.
     * 直接选 .d2s 也可导入，自动放成 <名>/<名>.d2s。 */
    static Summary unzip(InputStream input,File staging,String scope,File live)throws IOException {
        plainPath(staging);if(staging.exists())throw new IOException("Staging already exists");Files.createDirectories(staging.toPath());
        BufferedInputStream raw=new BufferedInputStream(input);raw.mark(8);byte[] head=new byte[4];int got=0;
        while(got<4){int n=raw.read(head,got,4-got);if(n<0)break;got+=n;}raw.reset();
        if(got==4&&(head[0]&255)==0x55&&(head[1]&255)==0xaa&&(head[2]&255)==0x55&&(head[3]&255)==0xaa){
            try{d2s(raw,staging,live);Summary result=inspect(staging);if(result.files==0)throw new IOException("Archive contains no saves");return result;}
            catch(IOException|RuntimeException ex){delete(staging);throw ex;}
        }
        Set<String> names=new HashSet<>();Paths paths=new Paths();long total=0;int entries=0;byte[] buffer=new byte[65536];
        try(ZipInputStream zip=new ZipInputStream(new BufferedInputStream(raw))){ZipEntry e;
            while((e=zip.getNextEntry())!=null){if(++entries>MAX_FILES*2)throw new IOException("Too many ZIP entries");
                String name=valid(e.getName(),e.isDirectory());
                if(!names.add(name.toLowerCase(Locale.ROOT)))throw new IOException("Duplicate archive path");
                paths.add(name,e.isDirectory());
                if(name.equals(MARKER)){
                    if(e.isDirectory())throw new IOException("Invalid archive marker");
                    ByteArrayOutputStream meta=new ByteArrayOutputStream();int n;
                    while((n=zip.read(buffer))!=-1){if(meta.size()+n>16384)throw new IOException("Oversized archive marker");meta.write(buffer,0,n);}
                    Matcher m=Pattern.compile("\"scope\"\\s*:\\s*\"([^\"]*)\"").matcher(new String(meta.toByteArray(),StandardCharsets.UTF_8));
                    if(m.find()&&!plainScope(scope).equals(plainScope(m.group(1))))throw new IOException("Archive belongs to a different save scope");
                    zip.closeEntry();continue;
                }
                File file=new File(staging,name);plainPath(file);
                if(e.isDirectory()){if(zip.read()!=-1)throw new IOException("Data in ZIP directory entry");Files.createDirectories(file.toPath());continue;}
                Files.createDirectories(file.getParentFile().toPath());long size=0;
                try(FileOutputStream out=new FileOutputStream(file)){int n;while((n=zip.read(buffer))!=-1){size+=n;total+=n;if(size>MAX_FILE||total>MAX_TOTAL)throw new IOException("Save archive limits exceeded");out.write(buffer,0,n);}out.getFD().sync();}
                zip.closeEntry();
            }
            Summary result=inspect(staging);if(result.files==0)throw new IOException("Archive contains no saves");return result;
        }catch(IOException|RuntimeException ex){delete(staging);throw ex;}
    }
    static final int D2S_NAME=299,D2S_NAME_BYTES=48;
    static long d2sChecksum(byte[] b){
        long sum=0;
        for(int i=0;i<b.length;i++){int v=(i>=12&&i<16)?0:b[i]&255;sum=(((sum<<1)|(sum>>>31))&0xffffffffL)+v&0xffffffffL;}
        return sum;
    }
    private static long le32(byte[] b,int at){return (b[at]&255L)|(b[at+1]&255L)<<8|(b[at+2]&255L)<<16|(b[at+3]&255L)<<24;}
    /* Letters plus one inner underscore survive (a hyphen becomes the underscore); the game asks to rename other
     * names, e.g. Chinese ones, and then stalls on this build.
     * 只保留英文字母和一个中间下划线（连字符转为下划线）。 */
    static final String[] CLASSES={"Amazon","Sorceress","Necromancer","Paladin","Barbarian","Druid","Assassin","Warlock"};
    static String characterName(String original,File live){return characterName(original,live,-1);}
    /* When nothing usable is left, the class name stands in (class byte at 0x18). */
    static String characterName(String original,File live,int characterClass){
        String letters=original.replace('-','_').replaceAll("[^A-Za-z_]","").replaceAll("_+","_").replaceAll("^_|_$","");
        int bar=letters.indexOf('_');if(bar>=0)letters=letters.substring(0,bar+1)+letters.substring(bar+1).replace("_","");
        if(letters.length()>15)letters=letters.substring(0,15).replaceAll("_$","");
        if(letters.replace("_","").length()<2)letters=characterClass>=0&&characterClass<CLASSES.length?CLASSES[characterClass]:"Hero";
        String name=letters;
        for(int i=0;i<26*26&&live!=null&&new File(live,name).exists();i++){
            String suffix=i<26?String.valueOf((char)('A'+i)):""+(char)('A'+(i/26-1))+(char)('A'+i%26);
            name=letters.substring(0,Math.min(letters.length(),15-suffix.length()))+suffix;
        }
        return name;
    }
    private static void d2s(InputStream in,File staging,File live)throws IOException {
        ByteArrayOutputStream all=new ByteArrayOutputStream();byte[] buffer=new byte[65536];int n;
        while((n=in.read(buffer))!=-1){if(all.size()+n>MAX_FILE)throw new IOException("Character file too large");all.write(buffer,0,n);}
        byte[] b=all.toByteArray();
        if(b.length<D2S_NAME+D2S_NAME_BYTES||le32(b,4)!=0x69||le32(b,8)!=b.length||le32(b,12)!=d2sChecksum(b))throw new IOException("Invalid character file");
        int end=D2S_NAME;while(end<D2S_NAME+D2S_NAME_BYTES&&b[end]!=0)end++;
        String original=new String(b,D2S_NAME,end-D2S_NAME,StandardCharsets.UTF_8),name=characterName(original,live,b[0x18]&255);
        if(!name.equals(original)){
            Arrays.fill(b,D2S_NAME,D2S_NAME+D2S_NAME_BYTES,(byte)0);byte[] ascii=name.getBytes(StandardCharsets.US_ASCII);System.arraycopy(ascii,0,b,D2S_NAME,ascii.length);
            long sum=d2sChecksum(b);for(int i=0;i<4;i++)b[12+i]=(byte)(sum>>>(8*i));
        }
        File dir=new File(staging,name);Files.createDirectories(dir.toPath());
        try(FileOutputStream out=new FileOutputStream(new File(dir,name+".d2s"))){out.write(b);out.getFD().sync();}
        try(FileOutputStream out=new FileOutputStream(new File(dir,name+".d2s.meta"))){
            out.write(("{\"last_modified_time\":"+System.currentTimeMillis()+"000000}").getBytes(StandardCharsets.UTF_8));out.getFD().sync();}
    }
    static Summary preview(File live,File imported)throws IOException {
        Summary summary=inspect(imported),existing=inspect(live);Paths paths=new Paths();Set<String> exact=new HashSet<>(existing.paths);
        for(String name:existing.paths)paths.add(name,false);
        for(String name:summary.paths)if(!exact.contains(name))paths.add(name,false);
        for(String name:summary.paths){File target=new File(live,name);plainPath(target);
            if(target.isDirectory())throw new IOException("Save file conflicts with directory");
            for(File parent=target.getParentFile();parent!=null&&!parent.equals(live);parent=parent.getParentFile())if(parent.exists()&&!parent.isDirectory())throw new IOException("Save directory conflicts with file");
            if(target.isFile())summary.conflicts++;
        }
        return summary;
    }
    private static File sibling(File live,String suffix){return new File(live.getParentFile(),live.getName()+suffix);}
    static File latestBackup(File files,String scope)throws IOException {
        if(!scope.matches("(debug-)?(base|mod-[a-z0-9_-]{1,48})|debug"))throw new IOException("Invalid backup scope");
        File dir=new File(files.getCanonicalFile(),"d2r-mobile/save-backups");plainPath(dir);if(!dir.exists())return null;
        File[] list=dir.listFiles();if(list==null)throw new IOException("Cannot list backups");File latest=null;
        for(File f:list)if(f.getName().startsWith(scope+"-")&&f.getName().matches(Pattern.quote(scope)+"-[0-9]+-[a-f0-9-]{36}\\.zip")){
            regular(f);if(latest==null||f.lastModified()>latest.lastModified())latest=f;
        }
        return latest;
    }
    static void recover(File live,Sync sync)throws IOException {
        plainPath(live);File journal=sibling(live,".transfer-pending"),old=sibling(live,".transfer-old"),next=sibling(live,".transfer-next");
        plainPath(journal);plainPath(old);plainPath(next);
        if(!journal.exists()){
            if(old.exists())throw new IOException("Unjournaled save recovery requires attention");
            delete(next);return;
        }
        regular(journal);
        if(!live.exists()&&old.exists()){Files.move(old.toPath(),live.toPath(),StandardCopyOption.ATOMIC_MOVE);sync.directory(live.getParentFile());}
        if(live.exists())inspect(live);
        delete(old);delete(next);Files.delete(journal.toPath());sync.directory(live.getParentFile());
    }
    static File merge(File live,File imported,File backups,String scope,Sync sync)throws IOException {
        recover(live,sync);preview(live,imported);File parent=live.getParentFile();LauncherSettingsStore.directory(parent);
        File next=sibling(live,".transfer-next"),old=sibling(live,".transfer-old"),journal=sibling(live,".transfer-pending");
        LauncherSettingsStore.directory(next);File backup=null;boolean hadLive=live.exists(),published=false;
        try{
            if(live.exists())copyTree(live,next,false);
            copyTree(imported,next,true);inspect(next);syncTree(next,sync);
            if(inspect(live).files>0){
                LauncherSettingsStore.directory(backups);backup=new File(backups,scope+"-"+System.currentTimeMillis()+"-"+UUID.randomUUID()+".zip");
                File partial=new File(backups,backup.getName()+".partial");
                try{
                    try(FileOutputStream out=new FileOutputStream(partial)){zip(live,new NonClosingOutput(out),scope);out.getFD().sync();}
                    Files.move(partial.toPath(),backup.toPath(),StandardCopyOption.ATOMIC_MOVE);sync.directory(backups);
                }finally{Files.deleteIfExists(partial.toPath());}
            }
            LauncherSettingsStore.write(journal,"save-transfer-v1\n");sync.directory(parent);
            if(live.exists()){Files.move(live.toPath(),old.toPath(),StandardCopyOption.ATOMIC_MOVE);sync.directory(parent);}
            Files.move(next.toPath(),live.toPath(),StandardCopyOption.ATOMIC_MOVE);published=true;sync.directory(parent);
        }catch(IOException|RuntimeException failure){
            /* 换入失败时优先恢复旧目录；备份 ZIP 始终保留。
             * Restore the old directory on a failed publication; retain the backup ZIP. */
            try{if(old.exists()){delete(live);Files.move(old.toPath(),live.toPath(),StandardCopyOption.ATOMIC_MOVE);sync.directory(parent);}
                else if(!hadLive&&published){delete(live);sync.directory(parent);}
                delete(next);Files.deleteIfExists(journal.toPath());sync.directory(parent);
            }catch(IOException recovery){failure.addSuppressed(recovery);}
            throw failure;
        }
        /* 发布持久化后不再回滚：清理失败保留 journal，下次启动再清理旧代。
         * Never roll back after durable publication: retain the journal on cleanup failure and retry next launch. */
        try{delete(old);Files.delete(journal.toPath());sync.directory(parent);}catch(IOException cleanup){System.err.println("Save transfer cleanup deferred: "+cleanup);}
        return backup;
    }
    private static void syncTree(File dir,Sync sync)throws IOException {
        File[] children=dir.listFiles();if(children==null)throw new IOException("Cannot list staged saves");
        for(File child:children)if(child.isDirectory())syncTree(child,sync);
        sync.directory(dir);
    }
    private static void copyTree(File from,File to,boolean replace)throws IOException {
        Summary s=inspect(from);byte[] b=new byte[65536];
        for(String name:s.paths){File src=new File(from,name),dst=new File(to,name);plainPath(dst);LauncherSettingsStore.directory(dst.getParentFile());
            if(!replace&&dst.exists())throw new IOException("Duplicate save copy");
            try(InputStream in=Files.newInputStream(src.toPath(),LinkOption.NOFOLLOW_LINKS);FileOutputStream out=new FileOutputStream(dst)){
                int n;while((n=in.read(b))!=-1)out.write(b,0,n);out.getFD().sync();}
        }
    }
    static void delete(File file)throws IOException {
        if(Files.isSymbolicLink(file.toPath()))throw new IOException("Refusing symbolic tree deletion");
        if(!file.exists())return;
        if(file.isDirectory()){File[] children=file.listFiles();if(children==null)throw new IOException("Cannot list temporary saves");for(File child:children)delete(child);}
        Files.delete(file.toPath());
    }
    static final class NonClosingOutput extends FilterOutputStream {
        NonClosingOutput(OutputStream out){super(out);}
        @Override public void close()throws IOException{flush();}
    }
}
