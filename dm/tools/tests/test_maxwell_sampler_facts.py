import struct
import unittest
from maxwell_sampler_facts import infer,infer_images


def instruction(prefix,tid=8,dimension=2):
    return (int(prefix,2)<<(64-len(prefix))) | (tid<<36) | (dimension<<28)


class SamplerFactsTests(unittest.TestCase):
    def code(self,*words):
        return bytes(0x50)+struct.pack("<4Q",0,*words,*([0]*(3-len(words))))

    def test_typed_texture_and_real_slot(self):
        code=self.code(instruction("11000",12,6))
        facts,evidence,conflicts=infer(code,"ps",len(code))
        self.assertEqual(facts,{"2":"cube"})
        self.assertEqual(evidence[0]["offset"],0x58)
        self.assertEqual(conflicts,{})

    def test_conflicting_types_excluded(self):
        code=self.code(instruction("11000",8,2),instruction("11000",8,6))
        facts,_,conflicts=infer(code,"ps",len(code))
        self.assertEqual(facts,{})
        self.assertEqual(conflicts,{"0":["2d","cube"]})

    def test_integer_1d_fetch_and_bindless_remain_unknown(self):
        code=self.code(instruction("11011100",8,0),instruction("1101111001",8,2))
        self.assertEqual(infer(code,"ps",len(code))[0],{})

    def test_scheduling_and_constants_are_not_instructions(self):
        word=instruction("11000",8,6)
        code=bytes(0x50)+struct.pack("<4Q",word,0,0,0)+struct.pack("<4Q",0,word,0,0)
        self.assertEqual(infer(code,"ps",0x70)[0],{})

    def test_invalid_program_range_rejected(self):
        with self.assertRaises(ValueError):infer(bytes(0x50),"ps",0x100)

    def test_image_dimensions_use_the_image_handle_bank(self):
        word=(int("1110101100101",2)<<51)|(72<<36)|(3<<33)
        code=self.code(word)
        self.assertEqual(infer(code,"ps",len(code))[0],{})
        facts,evidence=infer_images(code,"ps",len(code))
        self.assertEqual(facts,{"0":{"type":"2d"}})
        self.assertEqual(evidence[0]["opcode"],"Sust")


if __name__=="__main__":unittest.main()
