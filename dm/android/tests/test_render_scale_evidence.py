"""Reject mislabeled performance samples, including partial target restoration."""
import copy
import unittest
from capture_present_profile import verify_render_scale


class RenderScaleEvidenceTest(unittest.TestCase):
    def snapshot(self, scale, size):
        return {'renderer': dict(readable=True, version_verified=True, scale=scale,
                                 internal=list(size), duplicate_internal=list(size), output=[1280, 720])}

    def test_both_modes(self):
        for scale, size in ((1.0, (1280, 720)), (.75, (960, 540))):
            verify_render_scale(self.snapshot(scale, size), scale)

    def test_false_labels_and_partial_rebuild(self):
        good = self.snapshot(.75, (960, 540))
        for field, wrong in (('scale', 1.0), ('scale', float('nan')), ('readable', False),
                             ('version_verified', False), ('internal', [1280, 720]),
                             ('duplicate_internal', [1280, 720]), ('output', [960, 540])):
            bad = copy.deepcopy(good)
            bad['renderer'][field] = wrong
            with self.subTest(field=field, wrong=wrong), self.assertRaises(RuntimeError):
                verify_render_scale(bad, .75)
        with self.assertRaises(RuntimeError):
            verify_render_scale({}, .75)

    def test_restoration_must_be_complete(self):
        restored = self.snapshot(1.0, (1280, 720))
        restored['renderer']['duplicate_internal'] = [960, 540]
        with self.assertRaises(RuntimeError):
            verify_render_scale(restored, 1.0)


if __name__ == '__main__':
    unittest.main()
