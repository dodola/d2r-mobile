"""Standalone Android debug workspace built from native game widgets.
Chronicle supplies a registered panel shell; its ordinary UI stays separate.
Coordinates use the game 2160-high canvas and a centred 3060x2040 workspace.
Runtime (d2r_debug_ui.c) moves whole groups off-screen, toggles the "Hi"
highlight rectangles and fills the text boxes; this file only declares them.
"""
import json

ROWS = 6
CATEGORIES = ('暗金装备', '套装装备', '符文', '宝石', '武器底材', '护甲底材', '药水与杂物', '场景跳转', '调试工具')
FILTERS = ('全部', '武器', '护甲', '饰品', '')
BORDER = 'PANEL/TooltipBorder/TooltipBorder'
GOLD = '$FontColorGoldYellow'
LIGHT_GOLD = '$FontColorLightGold'
GREEN = '$FontColorGreen'
WHITE = '$FontColorWhite'
GRAY = '$FontColorGray'


def text(name, x, y, width, height, value='', size=44, color=WHITE, h='left'):
    return {'type': 'TextBoxWidget', 'name': name, 'fields': {
        'rect': {'x': x, 'y': y, 'width': width, 'height': height}, 'text': value,
        'style': {'fontColor': color, 'pointSize': size,
                  'alignment': {'h': h, 'v': 'center'}}}}


def panel(name, x, y, width, height, color=(0.055, 0.06, 0.07, 1.0)):
    return {'type': 'NineTileImageWidget', 'name': name, 'fields': {
        'rect': {'x': x, 'y': y, 'width': width, 'height': height},
        'filename': BORDER, 'tileScale': 1, 'backgroundColor': list(color)}}


def rule(name, x, y, width, height, color):
    return {'type': 'RectangleWidget', 'name': name, 'fields': {
        'rect': {'x': x, 'y': y, 'width': width, 'height': height}, 'color': list(color)}}


def button(name, label, x, y, width=560, height=150, message=None, size=44,
           color=(0.10, 0.12, 0.13, 1.0), highlight=False, text_color=WHITE, h='center'):
    # Native image buttons derive their bounds from the sprite, ignoring width.
    # A fixed transparent native key sprite supplies the hit area; the border
    # and text are sibling children so text is not distorted by image scaling.
    # The runtime widens every ButtonWidget to its parent box once loaded.
    kids = [{'type': 'NineTileImageWidget', 'name': 'Border', 'fields': {
        'rect': {'width': width, 'height': height},
        'filename': BORDER, 'tileScale': 1, 'backgroundColor': list(color)}}]
    if highlight:
        kids.append(rule('TouchDBG_' + name + 'Hi', 6, 6, width - 12, height - 12, (0.62, 0.46, 0.16, 0.42)))
    kids += [
        {'type': 'ButtonWidget', 'name': 'TouchDBG_' + name, 'fields': {
            'rect': {'width': width, 'height': height},
            'filename': 'Ztouch_controller_buttons_ui/switch_ButtonRight',
            'sound': 'cursor_menu_select_hd',
            'onClickMessage': message or 'PanelManager:ClosePanel:TouchDBG_' + name}},
        text('TouchDBG_' + name + 'Text', 36, 0, width - 72, height, label, size, text_color, h)]
    return {'type': 'Widget', 'name': 'TouchDBG_' + name + 'Box', 'fields': {
        'rect': {'x': x, 'y': y, 'width': width, 'height': height}}, 'children': kids}


def group(name, children):
    return {'type': 'Widget', 'name': name, 'fields': {
        'rect': {'x': 0, 'y': 0, 'width': 3060, 'height': 2040}}, 'children': children}


def board():
    chrome = [
        panel('Frame', 0, 0, 3060, 2040, (0.035, 0.04, 0.047, 1.0)),
        text('TouchDBG_Title', 60, 24, 1500, 110, 'DEBUG 调试工作台', 72, LIGHT_GOLD),
        text('TouchDBG_Status', 60, 124, 2200, 70, '', 34, GRAY),
        button('Close', '返回游戏', 2420, 36, 580, 150, 'PanelManager:ClosePanel:ChroniclePanel',
               48, (0.20, 0.10, 0.08, 1.0)),
        rule('TouchDBG_TitleRule', 60, 206, 2940, 4, (0.62, 0.46, 0.16, 1.0)),
        panel('SideFrame', 40, 240, 600, 1760, (0.05, 0.055, 0.065, 1.0)),
        panel('MainFrame', 670, 240, 2350, 1760, (0.05, 0.055, 0.065, 1.0))]
    chrome += [button('Category%d' % i, label, 70, 270 + i * 170, 540, 150, size=46, highlight=True)
               for i, label in enumerate(CATEGORIES)]
    rows = []
    for i in range(ROWS):
        row = button('Row%d' % i, '', 710, 420 + i * 165, 2270, 150, highlight=True)
        label = row['children'][-1]
        label['fields']['style']['alignment']['h'] = 'left'
        label['name'] = 'TouchDBG_RowText%d' % i
        for suffix, color in (('U', LIGHT_GOLD), ('S', GREEN)):
            extra = json.loads(json.dumps(label))
            extra['name'] = 'TouchDBG_RowText%s%d' % (suffix, i)
            extra['fields']['style']['fontColor'] = color
            row['children'].append(extra)
        rows.append(row)
    rows += [button('Filter%d' % i, label, 710 + i * 460, 275, 440, 115, size=42, highlight=True)
             for i, label in enumerate(FILTERS)]
    rows += [button('Prev', '上一页', 710, 1440, 600, 160),
             text('TouchDBG_Page', 1330, 1440, 700, 160, '', 40, GRAY, 'center'),
             button('Next', '下一页', 2380, 1440, 600, 160),
             text('TouchDBG_Selection', 710, 1630, 2270, 100, '请先选择分类和条目', 48, LIGHT_GOLD),
             button('Add', '放到地面', 710, 1760, 1100, 190, size=52, color=(0.26, 0.19, 0.07, 1.0))]
    tools = [text('TouchDBG_ToolsTitle', 710, 275, 2270, 90, '常用调试工具', 56, LIGHT_GOLD),
             button('Heal', '恢复生命与法力', 710, 395, 1100, 190, size=50),
             button('Gold', '金币 +100,000', 1880, 395, 1100, 190, size=50),
             button('Reveal', '揭示当前地图', 710, 625, 1100, 190, size=50),
             button('Skill', '技能点 +10', 1880, 625, 1100, 190, size=50),
             button('Stat', '属性点 +100', 710, 855, 1100, 190, size=50),
             text('TouchDBG_ShopNote', 710, 1110, 2270, 300,
                  '调试商店（实时）：所有商人的商品价格降为 1 金币；符文、完美宝石、'
                  '回复药剂、传送门卷册、珠宝、钥匙等已加入商人的固定库存。开启调试后下次启动生效，'
                  '商店库存在新游戏中刷新。', 40, WHITE)]
    chrome += [group('TouchDBG_ListGroup', rows), group('TouchDBG_ToolsGroup', tools)]
    chrome.append(text('TouchDBG_Notice', 710, 1960, 2270, 70, '', 34, GRAY))
    # Healing/gold only appear in Tools; runtime moves the idle group out.
    return {'type': 'Widget', 'name': 'TouchDBG_Body', 'fields': {
        'rect': {'x': 99999}, 'fitToScreen': True}, 'children': [
        {'type': 'RectangleWidget', 'name': 'Dim', 'fields': {
            'fitToScreen': True, 'color': [0.0, 0.0, 0.0, 1.0]}, 'children': [
            {'type': 'ClickCatcherWidget', 'name': 'Catcher', 'fields': {'fitToParent': True}}]},
        {'type': 'Widget', 'name': 'Workspace', 'fields': {
            'anchor': {'x': 0.5, 'y': 0.5},
            'rect': {'x': -1530, 'y': -1020, 'width': 3060, 'height': 2040}},
         'children': chrome}]}


def inject(s):
    end=s.rstrip().rstrip('}').rstrip()
    if not end.endswith(']'): raise ValueError('Chronicle: missing root children')
    at=len(end)-1
    return s[:at].rstrip()+',\n'+json.dumps(board(),ensure_ascii=False)+'\n'+s[at:]


def hud(s):
    # HelpButtonPanel is already registered and follows the in-game HUD lifetime.
    entry=button('Open', 'DEBUG', 0, 0, 440, 160)
    entry['name']='TouchDBG_Entry'
    entry['fields']['rect']['x']=99999
    return json.dumps({'type':'HelpButtonPanel','name':'HelpButtonPanel','fields':{
        'priority':-1, 'anchor':{'x':0,'y':0},
        'rect':{'x':80,'y':390},
        'skillUpActiveHelpPanelPosition':{'x':80,'y':390}},
        'children':[{'type':'ButtonWidget','name':'Help','fields':{'rect':{'x':99999},
        'filename':'PANEL/HUD_02/quest_button'}},entry]},ensure_ascii=False),1
