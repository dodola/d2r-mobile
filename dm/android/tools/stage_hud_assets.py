#!/usr/bin/env python3
"""Stage verified upstream HUD art as Android APK assets (never tracked art)."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from PIL import Image

parser=argparse.ArgumentParser()
parser.add_argument('--ios-generated-root',type=Path,required=True,help='iOS Sources parent containing Sources/TouchArt')
parser.add_argument('--generated-assets',type=Path,required=True,help='DM_GEN_ASSETS output')
parser.add_argument('--output',type=Path,required=True,help='new APK asset root; creates d2r_hud below it')
args=parser.parse_args()
source=args.ios_generated_root/'Sources/TouchArt'
manifest=json.loads((source/'native_art_manifest.json').read_text(encoding='utf-8'))
ui_source=args.generated_assets/'NativeHUD/v4/hud_v4_ui'
ui=json.loads((ui_source/'ui.json').read_text(encoding='utf-8'))
assert ui['ref']==[2868,1320]
out=args.output/'d2r_hud';out.mkdir(parents=True,exist_ok=True)
staged=[]
def copy(path,relative):
    target=out/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
    staged.append(dict(path=str(target.relative_to(args.output)).replace('\\','/'),bytes=target.stat().st_size,
                       sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
for row in manifest['assets']:
    path=source/row['asset'];data=path.read_bytes()
    assert hashlib.sha256(data).hexdigest()==row['decoded_png_sha256'],path
    with Image.open(path) as image:assert list(image.size)==row['atlas_px'],path
    copy(path,row['asset'])
    if row['asset'].startswith('dm_native_skill_'):
        silhouette=ui_source/('sil_'+row['asset'])
        with Image.open(silhouette) as image:assert list(image.size)==row['atlas_px'],silhouette
socket=args.ios_generated_root/'Sources/ControlArt/dm_mobile_socket.png'
with Image.open(socket) as image:assert image.width==image.height and image.width>0,socket
copy(socket,socket.name)
for section in ('vital','lv'):
    glyphs=ui[section]['glyphs']
    required='生命法力: /0123456789LifeMana' if section=='vital' else 'Lv 0123456789·%'
    assert all(ch in glyphs for ch in required)
    for glyph in glyphs.values():
        if glyph['file']:
            with Image.open(ui_source/glyph['file']) as image:assert image.width>0 and image.height>0
for path in sorted(ui_source.iterdir()):
    if path.is_file() and path.suffix in ('.png','.json'):copy(path,Path('hud_v4_ui')/path.name)
report=dict(passed=True,official_atlases=9,fixed_function_assets=3,socket_source=str(socket),reference_px=ui['ref'],files=staged,
            native_art_source=str(args.ios_generated_root/'Sources/dm_touch_native_art.c'))
(args.output/'d2r-hud-assets-manifest.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('ANDROID_HUD_ASSETS_OK atlases=9 glyph_sections=2 files=%d bytes=%d' % (len(staged),sum(x['bytes'] for x in staged)))
