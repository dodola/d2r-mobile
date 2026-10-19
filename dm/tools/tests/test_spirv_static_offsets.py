from pathlib import Path
import struct
import tempfile
import unittest
from spirv_static_offsets import fold_sample_offsets


def inst(op,*operands):return [(len(operands)+1)<<16|op,*operands]


class StaticOffsetsTests(unittest.TestCase):
    def write(self,directory,instructions):
        words=[0x07230203,0x10000,0,100,0]+[v for row in instructions for v in row]
        path=Path(directory)/"fixture.spv";path.write_bytes(struct.pack(f"<{len(words)}I",*words));return path

    def test_single_assignment_literal_and_forward_constant(self):
        rows=[inst(21,1,32,1),inst(23,2,1,2),inst(59,3,10,7),inst(43,1,11,7),
              inst(62,10,11),inst(61,1,12,10),inst(44,2,13,12,12),inst(43,1,14,7)]
        with tempfile.TemporaryDirectory() as directory:
            path=self.write(directory,rows);self.assertEqual(fold_sample_offsets(path,b"",[]),1)
            words=struct.unpack(f"<{path.stat().st_size//4}I",path.read_bytes())
            # The prior literal is reusable; a later declaration must not be referenced.
            self.assertIn(tuple(inst(44,2,13,11,11)),[tuple(words[i:i+5]) for i in range(len(words)-4)])

    def test_dynamic_load_is_not_guessed(self):
        rows=[inst(21,1,32,1),inst(23,2,1,2),inst(59,3,10,2),inst(61,1,12,10),inst(44,2,13,12,12)]
        with tempfile.TemporaryDirectory() as directory:
            path=self.write(directory,rows);before=path.read_bytes()
            with self.assertRaises(ValueError):fold_sample_offsets(path,b"",[])
            self.assertEqual(path.read_bytes(),before)

    def test_immutable_c1_word_and_short_buffer(self):
        rows=[inst(21,1,32,0),inst(23,2,1,2),inst(23,3,1,4),inst(43,1,4,4),inst(28,5,3,4),
              inst(30,6,5),inst(32,7,2,6),inst(32,8,2,1),inst(59,7,9,2),inst(71,5,6,16),
              inst(72,6,0,35,0),inst(71,9,33,2),inst(43,1,10,0),inst(43,1,11,1),
              inst(65,8,12,9,10,10,11),inst(61,1,13,12),inst(44,2,14,13,13)]
        with tempfile.TemporaryDirectory() as directory:
            path=self.write(directory,rows)
            with self.assertRaises(ValueError):fold_sample_offsets(path,bytes(4),[{"binding":2,"slot":1}])
            self.assertEqual(fold_sample_offsets(path,struct.pack("<4I",0,0xf0,0,0),[{"binding":2,"slot":1}]),1)


if __name__=="__main__":unittest.main()
