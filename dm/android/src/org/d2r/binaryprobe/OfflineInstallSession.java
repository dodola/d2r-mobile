package org.d2r.binaryprobe;

import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.*;

/** One process-owned import job. Never holds an Activity or asks for broad file access. */
final class OfflineInstallSession {
    interface Observer { void changed(); }
    private static OfflineInstallSession instance;
    static synchronized OfflineInstallSession get(Context context) {
        if(instance==null)instance=new OfflineInstallSession(context.getApplicationContext());return instance;
    }
    final Context context;
    private final Handler ui=new Handler(Looper.getMainLooper());
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private OfflinePackageInstaller.Manifest manifest;
    private Observer observer;
    final OfflineInstallState state=new OfflineInstallState();
    private volatile InputStream source;
    private long lastProgress;
    private int currentFile,fileCount;
    String title="正在检查游戏文件",detail="首次打开时会自动检查已安装的资源。";
    long done=0,total=0,requiredBytes=0;
    private OfflineInstallSession(Context context) {this.context=context;worker.execute(this::prepare);}
    void attach(Observer observer) {this.observer=observer;observer.changed();}
    void detach(Observer observer) {if(this.observer==observer)this.observer=null;}
    private void notifyObserver(){if(observer!=null)observer.changed();}
    private OfflinePackageInstaller installer()throws IOException {
        OfflinePackageInstaller.Progress listener=new OfflinePackageInstaller.Progress(){
            public void update(String phase,long current,long total){progress(phase,current,total);}
            public void file(int current,int count){currentFile=current;fileCount=count;}
        };
        return new OfflinePackageInstaller(context.getFilesDir(),manifest,listener,state::cancelled,()->context.getFilesDir().getUsableSpace());
    }
    /** The APK's pinned resource manifest; also used by the game to recognise a receipt-verified install. */
    static OfflinePackageInstaller.Manifest readManifest(Context context)throws Exception {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        try(InputStream in=context.getAssets().open("offline-manifest.json")) {
            byte[] b=new byte[16384];int n;
            while((n=in.read(b))!=-1){if(bytes.size()+n>OfflinePackageInstaller.MAX_MANIFEST)throw new IOException("Manifest too large");bytes.write(b,0,n);}
        }
        byte[] json=bytes.toByteArray();JSONObject data=new JSONObject(new String(json,StandardCharsets.UTF_8));
        if(data.getInt("format")!=1||data.getInt("game_build")!=OfflinePackageInstaller.GAME_BUILD)throw new IOException("Unsupported game build");
        JSONArray list=data.getJSONArray("files");ArrayList<OfflinePackageInstaller.Entry> entries=new ArrayList<>();
        for(int i=0;i<list.length();i++){JSONObject e=list.getJSONObject(i);entries.add(new OfflinePackageInstaller.Entry(e.getString("path"),e.getLong("bytes"),e.getString("sha256")));}
        OfflinePackageInstaller.Manifest result=new OfflinePackageInstaller.Manifest(json,entries);
        if(data.getLong("total_bytes")!=result.total)throw new IOException("Manifest size mismatch");
        return result;
    }
    private void loadManifest()throws Exception {manifest=readManifest(context);}
    private void prepare() {
        try {
            loadManifest();OfflinePackageInstaller installer=installer();
            if(installer.prepareExisting()){finishReady();return;}
            String text=installer.hasInterruptedImport()?"上次导入未完成。请重新选择完整资源包；已校验的文件会复用。":"请先把配套的离线资源包下载到手机，无需解压。";
            idle("准备离线游戏",text);
        }catch(Exception e){error(e);}
    }
    void start(Uri uri) {
        if(manifest==null||!state.beginImport())return;
        done=0;total=0;lastProgress=0;currentFile=0;fileCount=0;
        requiredBytes=0;
        title="正在打开资源包";detail="导入完成后，点击“启动游戏”进入。建议保持此页面打开。";notifyObserver();
        worker.execute(()->{
            try(InputStream in=context.getContentResolver().openInputStream(uri)) {
                if(in==null)throw new IOException("Cannot open selected document");source=in;
                installer().install(in);
            }catch(Exception e){error(e);return;}finally{source=null;}
            finishReady();
        });
    }
    void cancel() {
        if(!state.requestCancel())return;
        InputStream in=source;
        // A provider may block in read(). Closing from another thread unblocks it.
        if(in!=null)new Thread(()->{try{in.close();}catch(IOException ignored){}},"cancel-import").start();
        title="正在停止导入";detail="已校验的文件会保留，可重新选择资源包重试。";notifyObserver();
    }
    private void progress(String phase,long current,long max) {
        if("commit".equals(phase))state.beginCommit();
        long now=SystemClock.elapsedRealtime();
        if(!"commit".equals(phase)&&current!=max&&now-lastProgress<250)return;lastProgress=now;
        final String count="正在处理第 "+currentFile+" / "+fileCount+" 个文件。";
        ui.post(()->{
            if(state.cancelled()||(state.phase()==OfflineInstallState.Phase.COMMITTING&&!"commit".equals(phase)))return;
            done=current;total=max;
            title="commit".equals(phase)?"正在完成安装":"verify".equals(phase)?"正在校验已有文件":"正在导入并校验";
            detail="commit".equals(phase)?"校验已通过，正在保存安装结果。此时无法取消，请稍候。":count+"\n完成后请点击“启动游戏”。建议保持此页面打开。";
            notifyObserver();
        });
    }
    private void finishReady() {ui.post(()->{state.finishReady();title="游戏已准备好";detail="资源检查已完成。确认下方设置后即可启动，首次加载可能需要一些时间。";done=total;notifyObserver();});}
    private void idle(String heading,String text) {ui.post(()->{state.finishWaiting();title=heading;detail=text;done=0;total=0;notifyObserver();});}
    private void error(Exception exception) {
        String message;
        boolean stopped=state.cancelled();
        if(stopped)message="导入已停止。请重新选择资源包重试；已校验的文件会复用。";
        else if(exception instanceof OfflinePackageInstaller.Failure) {
            OfflinePackageInstaller.Failure failure=(OfflinePackageInstaller.Failure)exception;
            switch(failure.reason) {
                case "space": requiredBytes=failure.needed;message="手机存储空间不足。请清理空间后重新选择资源包。";break;
                case "format": message="这不是配套的离线资源包。请选择 D2R-Offline-94012.d2rpack 文件，无需解压。";break;
                case "version": message="资源包与当前安装的 APK 版本不匹配。请使用同一发布目录中的 APK 和资源包。";break;
                case "truncated": case "checksum": message="资源包不完整或校验失败。请重新下载完整资源包后再选择。";break;
                default: message="安装文件无法通过检查。请重新下载配套 APK 和资源包。";
            }
        }else if(manifest==null)message="APK 缺少有效的安装清单。请重新下载配套 APK 后覆盖安装。";
        else message="无法读取资源包或保存文件。请确认文件已完整下载到手机，并检查可用空间后重试。";
        android.util.Log.e("D2ROfflineInstall","Offline preparation failed",exception);
        final String text=message;
        ui.post(()->{state.finishFailed();title=stopped?"导入已停止":"暂时无法完成安装";detail=text;done=0;total=0;notifyObserver();});
    }
    long payloadBytes(){return manifest==null?0:manifest.total;}
    boolean canSelect(){return manifest!=null&&state.canSelect();}
}
