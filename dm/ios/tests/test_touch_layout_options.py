"""现有自定义页中两位置换组开关的跨语言契约。 | Cross-language contract for the two-position switch in the existing Custom page."""
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'ios/tools'))
import touch_layout as layout


def generated_page(platform):
    keys = layout._custom_keys(platform)
    out, count = layout._custom_page(
        '"name": "VideoOptionsPanel", "name": "OptionsTable"', keys)
    page = json.loads(re.sub(r',\s*([}\]])', r'\1', out))
    nodes = {}

    def walk(node):
        if isinstance(node, dict):
            if 'name' in node:
                assert node['name'] not in nodes, node['name']
                nodes[node['name']] = node
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(page)
    _, _, expected_height = layout._custom_geometry(keys)
    assert nodes['TouchCustomRows']['fields']['rect']['height'] == expected_height
    assert count == len(keys) + 2
    return keys, nodes, expected_height


host = (root / 'host/dm_custom.c').read_text(encoding='utf-8')
host_keys = re.findall(
    r'"([a-z_0-9]+)"',
    re.search(r'k_keys\[\].*?=\s*\{(.*?)\}', host, re.S)[1])
assert host_keys == [key for key, _ in layout._CUSTOM_ALL] + list(layout._ANDROID_KEYS)
defaults = [int(x) for x in re.search(
    r'k_def\[NK\].*?=\s*\{(.*?)\}', host, re.S)[1].split(',')]
assert len(defaults) == len(host_keys)
assert defaults[host_keys.index('touch_swap_above')] == 0
assert 'TouchCustom_touch_swap_above_near' in host

# iOS and macOS keep the complete shared Custom page, including iCloud.
ios_keys, ios_nodes, ios_height = generated_page('ios')
assert ios_keys == layout._CUSTOM_ALL
assert 'TouchCustom_icloud' in ios_nodes

# Android intentionally omits the platform-cloud row while preserving q3d and
# the touch-layout swap setting. The host key table remains shared and ordered.
android_keys, android_nodes, android_height = generated_page('android')
assert 'icloud' not in [key for key, _ in android_keys]
assert 'TouchCustom_icloud' not in android_nodes
assert 'TouchCustom_q3d' not in android_nodes
assert 'TouchCustom_touch_swap_above' in android_nodes
assert android_height > ios_height
assert 'TouchAndroidSettings' not in ios_nodes and 'TouchAndroidSettings' not in android_nodes
for key in ('show_fps', 'clear_merge', 'store_elide'):
    assert android_nodes['TouchCustom_' + key]['type'] == 'ToggleButtonWidget'
for mode in range(4):
    assert android_nodes['TouchCustom_render_mode_' + str(mode)]['type'] == 'ToggleButtonWidget'
for key in ('mod', 'map_reveal'):
    assert android_nodes['TouchCustom_' + key]['type'] == 'ButtonWidget'
    assert android_nodes['TouchCustom_' + key]['fields']['onClickMessage'].endswith('TouchCustom_' + key)
assert android_nodes['TouchCustom_lang_Text']['fields']['rect']['y'] == layout._CUSTOM_TITLE_H
for lang in ('Zh', 'En', 'Es', 'Ru'):
    name = 'TouchCustomLang%s_Text' % lang
    assert android_nodes[name]['fields']['style/pointSize'] == 36
    assert 'style/pointSize' not in ios_nodes[name]['fields']

for nodes in (ios_nodes, android_nodes):
    for name in ('TouchCustom_touch_swap_above', 'TouchCustom_touch_swap_above_near'):
        assert nodes[name]['type'] == 'ToggleButtonWidget'
        assert nodes[name]['fields']['onClickMessage'].endswith(name)
    assert nodes['TouchCustom_touch_swap_above_Text']['fields']['rect']['height'] == 360
    for suffix in ('Near', 'Far'):
        assert any(n.get('fields', {}).get('text') == '@dmCustomTouchSwap' + suffix
                   for n in nodes.values())

print('PASS touch layout options: iOS iCloud retained, Android row omitted, '
      'dynamic geometry, labels, defaults and host order verified')
