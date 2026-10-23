/* 进程拥有的存档任务：文件选择、预览确认与后台处理跨 Activity 重建保留。
 * Process-owned save jobs retain picker, preview and worker state across Activity recreation.
 * 只使用 SAF 文档流；Android fsync 目录保证提交边界，客体启动与传输互斥。
 * Uses SAF document streams; Android directory fsync defines commit boundaries, mutually exclusive with guest startup. */
package org.d2r.binaryprobe;

import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.system.*;
import java.io.*;
import java.util.concurrent.*;

final class SaveTransferSession {
    enum Phase {IDLE,PICKING,READING,PREVIEW,IMPORTING,EXPORTING,DONE,FAILED}
    private static SaveTransferSession instance;
    static synchronized SaveTransferSession get(Context c){if(instance==null)instance=new SaveTransferSession(c.getApplicationContext());return instance;}
    private final Context context;
    private final Handler ui=new Handler(Looper.getMainLooper());
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private Runnable observer;
    Phase phase=Phase.IDLE;
    String scope="base",messageKey="dmLaunchSaveHint";
    SaveArchive.Summary summary;
    File staging,backup,latestBackup;
    String latestScope="";
    private File exportSource;
    private int refreshGeneration;
    private SaveTransferSession(Context c){context=c;}
    void attach(Runnable r){observer=r;r.run();}
    void detach(Runnable r){if(observer==r)observer=null;}
    private void changed(){if(observer!=null)observer.run();}
    boolean locked(){return phase==Phase.PICKING||phase==Phase.READING||phase==Phase.PREVIEW||phase==Phase.IMPORTING||phase==Phase.EXPORTING;}
    boolean working(){return phase==Phase.READING||phase==Phase.IMPORTING||phase==Phase.EXPORTING;}
    boolean reserve(String selected){
        if(locked()||!GameSessionGate.beginTransfer())return false;
        scope=selected;phase=Phase.PICKING;summary=null;backup=null;exportSource=null;changed();return true;
    }
    void refreshBackup(String selected){if(locked())return;int generation=++refreshGeneration;
        worker.execute(()->{File found=null;try{found=SaveArchive.latestBackup(context.getFilesDir(),selected);}catch(IOException e){android.util.Log.w("D2RSave","backup list",e);}
            File result=found;ui.post(()->{if(generation==refreshGeneration){latestBackup=result;latestScope=selected;changed();}});
        });
    }
    boolean reserveBackup(String selected){File source=latestBackup;
        if(!selected.equals(latestScope)||source==null||!reserve(selected))return false;exportSource=source;return true;
    }
    void cancel(){if(phase!=Phase.PICKING&&phase!=Phase.PREVIEW)return;
        File abandoned=staging;staging=null;phase=Phase.IDLE;messageKey="dmLaunchCancelled";GameSessionGate.endTransfer();changed();
        if(abandoned!=null)worker.execute(()->{try{SaveArchive.delete(abandoned);}catch(IOException e){android.util.Log.w("D2RSave","staging cleanup",e);}});
    }
    static final SaveArchive.Sync SYNC=dir->{
        java.io.FileDescriptor fd=null;
        try{fd=Os.open(dir.getAbsolutePath(),OsConstants.O_RDONLY|OsConstants.O_NOFOLLOW|OsConstants.O_CLOEXEC,0);
            if(!OsConstants.S_ISDIR(Os.fstat(fd).st_mode))throw new IOException("Save sync path is not a directory");Os.fsync(fd);}
        catch(ErrnoException e){throw new IOException("Save directory sync failed",e);}
        finally{if(fd!=null)try{Os.close(fd);}catch(ErrnoException e){android.util.Log.w("D2RSave","directory close",e);}}
    };
    static void recover(Context c)throws IOException {
        File files=c.getFilesDir(),base=SaveArchive.target(files,"base");
        SaveArchive.recover(base,SYNC);
        for(String mod:new ModSelectionStore(files).installed())SaveArchive.recover(SaveArchive.target(files,"mod-"+mod),SYNC);
    }
    void read(Uri uri){if(phase!=Phase.PICKING)return;phase=Phase.READING;messageKey="dmLaunchReading";changed();final String targetScope=scope;
        worker.execute(()->{File unpack=null;
            try{
                recover(context);File parent=new File(context.getFilesDir().getCanonicalFile(),"d2r-mobile/save-transfer");LauncherSettingsStore.directory(parent);
                unpack=java.nio.file.Files.createTempDirectory(parent.toPath(),"import-").toFile();java.nio.file.Files.delete(unpack.toPath());
                try(InputStream in=context.getContentResolver().openInputStream(uri)){
                    if(in==null)throw new IOException("Document unavailable");SaveArchive.unzip(in,unpack,targetScope);}
                SaveArchive.Summary preview=SaveArchive.preview(SaveArchive.target(context.getFilesDir(),targetScope),unpack);File ready=unpack;
                ui.post(()->{staging=ready;summary=preview;phase=Phase.PREVIEW;changed();});
            }catch(Exception e){if(unpack!=null)try{SaveArchive.delete(unpack);}catch(IOException cleanup){e.addSuppressed(cleanup);}fail("dmLaunchImportFailed",e);}
        });
    }
    void confirm(){if(phase!=Phase.PREVIEW||staging==null)return;phase=Phase.IMPORTING;messageKey="dmLaunchImporting";changed();File source=staging;String targetScope=scope;
        worker.execute(()->{
            try{File saved=SaveArchive.merge(SaveArchive.target(context.getFilesDir(),targetScope),source,new File(context.getFilesDir().getCanonicalFile(),"d2r-mobile/save-backups"),targetScope,SYNC);
                try{SaveArchive.delete(source);}catch(IOException cleanup){android.util.Log.w("D2RSave","staging cleanup",cleanup);}
                ui.post(()->{staging=null;backup=saved;if(saved!=null){latestBackup=saved;latestScope=targetScope;}phase=Phase.DONE;messageKey="dmLaunchImported";GameSessionGate.endTransfer();changed();});
            }catch(Exception e){try{SaveArchive.delete(source);}catch(IOException cleanup){e.addSuppressed(cleanup);}fail("dmLaunchImportFailed",e);}
        });
    }
    void export(Uri uri){if(phase!=Phase.PICKING)return;phase=Phase.EXPORTING;messageKey="dmLaunchExporting";changed();String targetScope=scope;File previous=exportSource;
        worker.execute(()->{try{
            recover(context);File live=SaveArchive.target(context.getFilesDir(),targetScope);SaveArchive.Summary exported=previous==null?SaveArchive.inspect(live):null;
            if(exported!=null&&exported.files==0){ui.post(()->{phase=Phase.FAILED;messageKey="dmLaunchNoSaves";GameSessionGate.endTransfer();changed();});return;}
            try(OutputStream out=context.getContentResolver().openOutputStream(uri,"wt")){
                if(out==null)throw new IOException("Document unavailable");
                if(previous==null)SaveArchive.zip(live,out,targetScope);
                else{SaveArchive.plainPath(previous);try(InputStream in=java.nio.file.Files.newInputStream(previous.toPath(),java.nio.file.LinkOption.NOFOLLOW_LINKS)){
                    byte[] b=new byte[65536];long count=0;int n;while((n=in.read(b))!=-1){count+=n;if(count>SaveArchive.MAX_TOTAL+(2L<<20))throw new IOException("Oversized backup");out.write(b,0,n);}}}
            }
            ui.post(()->{summary=exported;phase=Phase.DONE;messageKey="dmLaunchExported";GameSessionGate.endTransfer();changed();});
        }catch(Exception e){fail("dmLaunchExportFailed",e);}});
    }
    void pickerFailed(){if(phase==Phase.PICKING){phase=Phase.FAILED;messageKey="dmLaunchPickerFailed";GameSessionGate.endTransfer();changed();}}
    private void fail(String key,Exception error){android.util.Log.e("D2RSave",key,error);
        ui.post(()->{staging=null;phase=Phase.FAILED;messageKey=key;GameSessionGate.endTransfer();changed();});
    }
}
