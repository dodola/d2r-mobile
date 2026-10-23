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
    static File target(File files,String scope)throws IOException {
        File root=new File(files.getCanonicalFile(),"d2r-mobile");
        if("base".equals(scope))return new File(root,"save");
        if(!scope.startsWith("mod-")||!ModSelectionStore.valid(scope.substring(4)))throw new IOException("Invalid save scope");
        File mod=new File(root,"mods/"+scope.substring(4));
        regular(new File(mod,"package.txt"));return new File(mod,"save");
    }
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
    static Summary unzip(InputStream raw,File staging,String scope)throws IOException {
        plainPath(staging);if(staging.exists())throw new IOException("Staging already exists");Files.createDirectories(staging.toPath());
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
                    if(m.find()&&!scope.equals(m.group(1)))throw new IOException("Archive belongs to a different save scope");
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
        if(!scope.matches("base|mod-[a-z0-9_-]{1,48}"))throw new IOException("Invalid backup scope");
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
