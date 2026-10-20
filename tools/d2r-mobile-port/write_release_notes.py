"""Write novice-facing local release instructions and checksums for the game 94012 offline release, after a verified build."""
import argparse
import hashlib
import json
import os
from pathlib import Path

ROOT=Path(os.environ.get('ANDROID_SWITCH_WORKSPACE',str(Path(__file__).resolve().parents[2]))).resolve()
RELEASE=ROOT/'analysis/d2r-mobile-port/release-94012'
PACKAGE='org.d2r.mobile'
PACK_NAME='D2R-Offline-94012.d2rpack'


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1<<22),b''):h.update(block)
    return h.hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack-dir',type=Path,default=RELEASE/'pack')
    parser.add_argument('--apk-dir',type=Path,default=RELEASE/'apk')
    parser.add_argument('--out',type=Path,default=RELEASE)
    args=parser.parse_args()
    pack=json.loads((args.pack_dir/'offline-pack-build.json').read_text(encoding='utf-8'))
    report=json.loads((args.apk_dir/'build-report.json').read_text(encoding='utf-8'))
    apk_path=args.apk_dir/'D2R-Mobile.apk';pack_path=args.pack_dir/PACK_NAME
    if not pack['fully_verified'] or pack['game_build']!=94012 or pack_path.stat().st_size!=pack['bytes']:
        raise ValueError('Pack is not the verified 94012 release artifact')
    apk_sha=sha(apk_path)
    if not report.get('release') or report['debug_signed'] or report['package']!=PACKAGE or apk_sha!=report['apk_sha256']:
        raise ValueError('APK differs from its release build report')
    if report['offline_manifest']['manifest_sha256']!=pack['manifest_sha256']:
        raise ValueError('APK and resource pack were built from different offline manifests')
    apk_mb=round(apk_path.stat().st_size/1048576);pack_gb=pack['bytes']/1e9
    text=f'''D2R Mobile 安卓离线安装版（游戏版本 94012，含「术士君临」）

先准备这两个文件：
  D2R-Mobile.apk                  约 {apk_mb} MB，应用安装包（包名 {PACKAGE}）
  {PACK_NAME}        约 {pack_gb:.2f} GB，完整离线资源包

首次安装，按以下步骤操作：
1. 手机建议先准备约 20 GB 可用空间，用于下载和导入后的游戏资源。
   如果资源包已经下载到手机，导入时还需额外约 9.5 GB 可用空间。
2. 打开 D2R-Mobile.apk 安装。系统如询问是否允许当前浏览器或文件管理器
   安装应用，请按系统提示完成。无需 Root，无需电脑。
3. 打开桌面上的 D2R Mobile。
4. 点击“选择离线资源包”，找到 {PACK_NAME} 并选择。
   无需解压，也不要把它改成 .zip 或 .obb。
   若“下载内容”中看不到文件，可从文件选择器左侧进入“手机/平板存储”，
   再打开 Download（下载）或浏览器实际下载目录。
5. 建议保持安装页面打开，等待导入和校验完成，再点击“启动游戏”。
   大文件和图形缓存较多，最后阶段也需要一些时间，请以页面进度为准。

安装完成后：
  后续打开桌面图标，在启动入口点击“启动游戏”，不用再次选择资源包。
  展开“启动设置”，可选择图形驱动和已安装 MOD，会记住选择。
  图形驱动提供“自动推荐（推荐）”“系统驱动”“内置兼容驱动（Turnip）”。
  自动模式在骁龙（高通）手机上使用内置 Turnip 驱动，其他手机使用系统驱动；
  若连续两次启动都没有出画面，会自动改用系统驱动。也可以在入口手动切换后再启动。
  可以删除下载目录中的 .d2rpack，游戏已保存到应用内部。
  更新请覆盖安装；不要为更新而卸载，卸载会删除游戏数据和存档。

与旧测试包的关系：
  本安装版使用独立的应用名 {PACKAGE}，可与旧的测试包并存，互不影响。
  存档保存在各自应用内部，不会自动从旧测试包迁移。

遇到问题：
  空间不足：清理手机空间，再重新选择资源包。
  文件不完整/校验失败：重新下载完整资源包，使用同一发布目录的 APK。
  版本不匹配：APK 与资源包必须成套使用。
  停止导入需要确认，完成安装期间不能取消。
  切到后台不会主动停止导入；如应用被系统回收，重新选择原资源包重试。
  手动停止后也可选择原资源包重试。
    已校验完成的文件会复用；被中断的单个文件需要重新导入。
  游戏启动阶段出错：保留错误信息。这与资源包导入失败是不同的问题。

设备范围：Android 10 及以上，64 位 ARM，支持所需 Vulkan 功能。
  这份安装版不代表所有设备、全部关卡和长时间运行均已验证。

offline-manifest.json、build-report.json、offline-pack-build.json 是配套校验记录，
普通安装只需 APK 和 .d2rpack 两个文件。SHA256SUMS.txt 供需要时核对下载。
'''
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/'安装说明.txt').write_text(text,encoding='utf-8-sig')
    (args.out/'SHA256SUMS.txt').write_text(f'{apk_sha}  {apk_path.name}\n{pack["sha256"]}  {pack_path.name}\n',encoding='ascii')
    print('Wrote installation instructions and SHA-256 checksums to '+str(args.out))


if __name__=='__main__':main()
