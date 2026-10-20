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
import java.io.IOException;
import java.util.ArrayList;
import java.util.Locale;

/** The launcher owns resource preparation; native game code loads only after success. */
public final class InstallerActivity extends Activity {
    private static final int PICK_PACKAGE=917, GOLD=0xffc8ad7f, MUTED=0xffb5b1aa, ERROR=0xffffb5a7;
    private OfflineInstallSession session;
    private final OfflineInstallSession.Observer observer=this::render;
    private TextView heading,message,amount,help,steps,feedback,configuration,driverDetail,modDetail;
    private ProgressBar progress;
    private Button choose,cancel,settingsToggle,helpToggle;
    private RadioGroup drivers;
    private LinearLayout settings;
    private AlertDialog cancelDialog,modDialog;
    private boolean resumed,openingGame,picking,settingsExpanded,helpExpanded,bindingDrivers,modValid;
    private String modId="";

    private int dp(int n){return Math.round(n*getResources().getDisplayMetrics().density);}
    private TextView text(String value,int size,boolean bold) {
        TextView view=new TextView(this);view.setText(value);view.setTextColor(TouchControlsView.TEXT);view.setTextSize(size);
        view.setLineSpacing(dp(3),1);if(bold)view.setTypeface(Typeface.DEFAULT,Typeface.BOLD);return view;
    }
    private void add(LinearLayout column,View view,int top) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);p.topMargin=dp(top);column.addView(view,p);
    }
    private LinearLayout column(){LinearLayout view=new LinearLayout(this);view.setOrientation(LinearLayout.VERTICAL);return view;}
    private Button button(String label,boolean primary) {
        Button button=new Button(this);button.setText(label);button.setAllCaps(false);button.setTextSize(16);
        button.setMinHeight(dp(52));button.setPadding(dp(12),dp(8),dp(12),dp(8));
        button.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{}},
            new int[]{MUTED,primary?0xff191510:TouchControlsView.TEXT}));
        button.setBackgroundTintList(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{android.R.attr.state_pressed},new int[]{android.R.attr.state_focused},new int[]{}},
            new int[]{0xff34312d,primary?0xffe4ceaa:0xff4b4236,primary?0xffe4ceaa:0xff4b4236,primary?GOLD:0xff292621}));
        return button;
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);session=OfflineInstallSession.get(this);
        picking=state!=null&&state.getBoolean("picking");
        settingsExpanded=state!=null&&state.getBoolean("settingsExpanded");helpExpanded=state!=null&&state.getBoolean("helpExpanded");
        getWindow().setStatusBarColor(0xff0d0d0f);getWindow().setNavigationBarColor(0xff0d0d0f);
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(true);scroll.setBackgroundColor(0xff0d0d0f);
        scroll.setOnApplyWindowInsetsListener((view,insets)->{
            view.setPadding(insets.getSystemWindowInsetLeft(),insets.getSystemWindowInsetTop(),insets.getSystemWindowInsetRight(),insets.getSystemWindowInsetBottom());return insets;
        });
        LinearLayout center=column();center.setGravity(Gravity.CENTER_HORIZONTAL);center.setPadding(dp(20),dp(28),dp(20),dp(24));
        scroll.addView(center,new ScrollView.LayoutParams(-1,-2));
        LinearLayout content=column();center.addView(content,new LinearLayout.LayoutParams(-1,-2));
        // Use measured space so split-screen and landscape do not inherit the full display width.
        center.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob)->{
            int width=Math.min(dp(600),Math.max(0,r-l-center.getPaddingLeft()-center.getPaddingRight()));
            if(content.getLayoutParams().width!=width){content.getLayoutParams().width=width;content.requestLayout();}
        });
        TextView brand=text("D2R MOBILE",14,true);brand.setTextColor(GOLD);add(content,brand,0);
        heading=text("正在检查游戏文件",28,true);heading.setAccessibilityHeading(true);add(content,heading,10);
        steps=text("检查资源  →  导入并校验  →  启动游戏",14,false);steps.setTextColor(MUTED);add(content,steps,10);
        LinearLayout card=column();card.setPadding(dp(18),dp(18),dp(18),dp(14));
        GradientDrawable background=new GradientDrawable();background.setColor(0xff1b1916);background.setCornerRadius(dp(14));
        background.setStroke(dp(1),0xff463c2e);card.setBackground(background);add(content,card,22);
        message=text("",16,false);message.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);add(card,message,0);
        progress=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);progress.setMax(1000);
        progress.setProgressTintList(ColorStateList.valueOf(GOLD));progress.setIndeterminateTintList(ColorStateList.valueOf(GOLD));
        progress.setContentDescription("游戏资源处理进度");add(card,progress,14);
        amount=text("",14,false);amount.setTextColor(MUTED);add(card,amount,10);
        configuration=text("",14,false);add(card,configuration,14);
        choose=button("选择离线资源包",true);choose.setOnClickListener(v->{if(session.state.ready())openGame();else pick();});add(card,choose,16);
        cancel=button("停止导入",false);cancel.setOnClickListener(v->confirmCancel());add(card,cancel,6);
        feedback=text("",14,false);feedback.setTextColor(ERROR);feedback.setVisibility(View.GONE);
        feedback.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);add(content,feedback,10);
        settingsToggle=button("",false);settingsToggle.setOnClickListener(v->{settingsExpanded=!settingsExpanded;updateExpansion();});add(content,settingsToggle,14);
        settings=column();settings.setPadding(dp(8),0,dp(8),dp(8));add(content,settings,0);
        TextView driverHeading=text("图形驱动",17,true);driverHeading.setAccessibilityHeading(true);add(settings,driverHeading,14);
        drivers=new RadioGroup(this);drivers.setOrientation(RadioGroup.VERTICAL);drivers.setSaveEnabled(false);
        String[] values={GraphicsDriverPolicy.AUTO,GraphicsDriverPolicy.SYSTEM,GraphicsDriverPolicy.TURNIP};
        String[] labels={"自动推荐（推荐）","系统驱动","内置兼容驱动（Turnip）"};
        for(int i=0;i<values.length;i++) {
            RadioButton option=new RadioButton(this);option.setId(View.generateViewId());option.setTag(values[i]);option.setSaveEnabled(false);
            option.setText(labels[i]);option.setTextSize(16);option.setTextColor(TouchControlsView.TEXT);option.setMinHeight(dp(48));
            option.setButtonTintList(ColorStateList.valueOf(GOLD));drivers.addView(option,new RadioGroup.LayoutParams(-1,-2));
        }
        add(settings,drivers,4);driverDetail=text("",14,false);driverDetail.setTextColor(MUTED);add(settings,driverDetail,4);
        drivers.setOnCheckedChangeListener((group,id)->{
            if(bindingDrivers)return;RadioButton selected=group.findViewById(id);if(selected==null)return;
            if(GraphicsDriverSettings.save(this,(String)selected.getTag()))showFeedback("驱动已保存，下次启动游戏时生效。",false);
            else showFeedback("驱动选择未能保存，请重试。",true);
            bindDriver();updateConfiguration();
        });
        TextView modHeading=text("MOD",17,true);modHeading.setAccessibilityHeading(true);add(settings,modHeading,18);
        modDetail=text("",14,false);add(settings,modDetail,8);
        Button mods=button("选择已安装 MOD",false);mods.setOnClickListener(v->chooseMod());add(settings,mods,6);
        TextView modHelp=text("选择在启动游戏时生效。MOD 使用独立存档；关闭 MOD 后可回到原版存档。",14,false);modHelp.setTextColor(MUTED);add(settings,modHelp,8);
        helpToggle=button("",false);helpToggle.setOnClickListener(v->{helpExpanded=!helpExpanded;updateExpansion();});add(content,helpToggle,8);
        help=text("",14,false);help.setTextColor(MUTED);add(content,help,8);
        setContentView(scroll);bindDriver();refreshMod();updateExpansion();
        if(state!=null&&state.getString("feedback")!=null)showFeedback(state.getString("feedback"),state.getBoolean("feedbackError"));
    }
    private void updateExpansion(){
        settings.setVisibility(settingsExpanded?View.VISIBLE:View.GONE);settingsToggle.setText("启动设置 · "+(settingsExpanded?"收起":"展开"));
        settingsToggle.setContentDescription("图形驱动和 MOD 设置，当前已"+(settingsExpanded?"展开":"折叠"));
        help.setVisibility(helpExpanded?View.VISIBLE:View.GONE);helpToggle.setText("资源包与存档说明 · "+(helpExpanded?"收起":"展开"));
    }
    private static String driverLabel(String mode){
        return GraphicsDriverPolicy.SYSTEM.equals(mode)?"系统驱动":GraphicsDriverPolicy.TURNIP.equals(mode)?"内置 Turnip":"自动推荐";
    }
    private void bindDriver(){
        String mode=GraphicsDriverSettings.read(this);bindingDrivers=true;
        for(int i=0;i<drivers.getChildCount();i++)if(mode.equals(drivers.getChildAt(i).getTag()))drivers.check(drivers.getChildAt(i).getId());
        bindingDrivers=false;
        driverDetail.setText(GraphicsDriverPolicy.SYSTEM.equals(mode)?"使用手机自带的图形驱动。启动失败时可返回此处更换。":
            GraphicsDriverPolicy.TURNIP.equals(mode)?"使用 APK 内的 Turnip 驱动，适用于部分骁龙设备。其他设备建议使用自动推荐。":
            "按设备选择驱动，当前设备推荐"+(GraphicsDriverSettings.recommendedTurnip()?"内置 Turnip":"系统驱动")+"。连续启动失败后会尝试系统驱动。");
    }
    private void updateConfiguration(){
        configuration.setText("驱动："+driverLabel(GraphicsDriverSettings.read(this))+"\nMOD："+(modValid?(modId.isEmpty()?"不加载（原版存档）":modId+"（独立存档）"):"选择异常，请在启动设置中重新选择"));
        configuration.setTextColor(modValid?TouchControlsView.TEXT:ERROR);
    }
    private boolean refreshMod(){
        try{
            ModSelectionStore store=new ModSelectionStore(getFilesDir());modId=store.selected();
            modValid=modId.isEmpty()||store.installed().contains(modId);
            modDetail.setText(modValid?(modId.isEmpty()?"当前：不加载 MOD":"当前："+modId):"已选择的 MOD 不存在，请重新选择或关闭 MOD。");
        }catch(IOException e){modValid=false;modDetail.setText("无法读取 MOD 设置，请重新选择“不加载 MOD”或已安装包。");}
        modDetail.setTextColor(modValid?MUTED:ERROR);updateConfiguration();return modValid;
    }
    private void chooseMod(){
        if(modDialog!=null)return;
        try{
            ModSelectionStore store=new ModSelectionStore(getFilesDir());ArrayList<String> ids=store.installed();ids.add(0,"");
            String[] labels=new String[ids.size()];labels[0]="不加载 MOD（原版）";
            for(int i=1;i<labels.length;i++)labels[i]=ids.get(i);
            int checked=modValid?ids.indexOf(modId):-1;
            modDialog=new AlertDialog.Builder(this).setTitle(ids.size()==1?"暂无已安装 MOD":"选择启动 MOD")
                .setSingleChoiceItems(labels,checked,(dialog,index)->{
                    try{store.select(ids.get(index));refreshMod();showFeedback("MOD 选择已保存，启动游戏时生效。",false);dialog.dismiss();}
                    catch(IOException e){showFeedback("MOD 选择保存失败，请重试。",true);}
                }).setNegativeButton("返回",null).create();
            modDialog.setOnDismissListener(d->{modDialog=null;if(!isFinishing())settingsToggle.requestFocus();});modDialog.show();
        }catch(IOException e){showFeedback("无法读取已安装 MOD，请检查应用可用空间后重试。",true);}
    }
    private void showFeedback(String value,boolean error){
        feedback.setText(value);feedback.setTextColor(error?ERROR:GOLD);feedback.setVisibility(value.isEmpty()?View.GONE:View.VISIBLE);
    }
    private void openGame() {
        if(!session.state.ready()||!resumed||openingGame)return;
        if(!refreshMod()){settingsExpanded=true;updateExpansion();showFeedback("请先在启动设置中修复 MOD 选择。",true);chooseMod();return;}
        try{openingGame=true;render();startActivity(new Intent(this,GameActivity.class));finish();}
        catch(android.content.ActivityNotFoundException|SecurityException e){openingGame=false;showFeedback("无法打开游戏，请检查 APK 是否完整安装。",true);render();}
    }
    private void pick() {
        if(picking||!session.canSelect())return;
        Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT);intent.addCategory(Intent.CATEGORY_OPENABLE);intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_LOCAL_ONLY,true);intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try{picking=true;showFeedback("",false);render();startActivityForResult(intent,PICK_PACKAGE);}
        catch(android.content.ActivityNotFoundException|SecurityException e){picking=false;showFeedback("无法打开文件选择器，请启用系统“文件”应用后重试。",true);render();}
    }
    private void confirmCancel(){
        if(!session.state.canCancel()||cancelDialog!=null)return;
        cancelDialog=new AlertDialog.Builder(this).setTitle("停止本次导入？")
            .setMessage("已校验的文件会保留。下次选择同一资源包时会复用这些文件；尚未完成的文件需要重新导入。")
            .setNegativeButton("继续导入",null).setPositiveButton("停止导入",(d,w)->session.cancel()).create();
        cancelDialog.setOnDismissListener(d->{cancelDialog=null;if(!isFinishing())choose.requestFocus();});cancelDialog.show();
        cancelDialog.getButton(AlertDialog.BUTTON_NEGATIVE).requestFocus();
    }
    private String size(long n){
        if(n>=1_000_000_000L)return String.format(Locale.SIMPLIFIED_CHINESE,"%.2f GB",n/1_000_000_000.0);
        if(n>=1_000_000L)return String.format(Locale.SIMPLIFIED_CHINESE,"%.1f MB",n/1_000_000.0);
        return String.format(Locale.SIMPLIFIED_CHINESE,"%.0f KB",Math.max(0,n)/1000.0);
    }
    private void render() {
        if(isFinishing())return;
        OfflineInstallState.Phase phase=session.state.phase();boolean busy=session.state.busy(),ready=session.state.ready();
        heading.setText(session.title);if(!session.detail.contentEquals(message.getText()))message.setText(session.detail);
        steps.setText(ready?"资源已就绪 · 可直接启动":phase==OfflineInstallState.Phase.CHECKING?"第 1 步 · 检查本地资源":
            busy?"第 2 步 · 导入并校验资源":"第 1 步 · 选择配套的 .d2rpack 资源包");
        progress.setIndeterminate(busy&&(session.total==0||phase==OfflineInstallState.Phase.CANCELLING));
        progress.setProgress(session.total>0?(int)Math.min(1000,session.done*1000.0/session.total):0);progress.setVisibility(busy?View.VISIBLE:View.GONE);
        long needed=session.requiredBytes>0?session.requiredBytes:session.payloadBytes()+OfflinePackageInstaller.RESERVE;
        long available=getFilesDir().getUsableSpace();
        amount.setText(session.payloadBytes()==0?(busy?"正在读取安装信息…":"安装信息不可用，请重新安装配套 APK。"):busy&&session.total>0?
            String.format(Locale.SIMPLIFIED_CHINESE,"本阶段 %.1f%% · %s / %s",session.done*100.0/session.total,size(session.done),size(session.total)):
            "资源 "+size(session.payloadBytes())+" · 可用空间 "+size(available)+(busy||ready?"":"\n首次导入需额外空间约 "+size(needed)));
        amount.setTextColor(!busy&&!ready&&session.payloadBytes()>0&&available<needed?ERROR:MUTED);
        choose.setEnabled((ready||session.canSelect())&&!picking&&!openingGame);
        choose.setText(openingGame?"正在启动…":picking?"正在选择资源包…":ready?"启动游戏":busy?"正在处理，请稍候…":
            phase==OfflineInstallState.Phase.FAILED||phase==OfflineInstallState.Phase.STOPPED?"重新选择资源包":"选择离线资源包");
        configuration.setVisibility(ready?View.VISIBLE:View.GONE);
        boolean importing=phase==OfflineInstallState.Phase.IMPORTING||phase==OfflineInstallState.Phase.CANCELLING||phase==OfflineInstallState.Phase.COMMITTING;
        cancel.setVisibility(importing?View.VISIBLE:View.GONE);cancel.setEnabled(session.state.canCancel());
        cancel.setText(phase==OfflineInstallState.Phase.CANCELLING?"正在停止…":phase==OfflineInstallState.Phase.COMMITTING?"正在完成安装，无法取消":"停止导入");
        if(cancelDialog!=null&&!session.state.canCancel())cancelDialog.dismiss();
        help.setText(ready?"游戏资源已安装，无需重新导入。启动失败时可在“启动设置”中更换图形驱动或关闭 MOD。\n更新请覆盖安装；卸载应用会删除游戏数据和存档。":
            (session.payloadBytes()>0?"首次导入需额外可用空间约 "+size(needed)+"。\n":"")+
            "选择配套 .d2rpack 文件，无需解压或移动目录。建议保持页面打开；切到后台不会主动取消，但系统回收应用后需要重新选择资源包。\n已校验文件会复用。导入完成后可删除下载的资源包；卸载应用会删除游戏数据和存档。");
        if(busy&&resumed)getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
    @Override public void onStart(){super.onStart();session.attach(observer);}
    @Override public void onResume(){super.onResume();resumed=true;bindDriver();refreshMod();render();}
    @Override public void onPause(){resumed=false;getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);super.onPause();}
    @Override public void onStop(){session.detach(observer);super.onStop();}
    @Override public void onDestroy(){if(cancelDialog!=null)cancelDialog.dismiss();if(modDialog!=null)modDialog.dismiss();super.onDestroy();}
    @Override protected void onSaveInstanceState(Bundle state){
        state.putBoolean("picking",picking);state.putBoolean("settingsExpanded",settingsExpanded);state.putBoolean("helpExpanded",helpExpanded);
        state.putString("feedback",feedback.getText().toString());state.putBoolean("feedbackError",feedback.getCurrentTextColor()==ERROR);super.onSaveInstanceState(state);
    }
    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);if(request!=PICK_PACKAGE)return;picking=false;
        if(result==RESULT_OK&&data!=null&&data.getData()!=null){showFeedback("",false);session.start(data.getData());}
        else showFeedback("未选择资源包，可随时重新选择。",false);render();
    }
    @Override public void onBackPressed(){
        if(session.state.canCancel())confirmCancel();
        else if(session.state.busy())Toast.makeText(this,"正在处理资源，请稍候。可按主屏幕键暂时离开。",Toast.LENGTH_SHORT).show();
        else super.onBackPressed();
    }
}
