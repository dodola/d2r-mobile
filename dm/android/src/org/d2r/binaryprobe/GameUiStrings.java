package org.d2r.binaryprobe;

import java.util.Locale;

/** Frontend text follows the running game's copied HUD language, never Custom's pending choice. */
final class GameUiStrings {
    private volatile boolean chinese;
    GameUiStrings(Locale fallback){chinese=fallback!=null&&"zh".equals(fallback.getLanguage());}
    boolean chinese(){return chinese;}
    boolean updateHudLanguage(long[] hud){
        if(hud==null||!((hud.length==88&&hud[0]==1)||(hud.length==107&&hud[0]==2)))return false;
        boolean next=hud[18]!=0;if(next==chinese)return false;chinese=next;return true;
    }
    String text(String zh){
        if(chinese)return zh;
        switch(zh){
            case "游戏画面":return "Game view";
            case "菜单操控":return "Menu controls";
            case "触摸操控":return "Touch controls";
            case "切换到菜单操控":return "Switch to menu controls";
            case "切换到战斗触摸操控":return "Switch to combat touch controls";
            case "触摸操控：左侧摇杆移动，右侧技能按钮拖动瞄准，底部药水":return "Touch controls: move with the left stick, drag the right skill buttons to aim, potions along the bottom";
            case "确认":return "Confirm";
            case "返回":return "Back";
            case "开始":return "Start";
            case "取消":return "Cancel";
            case "向左移动":return "Move left";
            case "向右移动":return "Move right";
            case "向上移动":return "Move up";
            case "向下移动":return "Move down";
            case "正在准备游戏文件":return "Preparing game files";
            case "等待游戏文件":return "Waiting for game files";
            case "正在读取游戏资源":return "Reading game resources";
            case "正在装载游戏":return "Loading game";
            case "正在初始化游戏":return "Initializing game";
            case "游戏已退出":return "Game exited";
            case "游戏文件准备失败":return "Could not prepare game files";
            case "游戏初始化失败":return "Could not initialize game";
            case "游戏窗口不可用":return "Game window unavailable";
            case "游戏启动失败":return "Could not start game";
            case "无法读取游戏状态":return "Could not read game status";
            case "触摸输入不可用":return "Touch input unavailable";
            case "操作输入不可用":return "Action input unavailable";
            case "错误 ":return "Error ";
            case "不能创建游戏目录":return "Could not create game directory";
            case "模块清单无效":return "Invalid module manifest";
            case "模块清单重复或校验值无效":return "Duplicate module or invalid checksum";
            case "游戏模块校验失败":return "Game module checksum mismatch";
            case "不能保存游戏模块":return "Could not save game module";
            case "游戏模块清单不完整":return "Incomplete game module manifest";
            case "缺少原游戏资源":return "Original game resources missing";
            case "游戏资源版本不受支持":return "Unsupported game resource version";
            case "游戏模块与资源版本不匹配":return "Game module and resource versions differ";
            case "游戏资源校验失败":return "Game resource checksum mismatch";
            case "追加内容清单无效":return "Invalid add-on content manifest";
            case "缺少游戏图形缓存":return "Game graphics cache missing";
            case "输入内容":return "Enter text";
            case "设置":return "Settings";
            case "存档同步目录":return "Save sync folder";
            case "存档同步":return "Save sync";
            case "是否传输由游戏「自定义」中的存档同步开关控制。":return "Transfers are controlled by Save Sync in the game's Custom settings.";
            case "所选目录将在下次启动游戏时用于同步。":return "The selected folder will be used for sync the next time the game starts.";
            case "将在下次启动游戏时断开目录。":return "The folder will be disconnected the next time the game starts.";
            case "已连接":return "Connected";
            case "待重启":return "Restart required";
            case "未授权":return "Not authorized";
            case "授权已失效":return "Access expired";
            case "同步失败":return "Sync failed";
            case "选择目录":return "Choose folder";
            case "重新选择":return "Choose another folder";
            case "断开":return "Disconnect";
            case "关闭":return "Close";
            case "已同步到所选目录":return "Synced to the selected folder";
            case "正在同步到所选目录…":return "Syncing to the selected folder…";
            case "无法打开目录选择器":return "Could not open the folder picker";
            case "需要目录的读写授权":return "Read and write access to the folder is required";
            case "无法保存目录授权":return "Could not save folder access";
            case "无法保存目录设置":return "Could not save folder settings";
            default:return zh;
        }
    }
    String modeLabel(boolean touch){return text(touch?"菜单操控":"触摸操控");}
    String modeDescription(boolean touch){return text(touch?"切换到菜单操控":"切换到战斗触摸操控");}
    String menuDescription(String label){
        switch(label){case "←":return text("向左移动");case "→":return text("向右移动");case "↑":return text("向上移动");case "↓":return text("向下移动");default:return text(label);}
    }
    String controlLabel(int id,boolean navigation,boolean cancel){
        if(navigation&&id>=0&&id<5)return (chinese?NAV_ZH:NAV_EN)[id];
        if(id==11&&cancel)return text("取消");
        if(id>=0&&id<13)return (chinese?CONTROL_ZH:CONTROL_EN)[id];
        if(id>=20&&id<24)return text(MOVE_ZH[id-20]);
        return "";
    }
    String minimumCharacters(int min){return chinese?"至少输入 "+min+" 个字符":"Enter at least "+min+" characters";}
    private static final String[] CONTROL_ZH={"主技能","技能1","技能2","技能3","技能4","回城","药水1","药水2","药水3","药水4","菜单","地图","切换技能组"};
    private static final String[] CONTROL_EN={"Main skill","Skill 1","Skill 2","Skill 3","Skill 4","Town portal","Potion 1","Potion 2","Potion 3","Potion 4","Menu","Map","Switch skill set"};
    private static final String[] NAV_ZH={"确认／交换","返回","操作","绑定","下一页"},NAV_EN={"Confirm / Swap","Back","Action","Bind","Next Page"};
    private static final String[] MOVE_ZH={"向左移动","向右移动","向上移动","向下移动"};
}
