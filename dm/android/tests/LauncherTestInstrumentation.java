/* 模拟器独立 UID 上验证真实启动器、SAF 流、旋转重建与合成存档往返。
 * Verifies the real launcher, SAF streams, recreation and synthetic save transfers under an isolated emulator UID. */
package org.d2r.binaryprobe;
import android.app.*;
import android.content.*;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.*;
import android.widget.*;
import java.io.*;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.function.BooleanSupplier;
import java.util.zip.*;

public final class LauncherTestInstrumentation extends Instrumentation {
    private int checks;private InstallerActivity activity;
    private static final String PKG="org.d2r.launcher.qa";
    private void check(boolean ok,String message){checks++;if(!ok)throw new AssertionError(message);}
    private void main(Runnable action){runOnMainSync(action);waitForIdleSync();}
    private Object field(String name){try{Field f=InstallerActivity.class.getDeclaredField(name);f.setAccessible(true);return f.get(activity);}catch(Exception e){throw new AssertionError(e);}}
    private File file(String path){try{return new File(getTargetContext().getFilesDir().getCanonicalFile(),path);}catch(IOException e){throw new AssertionError(e);}}
    private void put(String path,String value)throws IOException {File f=file(path);Files.createDirectories(f.getParentFile().toPath());Files.write(f.toPath(),value.getBytes(StandardCharsets.UTF_8));}
    private String read(String path)throws IOException{return new String(Files.readAllBytes(file(path).toPath()),StandardCharsets.UTF_8);}
    private boolean visible(String name){android.view.View v=(android.view.View)field(name);android.graphics.Rect r=new android.graphics.Rect();return v.getGlobalVisibleRect(r)&&r.height()>=v.getHeight();}
    private void until(BooleanSupplier condition,String message)throws Exception {long end=SystemClock.elapsedRealtime()+20000;
        while(SystemClock.elapsedRealtime()<end){final boolean[] done={false};main(()->done[0]=condition.getAsBoolean());if(done[0])return;Thread.sleep(50);}throw new AssertionError(message);}
    private void screenshot(String name)throws Exception {waitForIdleSync();getUiAutomation().waitForIdle(700,5000);Bitmap b=getUiAutomation().takeScreenshot();try(OutputStream out=new FileOutputStream(file(name+".png"))){b.compress(Bitmap.CompressFormat.PNG,100,out);}b.recycle();}
    private void recreate(Integer orientation)throws Exception {ActivityMonitor m=addMonitor(InstallerActivity.class.getName(),null,false);
        main(()->{if(orientation==null)activity.recreate();else activity.setRequestedOrientation(orientation);});
        Activity next=waitForMonitorWithTimeout(m,15000);removeMonitor(m);check(next instanceof InstallerActivity,"activity recreated");activity=(InstallerActivity)next;waitForIdleSync();}
    private void select(String valueField,int index)throws Exception {main(()->{android.view.View row=(android.view.View)field(valueField);while(!row.isClickable())row=(android.view.View)row.getParent();row.performClick();});
        if(valueField.equals("qualityValue"))screenshot("launcher-choice");
        main(()->{AlertDialog d=(AlertDialog)field("choiceDialog");check(d!=null,"choice dialog opens");ListView list=d.getListView();list.performItemClick(null,index,list.getAdapter().getItemId(index));});}
    @Override public void onCreate(Bundle b){super.onCreate(b);start();}
    @Override public void onStart(){Bundle result=new Bundle();try{
        check(PKG.equals(getTargetContext().getPackageName()),"isolated test package only");
        for(String path:new String[]{"d2r-mobile","export-check","backup-check","backup-export-check","incoming","import.zip","export.zip","backup-export.zip","invalid.zip"})SaveArchive.delete(file(path));
        put("game.romfs","r");put("d2r-mobile/shaders/shader_index.bin","s");
        put("d2r-mobile/save/hero/hero.d2s","original-character");put("d2r-mobile/save/other/other.d2s","other-character");
        put("d2r-mobile/romfs-overlay-variants/item_notes/data/.build.config","abc");
        put("d2r-mobile/mods/demo/package.txt","d2r-mod-v1\nbuild db0507f36b84200ac05f75d76ed5026f\nbase a9993e364706816aba3e25717850c26c9cd0d89d\n");
        activity=(InstallerActivity)startActivitySync(new Intent(getTargetContext(),InstallerActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        OfflineInstallSession resources=OfflineInstallSession.get(getTargetContext());SaveTransferSession saves=SaveTransferSession.get(getTargetContext());
        until(()->resources.state.ready(),"synthetic resource install ready");
        String maps=new String(Files.readAllBytes(java.nio.file.Paths.get("/proc/self/maps")),StandardCharsets.UTF_8);
        check(!maps.contains("libd2r_mobile_android.so"),"launcher does not load native guest");
        select("qualityValue",3);check(new LauncherSettingsStore(getTargetContext().getFilesDir()).scale().equals("0.75"),"quality selection writes shared file");
        main(()->((Switch)field("fps")).performClick());check(new LauncherSettingsStore(getTargetContext().getFilesDir()).fps(),"FPS switch persists");
        select("driverValue",1);check(GraphicsDriverSettings.read(getTargetContext()).equals("system"),"driver selection persists");
        recreate(null);check(((Switch)field("fps")).isChecked(),"FPS survives recreation");
        select("modValue",1);check(new ModSelectionStore(getTargetContext().getFilesDir()).selected().equals("demo"),"MOD selection persists");
        check(((TextView)field("scopeView")).getText().toString().contains("demo"),"save destination follows MOD");select("modValue",0);
        main(()->check(visible("choose"),"start action visible in portrait"));
        main(()->check(saves.reserve("base"),"reserve export"));main(()->saves.export(Uri.parse("content://"+PKG+".saves/export.zip")));
        until(()->saves.phase==SaveTransferSession.Phase.DONE,"export completes");
        File unpack=file("export-check");try(InputStream in=new FileInputStream(file("export.zip"))){SaveArchive.Summary s=SaveArchive.unzip(in,unpack,"base");check(s.files==2&&s.characters==2,"provider export has complete saves");}
        File incoming=file("incoming");put("incoming/hero/hero.d2s","imported-character");put("incoming/new/new.d2s","new-character");
        try(OutputStream out=new FileOutputStream(file("import.zip"))){SaveArchive.zip(incoming,out,"base");}
        main(()->check(saves.reserve("base"),"reserve import"));main(()->saves.read(Uri.parse("content://"+PKG+".saves/import.zip")));
        until(()->saves.phase==SaveTransferSession.Phase.PREVIEW,"import preview");
        check(saves.summary.conflicts==1&&saves.summary.files==2,"preview conflict count");
        check(read("d2r-mobile/save/hero/hero.d2s").equals("original-character"),"preview never changes saves");
        main(()->check(!GameSessionGate.beginGuest(),"game cannot start during preview"));
        recreate(null);until(()->field("previewDialog")!=null,"preview dialog restored");
        main(()->((AlertDialog)field("previewDialog")).getButton(AlertDialog.BUTTON_POSITIVE).performClick());
        until(()->saves.phase==SaveTransferSession.Phase.DONE,"confirmed import completes");
        check(read("d2r-mobile/save/hero/hero.d2s").equals("imported-character"),"imported character published");
        check(read("d2r-mobile/save/other/other.d2s").equals("other-character"),"unrelated character retained");check(saves.backup!=null&&saves.backup.isFile(),"automatic backup retained");
        File before=file("backup-check");try(InputStream in=new FileInputStream(saves.backup)){SaveArchive.unzip(in,before,"base");}
        check(read("backup-check/hero/hero.d2s").equals("original-character"),"backup contains pre-import generation");
        main(()->{saves.latestBackup=null;saves.latestScope="";saves.refreshBackup("base");});
        until(()->saves.latestBackup!=null&&saves.latestScope.equals("base"),"backup rediscovered from disk");
        check(((Button)field("exportBackup")).getVisibility()==android.view.View.VISIBLE,"backup export entry visible");
        select("modValue",1);until(()->saves.latestScope.equals("mod-demo"),"MOD backup lookup completes");
        check(saves.latestBackup==null&&((Button)field("exportBackup")).getVisibility()==android.view.View.GONE,"base backup hidden in MOD scope");
        select("modValue",0);until(()->saves.latestScope.equals("base")&&saves.latestBackup!=null,"base backup restored after MOD switch");
        main(()->check(saves.reserveBackup("base"),"reserve automatic backup export"));
        main(()->saves.export(Uri.parse("content://"+PKG+".saves/backup-export.zip")));
        until(()->saves.phase==SaveTransferSession.Phase.DONE,"backup export completes");
        try(InputStream in=new FileInputStream(file("backup-export.zip"))){SaveArchive.unzip(in,file("backup-export-check"),"base");}
        check(read("backup-export-check/hero/hero.d2s").equals("original-character"),"backup export contains old generation");
        check(read("d2r-mobile/save/hero/hero.d2s").equals("imported-character"),"backup export leaves current generation intact");
        main(()->((ScrollView)((android.view.View)field("body")).getParent().getParent().getParent()).fullScroll(android.view.View.FOCUS_DOWN));
        until(()->visible("importSave")&&visible("exportBackup"),"save actions reachable by scrolling");
        screenshot("launcher-portrait");
        try(ZipOutputStream z=new ZipOutputStream(new FileOutputStream(file("invalid.zip")))){z.putNextEntry(new ZipEntry("../escape"));z.write(1);z.closeEntry();}
        main(()->check(saves.reserve("base"),"reserve invalid import"));main(()->saves.read(Uri.parse("content://"+PKG+".saves/invalid.zip")));
        until(()->saves.phase==SaveTransferSession.Phase.FAILED,"invalid import rejected");check(read("d2r-mobile/save/hero/hero.d2s").equals("imported-character"),"bad ZIP leaves save intact");
        main(()->check(saves.reserve("base"),"failure releases job"));main(saves::cancel);
        check(!GameSessionGate.guestStarted(),"launcher never starts native guest");
        recreate(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        until(()->activity.getResources().getConfiguration().orientation==android.content.res.Configuration.ORIENTATION_LANDSCAPE&&(Boolean)field("wide"),"landscape two-column layout drawn");
        main(()->check(visible("choose"),"start action visible in landscape"));
        screenshot("launcher-landscape");
        main(()->((Button)field("importSave")).performClick());
        until(()->saves.phase==SaveTransferSession.Phase.PICKING,"system picker starts");
        getUiAutomation().waitForIdle(500,5000);sendKeyDownUpSync(android.view.KeyEvent.KEYCODE_BACK);
        until(()->saves.phase==SaveTransferSession.Phase.IDLE,"system picker cancellation returns");
        check(read("d2r-mobile/save/hero/hero.d2s").equals("imported-character"),"picker cancellation leaves saves unchanged");
        result.putString("stream","LAUNCHER_ANDROID checks="+checks+" synthetic_only=1 device="+Build.MODEL+" passed=1\n");finish(Activity.RESULT_OK,result);
    }catch(Throwable error){android.util.Log.e("D2RLauncherQA","failed",error);result.putString("stream","LAUNCHER_ANDROID failed="+error+" checks="+checks+"\n");finish(Activity.RESULT_CANCELED,result);}}
}
