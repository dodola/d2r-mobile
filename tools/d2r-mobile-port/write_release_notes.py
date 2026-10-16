"""Write novice-facing local release instructions and checksums, after a verified build."""
import hashlib
import json
import os
from pathlib import Path

ROOT=Path(os.environ.get('ANDROID_SWITCH_WORKSPACE',str(Path(__file__).resolve().parents[2]))).resolve()
OUT=ROOT/'analysis/d2r-mobile-port/offline-release'


def main():
    pack=json.loads((OUT/'offline-pack-build.json').read_text(encoding='utf-8'))
    apk=json.loads((OUT/'D2R-Mobile.build.json').read_text(encoding='utf-8'))
    apk_path=OUT/'D2R-Mobile.apk';pack_path=OUT/'D2R-Offline-91738.d2rpack'
    if not pack['fully_verified'] or pack_path.stat().st_size!=pack['bytes']:
        raise ValueError('Pack is not the verified release artifact')
    if hashlib.sha256(apk_path.read_bytes()).hexdigest()!=apk['sha256']:
        raise ValueError('APK differs from its build report')
    text='''D2R Mobile 安卓离线安装版

先准备这两个文件：
  D2R-Mobile.apk                  约 69 MB，应用安装包
  D2R-Offline-91738.d2rpack        约 9.18 GB，完整离线资源包

首次安装，按以下步骤操作：
1. 手机建议先准备约 20 GB 可用空间，用于下载和导入后的游戏资源。
   如果资源包已经下载到手机，导入时还需额外约 9.5 GB 可用空间。
2. 打开 D2R-Mobile.apk 安装。系统如询问是否允许当前浏览器或文件管理器
   安装应用，请按系统提示完成。无需 Root，无需电脑。
3. 打开桌面上的 D2R Mobile（熔火头盔图标）。
4. 点击“选择离线资源包”，找到 D2R-Offline-91738.d2rpack 并选择。
   无需解压，也不要把它改成 .zip 或 .obb。
   若“下载内容”中看不到文件，可从文件选择器左侧进入“手机/平板存储”，
   再打开 Download（下载）或浏览器实际下载目录。
5. 保持安装页面打开，等待导入和校验完成。游戏会自动启动。
   大文件和图形缓存较多，最后阶段也需要一些时间，请以页面进度为准。

安装完成后：
  后续点击桌面图标即可启动，不用再次选择资源包。
  可以删除下载目录中的 .d2rpack，游戏已保存到应用内部。
  更新请覆盖安装；不要为更新而卸载，卸载会删除游戏数据和存档。

遇到问题：
  空间不足：清理手机空间，再重新选择资源包。
  文件不完整/校验失败：重新下载完整资源包，使用同一发布目录的 APK。
  版本不匹配：APK 与资源包必须成套使用。
  取消或切到后台：重新打开应用并选择原资源包重试。
    已校验完成的文件会复用；被中断的单个文件需要重新导入。
  游戏启动阶段出错：保留错误信息。这与资源包导入失败是不同的问题。

设备范围：Android 10 及以上，64 位 ARM，支持所需 Vulkan 功能。
  本次在 ROD-W09 平板验证了首次选择、完整导入、全部文件 SHA-256 校验
  和自动进入原游戏主菜单。其他机型仍可能存在图形或驱动兼容差异；
  这份安装版不代表所有设备、全部关卡和长时间运行均已验证。

offline-manifest.json、*.build.json、offline-pack-build.json 是配套校验记录，
普通安装只需 APK 和 .d2rpack 两个文件。SHA256SUMS.txt 供需要时核对下载。
'''
    (OUT/'安装说明.txt').write_text(text,encoding='utf-8-sig')
    (OUT/'SHA256SUMS.txt').write_text(f'{apk["sha256"]}  {apk_path.name}\n{pack["sha256"]}  {pack_path.name}\n',encoding='ascii')
    print('Wrote installation instructions and SHA-256 checksums to '+str(OUT))


if __name__=='__main__':main()
