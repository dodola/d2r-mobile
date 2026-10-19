package org.d2r.binaryprobe;

import java.util.Locale;

public final class GameUiStringsTest {
    private static int checks;
    private static void check(boolean value){checks++;if(!value)throw new AssertionError("check "+checks);}
    public static void main(String[] args){
        GameUiStrings s=new GameUiStrings(Locale.SIMPLIFIED_CHINESE);
        check(s.chinese()&&s.modeLabel(false).equals("触摸操控"));
        check(!s.updateHudLanguage(null)&&!s.updateHudLanguage(new long[19])&&s.chinese());
        long[] h=new long[107];h[0]=99;check(!s.updateHudLanguage(h)&&s.chinese());
        h[0]=2;h[18]=0;check(s.updateHudLanguage(h)&&!s.chinese());
        check(s.modeLabel(false).equals("Touch controls")&&s.modeLabel(true).equals("Menu controls"));
        check(s.modeDescription(true).equals("Switch to menu controls"));
        check(s.menuDescription("←").equals("Move left")&&s.text("确认").equals("Confirm"));
        check(s.controlLabel(0,true,false).equals("Confirm / Swap"));
        check(s.controlLabel(0,false,false).equals("Main skill"));
        check(s.controlLabel(11,false,true).equals("Cancel"));
        check(s.controlLabel(11,false,false).equals("Map"));
        check(s.controlLabel(12,false,false).equals("Switch skill set"));
        check(s.controlLabel(23,false,false).equals("Move down"));
        check(s.text("正在初始化游戏").equals("Initializing game"));
        check(s.minimumCharacters(3).equals("Enter at least 3 characters"));
        check(!s.updateHudLanguage(h)&&!s.chinese());
        // A changed current HUD overrides the device locale; unrelated or old ABI data does not.
        h[18]=1;check(s.updateHudLanguage(h)&&s.chinese());
        check(s.controlLabel(0,true,false).equals("确认／交换"));
        check(s.controlLabel(11,false,true).equals("取消"));
        check(s.menuDescription("↑").equals("向上移动"));
        GameUiStrings english=new GameUiStrings(Locale.US);check(!english.chinese());
        long[] old=new long[88];old[0]=1;old[18]=1;check(english.updateHudLanguage(old)&&english.chinese());
        for(int id=0;id<13;id++)check(!english.controlLabel(id,false,false).isEmpty());
        System.out.println("ANDROID_GAME_UI_STRINGS checks="+checks+" hud_current_language=1 menu=1 accessibility=1 passed=1");
    }
}
