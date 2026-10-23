"""从统一语言表生成无 native 依赖的启动器译文。 / Generate native-independent launcher translations from the shared table."""
import argparse
import csv
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
rows = list(csv.DictReader((root/'dm/i18n/ui_text.tsv').open(encoding='utf-8'), delimiter='\t'))
lines = ['/* 从 ui_text.tsv 生成，请使用 generate_launcher_strings.py 更新。',
         ' * Generated from ui_text.tsv; update with generate_launcher_strings.py. */',
         'package org.d2r.binaryprobe;', 'import java.util.Locale;',
         'final class LauncherStrings {', '    private final Locale locale;', '    private final int language;',
         '    LauncherStrings(Locale locale){this.locale=locale;String l=locale.getLanguage();',
         '        language=l.equals("zh")?0:l.equals("es")?(locale.getCountry().equals("MX")?3:2):l.equals("ru")?4:1;}',
         '    String text(String key){String[] value;', '        switch(key){']
for row in rows:
    if not row['键'].startswith('dmLaunch'):
        continue
    values = [row[c].replace('\\n', '\n') for c in ('中文', 'English', 'Español', 'Español MX', 'Русский')]
    values[3] = values[3] or values[2]
    lines.append('            case '+json.dumps(row['键'])+':value=new String[]{'+','.join(json.dumps(v, ensure_ascii=False) for v in values)+'};break;')
lines += ['            default:return key;', '        }return value[language];}',
          '    String format(String key,Object... args){return String.format(locale,text(key),args);}', '}']
text = '\n'.join(lines)+'\n'
target = root/'dm/android/src/org/d2r/binaryprobe/LauncherStrings.java'
if args.check:
    assert target.read_text(encoding='utf-8') == text, 'Regenerate launcher strings'
else:
    target.write_text(text, encoding='utf-8', newline='\n')
print('Launcher strings verified' if args.check else 'Launcher strings generated')
