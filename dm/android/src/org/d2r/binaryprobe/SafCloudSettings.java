package org.d2r.binaryprobe;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import org.json.JSONObject;

/** Owns the next-launch folder choice; a running provider is never replaced here. */
final class SafCloudSettings {
    private static final int REQUEST_FOLDER=0x5343;
    private static final int ACCESS=Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
    private final Activity activity;
    private final GameUiStrings strings;
    private final SharedPreferences preferences;
    private final Handler ui=new Handler(Looper.getMainLooper());
    private final ExecutorService worker=Executors.newSingleThreadExecutor(r -> new Thread(r,"save-folder-settings"));
    private final Object bindingLock=new Object(),grantLock=new Object();
    private final Selection startup;
    private volatile Selection desired;
    private volatile boolean startupAuthorized,desiredAuthorized,providerFailed,prepared,destroyed;
    private boolean resumed,editing,reopen,choiceChanged,menuChinese;
    private boolean nativeConfigured,nativeEnabled,nativeBusy;
    private long nativeSuccess;
    private int nativeError;
    private AlertDialog menu,dialog;

    SafCloudSettings(Activity activity,GameUiStrings strings){
        this.activity=activity;this.strings=strings;
        preferences=activity.getSharedPreferences("d2r-cloud-settings",Activity.MODE_PRIVATE);
        startup=Selection.read(preferences);desired=startup;
    }

    /** Called on game-files after module preparation and before any native start. */
    void prepareProvider(){
        File cache=new File(activity.getFilesDir(),"d2r-cloud-provider");
        SafCloudStore store=null;
        String identity=startup.providerId;
        try{
            synchronized(grantLock){cleanupRetiredGrants();}
            if(!cache.isDirectory()&&!cache.mkdirs())throw new IOException("Cannot create provider cache");
            if(startup.present()&&startup.valid()&&hasGrant(startup)){
                store=new SafCloudStore(activity.getApplicationContext(),startup.uri());
                if(!identity.equals(store.providerId()))throw new IOException("Folder identity changed");
                startupAuthorized=true;
            }
            synchronized(bindingLock){
                if(destroyed){if(store!=null)store.close();return;}
                int result=GameActivity.nativeCloudProvider(store,identity,cache.getAbsolutePath());
                if(result!=0)throw new IOException("Provider unavailable");
                // Native owns a successfully bound store through shutdown/flush.
                store=null;
            }
        }catch(Exception failure){
            providerFailed=true;
            if(store!=null)store.close();
            // A failed or absent SAF grant must never enable the old path provider.
            synchronized(bindingLock){if(!destroyed)GameActivity.nativeCloudProvider(null,identity,cache.getAbsolutePath());}
            android.util.Log.w("D2RCloud","Save folder unavailable at startup");
        }finally{
            desiredAuthorized=startupAuthorized;prepared=true;
            ui.post(this::refreshText);
        }
    }

    void showMenu(){
        if(!canShow()||editing)return;
        dismissDialogs();
        menuChinese=strings.chinese();
        menu=new AlertDialog.Builder(activity).setTitle(t("设置"))
            .setItems(new String[]{t("存档同步目录")},(d,which) -> showFolder())
            .setNegativeButton(t("关闭"),null).create();
        menu.setOnDismissListener(d -> menu=null);GameActivity.installDialogBack(menu);menu.show();
    }

    private void showFolder(){
        if(!canShow()){reopen=true;return;}
        dismissDialogs();
        Selection current=desired;
        AlertDialog.Builder builder=new AlertDialog.Builder(activity).setTitle(t("存档同步"))
            .setMessage(message()).setPositiveButton(t(current.present()?"重新选择":"选择目录"),(d,which) -> chooseFolder())
            .setNegativeButton(t("关闭"),null);
        if(current.present())builder.setNeutralButton(t("断开"),(d,which) -> disconnect());
        dialog=builder.create();dialog.setOnDismissListener(d -> dialog=null);GameActivity.installDialogBack(dialog);dialog.show();
    }

    private void chooseFolder(){
        if(editing||destroyed)return;
        Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(ACCESS|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION|Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        try{activity.startActivityForResult(intent,REQUEST_FOLDER);}
        catch(RuntimeException unavailable){toast("无法打开目录选择器");reopen=true;showFolder();}
    }

    boolean onActivityResult(int request,int result,Intent data){
        if(request!=REQUEST_FOLDER)return false;
        reopen=true;
        if(result!=Activity.RESULT_OK||data==null||data.getData()==null){reopenIfReady();return true;}
        Uri uri=data.getData();
        if((data.getFlags()&ACCESS)!=ACCESS){toast("需要目录的读写授权");reopenIfReady();return true;}
        editing=true;
        execute(() -> {
            Selection next=null;
            synchronized(grantLock){
                boolean acquired=false;
                Selection previous=desired;
                try{
                    next=Selection.from(uri);
                    boolean existed=hasAnyGrant(uri.toString());
                    activity.getContentResolver().takePersistableUriPermission(uri,ACCESS);
                    acquired=!existed;
                    if(!hasGrant(next))throw new IOException("Missing persisted read/write access");
                    Set<String> previousOwned=ownedGrants(),owned=new HashSet<>(previousOwned);
                    if(acquired)owned.add(uri.toString());
                    if(!next.write(preferences,owned)){
                        // Android updates its in-memory map even when disk commit fails.
                        previous.write(preferences,previousOwned);
                        throw new IOException("Cannot persist folder choice");
                    }
                    desired=next;
                    cleanupRetiredGrants();
                }catch(Exception denied){
                    if(acquired&&!startup.exactUri.equals(uri.toString())&&!previous.exactUri.equals(uri.toString()))release(uri.toString());
                    next=null;
                }
            }
            final Selection selected=next;
            ui.post(() -> {
                if(destroyed)return;
                editing=false;
                if(selected==null)toast("无法保存目录授权");
                else{desired=selected;desiredAuthorized=true;choiceChanged=true;}
                reopenIfReady();
            });
        });
        return true;
    }

    private void disconnect(){
        if(editing||destroyed)return;
        editing=true;reopen=true;
        execute(() -> {
            boolean saved;
            synchronized(grantLock){
                Selection previous=desired;Set<String> owned=ownedGrants();
                saved=Selection.empty().write(preferences,owned);
                if(saved){desired=Selection.empty();cleanupRetiredGrants();}
                else previous.write(preferences,owned);
            }
            // Retain the OS grant until the running native provider has shut down.
            ui.post(() -> {
                if(destroyed)return;
                editing=false;
                if(saved){desired=Selection.empty();desiredAuthorized=false;choiceChanged=true;}
                else toast("无法保存目录设置");
                reopenIfReady();
            });
        });
    }

    void onResume(){
        resumed=true;reopenIfReady();
        final Selection checked=desired;
        execute(() -> {
            boolean ready=prepared;
            boolean active=ready&&startup.valid()&&hasGrant(startup);
            boolean selected=checked.valid()&&hasGrant(checked);
            ui.post(() -> {
                if(destroyed)return;
                if(ready)startupAuthorized=active;
                if(desired.same(checked))desiredAuthorized=selected;
                refreshText();
            });
        });
    }

    void onPause(){resumed=false;}

    void close(){
        synchronized(bindingLock){destroyed=true;}
        dismissDialogs();worker.shutdownNow();
        // Do not close the native-owned store: final save publication may still run.
    }

    void updateNativeStatus(String raw){
        if(raw==null||destroyed)return;
        try{
            JSONObject state=new JSONObject(raw);
            if(!state.optBoolean("available",true))return;
            nativeConfigured=state.optBoolean("configured");nativeEnabled=state.optBoolean("enabled");
            nativeBusy=state.optBoolean("busy");nativeError=state.optInt("error");
            nativeSuccess=state.optLong("successful_attempts");refreshText();
        }catch(Exception ignored){/* A status refresh must not interrupt local gameplay. */}
    }

    void refreshText(){
        if(destroyed)return;
        if(dialog!=null&&dialog.isShowing()){
            dialog.setTitle(t("存档同步"));dialog.setMessage(message());
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setText(t(desired.present()?"重新选择":"选择目录"));
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setText(t("关闭"));
            if(desired.present())dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setText(t("断开"));
        }
        if(menu!=null&&menu.isShowing()&&menuChinese!=strings.chinese())showMenu();
    }

    private String message(){
        String state;
        if(restartRequired())state="待重启";
        else if(!desired.present())state="未授权";
        else if(!desired.valid()||prepared&&(!desiredAuthorized||!startupAuthorized))state="授权已失效";
        else if(providerFailed||nativeError!=0)state="同步失败";
        else state="已连接";
        String message=t("是否传输由游戏「自定义」中的存档同步开关控制。")+"\n\n"+t(state);
        if(restartRequired())message+="\n"+t(desired.present()?"所选目录将在下次启动游戏时用于同步。":"将在下次启动游戏时断开目录。");
        else if("已连接".equals(state)&&nativeConfigured&&nativeEnabled&&nativeBusy)message+="\n"+t("正在同步到所选目录…");
        else if("已连接".equals(state)&&nativeConfigured&&nativeSuccess>0&&nativeError==0)message+="\n"+t("已同步到所选目录");
        return message;
    }

    private boolean hasGrant(Selection selection){
        if(!selection.valid())return false;
        try{
            for(UriPermission permission:activity.getContentResolver().getPersistedUriPermissions())
                if(selection.exactUri.equals(permission.getUri().toString())&&permission.isReadPermission()&&permission.isWritePermission())return true;
        }catch(RuntimeException denied){return false;}
        return false;
    }

    private boolean hasAnyGrant(String exactUri){
        for(UriPermission permission:activity.getContentResolver().getPersistedUriPermissions())
            if(exactUri.equals(permission.getUri().toString()))return true;
        return false;
    }
    private Set<String> ownedGrants(){
        return new HashSet<>(preferences.getStringSet("owned_grants",java.util.Collections.emptySet()));
    }
    private boolean release(String exactUri){
        try{
            if(hasAnyGrant(exactUri))activity.getContentResolver().releasePersistableUriPermission(Uri.parse(exactUri),ACCESS);
            return true;
        }catch(RuntimeException denied){return false;}
    }
    /** Only grants first acquired by this setting can be retired here. */
    private void cleanupRetiredGrants(){
        Set<String> owned=ownedGrants();boolean changed=false;
        for(String uri:new HashSet<>(owned))
            if(!uri.equals(startup.exactUri)&&!uri.equals(desired.exactUri)&&release(uri)){
                owned.remove(uri);changed=true;
            }
        if(changed)preferences.edit().putStringSet("owned_grants",owned).commit();
    }

    private void execute(Runnable work){
        if(destroyed)return;
        try{worker.execute(work);}catch(RejectedExecutionException ignored){}
    }
    private boolean canShow(){return resumed&&!destroyed&&!activity.isFinishing()&&!activity.isDestroyed();}
    private boolean restartRequired(){return choiceChanged||!desired.same(startup);}
    private void reopenIfReady(){if(reopen&&!editing&&canShow()){reopen=false;showFolder();}}
    private void toast(String text){if(!destroyed)Toast.makeText(activity,t(text),Toast.LENGTH_LONG).show();}
    private String t(String text){return strings.text(text);}
    private void dismissDialogs(){
        if(menu!=null){menu.dismiss();menu=null;}
        if(dialog!=null){dialog.dismiss();dialog=null;}
    }

    private static final class Selection {
        final String exactUri,authority,documentId,providerId;
        Selection(String uri,String authority,String documentId,String providerId){
            this.exactUri=uri;this.authority=authority;this.documentId=documentId;this.providerId=providerId;
        }
        static Selection empty(){return new Selection("","","","");}
        static Selection read(SharedPreferences preferences){
            return new Selection(preferences.getString("tree_uri",""),preferences.getString("tree_authority",""),
                preferences.getString("tree_document_id",""),preferences.getString("provider_id",""));
        }
        static Selection from(Uri uri)throws IOException{
            if(uri==null||!"content".equals(uri.getScheme())||uri.getAuthority()==null||uri.getAuthority().isEmpty()||!DocumentsContract.isTreeUri(uri))
                throw new IOException("Invalid document tree");
            try{return new Selection(uri.toString(),uri.getAuthority(),DocumentsContract.getTreeDocumentId(uri),SafCloudStore.providerId(uri));}
            catch(IllegalArgumentException invalid){throw new IOException("Invalid document tree",invalid);}
        }
        boolean present(){return !exactUri.isEmpty();}
        Uri uri(){return Uri.parse(exactUri);}
        boolean valid(){
            if(!present())return false;
            try{
                Selection canonical=from(uri());
                return authority.equals(canonical.authority)&&documentId.equals(canonical.documentId)&&providerId.equals(canonical.providerId);
            }catch(Exception invalid){return false;}
        }
        boolean same(Selection other){return exactUri.equals(other.exactUri)&&authority.equals(other.authority)&&documentId.equals(other.documentId)&&providerId.equals(other.providerId);}
        boolean write(SharedPreferences preferences,Set<String> owned){
            return preferences.edit().putString("tree_uri",exactUri).putString("tree_authority",authority)
                .putString("tree_document_id",documentId).putString("provider_id",providerId).putStringSet("owned_grants",owned).commit();
        }
    }
}
