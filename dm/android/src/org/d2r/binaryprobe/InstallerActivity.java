/* 启动前设置与存档入口；交互参考 sydeEvans/d2r-android 5d8e322（MIT）。
 * Pre-launch settings and saves; interaction inspired by sydeEvans/d2r-android 5d8e322 (MIT).
 * 保留离线安装状态机，参数复用游戏内协议，文件任务由进程会话拥有。
 * Retains the installer state machine, shares in-game settings and gives file jobs process ownership. */
package org.d2r.binaryprobe;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.util.*;

public final class InstallerActivity extends Activity {
    private static final int PICK_PACKAGE=917,EXPORT_SAVE=918,IMPORT_SAVE=919;
    private static final int GOLD=0xffd4b17b,MUTED=0xffa9a49b,INK=0xff101114,TEXT=0xffeee8dd,ERROR=0xffffb5a7;
    private OfflineInstallSession session;
    private SaveTransferSession saves;
    private LauncherSettingsStore store;
    private LauncherStrings strings;
    private final OfflineInstallSession.Observer observer=this::render;
    private final Runnable saveObserver=this::renderSaves;
    private TextView heading,message,amount,feedback,driverValue,qualityValue,modValue,scopeView,saveStatus;
    private ProgressBar progress,saveProgress;
    private Button choose,cancel,exportSave,importSave,exportBackup;
    private Switch fps;
    private LinearLayout settings,body,resourceCard,optionsColumn;
    private AlertDialog cancelDialog,choiceDialog,previewDialog;
    private boolean resumed,openingGame,picking,binding,modValid,steppedAside,wide,layoutBound;
    private String modId="";
    private String t(String key){return strings.text("dmLaunch"+key);}
    private String f(String key,Object... args){return strings.format("dmLaunch"+key,args);}
    private int dp(int n){return Math.round(n*getResources().getDisplayMetrics().density);}
    private TextView text(String value,int size,int color,boolean bold){
        TextView v=new TextView(this);v.setText(value);v.setTextColor(color);v.setTextSize(size);v.setLineSpacing(dp(3),1);
        if(bold)v.setTypeface(Typeface.DEFAULT,Typeface.BOLD);return v;
    }
    private LinearLayout column(){LinearLayout v=new LinearLayout(this);v.setOrientation(LinearLayout.VERTICAL);return v;}
    private void add(LinearLayout parent,View v,int top){LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);p.topMargin=dp(top);parent.addView(v,p);}
    private GradientDrawable background(int color,int border,int radius){GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(dp(radius));if(border!=0)d.setStroke(dp(1),border);return d;}
    private LinearLayout card(String title){LinearLayout c=column();c.setPadding(dp(20),dp(20),dp(20),dp(20));c.setBackground(background(0xff1b1c20,0xff39342c,18));
        TextView h=text(title,19,GOLD,true);h.setAccessibilityHeading(true);add(c,h,0);return c;}
    private Button button(String label,boolean primary){Button b=new Button(this);b.setText(label);b.setTextSize(16);b.setAllCaps(false);b.setMinHeight(dp(52));
        b.setPadding(dp(12),dp(8),dp(12),dp(8));b.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{}},new int[]{MUTED,primary?INK:TEXT}));
        b.setBackgroundTintList(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{android.R.attr.state_pressed},new int[]{}},new int[]{0xff343438,primary?0xffedcd9b:0xff49433a,primary?GOLD:0xff302e2a}));return b;}
    private TextView row(LinearLayout parent,String title,Runnable action){LinearLayout r=column();r.setPadding(dp(4),dp(12),dp(4),dp(12));r.setMinimumHeight(dp(64));
        add(r,text(title,14,MUTED,false),0);TextView value=text("",17,TEXT,true);add(r,value,4);r.setFocusable(true);r.setClickable(true);
        android.util.TypedValue attr=new android.util.TypedValue();getTheme().resolveAttribute(android.R.attr.selectableItemBackground,attr,true);r.setBackgroundResource(attr.resourceId);
        r.setOnClickListener(v->action.run());add(parent,r,4);return value;}
    @Override public void onCreate(Bundle state){super.onCreate(state);
        strings=new LauncherStrings(getResources().getConfiguration().getLocales().get(0));
        if(GameSessionGate.guestRunning()){
            steppedAside=true;startActivity(new Intent(this,GameActivity.class).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));finish();return;
        }
        session=OfflineInstallSession.get(this);saves=SaveTransferSession.get(this);store=new LauncherSettingsStore(getFilesDir());
        picking=state!=null&&state.getBoolean("picking");
        getWindow().setStatusBarColor(INK);getWindow().setNavigationBarColor(INK);
        LinearLayout screen=column();screen.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,new int[]{0xff222029,INK,0xff1b1712}));
        screen.setOnApplyWindowInsetsListener((v,i)->{v.setPadding(i.getSystemWindowInsetLeft(),i.getSystemWindowInsetTop(),i.getSystemWindowInsetRight(),i.getSystemWindowInsetBottom());return i;});
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(true);screen.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
        LinearLayout center=column();center.setGravity(Gravity.CENTER_HORIZONTAL);center.setPadding(dp(20),dp(28),dp(20),dp(28));scroll.addView(center);
        LinearLayout content=column();center.addView(content,new LinearLayout.LayoutParams(-1,-2));
        add(content,text("D2R MOBILE",14,GOLD,true),0);TextView title=text(t("Title"),30,TEXT,true);title.setAccessibilityHeading(true);add(content,title,10);
        add(content,text(t("Subtitle"),15,MUTED,false),8);
        body=column();add(content,body,24);resourceCard=card(t("Resources"));optionsColumn=column();
        body.addView(resourceCard,new LinearLayout.LayoutParams(-1,-2));body.addView(optionsColumn,new LinearLayout.LayoutParams(-1,-2));
        center.addOnLayoutChangeListener((v,l,tp,r,b,ol,ot,or,ob)->{
            int w=Math.min(dp(960),Math.max(0,r-l-center.getPaddingLeft()-center.getPaddingRight()));
            if(content.getLayoutParams().width!=w){content.getLayoutParams().width=w;content.requestLayout();}
            boolean next=w>=dp(720);if(next!=wide||!layoutBound){wide=next;layoutBound=true;body.setOrientation(wide?LinearLayout.HORIZONTAL:LinearLayout.VERTICAL);
                LinearLayout.LayoutParams a=new LinearLayout.LayoutParams(wide?0:-1,-2,wide?0.9f:0),o=new LinearLayout.LayoutParams(wide?0:-1,-2,wide?1.1f:0);
                o.leftMargin=wide?dp(18):0;o.topMargin=wide?0:dp(18);resourceCard.setLayoutParams(a);optionsColumn.setLayoutParams(o);}
        });
        heading=text(session.title,22,TEXT,true);heading.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);add(resourceCard,heading,20);
        message=text(t("ResourceHint"),15,MUTED,false);add(resourceCard,message,10);
        progress=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);progress.setMax(1000);progress.setProgressTintList(ColorStateList.valueOf(GOLD));progress.setIndeterminateTintList(ColorStateList.valueOf(GOLD));add(resourceCard,progress,16);
        amount=text("",13,MUTED,false);add(resourceCard,amount,12);
        choose=button(t("PickPackage"),true);choose.setOnClickListener(v->{if(session.state.ready())openGame();else pickPackage();});
        cancel=button(t("Stop"),false);cancel.setOnClickListener(v->confirmCancel());add(resourceCard,cancel,4);
        Button help=button(t("Help"),false);help.setOnClickListener(v->new AlertDialog.Builder(this).setTitle(t("Help")).setMessage(t("HelpBody")).setPositiveButton(android.R.string.ok,null).show());add(resourceCard,help,16);
        LinearLayout graphics=card(t("Display"));optionsColumn.addView(graphics,new LinearLayout.LayoutParams(-1,-2));settings=column();add(graphics,settings,8);
        qualityValue=row(settings,t("Quality"),this::chooseQuality);driverValue=row(settings,t("Driver"),this::chooseDriver);
        fps=new Switch(this);fps.setText(t("Fps"));fps.setContentDescription(t("FpsHint"));fps.setTextSize(16);fps.setTextColor(TEXT);fps.setMinHeight(dp(56));fps.setThumbTintList(ColorStateList.valueOf(GOLD));add(settings,fps,4);
        fps.setOnCheckedChangeListener((v,on)->{if(binding)return;if(!canEdit()){bindSettings();return;}try{store.fps(on);showFeedback(t("Saved"),false);}catch(IOException e){settingsError(e);}bindSettings();});
        modValue=row(settings,t("Mod"),this::chooseMod);
        LinearLayout saveCard=card(t("Saves"));add(optionsColumn,saveCard,18);scopeView=text("",15,TEXT,true);add(saveCard,scopeView,14);
        add(saveCard,text(t("SaveScopeHint"),13,MUTED,false),6);
        LinearLayout actions=new LinearLayout(this);exportSave=button(t("Export"),false);importSave=button(t("Import"),false);
        LinearLayout.LayoutParams left=new LinearLayout.LayoutParams(0,-2,1),right=new LinearLayout.LayoutParams(0,-2,1);right.leftMargin=dp(8);actions.addView(exportSave,left);actions.addView(importSave,right);add(saveCard,actions,14);
        exportSave.setOnClickListener(v->pickSave(false));importSave.setOnClickListener(v->pickSave(true));
        exportBackup=button(t("ExportBackup"),false);exportBackup.setOnClickListener(v->pickBackup());add(saveCard,exportBackup,6);
        saveProgress=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);saveProgress.setIndeterminate(true);saveProgress.setIndeterminateTintList(ColorStateList.valueOf(GOLD));add(saveCard,saveProgress,12);
        saveStatus=text(t("SaveHint"),14,MUTED,false);saveStatus.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);add(saveCard,saveStatus,10);
        feedback=text("",14,GOLD,false);feedback.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);add(content,feedback,14);
        try{add(content,text(f("Version",getPackageManager().getPackageInfo(getPackageName(),0).versionName),12,MUTED,false),18);}catch(Exception ignored){}
        LinearLayout dock=column();dock.setPadding(dp(20),dp(8),dp(20),dp(8));dock.setBackgroundColor(INK);add(dock,choose,0);screen.addView(dock,new LinearLayout.LayoutParams(-1,-2));
        setContentView(screen);bindSettings();render();
        if(state!=null&&state.getString("feedback")!=null)showFeedback(state.getString("feedback"),state.getBoolean("feedbackError"));
    }
    private boolean canEdit(){if(GameSessionGate.guestStarted()){showFeedback(t("GameRunning"),true);return false;}
        if(saves.locked()||session.state.busy()||picking||openingGame){showFeedback(t("Busy"),false);return false;}return true;}
    private void settingsError(Exception e){android.util.Log.w("D2RLauncher","settings",e);showFeedback(t("SettingsFailed"),true);}
    private String driverName(String mode){return t(GraphicsDriverPolicy.SYSTEM.equals(mode)?"System":GraphicsDriverPolicy.TURNIP.equals(mode)?"Turnip":"AutoDriver");}
    private String scope(){return modId.isEmpty()?"base":"mod-"+modId;}
    private String scopeName(String scope){return scope.equals("base")?t("Base"):scope.substring(4);}
    private String qualityName(String value){for(int i=0;i<LauncherSettingsStore.SCALES.length;i++)if(value.equals(LauncherSettingsStore.SCALES[i]))return qualityNames()[i];return f("QualityCustom",value);}
    private String[] qualityNames(){return new String[]{t("QualityAuto"),t("QualityFull"),t("QualityBalanced"),t("QualityFast")};}
    private void bindSettings(){if(steppedAside)return;binding=true;
        try{qualityValue.setText(qualityName(store.scale())+"  ›");fps.setChecked(store.fps());}
        catch(IOException e){qualityValue.setText(t("ConfigInvalid"));showFeedback(t("ConfigInvalid"),true);}
        driverValue.setText(driverName(GraphicsDriverSettings.read(this))+"  ›");
        try{ModSelectionStore mods=new ModSelectionStore(getFilesDir());modId=mods.selected();modValid=modId.isEmpty()||mods.installed().contains(modId);}
        catch(IOException e){modValid=false;}
        modValue.setText((modValid?(modId.isEmpty()?t("NoMod"):modId):t("ModMissing"))+"  ›");modValue.setTextColor(modValid?TEXT:ERROR);
        scopeView.setText(modValid?f("Scope",scopeName(scope())):t("ModMissing"));binding=false;
    }
    private void choice(String title,String hint,String[] labels,int selected,java.util.function.IntConsumer apply){
        if(!canEdit()||choiceDialog!=null)return;
        choiceDialog=new AlertDialog.Builder(this).setTitle(title).setSingleChoiceItems(labels,selected,(d,i)->{apply.accept(i);d.dismiss();})
            .setNeutralButton(t("More"),(d,w)->new AlertDialog.Builder(this).setTitle(title).setMessage(hint).setPositiveButton(android.R.string.ok,null).show()).setNegativeButton(t("Cancel"),null).create();
        choiceDialog.setOnDismissListener(d->choiceDialog=null);choiceDialog.show();
    }
    private void chooseQuality(){int selected=-1;try{selected=Arrays.asList(LauncherSettingsStore.SCALES).indexOf(store.scale());}catch(IOException ignored){}
        choice(t("Quality"),t("QualityHint"),qualityNames(),selected,i->{try{store.scale(i);showFeedback(t("Saved"),false);}catch(IOException e){settingsError(e);}bindSettings();});}
    private void chooseDriver(){String[] modes={GraphicsDriverPolicy.AUTO,GraphicsDriverPolicy.SYSTEM,GraphicsDriverPolicy.TURNIP};
        choice(t("Driver"),t("DriverHint"),new String[]{t("AutoDriver"),t("System"),t("Turnip")},Arrays.asList(modes).indexOf(GraphicsDriverSettings.read(this)),i->{
            if(GraphicsDriverSettings.save(this,modes[i]))showFeedback(t("Saved"),false);else showFeedback(t("SettingsFailed"),true);bindSettings();});}
    private void chooseMod(){if(!canEdit())return;try{ModSelectionStore mods=new ModSelectionStore(getFilesDir());ArrayList<String> ids=mods.installed();ids.add(0,"");
        String[] names=ids.toArray(new String[0]);names[0]=t("NoMod");choice(t("Mod"),t("ModHint"),names,modValid?ids.indexOf(modId):-1,i->{try{mods.select(ids.get(i));showFeedback(t("Saved"),false);}catch(IOException e){settingsError(e);}bindSettings();renderSaves();if(modValid)saves.refreshBackup(scope());});
        }catch(IOException e){settingsError(e);}}
    private void showFeedback(String value,boolean error){feedback.setText(value);feedback.setTextColor(error?ERROR:GOLD);feedback.setVisibility(value.isEmpty()?View.GONE:View.VISIBLE);}
    private void openGame(){if(!resumed||!session.state.ready()||!canEdit())return;bindSettings();if(!modValid){chooseMod();return;}
        try{store.scale();}catch(IOException e){showFeedback(t("ConfigInvalid"),true);chooseQuality();return;}
        try{openingGame=true;render();startActivity(new Intent(this,GameActivity.class));finish();}catch(RuntimeException e){openingGame=false;showFeedback(t("OpenFailed"),true);render();}}
    private void pickPackage(){if(!canEdit()||!session.canSelect())return;Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        intent.putExtra(Intent.EXTRA_LOCAL_ONLY,true);intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try{picking=true;render();startActivityForResult(intent,PICK_PACKAGE);}catch(RuntimeException e){picking=false;showFeedback(t("PickerFailed"),true);render();}}
    private void pickSave(boolean importing){if(!canEdit()||!modValid)return;
        try{if(!importing&&SaveArchive.inspect(SaveArchive.target(getFilesDir(),scope())).files==0){showFeedback(t("NoSaves"),false);return;}}
        catch(IOException e){settingsError(e);return;}
        if(!saves.reserve(scope()))return;
        Intent intent=new Intent(importing?Intent.ACTION_OPEN_DOCUMENT:Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType(importing?"*/*":"application/zip");
        intent.addFlags(importing?Intent.FLAG_GRANT_READ_URI_PERMISSION:Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        if(!importing)intent.putExtra(Intent.EXTRA_TITLE,"d2r-save-"+scope()+"-"+new java.text.SimpleDateFormat("yyyyMMdd-HHmmss",Locale.US).format(new Date())+".zip");
        try{startActivityForResult(intent,importing?IMPORT_SAVE:EXPORT_SAVE);}catch(RuntimeException e){saves.pickerFailed();}
    }
    private void pickBackup(){if(!canEdit()||!saves.reserveBackup(scope()))return;
        Intent intent=new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip");
        intent.putExtra(Intent.EXTRA_TITLE,saves.latestBackup.getName());intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try{startActivityForResult(intent,EXPORT_SAVE);}catch(RuntimeException e){saves.pickerFailed();}
    }
    private void renderSaves(){if(steppedAside||saveStatus==null)return;
        saveProgress.setVisibility(saves.working()?View.VISIBLE:View.GONE);boolean enabled=!saves.locked()&&!session.state.busy()&&!GameSessionGate.guestStarted()&&modValid&&!picking&&!openingGame;
        exportSave.setEnabled(enabled);importSave.setEnabled(enabled);
        exportBackup.setVisibility(scope().equals(saves.latestScope)&&saves.latestBackup!=null?View.VISIBLE:View.GONE);exportBackup.setEnabled(enabled);
        setEnabled(settings,!saves.locked()&&!session.state.busy()&&!GameSessionGate.guestStarted()&&!picking&&!openingGame);
        String value=strings.text(saves.messageKey);
        if(saves.phase==SaveTransferSession.Phase.DONE){value+="\n"+f("Scope",scopeName(saves.scope));if(saves.summary!=null)value+="\n"+f("FileCount",saves.summary.files,saves.summary.characters);if(saves.backup!=null)value+="\n"+f("Backup",saves.backup.getName());}
        saveStatus.setText(value);saveStatus.setTextColor(saves.phase==SaveTransferSession.Phase.FAILED?ERROR:MUTED);
        if(saves.phase==SaveTransferSession.Phase.PREVIEW&&resumed&&previewDialog==null){SaveArchive.Summary s=saves.summary;
            previewDialog=new AlertDialog.Builder(this).setTitle(t("Preview")).setMessage(f("PreviewBody",scopeName(saves.scope),s.files,s.characters,s.conflicts))
                .setNegativeButton(t("Cancel"),(d,w)->saves.cancel()).setPositiveButton(t("ConfirmImport"),(d,w)->saves.confirm()).create();
            previewDialog.setOnCancelListener(d->saves.cancel());previewDialog.setOnDismissListener(d->previewDialog=null);previewDialog.show();}
        renderStart();updateAwake();
    }
    private void setEnabled(View v,boolean enabled){v.setEnabled(enabled);if(v instanceof android.view.ViewGroup){android.view.ViewGroup g=(android.view.ViewGroup)v;for(int i=0;i<g.getChildCount();i++)setEnabled(g.getChildAt(i),enabled);}}
    private String size(long bytes){return bytes>=1_000_000_000L?String.format(Locale.getDefault(),"%.2f GB",bytes/1e9):String.format(Locale.getDefault(),"%.1f MB",bytes/1e6);}
    private void renderStart(){if(choose==null)return;boolean blocked=saves.locked()||picking||openingGame||GameSessionGate.guestStarted();
        choose.setEnabled(!blocked&&(session.state.ready()||session.canSelect()));
        choose.setText(openingGame?t("Starting"):picking?t("Picking"):session.state.ready()?t("Start"):t("PickPackage"));}
    private void render(){if(steppedAside||heading==null||isFinishing())return;boolean busy=session.state.busy(),ready=session.state.ready();
        heading.setText(ready?t("ResourceReady"):session.title);message.setText(ready?t("ReadyHint"):session.detail);
        progress.setIndeterminate(busy&&session.total==0);progress.setVisibility(busy?View.VISIBLE:View.GONE);progress.setProgress(session.total>0?(int)Math.min(1000,session.done*1000.0/session.total):0);
        amount.setText(busy&&session.total>0?f("Progress",session.done*100.0/session.total,size(session.done),size(session.total)):f("Space",size(session.payloadBytes()),size(getFilesDir().getUsableSpace())));
        cancel.setVisibility(session.state.canCancel()?View.VISIBLE:View.GONE);cancel.setEnabled(session.state.canCancel());
        if(cancelDialog!=null&&!session.state.canCancel())cancelDialog.dismiss();renderStart();renderSaves();
    }
    private void updateAwake(){if(resumed&&(session.state.busy()||saves.working()))getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);}
    private void confirmCancel(){if(!session.state.canCancel()||cancelDialog!=null)return;cancelDialog=new AlertDialog.Builder(this).setTitle(t("StopTitle")).setMessage(t("StopBody"))
        .setNegativeButton(t("Continue"),null).setPositiveButton(t("Stop"),(d,w)->session.cancel()).create();cancelDialog.setOnDismissListener(d->cancelDialog=null);cancelDialog.show();}
    @Override public void onStart(){super.onStart();if(!steppedAside){session.attach(observer);saves.attach(saveObserver);}}
    @Override public void onResume(){super.onResume();if(steppedAside)return;resumed=true;bindSettings();render();if(modValid)saves.refreshBackup(scope());if(GameSessionGate.guestStarted())showFeedback(t("GameRunning"),true);}
    @Override public void onPause(){resumed=false;getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);super.onPause();}
    @Override public void onStop(){if(!steppedAside){session.detach(observer);saves.detach(saveObserver);}super.onStop();}
    @Override public void onDestroy(){if(cancelDialog!=null)cancelDialog.dismiss();if(choiceDialog!=null)choiceDialog.dismiss();if(previewDialog!=null)previewDialog.dismiss();super.onDestroy();}
    @Override protected void onSaveInstanceState(Bundle state){if(!steppedAside){state.putBoolean("picking",picking);state.putString("feedback",feedback.getText().toString());state.putBoolean("feedbackError",feedback.getCurrentTextColor()==ERROR);}super.onSaveInstanceState(state);}
    @Override protected void onActivityResult(int request,int result,Intent data){super.onActivityResult(request,result,data);if(steppedAside)return;
        if(request==PICK_PACKAGE){picking=false;if(result==RESULT_OK&&data!=null&&data.getData()!=null)session.start(data.getData());render();}
        else if(request==EXPORT_SAVE||request==IMPORT_SAVE){if(result!=RESULT_OK||data==null||data.getData()==null)saves.cancel();else if(request==EXPORT_SAVE)saves.export(data.getData());else saves.read(data.getData());}}
    @Override public void onBackPressed(){if(steppedAside)return;if(session.state.canCancel())confirmCancel();else if(saves.locked()||session.state.busy())Toast.makeText(this,t("Busy"),Toast.LENGTH_SHORT).show();else super.onBackPressed();}
}
