import hashlib
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from bnsh_inventory import records,program_key


def fixture():
    data=bytearray(0x3000)
    data[:8]=b"BNSH\0\0\0\0";data[12:14]=b"\xff\xfe"
    struct.pack_into("<I",data,28,len(data));data[0x60:0x64]=b"grsc"
    struct.pack_into("<IQ",data,0x7c,2,0xc0)
    control=bytearray(0x880);control[:4]=bytes.fromhex("34127698")
    struct.pack_into("<II",control,4,1,16)
    struct.pack_into("<4I",control,0x6fc,0,0x100,0x100,0x30)
    for i in range(2):
        program,shader,c,code=0x200+i*0x100,0x400+i*0x80,0x800+i*0x900,0x2000+i*0x200
        struct.pack_into("<Q",data,0xc0+i*64+16,program)
        struct.pack_into("<Q",data,program+8,shader)
        struct.pack_into("<QQII",data,shader+8,c,code,0x100,0x880)
        data[c:c+0x880]=control
        data[code:code+4]=bytes.fromhex("78563412")
        struct.pack_into("<I",data,code+0x30,1<<10)
        struct.pack_into("<Q",data,code+0x88,0x1111+i)
    return data


class BnshTests(unittest.TestCase):
    def test_identical_control_distinct_program(self):
        a,b=list(records(bytes(fixture())))
        self.assertEqual(a["control"],b["control"])
        self.assertNotEqual(a["code"],b["code"])
        self.assertNotEqual(program_key(a["control"],a["code"]),program_key(b["control"],b["code"]))
    def test_truncated_archive(self):
        with self.assertRaises(ValueError):list(records(bytes(fixture()[:-1])))
    def test_out_of_bounds_pointer(self):
        data=fixture();struct.pack_into("<Q",data,0x408,(1<<64)-1)
        with self.assertRaises(ValueError):list(records(bytes(data)))
    def test_stage_mismatch(self):
        data=fixture();struct.pack_into("<I",data,0x2030,5<<10)
        with self.assertRaises(ValueError):list(records(bytes(data)))
    def test_unknown_control_version(self):
        data=fixture();struct.pack_into("<I",data,0x808,99)
        with self.assertRaises(ValueError):list(records(bytes(data)))
    def test_extended_control_keeps_runtime_identity_and_provenance(self):
        original=fixture();extended=fixture()
        struct.pack_into("<I",extended,0x41c,0x890)
        extended[0x1080:0x1090]=b"compiler-info!!!"
        a=list(records(bytes(original)))[0];b=list(records(bytes(extended)))[0]
        self.assertEqual(program_key(a["control"],a["code"]),program_key(b["control"],b["code"]))
        self.assertEqual(b["source_control_size"],0x890)
        self.assertEqual(b["source_control_sha256"],hashlib.sha256(extended[0x800:0x1090]).hexdigest())
    def test_extended_control_still_checks_entire_source_range(self):
        data=fixture();struct.pack_into("<I",data,0x41c,0xffffffff)
        with self.assertRaises(ValueError):list(records(bytes(data)))


if __name__=="__main__":unittest.main()
