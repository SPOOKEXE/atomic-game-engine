import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pixel-composer"))

from array_depth import SourceIndex, apply_runtime_depth_mutations, declaration_depth


class ArrayDepthTest(unittest.TestCase):
    def test_source_type_array_and_explicit_depth_are_added(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "scripts" / "types.gml"
            source.parent.mkdir()
            source.write_text(
                """
function nodeValue_Slider(_name, _value) { return new __NodeValue_Slider(_name, self, _value); }
function __NodeValue_Slider(_name, _node, _value) : NodeValue(_name, _node, CONNECT_TYPE.input, VALUE_TYPE.float, _value, "") constructor {
    setDisplay(VALUE_DISPLAY.slider);
}
function nodeValue_Vec3(_name, _value) { return new __NodeValue_Vec3(_name, self, _value); }
function __NodeValue_Array(_name, _node, _value) : NodeValue(_name, _node, CONNECT_TYPE.input, VALUE_TYPE.float, _value, "") constructor {
    type_array = 1;
}
function __NodeValue_Vec3(_name, _node, _value) : __NodeValue_Array(_name, _node, _value) constructor {
    setDisplay(VALUE_DISPLAY.vector);
}
""",
                encoding="utf-8",
            )
            index = SourceIndex(root, {})
            self.assertEqual(0, index.type_array("Slider"))
            self.assertEqual(0, declaration_depth(index.type_array("Slider"), "nodeValue_Slider()"))
            self.assertEqual(1, declaration_depth(index.type_array("Slider"), ").setArrayDepth(1);"))
            self.assertEqual(2, declaration_depth(index.type_array("Vec3"), ").setArrayDepth(1);"))

    def test_explicit_display_recomputes_type_array(self):
        self.assertEqual(1, declaration_depth(0, ").setDisplay(VALUE_DISPLAY.vector);"))
        self.assertEqual(0, declaration_depth(1, ").setDisplay(VALUE_DISPLAY.slider);"))

    def test_factory_forwarding_cycle_stays_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "scripts" / "cycle.gml"
            source.parent.mkdir()
            source.write_text(
                "function nodeValue_A(_name) { return nodeValue_B(_name); }\n"
                "function nodeValue_B(_name) { return nodeValue_A(_name); }\n",
                encoding="utf-8",
            )
            self.assertIsNone(SourceIndex(root, {}).type_array("A"))

    def test_dynamic_and_malformed_depths_remain_unknown(self):
        self.assertIsNone(declaration_depth(0, ").setArrayDepth(!_arr);"))
        self.assertIsNone(declaration_depth(0, ").setArrayDepth(1"))
        self.assertIsNone(declaration_depth(0, ").setArrayDepth(1).setArrayDepth(2);"))
        self.assertIsNone(declaration_depth(0, ").setDisplay(_display);"))

    def test_conditional_runtime_mutation_marks_only_its_input_unknown(self):
        inputs = [
            {"index": "0", "array_depth": 0},
            {"index": "1", "array_depth": 1},
        ]
        apply_runtime_depth_mutations(inputs, "if (_mode) inputs[0].setArrayDepth(1);")
        self.assertIsNone(inputs[0]["array_depth"])
        self.assertEqual(1, inputs[1]["array_depth"])

    def test_dynamic_index_mutation_does_not_claim_a_static_depth(self):
        inputs = [
            {"index": "0", "array_depth": 0},
            {"index": "1", "array_depth": 1},
        ]
        apply_runtime_depth_mutations(inputs, "inputs[i].array_depth = _depth;")
        self.assertTrue(all(item["array_depth"] is None for item in inputs))

    def test_repeated_runtime_display_changes_are_unknown(self):
        inputs = [{"index": "0", "array_depth": 0}]
        apply_runtime_depth_mutations(
            inputs,
            "if (_mode) inputs[0].setDisplay(VALUE_DISPLAY.vector); "
            "else inputs[0].setDisplay(VALUE_DISPLAY.slider);",
        )
        self.assertIsNone(inputs[0]["array_depth"])

    def test_single_conditional_runtime_display_change_is_unknown(self):
        inputs = [{"index": "0", "array_depth": 0}]
        apply_runtime_depth_mutations(
            inputs,
            "if (_mode) inputs[0].setDisplay(VALUE_DISPLAY.vector);",
        )
        self.assertIsNone(inputs[0]["array_depth"])

    def test_runtime_slider_updates_preserve_scalar_depth_for_rgb_hsv_inputs(self):
        inputs = [
            {"index": "0", "kind": "Slider", "array_depth": 0},
            {"index": "1", "kind": "Slider", "array_depth": 0},
            {"index": "2", "kind": "Float", "array_depth": 0},
            {"index": "3", "kind": "Bool", "array_depth": 0},
        ]
        resolved = apply_runtime_depth_mutations(
            inputs,
            "inputs[0].setType(VALUE_TYPE.float); inputs[0].setDisplay(VALUE_DISPLAY.slider); "
            "inputs[1].setType(VALUE_TYPE.integer); "
            "inputs[1].setDisplay(VALUE_DISPLAY.slider, { range: [0, 255, 0.1] }); "
            "inputs[2].setDisplay(VALUE_DISPLAY._default);",
        )
        self.assertEqual([0, 0, 0, 0], [item["array_depth"] for item in inputs])
        self.assertEqual({"0", "1", "2"}, resolved)

    def test_unknown_runtime_display_expression_remains_unknown(self):
        inputs = [{"index": "0", "kind": "Slider", "array_depth": 0}]
        apply_runtime_depth_mutations(inputs, "inputs[0].setDisplay(_display);")
        self.assertIsNone(inputs[0]["array_depth"])


if __name__ == "__main__":
    unittest.main()
