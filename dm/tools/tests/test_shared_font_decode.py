import struct
import unittest
from shared_font_decode import decode, valid_sfnt


def fixture():
    payload = b"test"
    return b"\x00\x01\x00\x00" + struct.pack(">HHHH", 1, 16, 0, 0) + struct.pack(">4sIII", b"cmap", int.from_bytes(payload, "big"), 28, 4) + payload


class SharedFontTests(unittest.TestCase):
    def test_wrapper_and_table_checksum(self):
        font = fixture()
        mask = b"\x13\x57\x9b\xdf"
        wrapped = bytes(8) + bytes(value ^ mask[i % 4] for i, value in enumerate(font))
        self.assertEqual(decode(wrapped), font)
        damaged = bytearray(wrapped)
        damaged[-1] ^= 1
        with self.assertRaises(ValueError):
            decode(damaged)

    def test_table_bounds_and_alignment(self):
        for offset, size in [(0, 4), (29, 3), (28, 5), (0xffffffff, 4)]:
            data = bytearray(fixture())
            struct.pack_into(">II", data, 20, offset, size)
            self.assertFalse(valid_sfnt(data))

    def test_truncation_and_ambiguous_input(self):
        for data in [b"", bytes(20), fixture()[:-1]]:
            self.assertFalse(valid_sfnt(data))
            with self.assertRaises(ValueError):
                decode(data)


if __name__ == "__main__":
    unittest.main()
