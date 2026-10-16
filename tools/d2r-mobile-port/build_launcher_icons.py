"""Create Android launcher density assets from the original generated PNG.

This is deterministic packaging/resampling, not a redraw. Requires Pillow.
"""
import argparse
import os
from pathlib import Path
from PIL import Image

SCRIPT_ROOT=Path(__file__).resolve().parents[2]
ROOT=Path(os.environ.get('ANDROID_SWITCH_WORKSPACE',str(SCRIPT_ROOT))).resolve()
REPO=Path(os.environ.get('D2R_ANDROID_SOURCE_ROOT',str(SCRIPT_ROOT if (SCRIPT_ROOT/'dm/android/src').is_dir() else ROOT/'runtime/d2r-mobile'))).resolve()


def centered(art,side,fraction,background):
    result=Image.new('RGBA',(side,side),background)
    image=art.copy();image.thumbnail((round(side*fraction),round(side*fraction)),Image.Resampling.LANCZOS)
    result.alpha_composite(image,((side-image.width)//2,(side-image.height)//2))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=REPO/'dm/android/artwork/d2r-icon-source.png')
    args=parser.parse_args()
    source=Image.open(args.source).convert('RGBA')
    if source.getextrema()[3][0]!=0:raise ValueError('Foreground source must preserve real transparency')
    art=source.crop(source.getbbox())
    res=REPO/'dm/android/res'
    foreground=res/'drawable-nodpi/ic_launcher_foreground.png';foreground.parent.mkdir(parents=True,exist_ok=True)
    centered(art,1024,0.60,(0,0,0,0)).save(foreground,optimize=True)
    for density,side in [('mdpi',48),('hdpi',72),('xhdpi',96),('xxhdpi',144),('xxxhdpi',192)]:
        path=res/f'mipmap-{density}/ic_launcher.png';path.parent.mkdir(parents=True,exist_ok=True)
        centered(art,side,0.80,(13,13,15,255)).convert('RGB').save(path,optimize=True)
    centered(art,512,0.80,(13,13,15,255)).convert('RGB').save(REPO/'dm/android/artwork/d2r-icon-preview.png',optimize=True)
    print('Created adaptive foreground, five legacy densities, and 512px preview')


if __name__=='__main__':main()
