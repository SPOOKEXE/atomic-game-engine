import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pixel-composer"))

from array_depth import ARRAY_DISPLAYS
from source_array_classification import SourceArrayClassification, apply_input_classification_mutations, _result


class SourceArrayClassificationTest(unittest.TestCase):
    def make_index(self, root):
        source = root / "scripts" / "types.gml"
        source.parent.mkdir()
        source.write_text('''
function nodeValue_Vector(name, value) { return new __NodeValue_Array(name, self, value); }
function nodeValue_IArray(name, value) { return new __NodeValue_IArray(name, self, value); }
function __NodeValue_Array(name, node, value) : NodeValue(name, node, CONNECT_TYPE.input, VALUE_TYPE.float, value) constructor { type_array = 1; }
function __NodeValue_IArray(name, node, value) : __NodeValue_Array(name, node, value) constructor { setType(VALUE_TYPE.integer); }
function nodeValue_Vec3(name, value) { return new __NodeValue_Vec3(name, self, value); }
function __NodeValue_Vec3(name, node, value) : __NodeValue_Array(name, node, value) constructor { setDisplay(VALUE_DISPLAY.vector); }
function nodeValue_Quaternion(name, value) { return new __NodeValue_Quaternion(name, self, value); }
function __NodeValue_Quaternion(name, node, value) : __NodeValue_Array(name, node, value) constructor { setDisplay(VALUE_DISPLAY.d3quarternion); }
function nodeValue_Curve(name, value) { return new __NodeValue_Curve(name, self, value); }
function __NodeValue_Curve(name, node, value) : NodeValue(name, node, CONNECT_TYPE.input, VALUE_TYPE.curve, value) constructor { }
function nodeValue_Slider(name, value) { return new __NodeValue_Array(name, self, value).setDisplay(VALUE_DISPLAY.slider); }
function nodeValue_A(name, value) { return nodeValue_B(name, value); }
function nodeValue_B(name, value) { return nodeValue_A(name, value); }
function nodeValue_Dynamic(name, value) { return new __NodeValue_Dynamic(name, self, value); }
function __NodeValue_Dynamic(name, node, value) : __NodeValue_Array(name, node, value) constructor { if (value) setDisplay(VALUE_DISPLAY.vector); }
function nodeValue_Method(name, value) { return new __NodeValue_Method(name, self, value); }
function __NodeValue_Method(name, node, value) : __NodeValue_Array(name, node, value) constructor { static onUpdate = function() { setDisplay(VALUE_DISPLAY.vector); }; }
''', encoding="utf-8")
        return SourceArrayClassification(root, {"nodeValue_Quat": "nodeValue_Quaternion"})

    def test_declaration_is_distinct_from_payload_and_depth(self):
        with tempfile.TemporaryDirectory() as directory:
            index = self.make_index(Path(directory))
            self.assertIs(index.classification("Vector"), False)
            self.assertIs(index.classification("IArray"), False)
            self.assertIs(index.classification("Vec3"), True)
            self.assertIs(index.classification("Quat"), True)
            self.assertIs(index.classification("Slider", ".setArrayDepth(2)"), False)
            self.assertIs(index.classification("Vector", ".setDisplay(VALUE_DISPLAY.number_array)"), True)
            self.assertIs(index.classification("Vec3", ".setDisplay(VALUE_DISPLAY.slider)"), False)
            self.assertIs(index.classification("Curve", ".setDisplay(VALUE_DISPLAY.slider)"), True)
            self.assertIs(index.classification("Vector", ".setType(VALUE_TYPE.curve).setDisplay(VALUE_DISPLAY.slider)"), True)
            self.assertIs(index.classification("Curve", ".setType(VALUE_TYPE.float)"), True)
            for display in ARRAY_DISPLAYS:
                self.assertIs(index.classification("Vector", f".setDisplay(VALUE_DISPLAY.{display})"), True)
            self.assertIs(index.classification("Vector", '// .setDisplay(VALUE_DISPLAY.vector)\n.setDefault("setDisplay(VALUE_DISPLAY.vector)")'), False)

    def test_unproved_constructor_or_branch_is_unknown(self):
        with tempfile.TemporaryDirectory() as directory:
            index = self.make_index(Path(directory))
            self.assertIsNone(index.classification("A"))
            self.assertIsNone(index.classification("Missing"))
            self.assertIsNone(index.classification("Dynamic"))
            self.assertIsNone(index.classification("Vector", ".setDisplay(runtimeDisplay)"))
            self.assertIs(index.classification("Method"), False)
            self.assertIs(index.classification("Generic_curve"), True)
            self.assertIs(index.classification("Generic_float"), False)

    def test_instance_setters_keep_order_aliases_and_unknown_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            index = self.make_index(Path(directory))
            values = [{"index": "0", "_source_classification_state": index.state("Vector")},
                      {"index": "1", "_source_classification_state": index.state("Curve")},
                      {"index": "2", "_source_classification_state": index.state("Vec3")}]
            apply_input_classification_mutations(values, '''function Node_Test() constructor {
inputs[0].setDisplay(VALUE_DISPLAY.vector);
inputs[1].setDisplay(VALUE_DISPLAY.slider);
var target = inputs[2];
target.setDisplay(VALUE_DISPLAY.slider);
}''')
            self.assertEqual([True, True, False], [value["source_array_classification"] for value in values])
            apply_input_classification_mutations(values, '''function Node_Test() constructor {
if (_mode) inputs[0].setDisplay(VALUE_DISPLAY.slider);
inputs[2].setDisplay(_display);
}''')
            self.assertIsNone(values[0]["source_array_classification"])
            self.assertIs(values[1]["source_array_classification"], True)
            self.assertIsNone(values[2]["source_array_classification"])
            apply_input_classification_mutations(values, 'function Node_Test() constructor { inputs[i].setType(_type); }')
            self.assertTrue(all(_result(value["_source_classification_state"]) is None for value in values))

    def test_alias_reassignment_targets_only_the_current_input(self):
        with tempfile.TemporaryDirectory() as directory:
            index = self.make_index(Path(directory))
            values = [{"index": str(i), "_source_classification_state": index.state("Vector")}
                      for i in range(2)]
            apply_input_classification_mutations(values, """function Node_Test() constructor {
var a = inputs[0];
a = inputs[1];
a.setDisplay(VALUE_DISPLAY.area);
}""")
            self.assertEqual([False, True], [_result(value["_source_classification_state"]) for value in values])
            apply_input_classification_mutations(values, """function Node_Test() constructor {
var a = inputs[0];
a = unknownReceiver;
a.setDisplay(VALUE_DISPLAY.area);
}""")
            self.assertIsNone(_result(values[0]["_source_classification_state"]))
            self.assertIs(_result(values[1]["_source_classification_state"]), True)

    def test_runtime_integer_float_switch_preserves_declared_classification(self):
        with tempfile.TemporaryDirectory() as directory:
            index = self.make_index(Path(directory))
            values = [{"index": "0", "_source_classification_state": index.state("Vector")}]
            apply_input_classification_mutations(values, '''function Node_Test() constructor {
static processData = function() {
var _type = _integer? VALUE_TYPE.integer : VALUE_TYPE.float;
inputs[0].setType(_type);
};
}''')
            self.assertIs(values[0]["source_array_classification"], False)
            apply_input_classification_mutations(values, '''function Node_Test() constructor {
static processData = function() {
var _type = _curve? VALUE_TYPE.curve : VALUE_TYPE.float;
inputs[0].setType(_type);
};
}''')
            self.assertIsNone(values[0]["source_array_classification"])

    def test_extractor_keeps_inner_outer_overrides_and_missing_semicolon_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_index(root)
            for name in ["scrollBox", "node_math", "node_vector_math"]:
                placeholder = root / "scripts" / name / (name + ".gml")
                placeholder.parent.mkdir()
                placeholder.write_text("", encoding="utf-8")
            (root / "scripts" / "node_test.gml").write_text('''function Node_Test() : Node() constructor {
newInput(0, nodeValue_Vector("Inner", []).setDisplay(VALUE_DISPLAY.number_array))
newInput(1, nodeValue_Vector("Default", []))
newInput(2, nodeValue_Vector("Outer", [])).setDisplay(VALUE_DISPLAY.vector);
newInput(3, nodeValue("Generic", self, CONNECT_TYPE.input, VALUE_TYPE.path, []).setDisplay(VALUE_DISPLAY.path_array));
}''', encoding="utf-8")
            matrix = root / "matrix.csv"
            matrix.write_text("node_id\nNode_Test\n", encoding="utf-8")
            output = root / "snapshot.json"
            script = Path(__file__).resolve().parents[1] / "pixel-composer" / "extract-source.py"
            process = subprocess.run([sys.executable, str(script), str(root), str(matrix), str(output)], capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            inputs = json.loads(output.read_text())["nodes"]["Node_Test"]["inputs"]
            self.assertEqual([True, False, True, True], [value["source_array_classification"] for value in inputs])


if __name__ == "__main__":
    unittest.main()
