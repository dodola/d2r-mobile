"""Build Switch 94012 shop/runeword data for the existing Android MOD loader.

Never executes PC D2RMM code or compiles PC BINs. Every field used below is
checked against the matching Switch TXT before modifying the native BIN.
"""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'dm/ios/tools'))
from wide_stash import _entry

VENDORS = {'Akara': (343, 360), 'Drognan': (348, 365), 'Ormus': (351, 368),
           'Jamella': (356, 373), 'Malah': (357, 374)}


def tsv(data):
    return list(csv.DictReader(io.StringIO(data.decode('utf-8-sig')), delimiter='\t'))


def records(data, size):
    if len(data) < 4 or len(data) != 4 + struct.unpack_from('<I', data)[0]*size:
        raise ValueError('unexpected Switch table record size')
    return range(struct.unpack_from('<I', data)[0])


def item_types(data):
    rows = [r for r in tsv(data) if r['ItemType'] != 'Expansion']
    return {r['Code']: i for i, r in enumerate(rows) if r['Code']}


def patch_shop(data, source, types, runes_up_to, extras):
    if not 0 <= runes_up_to <= 33:
        raise ValueError('runes_up_to must be 0..33')
    rows = {r['code']: r for r in tsv(source) if r['code']}
    out = bytearray(data)
    changed = []
    for i in records(data, 448):
        base = 4+i*448
        code = data[base+128:base+132].decode('ascii').strip(' \0')
        r = rows[code]
        for name, (lo, hi) in VENDORS.items():
            for offset, field in ((lo, name+'Min'), (hi, name+'Max')):
                if data[base+offset] != int(r[field] or 0):
                    raise ValueError('vendor schema mismatch: '+code+'/'+field)
        for offset, field in ((436, 'PermStoreItem'), (323, 'spawnable'), (437, 'multibuy')):
            if data[base+offset] != int(r[field] or 0):
                raise ValueError('item schema mismatch: '+code+'/'+field)
        if struct.unpack_from('<HH', data, base+302) != (types.get(r['type'], 0), types.get(r['type2'], 0)):
            raise ValueError('item type schema mismatch: '+code)
        chosen = (r['type'] == 'rune' and int(code[1:]) <= runes_up_to) or (
            extras and (r['type2'] == 'gem4' or code in ('rvl', 'toa', 'jew', 'pk1', 'pk2', 'pk3')))
        if not chosen:
            continue
        for lo, hi in VENDORS.values():
            out[base+lo] = out[base+hi] = 1
        out[base+436] = out[base+323] = 1
        if code == 'rvl':
            out[base+437] = 1
        if code in ('pk1', 'pk2', 'pk3'):
            struct.pack_into('<H', out, base+302, types['key'])
        changed.append(code)
    return bytes(out), changed


def patch_runewords(data, source, types, weapons=False, shields=False, ladder=False):
    rows = {r['Name']: r for r in tsv(source) if r['Name'] and r['Name'] != 'Expansion'}
    out = bytearray(data)
    changed = []
    weapon_types = {'swor','axe','pole','mace','hamm','h2h','club','scep','miss','staf','mele','wand','spea','knif'}
    for i in records(data, 232):
        base = 4+i*232
        key = data[base:base+64].split(b'\0',1)[0].decode('ascii')
        r = rows[key]
        for j in range(6):
            value = struct.unpack_from('<h', data, base+0x4a+2*j)[0]
            code = r['itype%d' % (j+1)]
            if value != types.get(code, 0):
                raise ValueError('runeword schema mismatch: '+key+'/'+code)
            replacement = 'weap' if weapons and code in weapon_types else 'shld' if shields and code == 'pala' else None
            if replacement:
                struct.pack_into('<h', out, base+0x4a+2*j, types[replacement])
        if ladder:
            out[base+0xe4] &= ~4
        if out[base:base+232] != data[base:base+232]:
            changed.append(key)
    return bytes(out), changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--runes-up-to', type=int, default=0)
    parser.add_argument('--shop-extras', action='store_true')
    parser.add_argument('--weapon-runewords', action='store_true')
    parser.add_argument('--shield-runewords', action='store_true')
    parser.add_argument('--ladder-runewords', action='store_true')
    args = parser.parse_args()
    if args.out.exists():
        parser.error('output must not already exist')
    if not 0 <= args.runes_up_to <= 33:
        parser.error('--runes-up-to must be 0..33')
    root = str(args.romfs)
    types = item_types(_entry(root, '/global/excel/itemtypes.txt')[1])
    changes = {}
    # Resolve both mode-specific copies. Missing optional base tables are
    # tolerated only when they are absent from the source manifest.
    manifest = (args.romfs/'data/build_manifest.psv').read_text(encoding='utf-8').lower()
    for sub in ('', 'base/'):
        for name in ('misc', 'runes'):
            suffix = '/global/excel/'+sub+name+'.bin'
            if suffix not in manifest:
                if sub:
                    continue
                raise ValueError('missing native table: '+suffix)
            path, data = _entry(root, suffix)
            txt_suffix='/global/excel/'+sub+name+'.txt'
            if txt_suffix not in manifest:
                txt_suffix='/global/excel/'+name+'.txt'
            source = _entry(root, txt_suffix)[1]
            if name == 'misc':
                patched, changed = patch_shop(data, source, types, args.runes_up_to, args.shop_extras)
            else:
                patched, changed = patch_runewords(data, source, types, args.weapon_runewords, args.shield_runewords, args.ladder_runewords)
            if changed:
                changes[path] = (patched, {'entries': changed, 'source_sha256': hashlib.sha256(data).hexdigest(),
                                          'sha256': hashlib.sha256(patched).hexdigest()})
    if not changes:
        parser.error('no table changes requested')
    args.out.mkdir(parents=True)
    for path, (data, _) in changes.items():
        target = args.out/path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (args.out/'gameplay-mod.json').write_text(json.dumps({
        'build': '94012', 'settings': {k:v for k,v in vars(args).items() if k not in ('romfs','out')},
        'tables': {p:info for p, (_,info) in changes.items()}}, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'tables': len(changes), 'out': str(args.out)}))


if __name__ == '__main__':
    main()
