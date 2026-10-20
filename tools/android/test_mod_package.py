"""Synthetic CASC fixtures; no copyrighted resources or game execution."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import Mock, patch

import mod_package as mod
import mod_console


def tvfs(rows):
    specs = b'z\0n\0b:64K*=n\0'
    cft, spans, names = bytearray(), bytearray(), {}
    width = mod.casc.offset_field_size(len(rows) * 25)
    for name, key, size, spec, decoded in rows:
        names[name.encode()] = len(spans)
        spans += b'\x01' + struct.pack('>II', 0, decoded) + len(cft).to_bytes(width, 'big')
        cft += key + struct.pack('>I', size) + bytes([spec]) + struct.pack('>I', decoded)
    paths, depth = mod.casc.trie_bytes(names)
    po = 46
    eo = po + len(paths)
    co = eo + len(specs)
    fo = co + len(cft)
    header = bytearray(b'TVFS\x01\x2e\x10\x10' + struct.pack('>I', 10))
    header += struct.pack('>IIIIII', po, len(paths), fo, len(spans), co, len(cft))
    header += struct.pack('>HII', depth, eo, len(specs))
    return header + paths + specs + cft + spans


def fixture(root):
    romfs, base = root / 'romfs', root / 'base'
    for target in (romfs, base):
        (target / 'data').mkdir(parents=True)
    (base / 'ctrl_touch').write_text('1')
    archive = bytearray()

    def append(content, spec):
        encoded = mod.casc.encode_by_espec(content, spec)
        key = mod.casc.ekey_for(encoded, 0, 1, len(archive))
        archive.extend(encoded)
        return key, len(encoded)

    a, al = append(b'touch-current', 'z')
    b, bl = append(b'original movie', 'b:64K*=n')
    c, cl = append(b'unchanged', 'n')
    index = tvfs([('data/global/ui/layouts/test.json', a, al, 0, 13),
                  ('data/hd/video.webm', b, bl, 4, 14),
                  ('data/local/string.json', c, cl, 2, 9)])
    key, size = append(index, 'b:256K*=z')
    container = tvfs([('vfs-1', key, size, 4, len(index))])
    root_key, root_size = append(container, 'b:256K*=z')
    config = (f'build-name = 94012\nbuild-target-platform = switch\n'
              f'vfs-1 = 0000 {key.hex()}\nvfs-1-size = {len(index)} {size}\n'
              f'vfs-root = 0000 {root_key.hex()}\nvfs-root-size = {len(container)} {root_size}\n'
              f'vfs-2 = 0000 {root_key.hex()}\nvfs-2-size = {len(container)} {root_size}\n')
    for target in (romfs, base):
        (target / 'data/00-00000001.data').write_bytes(archive)
        (target / 'data/.build.config').write_text(config)
    return romfs, base, index


class PackageTests(unittest.TestCase):
    def test_rewrite_roundtrip_preserves_touch_and_untouched_entries(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            romfs, base, before = fixture(root)
            payload = root / 'payload/hd'
            payload.mkdir(parents=True)
            (payload / 'video.webm').write_bytes(b'')
            files = mod.payload_files(payload.parent)
            out = root / 'sample'
            receipt = mod.build_package(romfs, base, files, out, 'sample')
            self.assertEqual(mod.verify_package(out), receipt)
            mod.verify_rewrite(before, out / 'overlay', files)
            self.assertEqual(len(receipt['files']), 2)
            self.assertFalse(any(p.is_symlink() for p in out.rglob('*')))
            archive = next((out / 'overlay/data').glob('*.data'))
            archive.write_bytes(archive.read_bytes()[:-1] + b'x')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                mod.verify_package(out)

    def test_overwrite_touch_path_uses_current_index(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            romfs, base, before = fixture(root)
            path = root / 'new.json'
            path.write_text('replacement')
            files = {'data/global/ui/layouts/test.json': path}
            with self.assertRaisesRegex(ValueError, 'Android controls'):
                mod.build_package(romfs, base, files, root / 'blocked', 'sample')
            mod.build_package(romfs, base, files, root / 'sample', 'sample', allow_ui=True)
            mod.verify_rewrite(before, root / 'sample/overlay', files)

    def test_table_txt_requires_matching_bin(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = root / 'table.txt'
            path.write_text('test')
            files = {'data/global/excel/items.txt': path}
            with self.assertRaisesRegex(ValueError, 'allow-tables'):
                mod.build_package(root, root, files, root / 'out', 'sample')
            with self.assertRaisesRegex(ValueError, 'matching compiled BIN'):
                mod.build_package(root, root, files, root / 'out', 'sample', allow_tables=True)

    def test_reject_unsafe_ids_and_paths(self):
        for name in ('../bad', '/hd/test.json', 'hd//file', 'hd/../file', 'hd/c:bad'):
            with self.assertRaises(ValueError):
                mod.logical_path(name)
        with self.assertRaises(ValueError):
            mod.build_package(Path('.'), Path('.'), {}, Path('unused'), 'a;bad')

    def test_copy_only_component_no_js_evaluation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, out = root / 'mod', root / 'out'
            (source / 'hd/video').mkdir(parents=True)
            (source / 'hd/video/intro.webm').write_bytes(b'')
            (source / 'mod.js').write_text("// comment\nD2RMM.copyFile('hd', /* destination */ 'hd', true);")
            mod.copy_component(source, out)
            self.assertEqual((out / 'hd/video/intro.webm').read_bytes(), b'')
            (source / 'mod.js').write_text("D2RMM.readTsv('global/excel/items.txt');")
            with self.assertRaisesRegex(ValueError, 'export it with D2RMM'):
                mod.copy_component(source, out)

    def test_raw_d2rmm_script_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'mod.js').write_text('anything')
            with self.assertRaisesRegex(ValueError, 'scripts need exported'):
                mod.payload_files(root)

    def test_encoding_spec_byte_offsets(self):
        table = b'z\0n\0b:64K*=n\0'
        self.assertEqual(mod.casc.encoding_spec(table, 4), 'b:64K*=n')
        self.assertEqual(mod.casc.encoding_spec(table, 2), 'n')
        for offset in (1, 3, 90):
            with self.assertRaises(SystemExit):
                mod.casc.encoding_spec(table, offset)

    def test_missing_path_does_not_emit_partial_package(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            romfs, base, _ = fixture(root)
            data = root / 'file'
            data.write_bytes(b'x')
            out = root / 'sample'
            with self.assertRaisesRegex(ValueError, 'absent in Switch index'):
                mod.build_package(romfs, base, {'data/hd/new.bin': data}, out, 'sample')
            self.assertFalse(out.exists())

    def test_activation_checks_app_stopped_and_submits_only_id(self):
        device = Mock()
        mod_console.activate(device, 'sample')
        device.stopped.assert_called_once()
        self.assertEqual(device.call.call_args.args[1], 'sample\n')
        with self.assertRaises(ValueError):
            mod_console.activate(device, 'x;bad')

    @patch.object(mod_console, 'verify_package')
    def test_install_rejects_running_app_before_writing(self, verify):
        verify.return_value = {'id': 'sample'}
        device = Mock()
        device.stopped.side_effect = RuntimeError('running')
        with self.assertRaises(RuntimeError):
            mod_console.install(device, Path('.'))
        device.call.assert_not_called()


if __name__ == '__main__':
    unittest.main()
