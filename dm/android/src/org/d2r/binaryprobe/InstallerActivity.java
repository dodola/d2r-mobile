package org.d2r.binaryprobe;

import android.app.Activity;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.*;
import android.widget.*;
import java.util.Locale;

/** The launcher owns resource preparation; native game code loads only after success. */
public final class InstallerActivity extends Activity {
    private static final int PICK_PACKAGE=917;
    private OfflineInstallSession session;
    private final OfflineInstallSession.Observer observer=this::render;
    private TextView heading,message,amount,help;
    private ProgressBar progress;
    private Button choose,cancel;
    private RadioGroup drivers;
    private TextView driverDetail;
    private boolean resumed,openingGame,picking,launchAfterImport;
    private int dp(int n){return Math.round(n*getResources().getDisplayMetrics().density);}
    private TextView text(String value,int size,boolean bold) {
        TextView view=new TextView(this);view.setText(value);view.setTextColor(TouchControlsView.TEXT);view.setTextSize(size);
        if(bold)view.setTypeface(Typeface.DEFAULT,Typeface.BOLD);return view;
    }
    private void add(LinearLayout column,View view,int top) {
        LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2);p.topMargin=dp(top);column.addView(view,p);
    }
    private Button button(String label) {
        Button button=new Button(this);button.setText(label);button.setAllCaps(false);button.setTextSize(17);
        button.setMinHeight(dp(52));button.setTextColor(new ColorStateList(new int[][]{new int[]{android.R.attr.state_enabled},new int[]{}},
            new int[]{TouchControlsView.TEXT,TouchControlsView.RIM}));
        button.setBackgroundTintList(new ColorStateList(new int[][]{new int[]{android.R.attr.state_pressed},new int[]{android.R.attr.state_focused},new int[]{}},
            new int[]{TouchControlsView.PRESSED,TouchControlsView.PRESSED,TouchControlsView.INK|0xff000000}));return button;
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);picking=state!=null&&state.getBoolean("picking");
        launchAfterImport=state!=null&&state.getBoolean("launchAfterImport");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(true);scroll.setBackgroundColor(0xff0d0d0f);
        scroll.setOnApplyWindowInsetsListener((view,insets)->{
            view.setPadding(insets.getSystemWindowInsetLeft(),insets.getSystemWindowInsetTop(),insets.getSystemWindowInsetRight(),insets.getSystemWindowInsetBottom());return insets;
        });
        LinearLayout center=new LinearLayout(this);center.setGravity(Gravity.CENTER);center.setOrientation(LinearLayout.VERTICAL);
        center.setPadding(dp(24),dp(24),dp(24),dp(24));scroll.addView(center,new ScrollView.LayoutParams(-1,-1));
        LinearLayout column=new LinearLayout(this);column.setOrientation(LinearLayout.VERTICAL);
        int width=Math.min(getResources().getDisplayMetrics().widthPixels-dp(48),dp(640));
        center.addView(column,new LinearLayout.LayoutParams(Math.max(dp(200),width),-2));
        TextView brand=text("D2R MOBILE · 离线安装",14,true);brand.setTextColor(TouchControlsView.RIM|0xff000000);add(column,brand,0);
        heading=text("正在检查游戏文件",28,true);heading.setAccessibilityHeading(true);add(column,heading,12);
        add(column,text("选择资源包  →  导入并校验  →  启动游戏",15,false),12);
        message=text("首次打开时会自动检查已安装的资源。",17,false);message.setMinLines(3);
        message.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);add(column,message,22);
        progress=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);progress.setMax(1000);
        progress.setProgressTintList(ColorStateList.valueOf(TouchControlsView.RIM));progress.setContentDescription("游戏资源安装进度");add(column,progress,8);
        amount=text("",14,false);amount.setTypeface(Typeface.MONOSPACE);amount.setMinLines(2);add(column,amount,8);
        TextView driverHeading=text("图形驱动",17,true);driverHeading.setAccessibilityHeading(true);add(column,driverHeading,12);
        drivers=new RadioGroup(this);drivers.setOrientation(RadioGroup.VERTICAL);
        String[] values={GraphicsDriverPolicy.AUTO,GraphicsDriverPolicy.SYSTEM,GraphicsDriverPolicy.TURNIP};
        String[] labels={"自动推荐（推荐）","系统驱动","内置兼容驱动（Turnip）"};
        String saved=GraphicsDriverSettings.read(this);
        for(int i=0;i<values.length;i++) {
            RadioButton option=new RadioButton(this);option.setId(View.generateViewId());option.setTag(values[i]);
            option.setText(labels[i]);option.setTextSize(16);option.setTextColor(TouchControlsView.TEXT);option.setMinHeight(dp(48));
            option.setButtonTintList(ColorStateList.valueOf(TouchControlsView.RIM|0xff000000));
            drivers.addView(option,new RadioGroup.LayoutParams(-1,-2));if(values[i].equals(saved))drivers.check(option.getId());
        }
        add(column,drivers,4);
        driverDetail=text("",14,false);driverDetail.setMinLines(3);driverDetail.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);add(column,driverDetail,4);
        showDriverDetail(saved);
        drivers.setOnCheckedChangeListener((group,id)->{
            RadioButton selected=group.findViewById(id);if(selected==null)return;
            String mode=(String)selected.getTag();
            if(GraphicsDriverSettings.save(this,mode))showDriverDetail(mode);
            else {message.setText("驱动选择未能保存，请重试。");String current=GraphicsDriverSettings.read(this);
                for(int i=0;i<group.getChildCount();i++)if(current.equals(group.getChildAt(i).getTag()))group.check(group.getChildAt(i).getId());}
        });
        choose=button("选择离线资源包");choose.setOnClickListener(v->{if(session.ready)openGame();else pick();});add(column,choose,12);
        cancel=button("取消导入");cancel.setOnClickListener(v->session.cancel());add(column,cancel,4);
        help=text("安装方法：先将配套 APK 和资源包下载到手机。安装 APK 后，在这里选择 .d2rpack 文件，无需解压、移动目录或连接电脑。",14,false);
        add(column,help,18);setContentView(scroll);session=OfflineInstallSession.get(this);
    }
    private void showDriverDetail(String mode) {
        String detail=GraphicsDriverPolicy.SYSTEM.equals(mode)?"使用手机自带的图形驱动。如果启动闪退或黑屏，请重新打开应用，切回自动推荐。":
            GraphicsDriverPolicy.TURNIP.equals(mode)?"使用安装包内的 Turnip 驱动，适用于部分骁龙设备。其他设备建议使用自动推荐。":
            "当前推荐："+(GraphicsDriverSettings.recommendedTurnip()?"内置兼容驱动":"系统驱动")+"。会记住你的选择；更换驱动在下次启动游戏时生效。";
        driverDetail.setText(detail);
    }
    private void openGame() {
        if(!session.ready||!resumed||openingGame)return;
        openingGame=true;startActivity(new Intent(this,GameActivity.class));finish();
    }
    private void pick() {
        if(picking||!session.canSelect())return;
        Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT);intent.addCategory(Intent.CATEGORY_OPENABLE);intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_LOCAL_ONLY,true);intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try{picking=true;choose.setEnabled(false);startActivityForResult(intent,PICK_PACKAGE);}
        catch(android.content.ActivityNotFoundException e){picking=false;message.setText("手机没有可用的文件选择器，请启用系统“文件”应用后重试。");choose.setEnabled(true);}
    }
    private String size(long n) {return String.format(Locale.SIMPLIFIED_CHINESE,"%.2f GB",n/1_000_000_000.0);}
    private void render() {
        if(isFinishing())return;
        heading.setText(session.title);message.setText(session.detail);
        progress.setIndeterminate(session.busy&&session.total==0);
        progress.setProgress(session.total>0?(int)Math.min(1000,session.done*1000/session.total):0);
        progress.setVisibility(session.busy?View.VISIBLE:View.INVISIBLE);
        amount.setText(session.payloadBytes()==0?"正在读取安装信息…":session.busy&&session.total>0?String.format(Locale.SIMPLIFIED_CHINESE,"%.1f%% · %s / %s",session.done*100.0/session.total,size(session.done),size(session.total)):
            "资源约 "+size(session.payloadBytes())+" · 手机可用 "+size(getFilesDir().getUsableSpace()));
        choose.setEnabled((session.ready||session.canSelect())&&!picking);choose.setText(session.ready?"启动游戏":"选择离线资源包");
        if(session.ready){heading.setText("游戏已准备好");message.setText("选择图形驱动后，点击“启动游戏”。如果启动失败，重新打开应用即可更换驱动。");}
        cancel.setVisibility(session.busy&&!session.ready?View.VISIBLE:View.INVISIBLE);cancel.setEnabled(session.busy&&!session.committing);
        long needed=session.requiredBytes>0?session.requiredBytes:session.payloadBytes()+OfflinePackageInstaller.RESERVE;
        help.setText(session.ready?"游戏资源已安装，无需重新导入。更新请覆盖安装；卸载应用会删除游戏数据和存档。":(session.payloadBytes()>0?"首次导入需额外可用空间约 "+size(needed)+"，请保留已下载的资源包直到导入完成。\n":"")+
            "选择 .d2rpack 文件即可，无需解压、移动目录或连接电脑。\n安装完成后可删除下载的资源包；卸载应用会删除游戏数据和存档。");
        if(session.ready&&launchAfterImport)openGame();
    }
    @Override public void onStart(){super.onStart();session.attach(observer);}
    @Override public void onResume(){super.onResume();resumed=true;render();}
    @Override public void onPause(){resumed=false;super.onPause();}
    @Override public void onStop(){session.detach(observer);if(!isChangingConfigurations()&&!openingGame&&session.busy&&!picking)session.cancel();super.onStop();}
    @Override protected void onSaveInstanceState(Bundle state){state.putBoolean("picking",picking);state.putBoolean("launchAfterImport",launchAfterImport);super.onSaveInstanceState(state);}
    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);if(request!=PICK_PACKAGE)return;picking=false;
        if(result==RESULT_OK&&data!=null&&data.getData()!=null){launchAfterImport=true;session.start(data.getData());}render();
    }
    @Override public void onBackPressed(){if(session.busy){session.cancel();}else super.onBackPressed();}
}
