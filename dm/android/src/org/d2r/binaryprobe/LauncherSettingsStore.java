/* 启动器沿用游戏内设置的文件，原子更新自己拥有的键并保留其他配置。
 * The launcher shares the in-game settings files, atomically updating only its own keys.
 * 依据 Android render_scale 和 dm_custom 的既有协议。
 * Based on the existing Android render_scale and dm_custom protocols. */
package org.d2r.binaryprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

final class LauncherSettingsStore {
    static final String[] SCALES={"auto","1.0","0.9","0.75"};
    private final File files;
    LauncherSettingsStore(File files){this.files=files;}
    /* Android 的 /data/user/0 是应用命名空间内的系统别名；只规范化可信根，其下仍拒绝链接。
     * Android aliases /data/user/0 within app namespaces; canonicalize only the trusted root, rejecting links below it. */
    private File root()throws IOException{return new File(files.getCanonicalFile(),"d2r-mobile");}
    static void directory(File dir)throws IOException {
        Path path=dir.toPath().toAbsolutePath();
        for(Path p=path;p!=null;p=p.getParent())if(Files.isSymbolicLink(p))throw new IOException("Symbolic link in settings path: "+p);
        Files.createDirectories(path);
        if(!Files.isDirectory(path,LinkOption.NOFOLLOW_LINKS))throw new IOException("Directory unavailable");
    }
    static String read(File file)throws IOException {
        for(Path path=file.toPath().toAbsolutePath();path!=null;path=path.getParent())if(Files.isSymbolicLink(path))throw new IOException("Symbolic settings path");
        if(Files.isSymbolicLink(file.toPath()))throw new IOException("Symbolic settings file");
        if(!file.exists())return "";
        if(!file.isFile()||file.length()>65536)throw new IOException("Invalid settings file");
        try(InputStream in=Files.newInputStream(file.toPath(),LinkOption.NOFOLLOW_LINKS)){
            ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] b=new byte[4096];int n;
            while((n=in.read(b))!=-1){if(out.size()+n>65536)throw new IOException("Settings too large");out.write(b,0,n);}
            return new String(out.toByteArray(),StandardCharsets.UTF_8);
        }
    }
    static void write(File file,String text)throws IOException {
        directory(file.getParentFile());if(Files.isSymbolicLink(file.toPath()))throw new IOException("Symbolic settings file");
        Path temp=Files.createTempFile(file.getParentFile().toPath(),".launcher-",".tmp");
        try{
            try(FileOutputStream out=new FileOutputStream(temp.toFile())){out.write(text.getBytes(StandardCharsets.UTF_8));out.getFD().sync();}
            Files.move(temp,file.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        }finally{Files.deleteIfExists(temp);}
    }
    String scale()throws IOException {
        String value=read(new File(root(),"diagnostics/render-scale.txt")).trim();
        if(value.isEmpty()||value.equals("auto"))return "auto";
        try{float f=Float.parseFloat(value);if(Float.isFinite(f)&&f>=0.5f&&f<=1f)return value;}
        catch(NumberFormatException ignored){}
        throw new IOException("Invalid render scale");
    }
    void scale(int index)throws IOException {
        if(index<0||index>=SCALES.length)throw new IOException("Invalid quality preset");
        write(new File(root(),"diagnostics/render-scale.txt"),SCALES[index]+"\n");
    }
    boolean fps()throws IOException {
        boolean result=false;
        for(String line:read(new File(root(),"sw_custom.txt")).split("\n"))if(line.startsWith("show_fps="))result=line.substring(9).trim().equals("1");
        return result;
    }
    void fps(boolean on)throws IOException {
        File file=new File(root(),"sw_custom.txt");StringBuilder text=new StringBuilder();
        for(String line:read(file).split("\n"))if(!line.startsWith("show_fps=")&&!line.isEmpty())text.append(line).append('\n');
        text.append("show_fps=").append(on?1:0).append('\n');write(file,text.toString());
    }
}
