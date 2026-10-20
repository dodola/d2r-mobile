"""Create the illustrated Chinese PDF guide from this release's real screenshots.

Requires the prepared AndroidSwitch workspace and reportlab/Pillow/pypdf.
System installation and file navigation are deliberately labelled diagrams.
"""
from pathlib import Path
from io import BytesIO
import json
import os
import shutil
from xml.sax.saxutils import escape

from PIL import Image
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.utils import ImageReader
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from reportlab.platypus import Paragraph

ROOT=Path(os.environ.get('ANDROID_SWITCH_WORKSPACE','D:/AndroidSwitch')).resolve()
REPO=Path(__file__).resolve().parents[2]
SHOTS=ROOT/'build/offline-installer-tests'
TEMP=ROOT/'tmp/pdfs/d2r-guide'
OUT=ROOT/'output/pdf/D2R-Mobile-图文安装指南.pdf'
RELEASE=ROOT/'analysis/d2r-mobile-port/release-94012/pack'
W,H=A4
M=44
CW=W-2*M
PAPER=colors.HexColor('#faf7f0')
INK=colors.HexColor('#252620')
MUTED=colors.HexColor('#6e6a60')
RED=colors.HexColor('#922e23')
LINE=colors.HexColor('#d8cfbf')
PALE=colors.HexColor('#f0e7d7')
DARK=colors.HexColor('#0d0d0f')
PARCHMENT=colors.HexColor('#e9d9ba')
GOLD=colors.HexColor('#b59a69')
WHITE=colors.white
TOTAL=8
PAGE=0
BOXES=[]


def main():
    global C
    TEMP.mkdir(parents=True,exist_ok=True);OUT.parent.mkdir(parents=True,exist_ok=True)
    pdfmetrics.registerFont(TTFont('Guide','C:/Windows/Fonts/msyh.ttc',subfontIndex=0))
    pdfmetrics.registerFont(TTFont('GuideBold','C:/Windows/Fonts/msyhbd.ttc',subfontIndex=0))
    pdfmetrics.registerFontFamily('Guide',normal='Guide',bold='GuideBold',italic='Guide',boldItalic='GuideBold')
    C=canvas.Canvas(str(OUT),pagesize=A4,pageCompression=1)
    C.setTitle('D2R Mobile 安卓离线安装图文指南')
    C.setAuthor('D2R Mobile 安装指南')
    C.setSubject('APK 安装、离线资源包选择、导入校验、启动设置与常见问题')

    cover()
    install()
    choose()
    locate()
    importing()
    finished()
    driver_selection()
    problems()
    C.save()
    assert PAGE==TOTAL
    from pypdf import PdfReader
    reader=PdfReader(str(OUT));assert len(reader.pages)==TOTAL
    text='\n'.join(page.extract_text() for page in reader.pages)
    for expected in ['D2R-Mobile.apk','D2R-Offline-94012.d2rpack','20 GB','9.5 GB','校验失败','覆盖安装']:
        assert expected in text,expected
    (TEMP/'text-extracted.txt').write_text(text,encoding='utf-8')
    (TEMP/'layout-boxes.json').write_text(json.dumps(BOXES,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    RELEASE.mkdir(parents=True,exist_ok=True);shutil.copy2(OUT,RELEASE/OUT.name)
    print(json.dumps({'pdf':str(RELEASE/OUT.name),'pages':TOTAL,'bytes':OUT.stat().st_size,'text_checks_passed':True},ensure_ascii=False))


def box(x,top,width,height,fill=WHITE,stroke=None,radius=0):
    C.setFillColor(fill)
    C.setStrokeColor(stroke or fill)
    if radius:C.roundRect(x,H-top-height,width,height,radius,stroke=bool(stroke),fill=1)
    else:C.rect(x,H-top-height,width,height,stroke=bool(stroke),fill=1)


def line(x1,t1,x2,t2,color=LINE,width=0.7):
    C.setStrokeColor(color);C.setLineWidth(width);C.line(x1,H-t1,x2,H-t2)


def p(text,x,top,width,size=12,leading=None,color=INK,bold=False,maxh=None,align=0):
    style=ParagraphStyle('text',fontName='GuideBold' if bold else 'Guide',fontSize=size,leading=leading or size*1.55,
                         textColor=color,wordWrap='CJK',alignment=align,spaceAfter=0,allowWidows=0,allowOrphans=0)
    paragraph=Paragraph(text,style);_,height=paragraph.wrap(width,H)
    assert top+height<=H-38,(PAGE,text,top,height)
    assert x>=M-2 and x+width<=W-M+2,(PAGE,text,x,width)
    if maxh is not None:assert height<=maxh,(PAGE,text,height,maxh)
    paragraph.drawOn(C,x,H-top-height)
    BOXES.append({'page':PAGE,'text':text,'x':x,'top':top,'width':width,'height':height})
    return height


def image(path,x,top,width,height=None,crop=None):
    img=Image.open(path)
    if crop:img=img.crop(crop)
    if height is None:height=width*img.height/img.width
    if path.name=='automatic-game-start.png':
        img.thumbnail((1800,1800),Image.Resampling.LANCZOS)
        compressed=BytesIO();img.convert('RGB').save(compressed,format='JPEG',quality=90,subsampling=0,optimize=True)
        compressed.seek(0);reader=ImageReader(compressed)
    else:reader=ImageReader(img)
    C.drawImage(reader,x,H-top-height,width=width,height=height,mask='auto')
    return height


def circle_number(number,x,top,color=RED,radius=12):
    C.setFillColor(color);C.circle(x+radius,H-top-radius,radius,stroke=0,fill=1)
    C.setFillColor(WHITE);C.setFont('GuideBold',11)
    C.drawCentredString(x+radius,H-top-radius-4,str(number))


def new_page(key,title,subtitle=None,dark=False):
    global PAGE
    PAGE+=1;C.bookmarkPage(key);C.addOutlineEntry(title,key,0,False)
    box(0,0,W,H,DARK if dark else PAPER)
    if not dark:
        p('D2R MOBILE  /  安卓离线安装指南',M,30,CW,9.2,color=MUTED,bold=True)
        line(M,61,W-M,61)
        p(title,M,81,CW,25,leading=33,bold=True)
        if subtitle:p(subtitle,M,128,CW,11.4,leading=17,color=MUTED,maxh=35)
    C.setFillColor(GOLD if dark else MUTED);C.setFont('Guide',8.5)
    C.drawString(M,24,'2026-10-16  ·  配套 APK + 离线资源包')
    C.drawRightString(W-M,24,f'{PAGE:02d} / {TOTAL:02d}')


def end_page():C.showPage()


def note(title,body,x,top,width,height):
    box(x,top,width,height,PALE)
    box(x,top,3,height,RED)
    p(title,x+15,top+12,width-30,13,bold=True,maxh=23)
    p(body,x+15,top+40,width-30,11.2,leading=17.4,maxh=height-47)


def cover():
    new_page('cover','暗黑离线安装指南',dark=True)
    p('D2R MOBILE  /  ANDROID',M,45,CW-110,10,color=GOLD,bold=True)
    p('暗黑离线安装指南',M,83,CW-90,30,leading=40,color=PARCHMENT,bold=True)
    p('安装 APK，首次导入资源包，完成后点击启动。',M,137,CW-90,12.2,color=PARCHMENT,maxh=42)
    image(REPO/'dm/android/artwork/d2r-icon-preview.png',W-M-91,56,91,91)
    gap=14;card=(CW-gap)/2
    for x,label,name,detail in [(M,'文件 1 · 应用安装包','D2R-Mobile.apk','约 69 MB · 点击它开始安装'),
                                (M+card+gap,'文件 2 · 完整离线资源包','D2R-Offline-94012.d2rpack','约 9.24 GB · 导入时选择它')]:
        box(x,201,card,103,colors.HexColor('#201d18'),colors.HexColor('#453b2d'))
        p(label,x+14,213,card-28,10,color=GOLD,bold=True)
        p(name,x+14,239,card-28,11.7,leading=17,color=PARCHMENT,bold=True,maxh=34)
        p(detail,x+14,278,card-28,9.5,color=PARCHMENT,maxh=17)
    p('建议先预留约 20 GB 手机空间',M,326,CW,17,leading=23,color=PARCHMENT,bold=True)
    p('包含下载和导入后的副本；资源包已在手机上时，还需额外约 9.5 GB。',M,359,CW,10.5,color=GOLD,maxh=32)
    stepw=(CW-24)/3
    for index,(label,key) in enumerate([('安装 APK','install'),('选择资源包','choose'),('校验后启动','import')]):
        x=M+index*(stepw+12);circle_number(index+1,x,411,color=RED)
        p(label,x+32,412,stepw-32,11.5,color=PARCHMENT,bold=True,maxh=35)
        C.linkRect('',key,(x,H-455,x+stepw,H-401),relative=0,thickness=0)
    p('无需解压资源包，也不用连接电脑。',M,457,CW,11,color=GOLD)
    image(SHOTS/'automatic-game-start.png',M,493,CW,crop=(0,80,2560,1520))
    p('旧版实机截图：游戏主菜单。新版导入完成后需点击启动。',M,786,CW,8.5,leading=12,color=GOLD)
    end_page()


def install():
    new_page('install','01  先安装 APK','请从同一份发布资料中取得 APK 和资源包，再按下面操作。')
    left=246
    steps=[('在下载目录找到 APK','打开手机的“文件”或浏览器下载记录，点击 <b>D2R-Mobile.apk</b>。'),
           ('按系统提示允许安装','如系统要求，允许当前浏览器或文件管理器安装应用，再返回安装页。不同品牌的选项名称可能不同。'),
           ('完成后点“打开”','也可以回到桌面，点击熔火头盔图标，应用名称为 <b>D2R Mobile</b>。')]
    for i,(title,body) in enumerate(steps):
        top=185+i*119;circle_number(i+1,M,top)
        p(title,M+34,top,left-34,13.3,bold=True,maxh=25)
        p(body,M+34,top+31,left-34,11.5,leading=18,maxh=79)
    x=325;top=197;width=W-M-x
    box(x,top,width,285,WHITE,LINE,radius=12)
    image(REPO/'dm/android/artwork/d2r-icon-preview.png',x+20,top+22,60,60)
    p('D2R Mobile',x+20,top+99,width-40,16,bold=True)
    p('是否安装此应用？',x+20,top+142,width-40,12,color=MUTED)
    box(x+width-91,top+220,72,39,RED,radius=6)
    p('安装',x+width-91,top+229,72,12,color=WHITE,bold=True,align=1)
    p('取消',x+20,top+229,65,12,color=MUTED)
    p('系统安装界面示意',x,top+305,width,9.2,color=MUTED,align=1)
    note('大的资源包先留着，下一步还要选',
         '先安装小的 <b>.apk</b>。大的 <b>.d2rpack</b> 是游戏资源，暂时不用打开或解压。系统安装页的按钮位置以你的手机实际显示为准。',M,590,CW,112)
    p('设备准备',M,731,CW,11,bold=True)
    p('Android 10 及以上、64 位 ARM，并支持游戏所需的 Vulkan 功能。不同机型的兼容性仍需实际验证。',M,755,CW,10.5,leading=16,maxh=34)
    end_page()


def choose():
    new_page('choose','02  打开应用，选择资源包','首次打开后，先等应用检查已有游戏文件。')
    x=M;top=183;width=CW;crop=(530,300,2030,1390)
    height=image(SHOTS/'guide-install-ready.png',x,top,width,crop=crop)
    scale=width/1500
    bx=x+(560-530)*scale;bt=top+(932-300)*scale;bw=1440*scale;bh=117*scale
    C.setStrokeColor(colors.HexColor('#f65e42'));C.setLineWidth(2)
    C.roundRect(bx,H-bt-bh,bw,bh,5,stroke=1,fill=0)
    p('实机截图：检查完成后，“选择离线资源包”按钮变为可点击。',M,top+height+12,CW,9.2,color=MUTED)
    p('点击红框中的“选择离线资源包”。',M,604,CW,16,leading=24,bold=True)
    p('接下来会打开手机系统的文件选择器。在里面找到已经下载好的 .d2rpack 文件。下一页给出查找路径。',M,640,CW,12,leading=19,maxh=58)
    note('还显示“正在检查游戏文件”？',
         '等检查完成、按钮可用后再点。已安装过资源的用户会看到启动入口，选择驱动后点击“启动游戏”，不需要重复导入。',M,716,CW,84)
    end_page()


def locate():
    new_page('locate','03  找到 .d2rpack 文件','文件夹位置因浏览器而异，重点是选对文件扩展名。')
    x=M;top=185;width=CW;height=321;side=123
    box(x,top,width,height,WHITE,LINE,radius=10)
    box(x,top,side,height,PALE)
    p('文件位置',x+14,top+19,side-28,11,bold=True)
    p('下载内容',x+14,top+74,side-28,10.5)
    box(x+8,top+111,side-16,55,WHITE)
    p('手机/平板存储',x+14,top+128,side-28,10.2,color=RED,bold=True)
    p('其他文件夹',x+14,top+194,side-28,10.5,color=MUTED)
    rx=x+side+18;rw=width-side-36
    p('手机存储  /  Download',rx,top+23,rw,11,color=MUTED,bold=True)
    line(rx,top+61,x+width-18,top+61)
    box(rx,top+84,42,47,PALE,radius=4)
    p('APK',rx,top+99,42,9.5,bold=True,align=1)
    p('D2R-Mobile.apk',rx+55,top+87,rw-55,11.5,bold=True)
    p('应用安装包 · 约 69 MB',rx+55,top+111,rw-55,9.8,color=MUTED)
    box(rx-5,top+165,rw+10,90,colors.HexColor('#fbefdf'),RED,radius=6)
    box(rx+5,top+185,42,47,RED,radius=4)
    p('PACK',rx+5,top+200,42,9,color=WHITE,bold=True,align=1)
    p('D2R-Offline-94012.d2rpack',rx+58,top+184,rw-68,11.4,bold=True,maxh=34)
    p('完整离线资源包 · 约 9.24 GB',rx+58,top+217,rw-68,9.8,color=RED)
    p('选择这一项',rx+4,top+273,rw-8,11,color=RED,bold=True)
    p('文件路径示意：系统文件选择器的布局可能不同。',M,519,CW,9.2,color=MUTED)
    p('要选 .d2rpack，别再次选 .apk。',M,552,CW,16,bold=True)
    p('“下载内容”里看不到文件？',M,600,CW,13,bold=True)
    p('点左侧菜单，进入“手机/平板存储”，再打开 Download 或实际下载文件夹。也可以先查看浏览器的下载记录，确认文件保存位置。',M,629,CW,11.8,leading=19,maxh=63)
    p('选中后，应用会自动开始导入。',M,711,CW,13,bold=True)
    p('不用解压，也不用手动放进 Android/data 或 OBB 目录。请保留资源包，直到导入完成。',M,741,CW,11.8,leading=19,maxh=48)
    end_page()


def importing():
    new_page('import','04  等待导入和校验','资源包较大，保持安装页面打开，等待应用完成全部检查。')
    top=180;height=image(SHOTS/'import-progress.png',M,top,CW,crop=(530,300,2030,1350))
    p('实机截图：资源包刚打开时显示 0%，随后开始导入。',M,top+height+12,CW,9.2,color=MUTED)
    col=(CW-24)/2
    p('保持这个页面打开',M,590,col,14,bold=True,color=RED)
    p('切到后台不会主动取消。如系统回收应用，重新打开后选择同一资源包重试，已校验文件会复用。',M,622,col,11.8,leading=19,maxh=93)
    p('末尾也要等一会儿',M+col+24,590,col,14,bold=True,color=RED)
    p('进度接近完成时，仍可能在处理图形缓存。等待显示“游戏已准备好”，再点击“启动游戏”。',M+col+24,622,col,11.8,leading=19,maxh=93)
    note('中断后可以重试',
         '已经校验完成的文件会复用；被中断的单个文件需要重新导入。导入完成后，才可以删除下载的资源包。',M,710,CW,84)
    end_page()


def finished():
    new_page('finish','05  点击启动游戏','导入完成后可确认驱动和 MOD 设置，再点击“启动游戏”。')
    top=182;height=image(SHOTS/'automatic-game-start.png',M,top,CW,crop=(0,80,2560,1520))
    p('旧版实机截图：原游戏主菜单，新版入口需手动点击启动。',M,top+height+12,CW,9.2,color=MUTED)
    rows=[('以后从入口启动游戏','打开桌面图标，选择驱动并点击“启动游戏”。无需重复导入资源。'),
          ('资源包可以删除','确认安装成功后，可以删除下载目录中的 .d2rpack，释放约 9.24 GB。'),
          ('更新请覆盖安装','保留应用本身再安装新版 APK；卸载应用会删除游戏数据和存档。')]
    for i,(title,body) in enumerate(rows):
        top=530+i*81;circle_number(i+1,M,top)
        p(title,M+36,top,CW-36,13.4,bold=True)
        p(body,M+36,top+30,CW-36,11.6,leading=18,maxh=41)
        if i<2:line(M,top+70,W-M,top+70)
    end_page()


def driver_selection():
    new_page('driver','06  选择图形驱动','启动失败时可从入口更换，默认保留“自动推荐”。')
    shot=ROOT/'analysis/d2r-mobile-port/TNA_AN00/startup-driver-fix/cold-1-entry.png'
    height=image(shot,M,180,200)
    p('旧版实机截图：新版先展开“启动设置”。',M,180+height+12,210,9,color=MUTED)
    x=M+228;width=CW-228
    rows=[('自动推荐（推荐）','按设备选择驱动；连续启动失败后会尝试系统驱动。'),
          ('系统驱动','使用手机自带的驱动。如启动失败，可在入口查看错误并手动选择其他驱动。'),
          ('内置兼容驱动（Turnip）','手动使用安装包内的驱动，适用于部分骁龙设备。其他设备建议使用自动推荐。')]
    for i,(title,body) in enumerate(rows):
        top=196+i*139
        p(title,x,top,width,13,bold=True,leading=20,maxh=44)
        p(body,x,top+49,width,11.5,leading=19,maxh=78)
    note('选择会自动记住',
         '展开“启动设置”选择驱动或已安装 MOD，再点击“启动游戏”。首次导入完成后也需点击启动；以后无需重复导入资源包。',M,694,CW,104)
    end_page()


def problems():
    new_page('problems','遇到问题，按这里处理','先看应用提示，再按对应的方法重试。')
    top=177;height=image(SHOTS/'invalid-package.png',M,top,CW,crop=(540,385,2020,785))
    p('实机错误提示示例：选到不配套的文件。按提示重新选择即可。',M,top+height+11,CW,9.2,color=MUTED)
    rows=[('找不到资源包','从“手机/平板存储”进入 Download，或按浏览器下载记录查找。'),
          ('手机空间不足','清理空间后重新选择。资源包已经在手机上时，导入还需额外约 9.5 GB。'),
          ('资源包版本不匹配','重新取得同一份发布资料中的 APK 与资源包，成套使用。'),
          ('文件不完整/校验失败','重新下载完整资源包。保留 .d2rpack 扩展名，无需解压。'),
          ('停止或系统回收后重试','切到后台不会主动取消；如已停止，再选原资源包复用已校验文件。'),
          ('导入后游戏启动失败','重新打开入口，切回自动推荐或尝试其他驱动。仍失败时保留机型和错误截图。')]
    for i,(title,body) in enumerate(rows):
        top=355+i*60
        if i%2==0:box(M,top,CW,60,WHITE)
        p(title,M+10,top+12,132,10.7,bold=True,maxh=39)
        p(body,M+157,top+10,CW-169,10.7,leading=16.4,maxh=45)
        line(M,top+60,W-M,top+60)
    p('验证与截图说明',M,736,CW,10,bold=True,color=MUTED)
    p('截图来自旧版 ROD-W09 实机测试。新版启动器完成代码和离线验证，页面交互仍待真机确认；系统安装弹窗和文件路径图为示意图。',M,758,CW,9.3,leading=14.3,color=MUTED,maxh=44)
    end_page()


if __name__=='__main__':main()
