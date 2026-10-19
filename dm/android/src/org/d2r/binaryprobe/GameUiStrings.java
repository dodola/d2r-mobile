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
            case "不能创建管线缓存目录":return "Could not create the pipeline cache directory";
            case "管线缓存名称无效":return "Invalid pipeline cache name";
            case "不能保存管线缓存":return "Could not save the pipeline cache";
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
