package org.d2r.binaryprobe;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.*;
import java.util.function.BooleanSupplier;
import java.util.function.LongSupplier;

/** Streaming, pinned-content installer. Independent of Activity and Android APIs. */
public final class OfflinePackageInstaller {
    public static final byte[] MAGIC="D2RPKG1\n".getBytes(StandardCharsets.US_ASCII);
    /** The only game build this installer and the APK's offline manifest are paired with. */
    public static final int GAME_BUILD=94012;
    public static final long RESERVE=256L*1024*1024;
    public static final int MAX_MANIFEST=4*1024*1024;
    public interface Progress {
        void update(String phase,long done,long total);
        default void file(int current,int count) {}
    }
    public static final class Failure extends IOException {
        public final String reason;
        public final long needed;
        Failure(String reason) { this(reason,0); }
        Failure(String reason,long needed) { super(reason);this.reason=reason;this.needed=needed; }
    }
    public static final class Entry {
        public final String path,hash;
        public final long bytes;
        public Entry(String path,long bytes,String hash) { this.path=path;this.bytes=bytes;this.hash=hash; }
    }
    public static final class Manifest {
        public final byte[] json;
        public final List<Entry> entries;
        public final String tag;
        public final long total;
        public Manifest(byte[] json,List<Entry> entries)throws IOException {
            if(json.length<2||json.length>MAX_MANIFEST||entries.size()<2||entries.size()>20000)throw new Failure("manifest");
            this.json=json.clone();this.entries=Collections.unmodifiableList(new ArrayList<>(entries));
            Set<String> names=new HashSet<>();long sum=0;
            for(Entry e:entries) {
                if(e.path==null||!e.path.matches("game\\.romfs|d2r-mobile/shaders/(shader_index\\.bin|[0-9a-f]{16}\\.(spv\\.gz|vkmeta))")||
                    !names.add(e.path)||e.bytes<0||e.bytes>32L*1024*1024*1024||e.hash==null||!e.hash.matches("[0-9a-f]{64}"))throw new Failure("manifest");
                sum+=e.bytes;if(sum>32L*1024*1024*1024)throw new Failure("manifest");
            }
            if(!names.contains("game.romfs")||!names.contains("d2r-mobile/shaders/shader_index.bin")||sum==0)throw new Failure("manifest");
            total=sum;tag=hex(digest().digest(json));
        }
    }
    private final File root,stage;
    private final Manifest manifest;
    private final Progress progress;
    private final BooleanSupplier cancelled;
    private final LongSupplier space;
    private final byte[] buffer=new byte[1024*1024];
    public OfflinePackageInstaller(File root,Manifest manifest,Progress progress,BooleanSupplier cancelled,LongSupplier space)throws IOException {
        this.root=root.getCanonicalFile();this.stage=new File(this.root,"d2r-offline-staging");
        this.manifest=manifest;this.progress=progress;this.cancelled=cancelled;this.space=space;
        checked(this.root,"d2r-offline-staging");
    }
    private void check()throws Failure { if(cancelled.getAsBoolean())throw new Failure("cancelled"); }
    private static MessageDigest digest() { try{return MessageDigest.getInstance("SHA-256");}catch(Exception e){throw new AssertionError(e);} }
    public static String hex(byte[] hash) { StringBuilder s=new StringBuilder();for(byte b:hash)s.append(String.format(Locale.ROOT,"%02x",b&255));return s.toString(); }
    private static File checked(File base,String path)throws IOException {
        File file=new File(base,path).getCanonicalFile();
        if(!file.getPath().startsWith(base.getCanonicalPath()+File.separator))throw new Failure("path");
        return file;
    }
    private static void mkdir(File dir)throws IOException { if(!dir.isDirectory()&&!dir.mkdirs())throw new IOException("Cannot create resource directory"); }
    private static void move(File from,File to)throws IOException {
        mkdir(to.getParentFile());Files.move(from.toPath(),to.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    private void exact(InputStream in,byte[] bytes,int count)throws IOException {
        int at=0;while(at<count){check();int n=in.read(bytes,at,count-at);if(n<0)throw new Failure("truncated");if(n==0)continue;at+=n;}
    }
    private boolean valid(File file,Entry entry,long before)throws IOException {
        if(!file.isFile()||file.length()!=entry.bytes)return false;
        MessageDigest md=digest();long done=0;
        try(InputStream in=new FileInputStream(file)) {
            int n;while((n=in.read(buffer))!=-1){check();done+=n;md.update(buffer,0,n);progress.update("verify",before+done,manifest.total);}
        }
        return done==entry.bytes&&hex(md.digest()).equals(entry.hash);
    }
    private boolean receiptMatches()throws IOException {
        File receipt=checked(root,"d2r-offline-ready");
        if(!receipt.isFile())return false;
        try(DataInputStream in=new DataInputStream(new FileInputStream(receipt))) {
            if(!in.readUTF().equals(manifest.tag)||in.readInt()!=manifest.entries.size())return false;
            for(Entry entry:manifest.entries) {
                check();File f=checked(root,entry.path);
                if(!f.isFile()||f.length()!=in.readLong()||f.lastModified()!=in.readLong()||f.length()!=entry.bytes)return false;
            }
            return in.read()==-1;
        }catch(IOException e){if(e instanceof Failure)throw e;return false;}
    }
    private void receipt()throws IOException {
        File temp=checked(root,"d2r-offline-ready.new");
        try(FileOutputStream file=new FileOutputStream(temp);DataOutputStream out=new DataOutputStream(file)) {
            out.writeUTF(manifest.tag);out.writeInt(manifest.entries.size());
            for(Entry entry:manifest.entries) {File f=checked(root,entry.path);out.writeLong(f.length());out.writeLong(f.lastModified());}
            out.flush();file.getFD().sync();
        }
        move(temp,checked(root,"d2r-offline-ready"));
    }
    /** True when a completed import's receipt covers this exact payload: same manifest, and every installed file
     * still has its imported length and modification time. Lets the game skip re-hashing a multi-GB image that the
     * installer already verified byte for byte. */
    public boolean receiptCovers(String path,long bytes,String hash)throws IOException {
        for(Entry entry:manifest.entries)if(entry.path.equals(path))return entry.bytes==bytes&&entry.hash.equals(hash)&&receiptMatches();
        return false;
    }
    public boolean hasInterruptedImport() { return stage.isDirectory(); }
    /** Recover a fully verified pending commit, or adopt a pre-existing installation. */
    public boolean prepareExisting()throws IOException {
        if(receiptMatches())return true;
        File commit=checked(stage,"commit");boolean recovering=false;
        if(commit.isFile()&&commit.length()==manifest.tag.length()) {
            byte[] text=new byte[manifest.tag.length()];try(InputStream in=new FileInputStream(commit)){exact(in,text,text.length);}
            recovering=manifest.tag.equals(new String(text,StandardCharsets.US_ASCII));
        }
        for(Entry entry:manifest.entries) {
            File target=checked(root,entry.path),pending=checked(stage,entry.path);
            if(!(target.isFile()&&target.length()==entry.bytes)&&!(recovering&&pending.isFile()&&pending.length()==entry.bytes))return false;
        }
        long done=0;boolean[] pending=new boolean[manifest.entries.size()];int index=0;
        for(Entry entry:manifest.entries) {
            progress.file(index+1,manifest.entries.size());
            boolean installed=valid(checked(root,entry.path),entry,done);
            pending[index++]=!installed;
            if(!installed&&!(recovering&&valid(checked(stage,entry.path),entry,done)))return false;
            done+=entry.bytes;
        }
        check();commit(pending);return true;
    }
    public void install(InputStream source)throws IOException {
        DataInputStream in=new DataInputStream(source);byte[] magic=new byte[MAGIC.length];exact(in,magic,magic.length);
        if(!Arrays.equals(magic,MAGIC))throw new Failure("format");
        int length;try{length=in.readInt();}catch(EOFException e){throw new Failure("truncated");}
        if(length<2||length>MAX_MANIFEST)throw new Failure("format");
        byte[] json=new byte[length];exact(in,json,length);
        if(!Arrays.equals(json,manifest.json))throw new Failure("version");
        mkdir(stage);
        // Never touch an installed resource until the entire input has been verified.
        boolean[] reused=new boolean[manifest.entries.size()],pending=new boolean[manifest.entries.size()];long need=RESERVE,done=0;
        for(int i=0;i<reused.length;i++) {
            Entry entry=manifest.entries.get(i);
            progress.file(i+1,reused.length);
            File part=checked(stage,entry.path+".part");
            if(part.exists()&&!part.delete())throw new IOException("Cannot discard incomplete resource file");
            boolean installed=valid(checked(root,entry.path),entry,done);
            reused[i]=installed||valid(checked(stage,entry.path),entry,done);pending[i]=!installed;
            if(!reused[i])need+=entry.bytes;done+=entry.bytes;
        }
        check();if(space.getAsLong()<need)throw new Failure("space",need);
        done=0;
        for(int i=0;i<reused.length;i++) {
            Entry entry=manifest.entries.get(i);File target=checked(stage,entry.path),part=checked(stage,entry.path+".part");
            progress.file(i+1,reused.length);
            mkdir(target.getParentFile());MessageDigest md=digest();long remaining=entry.bytes;
            FileOutputStream out=null;
            try {
                if(!reused[i])out=new FileOutputStream(part);
                while(remaining>0) {
                    check();int n=in.read(buffer,0,(int)Math.min(buffer.length,remaining));
                    if(n<0)throw new Failure("truncated");if(n==0)continue;
                    md.update(buffer,0,n);if(out!=null)out.write(buffer,0,n);remaining-=n;done+=n;
                    progress.update("import",done,manifest.total);
                }
                if(!hex(md.digest()).equals(entry.hash))throw new Failure("checksum");
                if(out!=null){out.getFD().sync();out.close();out=null;move(part,target);}
            }finally {try{if(out!=null)out.close();}finally{if(part.exists())part.delete();}}
        }
        check();if(in.read()!=-1)throw new Failure("format");
        // After this durable marker, restart can finish without asking for the pack again.
        File mark=checked(stage,"commit");
        try(FileOutputStream out=new FileOutputStream(mark)){out.write(manifest.tag.getBytes(StandardCharsets.US_ASCII));out.getFD().sync();}
        commit(pending);
    }
    private void commit(boolean[] usePending)throws IOException {
        progress.update("commit",manifest.total,manifest.total);
        int i=0;
        for(Entry entry:manifest.entries) {
            File pending=checked(stage,entry.path),target=checked(root,entry.path);
            if(usePending[i++])move(pending,target);
            else if(pending.isFile())pending.delete();
        }
        receipt();
        File mark=checked(stage,"commit");if(mark.exists())mark.delete();
        // Keep only empty directories; no recursive deletion can reach game saves.
        new File(stage,"d2r-mobile/shaders").delete();new File(stage,"d2r-mobile").delete();stage.delete();
    }
}
