// 「自定义」设置：我们自己加的功能的启停开关。接口见 dm_custom.h。
// 存储：存档提交目录旁的 sw_custom.txt（iOS 上在 Documents 里，升级包不丢），一行一个「键=0/1」；没有这个文件 / 没写的键 = 各键缺省（省电模式关，其余开）；不认识的行（旧版的 drop_filter、ver）读时跳过。
// 掉落筛选照红叶 mod 一件都不藏，整块拆掉，drop_filter 键与它的 ver=2 迁移一起删了
// 不写进游戏自己的设置存档：不依赖它的格式与写盘时机，应用 / 放弃 / 默认由我们自己按原版选项页的语义做。
// 界面：选项面板「自定义」页（借原版视频页 VideoOptionsPanel），每行一个原版勾选框，控件名 TouchCustom_<键>（布局 touch_layout.py 的 _custom_page）。勾选只记成待定值，
// 「应用」（OptionsPanelMessage:AcceptChanges）才生效并存盘，「放弃」（CancelChanges）丢掉待定值，「默认」（ResetToDefaults）待定值回各键缺省（省电模式关、其余开；还要点应用）
// 「自定义」(Custom) settings: on/off switches for the features we added ourselves. For the interface see dm_custom.h.
// Storage: sw_custom.txt next to the save commit directory (on iOS it is in Documents and survives app upgrades), one 'key=0/1' per line; without this file / for keys not written = each key's default (power-saving mode off, the rest on); unrecognized lines (drop_filter and ver from old versions) are skipped when reading.
// Drop filtering follows the Hongye community mod and hides nothing, so the whole block was removed, and the drop_filter key was deleted together with its ver=2 migration
// Not written into the game's own settings save: no dependence on its format or on when it writes to disk; apply / discard / default are done by ourselves following the semantics of the original game's options page.
// UI: the 「自定义」(Custom) page of the options panel (borrowing the original game's video page VideoOptionsPanel), one original-game checkbox per row, widget name TouchCustom_<key> (_custom_page in the layout touch_layout.py). Checking only records a pending value;
// only 「应用」(Apply) (OptionsPanelMessage:AcceptChanges) makes it take effect and saves to disk, 「放弃」(Discard) (CancelChanges) throws away the pending values, and 「默认」(Default) (ResetToDefaults) sets the pending values back to each key's default (power-saving mode off, the rest on; Apply still has to be tapped)
#include "dm_custom.h"
#include "dm_widget.h"
#include "dm_cursor.h"
#include "hle.h"
#include "dm_input.h"
#include "dm_i18n.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_keys[] = { "lang", "auto_mana", "merc_pot", "drop_light", "map_mark", "item_notes", "icloud", "pot_pick", "drop_haptic", "gem_pick", "store_btn", "eco_fps", "q3d", "loot_filter", "auto_buff", "touch_swap_above", "show_fps", "render_mode", "clear_merge", "store_elide", "mod", "map_reveal" };   // 顺序 = 自定义页从上到下的行序（touch_layout.py _CUSTOM_ROWS）：手柄在这页按这个顺序上下选行 | Order = row order of the Custom page from top to bottom (touch_layout.py _CUSTOM_ROWS): the controller selects rows up/down on this page in this order
#define NK (int)(sizeof k_keys / sizeof *k_keys)
static const int k_def[NK] = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0 };   // 缺省值：省电模式（30 帧）缺省关、3D 画质缺省流畅优先（q3d=0），换组默认在传送门左侧，其余缺省开 | Defaults: power-saving mode (30 fps) off, 3D quality smoothness first (q3d=0), switch left of portal by default, everything else on
// q3d（3D 画质）：0 = 流畅优先（掉帧就自动降 3D 比例）、1 = 画质优先（不自动降）。生效 = 调 hle_oe.c 的 dm_3d_quality_mode_set，读盘与点「应用」时当场调，下一帧控制器就按新档走。
// 页上是上下两项单选（同语言行）：TouchCustom_q3d 是「画质优先」那格（勾上 = 1），TouchCustom_q3d_smooth 是它的反面
// q3d (3D quality): 0 = smoothness first (automatically lowers the 3D scale on frame drops), 1 = quality first (no automatic lowering). Applied by calling hle_oe.c's dm_3d_quality_mode_set, immediately on load and on tapping "Apply", so the controller follows the new setting from the next frame.
// On the page it is a stacked two-option radio (like the language row): TouchCustom_q3d is the "Quality first" cell (checked = 1), TouchCustom_q3d_smooth is its inverse
#define K_Q3D 12
#define K_TOUCH_SWAP 15
enum { K_SHOW_FPS=16, K_RENDER_MODE, K_CLEAR_MERGE, K_STORE_ELIDE, K_MOD, K_MAP_REVEAL };
#if defined(DM_ANDROID_HOST) && DM_ANDROID_HOST
extern void dm_android_custom_settings_init(const char *) __attribute__((weak));
extern int dm_android_custom_mod_count(void) __attribute__((weak));
extern void dm_android_custom_settings_load(int *,int *,int *,int *) __attribute__((weak));
extern int dm_android_custom_settings_apply(int,int,int,int) __attribute__((weak));
extern void dm_android_custom_settings_clear_error(void) __attribute__((weak));
extern void dm_android_custom_status(int,int,char *,size_t) __attribute__((weak));
#endif
static int custom_choices(int i) {
    if(i==0||i==K_RENDER_MODE)return 4;
#if defined(DM_ANDROID_HOST) && DM_ANDROID_HOST
    if(i==K_MOD)return dm_android_custom_mod_count?dm_android_custom_mod_count():1;
#endif
    return 2;
}
void dm_3d_quality_mode_set(int mode);
static const char *rev_box(int i) { return i == K_Q3D ? "TouchCustom_q3d_smooth" : i == K_TOUCH_SWAP ? "TouchCustom_touch_swap_above_near" : NULL; }   // 单选行「反面那一项」的勾选框 | Checkbox of the radio row's "inverse option"
static _Atomic int g_val[NK];        // 已生效：1 开 / 0 关 | In effect: 1 on / 0 off
static _Atomic int g_pend[NK];       // 选项页上改了还没应用：-1 = 没改 | Changed on the options page but not applied yet: -1 = not changed
static _Atomic int g_loaded;         // 0 没读盘 / 1 已读 | 0 not read from disk / 1 read
// lang（语言开关，自定义页第一行）：四项单选 0 中文 / 1 English / 2 Español / 3 Русский（勾选框 TouchCustom_lang_zh / _en / _es / _ru 互斥）。它不是功能开关，生效方式不同——
// 下次启动挂存档时由 hle_fs.c 按它改写原版设置文件的 Language / VoiceLanguage（游戏运行中自己会把内存里的语言写回设置文件，所以只能在游戏读设置之前改；
// 见 dm_custom_lang_explicit）。文件里没有这个键（还没点过应用；旧版本写的布尔键 lang_en 不再读——原版设置文件里本来就记着上次选的语言）
// = 不动原版设置，勾选框按当前实际语言显示；「默认」不改它（按一下默认把语言切了不合适）
// lang (language switch, first row of the custom page): four-option radio 0 Chinese / 1 English / 2 Español / 3 Russian (checkboxes TouchCustom_lang_zh / _en / _es / _ru are mutually exclusive). It is not a feature switch and takes effect differently -
// on the next launch, when the save is mounted, hle_fs.c rewrites Language / VoiceLanguage in the original settings file according to it (while running, the game itself writes its in-memory language back to the settings file, so it can only be changed before the game reads settings;
// see dm_custom_lang_explicit). The key is absent from the file (Apply never tapped; the boolean key lang_en written by older versions is no longer read - the original settings file already remembers the last chosen language)
// = leave the original settings alone, checkboxes show the current actual language; "Default" does not change it (switching language with one tap of Default would be wrong)
#define LANG_N 4
static const char *const k_lang_box[LANG_N] = { "TouchCustom_lang_zh", "TouchCustom_lang_en", "TouchCustom_lang_es", "TouchCustom_lang_ru" };
static const char *const k_lang_hint[LANG_N] = { "TouchCustomLangHintZh", "TouchCustomLangHintEn", "TouchCustomLangHintEs", "TouchCustomLangHintRu" };
static const char *const k_lang_name[LANG_N] = { "zh", "en", "es", "ru" };   // 只进日志 | Log only
static _Atomic int g_lang_explicit = -1;   // 文件里写着的 lang：-1 没写 / 0 中文 / 1 English / 2 Español / 3 Русский | lang as written in the file: -1 not written / 0 Chinese / 1 English / 2 Español / 3 Russian
static int g_lang_test;   // 测试开关 DM_TEST_LANG=<0|1|2>：当作玩家选了这一项并应用（走 hle_fs.c 挂存档时的真实改写），但不写回 sw_custom.txt（容器各局共用） | Test switch DM_TEST_LANG=<0|1|2>: treated as if the player selected this option and applied it (goes through the real rewrite in hle_fs.c when mounting the save), but not written back to sw_custom.txt (the container is shared by all runs)
static int lang_running(void) { int g = dm_lang_group(); return g == DM_LG_ZH ? 0 : g == DM_LG_ES || g == DM_LG_MX ? 2 : g == DM_LG_RU ? 3 : 1; }   // 此刻实际在跑的语言落在哪一项 | Which option the currently running language falls into
// merc_pot（佣兵自动喂药）原先缺省关、现在缺省开。旧文件每次存盘都把全部键写一遍，里面的 merc_pot=0 分不出「没动过」还是「自己关的」，
// 所以另记一行 merc_pot_v2：只有在缺省改成开之后玩家自己在页上改过这一项才写。读盘时有 merc_pot_v2 以它为准；没有 = 没在新缺省下动过 → 开
// （旧文件里的 merc_pot=1 也是开，结果一样）。merc_pot 那一行照旧写当前值，旧版本的包读它不出错
// merc_pot (mercenary auto-feed potions) used to be off by default and is now on by default. Old files write all keys on every save, and the merc_pot=0 in them cannot tell "never touched" from "turned off by the player",
// so a separate line merc_pot_v2 is recorded: it is written only when the player has changed this item on the page after the default was changed to on. On load, if merc_pot_v2 exists it is authoritative; absent = never touched under the new default → on
// (merc_pot=1 in an old file is also on, the same result). The merc_pot line is still written with the current value, so packages of older versions read it without error
static _Atomic int g_merc_explicit = -1;   // 文件里写着的 merc_pot_v2：-1 没写 / 0 / 1 | merc_pot_v2 as written in the file: -1 not written / 0 / 1
#define K_MERC 2
static _Atomic int g_repaint;        // 默认 / 放弃之后：页上的勾选框按待定值 / 生效值重摆一遍 | After default / discard: lay out the checkboxes on the page again according to pending values / effective values

const char *dm_save_commitdir(void);
static int key_index(const char *k) { if (k) for (int i = 0; i < NK; i++) if (!strcmp(k, k_keys[i])) return i; return -1; }
static char g_dir[1024];   // iOS 外壳启动时给的 Documents（main.m）：启动期（存档挂上之前）就要能读，覆盖层选择用 | Documents given by the iOS shell at startup (main.m): must be readable already during startup (before the save is mounted), used for overlay selection
void dm_custom_init(const char *dir) { if (dir) strlcpy(g_dir, dir, sizeof g_dir);
#if defined(DM_ANDROID_HOST) && DM_ANDROID_HOST
    if(dm_android_custom_settings_init)dm_android_custom_settings_init(dir);
#endif
}
static int file_path(char *p, size_t n) {
    if (g_dir[0]) { snprintf(p, n, "%s/sw_custom.txt", g_dir); return 1; }
    const char *c = dm_save_commitdir(); if (!c) return 0; snprintf(p, n, "%s/../sw_custom.txt", c); return 1; }

static void load_once(void) {
    if (atomic_load(&g_loaded)) return;
    char p[1100]; if (!file_path(p, sizeof p)) return;   // 存档还没挂上：先按缺省开答，挂上后再读 | The save is not mounted yet: answer with the default (on) for now, and read again after it is mounted
    int v[NK]; for (int i = 0; i < NK; i++) v[i] = k_def[i];
    FILE *f = fopen(p, "r"); char ln[128];
    int lang = -1, merc = -1;
    while (f && fgets(ln, sizeof ln, f)) { char *eq = strchr(ln, '='); if (!eq) continue; *eq = 0; int i = key_index(ln); if (i > 0) { int value=atoi(eq+1); if(value>=0&&value<custom_choices(i))v[i]=value; }
        if (i == 0) { int l = atoi(eq + 1); if (l >= 0 && l < LANG_N) lang = v[0] = l; } if (!strcmp(ln, "merc_pot_v2")) merc = eq[1] != '0'; }
    if (f) fclose(f);
    { const char *tl = getenv("DM_TEST_LANG"); if (tl && *tl >= '0' && *tl < '0' + LANG_N) { lang = v[0] = *tl - '0'; g_lang_test = 1; LOG("自定义设置：测试开关 DM_TEST_LANG=%d（当作语言行选了 %s 并应用，不写回文件）", lang, k_lang_name[lang]); } }
    if (lang < 0) v[0] = lang_running();   // 没写过：按当前实际语言显示 | Never written: display according to the current actual language
    atomic_store(&g_lang_explicit, lang);
    v[K_MERC] = merc < 0 ? k_def[K_MERC] : merc; atomic_store(&g_merc_explicit, merc);
#if defined(DM_ANDROID_HOST) && DM_ANDROID_HOST
    if(dm_android_custom_settings_load)dm_android_custom_settings_load(&v[K_RENDER_MODE],&v[K_CLEAR_MERGE],&v[K_STORE_ELIDE],&v[K_MOD]);
    if(dm_android_custom_settings_load)v[K_Q3D]=v[K_RENDER_MODE]!=0;
#endif
    for (int i = 0; i < NK; i++) { atomic_store(&g_val[i], v[i]); atomic_store(&g_pend[i], -1); }
    dm_3d_quality_mode_set(v[K_Q3D]); LOG("自定义设置：3D 画质读盘为%s（q3d=%d，已交给 3D 比例控制）", v[K_Q3D] ? "画质优先" : "流畅优先", v[K_Q3D]);
    if (!atomic_exchange(&g_loaded, 1)) LOG("自定义设置：读盘 %s%s lang=%d%s auto_mana=%d merc_pot=%d drop_light=%d map_mark=%d item_notes=%d icloud=%d pot_pick=%d drop_haptic=%d gem_pick=%d store_btn=%d eco_fps=%d", p, f ? "" : "（没有文件，全按缺省）", v[0], lang < 0 ? "（没写，按当前语言）" : "", v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
}
int dm_custom_get(const char *key) {
    load_once(); int i = key_index(key); if (i < 0) return 1;
    return atomic_load(&g_loaded) ? atomic_load(&g_val[i]) : k_def[i];
}
int dm_custom_lang_explicit(void) {
    /* Reading a portable setting does not authorize a title's file edits.
     * Android's generic mount separately requires the verified D2R hook. */
    load_once(); return atomic_load(&g_loaded) ? atomic_load(&g_lang_explicit) : -1;
}
int dm_fps_setting(void) { return dm_custom_get("eco_fps") ? 30 : 60; }   // 图形层 nvn_mtl.m mtl_fps_target 每帧读（弱符号的强定义） | Read every frame by mtl_fps_target in the graphics layer nvn_mtl.m (strong definition of a weak symbol)
static void save(void) {
    char p[1100], t[1120]; if (!file_path(p, sizeof p)) { LOG("【红】自定义设置：存档目录没挂上，没存盘"); return; }
    snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "w"); int ok = f != NULL;
    for (int i = 0; ok && i < NK; i++) {
#if !defined(DM_ANDROID_HOST) || !DM_ANDROID_HOST
        if(i>=K_SHOW_FPS)continue;
#endif
        if (i == 0 && (atomic_load(&g_lang_explicit) < 0 || g_lang_test)) continue; ok = fprintf(f, "%s=%d\n", k_keys[i], atomic_load(&g_val[i])) > 0; }   // lang 只在玩家真改过之后才写进文件 | lang is written to the file only after the player actually changed it
    if (ok && atomic_load(&g_merc_explicit) >= 0) ok = fprintf(f, "merc_pot_v2=%d\n", atomic_load(&g_merc_explicit)) > 0;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(t, p) == 0;   // 先写临时文件再换名：写到一半被杀也不会留下半个文件 | Write a temporary file first and then rename: being killed halfway through the write does not leave half a file behind
    LOG("自定义设置：存盘%s", ok ? "" : "【红】失败");
}
void dm_custom_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)arg; if (!cat || !act || strcmp(cat, "OptionsPanelMessage")) return;
    load_once();
#if defined(DM_ANDROID_HOST) && DM_ANDROID_HOST
    if(strcmp(act,"AcceptChanges")&&dm_android_custom_settings_clear_error)dm_android_custom_settings_clear_error();
    if(!strcmp(act,"AcceptChanges")&&dm_android_custom_settings_apply) {
        int values[5];for(int k=0;k<5;k++){int pd=atomic_load(&g_pend[K_SHOW_FPS+k]);values[k]=pd>=0?pd:atomic_load(&g_val[K_SHOW_FPS+k]);}
        int changed=0;for(int k=0;k<5;k++)changed|=values[k]!=atomic_load(&g_val[K_SHOW_FPS+k]);
        if(changed&&dm_android_custom_settings_apply&&!dm_android_custom_settings_apply(values[1],values[2],values[3],values[4]))return;
        atomic_store(&g_pend[K_Q3D],values[1]!=0);
    }
#endif
    if (!strcmp(act, "AcceptChanges")) { int ch = 0;
        for (int i = 0; i < NK; i++) { int pd = atomic_exchange(&g_pend[i], -1); if (pd >= 0 && pd != atomic_load(&g_val[i])) { atomic_store(&g_val[i], pd); ch = 1; if (i == K_Q3D) { int dm_3d_quality_mode_get(void); float dm_3d_scale_cur(void); dm_3d_quality_mode_set(pd); LOG("自定义设置：3D 画质 → %s（已交给 3D 比例控制，读回模式 %d，此刻 3D 比例 %.2f）", pd ? "画质优先" : "流畅优先", dm_3d_quality_mode_get(), dm_3d_scale_cur()); } if (i == 0) atomic_store(&g_lang_explicit, pd); if (i == K_MERC) atomic_store(&g_merc_explicit, pd); if (i == 0) LOG("自定义设置：语言 → %s（应用；下次启动生效）", k_lang_name[pd]); else LOG("自定义设置：%s → %s（应用）", k_keys[i], pd ? "开" : "关"); } }
        if (ch) save(); }
    else if (!strcmp(act, "CancelChanges")) { for (int i = 0; i < NK; i++) atomic_store(&g_pend[i], -1); atomic_store(&g_repaint, 1); }
    else if (!strcmp(act, "ResetToDefaults")) { for (int i = 1; i < NK; i++) atomic_store(&g_pend[i], k_def[i]); atomic_store(&g_repaint, 1); }   // 从 1 起：语言那一格不归「默认」管 | Starts from 1: the language cell is not governed by 「默认」(Default)
}

void dm_custom_cycle_mod(void) {
    load_once();int pd=atomic_load(&g_pend[K_MOD]),shown=pd>=0?pd:atomic_load(&g_val[K_MOD]);
    atomic_store(&g_pend[K_MOD],(shown+1)%custom_choices(K_MOD));
}

/* Widget policy is shared; Android supplies a verified title transport. */
#include "dm_custom_ui.h"
#include "dm_custom_ui_native.inc"
#include "dm_custom_ui.inc"
