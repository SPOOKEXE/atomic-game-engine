import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pixel-composer"))
from surface_depth import depth_attribute


class SurfaceDepthTest(unittest.TestCase):
    def setUp(self):
        self.index = SimpleNamespace(
            factories={"nodeValue_Surface": ("Surface", None), "nodeValue_Dimension": ("Dimension", None)},
            classes={"Surface": ("NodeValue", "VALUE_TYPE.surface", ""),
                     "Dimension": ("NodeValue", "VALUE_TYPE.vector", "")})
        self.bases = {"Node_Test": "Node_Processor"}

    def attribute(self, body, declarations=(), inherited=(), bases=None):
        return depth_attribute("Node_Test", {"Node_Test": body}, bases or self.bases,
                               inherited, declarations, self.index, 1)

    def test_first_slot_type_and_constructor_order_choose_the_default(self):
        body = "function Node_Test() constructor { newInput(); attribute_surface_depth(); }"
        before = body.index("newInput")
        for kind, expected in (("Surface", "0"), ("Dimension", "1")):
            item = self.attribute(body, [(before, {"index": "0", "kind": kind})])
            self.assertEqual(expected, item["default"])
            self.assertEqual("-1", item["index"])
            self.assertEqual("color_depth", item["attribute"])
            self.assertEqual(9, len(item["choices"]))
        body = "function Node_Test() constructor { attribute_surface_depth(); newInput(); }"
        item = self.attribute(body, [(body.index("newInput"), {"index": "0", "kind": "Surface"})])
        self.assertEqual("1", item["default"])

    def test_inherited_first_surface_is_real_but_unknown_type_or_index_is_not_guessed(self):
        body = "function Node_Test() constructor { attribute_surface_depth(); }"
        self.assertEqual("0", self.attribute(body, inherited=[{"index": "0", "kind": "Surface"}])["default"])
        for item in ({"index": "unresolved", "kind": "Surface"}, {"index": "0", "kind": "Unknown"}):
            self.assertEqual("", self.attribute(body, inherited=[item])["default"])

    def test_time_remap_explicit_constructor_attribute_is_verified_without_processor_ancestry(self):
        name = "Node_Time_Remap"
        body = "function Node_Time_Remap() : Node() constructor { newInput(); attribute_surface_depth(); }"
        value = depth_attribute(name, {name: body}, {name: "Node"}, [],
                                [(body.index("newInput"), {"index": "0", "kind": "Surface"})], self.index, 1)
        self.assertEqual("0", value["default"])
        self.assertEqual("color_depth", value["attribute"])
        self.assertTrue(value["source_depth_input_enabled"])
        self.assertIsNone(self.attribute(body, bases={"Node_Test": "Node"}))
        nested = "function Node_Time_Remap() : Node() constructor { static late = function() { attribute_surface_depth(); } }"
        self.assertIsNone(depth_attribute(name, {name: nested}, {name: "Node"}, [], [], self.index, 1))

    def test_unverified_ancestry_nested_calls_and_comments_do_not_declare_depth(self):
        body = "function Node_Test() constructor { attribute_surface_depth(); }"
        self.assertIsNone(self.attribute(body, bases={"Node_Test": "Node_Value"}))
        self.assertIsNone(self.attribute("function Node_Test() { static late = function() { attribute_surface_depth(); } }"))
        self.assertIsNone(self.attribute('function Node_Test() { // attribute_surface_depth();\n var name="attribute_surface_depth()"; }'))


if __name__ == "__main__":
    unittest.main()
