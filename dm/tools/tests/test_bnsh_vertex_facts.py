from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from bnsh_vertex_facts import observed

class VertexFactsTests(unittest.TestCase):
    def test_numeric_layout_from_actual_bound_format(self):
        text="VERTEX_TYPE_MISMATCH shader=4a99d8876e055b56 location=7 shader_kind=0 format=3 required_kind=2"
        self.assertEqual(observed([text,text]),{"4a99d8876e055b56":{"7":"uint"}})
    def test_conflicting_layout_does_not_overwrite(self):
        text="VERTEX_TYPE_MISMATCH shader=4a99d8876e055b56 location=7 shader_kind=0 format=3 required_kind="
        with self.assertRaisesRegex(ValueError,"separate cache variants"):observed([text+"1",text+"2"])
    def test_bounds_and_empty_input(self):
        with self.assertRaises(ValueError):observed([""])
        with self.assertRaises(ValueError):observed(["VERTEX_TYPE_MISMATCH shader=4a99d8876e055b56 location=16 shader_kind=0 format=3 required_kind=2"])

if __name__=="__main__":unittest.main()
