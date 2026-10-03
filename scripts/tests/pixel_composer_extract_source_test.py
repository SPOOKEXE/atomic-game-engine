import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
EXTRACTOR = REPOSITORY / "scripts/pixel-composer/extract-source.py"


class PixelComposerExtractSourceTest(unittest.TestCase):
    def extract(self, root: Path, include_condition: bool = False, include_gradient: bool = False) -> dict:
        script_root = root / "source"
        files = {
            "scripts/scrollBox/scrollBox.gml": "",
            "scripts/node_math/node_math.gml": "",
            "scripts/node_vector_math/node_vector_math.gml": "",
            "scripts/node/node.gml": "function Node(_x, _y, _group = noone) constructor { }\n",
            "scripts/node_value_vec2/node_value_vec2.gml": """
function nodeValue_Vec2(_name, _value) { return new __NodeValue_Vec2(_name, self, _value); }
function __NodeValue_Vec2(_name, _node, _value) : NodeValue(_name, _node, VALUE_TYPE.float, _value) constructor {
    setDisplay(VALUE_DISPLAY.vector);
}
function NodeValue(_name, _node, _type, _value) constructor { type_array = 0; }
""",
            "scripts/node_value_float/node_value_float.gml": """
function nodeValue_Slider(_name, _value) { return new __NodeValue_Float(_name, self, _value); }
function nodeValue_Float(_name, _value) { return new __NodeValue_Float(_name, self, _value); }
function __NodeValue_Float(_name, _node, _value) : __NodeValue_Number(_name, _node, VALUE_TYPE.float, _value) constructor { }
""",
            "scripts/__node_value_number/__node_value_number.gml": """
function __NodeValue_Number(_name, _node, _type, _value) : NodeValue(_name, _node, CONNECT_TYPE.input, _type, _value) constructor { }
""",
            "scripts/node_value/node_value.gml": """
function NodeValue(_name, _node, _connect, _type, _value) constructor {
    array_depth = 0;
}
""",
            "scripts/node_3d_object/node_3d_object.gml": """
function Node_3D_Object(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    newInput(0, nodeValue_Vec3("Position", [0,0,0]));
}
""",
            "scripts/__node_3d_light/__node_3d_light.gml": """
function Node_3D_Light(_x, _y, _group = noone) : Node_3D_Object(_x, _y, _group) constructor {
    if(!LOADING && !APPENDING)
        inputs[0].setValue([ 0, 0, 1 ]);
}
""",
            "scripts/node_3d_light_point/node_3d_light_point.gml": """
function Node_3D_Light_Point(_x, _y, _group = noone) : Node_3D_Light(_x, _y, _group) constructor {
    newInput(1, nodeValue_Float("Radius", 4));
}
""",
            "scripts/node_3d_light_directional/node_3d_light_directional.gml": """
function Node_3D_Light_Directional(_x, _y, _group = noone) : Node_3D_Light(_x, _y, _group) constructor {
    newInput(1, nodeValue_Bool("Cast Shadow", false));
}
""",
            "scripts/node_struct/node_struct.gml": """
function Node_Struct(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    function createNewInput(index = array_length(inputs)) {
        inputs[index + 0] = nodeValue_Text("Key").setAnimable(false);
        inputs[index + 1] = nodeValue("value", self, CONNECT_TYPE.input, VALUE_TYPE.any, 0).setVisible(false, false);
    }
    setDynamicInput(2, false);
    newOutput(0, nodeValue_Output("Struct", VALUE_TYPE.struct, {}));
}
""",
            "scripts/node_string_insert/node_string_insert.gml": """
function Node_String_Insert(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    newInput(0, nodeValue_Text("Text"));
    newInput(1, nodeValue_Text("Insert Text"));
    newInput(2, nodeValue_Int("Position", 0));
    newOutput(0, nodeValue_Output("Text", VALUE_TYPE.text, ""));
}
""",
            "scripts/node_array_shift/node_array_shift.gml": """
function Node_Array_Shift(_x, _y) : Node(_x, _y) constructor {
    newInput(0, nodeValue("Array", self, CONNECT_TYPE.input, VALUE_TYPE.any, 0))
        .setArrayDepth(99)
        .setVisible(true, true);
}
""",
            "scripts/node_gm_room/node_gm_room.gml": """
function Node_GMRoom(_x, _y) : Node(_x, _y) constructor {
    newOutput(0, nodeValue_Surface("Room Preview"));
}
""",
            "scripts/node_color_rgb/node_color_rgb.gml": """
function Node_Color_RGB(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    newInput(3, nodeValue_Bool("Normalized", true));
    newInput(0, nodeValue_Slider("Red", 1));
    newInput(1, nodeValue_Slider("Green", 1));
    newInput(2, nodeValue_Slider("Blue", 1));
    newInput(4, nodeValue_Slider("Alpha", 1));
    newOutput(0, nodeValue_Output("Color", VALUE_TYPE.color, c_white));
    static onValueUpdate = function() {
        if (getInputData(3)) {
            inputs[0].setType(VALUE_TYPE.float);
            inputs[0].setDisplay(VALUE_DISPLAY.slider);
            inputs[1].setType(VALUE_TYPE.float);
            inputs[1].setDisplay(VALUE_DISPLAY.slider);
            inputs[2].setType(VALUE_TYPE.float);
            inputs[2].setDisplay(VALUE_DISPLAY.slider);
            inputs[4].setType(VALUE_TYPE.float);
            inputs[4].setDisplay(VALUE_DISPLAY.slider);
        } else {
            inputs[0].setType(VALUE_TYPE.integer);
            inputs[0].setDisplay(VALUE_DISPLAY.slider, { range: [0, 255, 0.1] });
            inputs[1].setType(VALUE_TYPE.integer);
            inputs[1].setDisplay(VALUE_DISPLAY.slider, { range: [0, 255, 0.1] });
            inputs[2].setType(VALUE_TYPE.integer);
            inputs[2].setDisplay(VALUE_DISPLAY.slider, { range: [0, 255, 0.1] });
            inputs[4].setType(VALUE_TYPE.integer);
            inputs[4].setDisplay(VALUE_DISPLAY.slider, { range: [0, 255, 0.1] });
        }
    }
}
""",
            "scripts/node_color_hsv/node_color_hsv.gml": """
function Node_Color_HSV(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    newInput(3, nodeValue_Bool("Normalized", true));
    newInput(0, nodeValue_Slider("Hue", 1));
    newInput(1, nodeValue_Slider("Saturation", 1));
    newInput(2, nodeValue_Slider("Value", 1));
    newInput(4, nodeValue_Slider("Alpha", 1));
    newOutput(0, nodeValue_Output("Color", VALUE_TYPE.color, c_white));
    static onValueUpdate = function() {
        if (getInputData(3)) {
            inputs[0].setType(VALUE_TYPE.float);
            inputs[0].setDisplay(VALUE_DISPLAY.slider);
            inputs[1].setType(VALUE_TYPE.float);
            inputs[1].setDisplay(VALUE_DISPLAY.slider);
            inputs[2].setType(VALUE_TYPE.float);
            inputs[2].setDisplay(VALUE_DISPLAY.slider);
            inputs[4].setType(VALUE_TYPE.float);
            inputs[4].setDisplay(VALUE_DISPLAY.slider);
        } else {
            inputs[0].setType(VALUE_TYPE.integer);
            inputs[0].setDisplay(VALUE_DISPLAY.slider, { range: [0, 255, 0.1] });
            inputs[1].setType(VALUE_TYPE.integer);
            inputs[1].setDisplay(VALUE_DISPLAY.slider, { range: [0, 255, 0.1] });
            inputs[2].setType(VALUE_TYPE.integer);
            inputs[2].setDisplay(VALUE_DISPLAY.slider, { range: [0, 255, 0.1] });
            inputs[4].setType(VALUE_TYPE.integer);
            inputs[4].setDisplay(VALUE_DISPLAY.slider, { range: [0, 255, 0.1] });
        }
    }
}
""",
            "scripts/node_comment_fixture/node_comment_fixture.gml": r"""
function Node_Comment_Fixture(_x, _y) : Node(_x, _y) constructor {
    // newInput(0, nodeValue_Text("Line comment phantom"));
    /*
    newInput(1, nodeValue_Text("Block comment phantom"));
    */
    var label = "escaped quote: \"keep\" and markers // /* stay text */";
    newInput(2, nodeValue_Text("Literal // and /* block markers */ with \"quotes\""));
}
""",
            "scripts/node_scatter_point_fibo/node_scatter_point_fibo.gml": """
function Node_Scatter_Point_Fibonacci(_x, _y) : Node(_x, _y) constructor {
    newInput(4, nodeValue_Float("Rotation", (1 + sqrt(5)) / 2));
}
""",
            "scripts/node_points_remap/node_points_remap.gml": """
function Node_Points_Remap(_x, _y) : Node(_x, _y) constructor {
    attributes.filter = [1, 1, 0,
                         1, 0, 0,
                         0, 0, 0];
    newInput(0, nodeValue_Vec2("Points", [[0,0]])).setArrayDepth(1);
}
""",
            "scripts/node_points_triangulate/node_points_triangulate.gml": """
function Node_Points_Triangulate(_x, _y) : Node(_x, _y) constructor {
    newInput(0, nodeValue_Vec2("Points", [0,0])).setArrayDepth(1);
}
""",
            "scripts/node_noise_simplex/node_noise_simplex.gml": """
function Node_Noise_Simplex(_x, _y) : Node(_x, _y) constructor {
    newInput(3, nodeValue_ISlider("Iteration", 1, [1,16,.1])).setMappable(9);
    newInput(2, nodeValue_Vec2("Scale", [.25,.25])).setUnitSimple().setMappable(8);
}
""",
            "scripts/node_fn_wave_table/node_fn_wave_table.gml": """
enum WAVETABLE_FN { sine, square, tri, saw }
function Node_Fn_WaveTable(_x, _y) : Node(_x, _y) constructor {
    wavetable_apply = function(typ) {
        attributes.wavetable[wavetable_selecting] = typ;
    };
    wavetable_menu = [
        new MenuItem("Sine", function() { return wavetable_apply(WAVETABLE_FN.sine); }),
        new MenuItem("Square", function() { return wavetable_apply(WAVETABLE_FN.square); }),
        new MenuItem("Triangle", function() { return wavetable_apply(WAVETABLE_FN.tri); }),
        new MenuItem("Sawtooth", function() { return wavetable_apply(WAVETABLE_FN.saw); }),
    ];
    attributes.wavetable = [
        WAVETABLE_FN.sine,
        WAVETABLE_FN.square,
        WAVETABLE_FN.tri,
    ];
}
""",
        }
        if include_condition or include_gradient:
            files["scripts/node_value_enum_scroll/node_value_enum_scroll.gml"] = '''
#macro nodeValue_EScroll nodeValue_Enum_Scroll
function __NodeValue_Enum_Scroll(_name, _node, _value, _data) : NodeValue(_name, _node, CONNECT_TYPE.input, VALUE_TYPE.integer, _value, "") constructor {
    clamp_range = true;
    static isConnectableStrict = function() { return false; }
    static lerpAnimKeys = function(from, to, rat) { return lerp(from.value, to.value, rat); }
    static getValue = function() { if(is_real(val)) val = clamp(val, 0, choicesAmount - 1); return val; }
    static setUnclamp = function() { clamp_range = false; }
}
'''
            files["scripts/node_value/node_value.gml"] = '''
function NodeValue(_name, _node, _connect, _type, _value) constructor {
    array_depth = 0;
    static isConnectable = function(value) { typeCompatible(value.type, type); searchNodeBackward(); return connect_type; }
}
'''
            files["scripts/node_value_types/node_value_types.gml"] = '''
function typeCompatible(fromType, toType) { value_bit(fromType); value_type_directional(fromType, toType); }
'''
            files["scripts/panel_graph/panel_graph.gml"] = '''
function suggest(input) { if(!input.isConnectableStrict(value)) return false; }
'''
            files["scripts/scrollBox/scrollBox.gml"] = '''
function __enum_array_gen(arr, __spr) {
    return array_map(arr, function(v,i) { return new scrollItem(v, __spr, i); });
}
function scrollItem(_name, _spr, _index) constructor { name = _name; }
function scrollBox(_data) : widget() constructor {
    until(data_list[ind] != -1 || ind == curr_val) { ind++; }
}
'''
            files["scripts/node_condition/node_condition.gml"] = '''
function Node_Condition(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    cond_array = __enum_array_gen(["Equal", "Not equal", "Less ", "Less or equal ", "Greater ", "Greater or equal"], s_node_condition_type);
    newInput(1, nodeValue_EScroll("Condition", 0, cond_array)).rejectArray();
    switch(_cond) {
        case 0: res = _chck == _valu; break;
        case 1: res = _chck != _valu; break;
        case 2: res = _chck < _valu; break;
        case 3: res = _chck <= _valu; break;
        case 4: res = _chck > _valu; break;
        case 5: res = _chck >= _valu; break;
    }
}
'''
        if include_gradient:
            files["scripts/node_gradient/node_gradient.gml"] = '''
function Node_Gradient(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    __gradTypes = __enum_array_gen(["Linear", "Circular", "Radial", "Diamond"], s_node_gradient_type);
    newInput(2, nodeValue_EScroll("Type", 0, __gradTypes)).setTopbar();
    newInput(3, nodeValue_Rotation("Angle", 0)).setMappable(10);
    newInput(4, nodeValue_Float("Radius", .5)).setMappable(11);
    newInput(5, nodeValue_Slider("Shift", 0)).setMappable(12);
    newInput(9, nodeValue_Slider("Scale", 1)).setMappable(13);
}
'''
        for relative, content in files.items():
            path = script_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")

        matrix = root / "matrix.csv"
        with matrix.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=["node_id"])
            writer.writeheader()
            for node in (
                "Node_3D_Light_Point",
                "Node_3D_Light_Directional",
                "Node_Struct",
                "Node_String_Insert",
                "Node_Array_Shift",
                "Node_GMRoom",
                "Node_Color_RGB",
                "Node_Color_HSV",
                "Node_Comment_Fixture",
                "Node_Fn_WaveTable",
                "Node_Points_Remap",
                "Node_Points_Triangulate",
                "Node_Noise_Simplex",
                "Node_Scatter_Point_Fibonacci",
                *(('Node_Condition',) if include_condition else ()),
                *(('Node_Gradient',) if include_gradient else ()),
            ):
                writer.writerow({"node_id": node})

        output = root / "source-inputs.json"
        subprocess.run([sys.executable, str(EXTRACTOR), str(script_root), str(matrix), str(output)], check=True)
        return json.loads(output.read_text(encoding="utf-8"))

    def test_fibonacci_square_root_constructor_default_keeps_exact_source_expression(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))
        item = snapshot["nodes"]["Node_Scatter_Point_Fibonacci"]["inputs"][0]
        self.assertEqual("4", item["index"])
        self.assertEqual("Float", item["kind"])
        self.assertEqual("(1 + sqrt(5)) / 2", item["default"])
        self.assertEqual("scripts/node_scatter_point_fibo/node_scatter_point_fibo.gml",
                         snapshot["nodes"]["Node_Scatter_Point_Fibonacci"]["file"])

    def test_simplex_mappable_controls_retain_two_component_range_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        inputs = {item["name"]: item for item in snapshot["nodes"]["Node_Noise_Simplex"]["inputs"]}
        self.assertEqual(
            ("range", "vector2"), (inputs["Iteration"]["mapped"], inputs["Iteration"]["mapped_range_type"])
        )
        self.assertEqual(
            ("range", "vector2"), (inputs["Scale"]["mapped"], inputs["Scale"]["mapped_range_type"])
        )
        evidence = snapshot["source_constructor_evidence"]["scripts/node_noise_simplex/node_noise_simplex.gml"]
        self.assertRegex(evidence["sha256"], r"^[0-9a-f]{64}$")
        self.assertGreater(evidence["bytes"], 0)

    def test_condition_extraction_recovers_exact_six_source_choices(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary), include_condition=True)
        condition = next(item for item in snapshot["nodes"]["Node_Condition"]["inputs"] if item["name"] == "Condition")
        self.assertEqual(["Equal", "Not equal", "Less ", "Less or equal ", "Greater ", "Greater or equal"], condition["choices"])
        self.assertEqual(list(range(6)), [entry["choice_index"] for entry in condition["source_choices"]["entries"]])
        self.assertEqual(6, condition["source_behavior"]["choice_clamp"]["choice_count"])
        self.assertRegex(snapshot["source_choice_generated_evidence"]["cond_array"]["sha256"], r"^[0-9a-f]{64}$")

    def test_gradient_extraction_preserves_exact_shapes_and_two_endpoint_maps(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary), include_gradient=True)
        inputs = {item["name"]: item for item in snapshot["nodes"]["Node_Gradient"]["inputs"]}
        shapes = inputs["Type"]
        self.assertEqual(["Linear", "Circular", "Radial", "Diamond"], shapes["choices"])
        self.assertEqual(list(range(4)), [item["choice_index"] for item in shapes["source_choices"]["entries"]])
        self.assertEqual(4, shapes["source_behavior"]["choice_clamp"]["choice_count"])
        for name in ("Angle", "Radius", "Shift", "Scale"):
            self.assertEqual("vector2", inputs[name]["mapped_range_type"])
            evidence = snapshot["source_constructor_evidence"]["scripts/node_gradient/node_gradient.gml"]
            self.assertRegex(evidence["sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(4, snapshot["source_choice_generated_evidence"]["__gradTypes"]["count"])

    def test_inherited_constructor_default_override_reaches_light_types(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        for node_id in ("Node_3D_Light_Point", "Node_3D_Light_Directional"):
            position = next(item for item in snapshot["nodes"][node_id]["inputs"] if item["name"] == "Position")
            self.assertEqual(position["default"], "[ 0, 0, 1 ]")
            self.assertEqual(
                position["constructor_default_override"],
                {"source_node": "Node_3D_Light", "method": "setValue", "guard": "!LOADING && !APPENDING"},
            )

        evidence = snapshot["source_constructor_evidence"]["scripts/__node_3d_light/__node_3d_light.gml"]
        self.assertRegex(evidence["sha256"], r"^[0-9a-f]{64}$")
        self.assertGreater(evidence["bytes"], 0)

    def test_dynamic_direct_input_assignments_recover_struct_key_value_template(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        dynamic = snapshot["nodes"]["Node_Struct"]["dynamic"]
        self.assertEqual((dynamic["fixed_length"], dynamic["data_length"]), (0, 2))
        self.assertEqual(
            [(item["index"], item["name"], item["kind"], item["default"]) for item in dynamic["template"]],
            [("0", "Key", "Text", ""), ("1", "value", "Generic_any", "0")],
        )
        self.assertIn("scripts/node_struct/node_struct.gml", snapshot["source_constructor_evidence"])

    def test_string_insert_inputs_and_output_are_read_from_constructor(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        node = snapshot["nodes"]["Node_String_Insert"]
        self.assertEqual([(item["name"], item["default"]) for item in node["inputs"]], [
            ("Text", ""), ("Insert Text", ""), ("Position", "0")
        ])
        self.assertEqual(node["outputs"], [{
            "index": "0", "name": "Text", "type": "VALUE_TYPE.text", "default": '""'
        }])

    def test_runtime_float_and_slider_displays_keep_proven_scalar_array_depth(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        for node_id, names in (
            ("Node_Color_RGB", ("Red", "Green", "Blue", "Alpha")),
            ("Node_Color_HSV", ("Hue", "Saturation", "Value", "Alpha")),
        ):
            inputs = snapshot["nodes"][node_id]["inputs"]
            for name in names:
                value = next(item for item in inputs if item["name"] == name)
                self.assertEqual(0, value["array_depth"], (node_id, name))
            self.assertEqual(0, value["array_depth"])
            self.assertIn(f"scripts/{node_id.lower()}/{node_id.lower()}.gml", snapshot["source_constructor_evidence"])
        self.assertIn("scripts/node_value/node_value.gml", snapshot["source_constructor_evidence"])
        self.assertIn("scripts/node_value_float/node_value_float.gml", snapshot["source_constructor_evidence"])
        self.assertIn("scripts/__node_value_number/__node_value_number.gml", snapshot["source_constructor_evidence"])

    def test_explicit_generic_array_depth_is_not_confused_with_array_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        value = snapshot["nodes"]["Node_Array_Shift"]["inputs"][0]
        self.assertEqual(99, value["array_depth"])
        self.assertIn("scripts/node_array_shift/node_array_shift.gml", snapshot["source_constructor_evidence"])

    def test_surface_output_constructor_is_extracted(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        self.assertEqual([{
            "index": "0", "name": "Room Preview", "type": "surface", "default": ""
        }], snapshot["nodes"]["Node_GMRoom"]["outputs"])
        self.assertIn("scripts/node_gm_room/node_gm_room.gml", snapshot["source_constructor_evidence"])

    def test_comment_declarations_are_ignored_and_escaped_strings_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        node = snapshot["nodes"]["Node_Comment_Fixture"]
        self.assertEqual(
            [("2", r'Literal // and /* block markers */ with \"quotes\"')],
            [(item["index"], item["name"]) for item in node["inputs"]],
        )

    def test_wave_table_constructor_attribute_default_is_source_backed(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        node = snapshot["nodes"]["Node_Fn_WaveTable"]
        attribute = next(item for item in node["inputs"] if item["name"] == "attribute wavetable")
        self.assertEqual(attribute["default"], "[WAVETABLE_FN.sine,WAVETABLE_FN.square,WAVETABLE_FN.tri]")
        self.assertEqual(attribute["array_depth"], 1)
        self.assertEqual(attribute["effective_type"], "array")
        self.assertEqual(attribute["array_element_type"], "integer")
        self.assertEqual(attribute["array_allowed_values"], [0, 1, 2, 3])
        self.assertIn("scripts/node_fn_wave_table/node_fn_wave_table.gml", snapshot["source_constructor_evidence"])

    def test_multiline_literal_attribute_preserves_all_filter_switches(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))
        node = snapshot["nodes"]["Node_Points_Remap"]
        attribute = next(item for item in node["inputs"] if item["name"] == "attribute filter")
        self.assertEqual("AttributeArray", attribute["kind"])
        self.assertEqual([1, 1, 0, 1, 0, 0, 0, 0, 0], json.loads(attribute["default"]))
        self.assertEqual("-1", attribute["index"])
        self.assertEqual("filter", attribute["attribute"])

    def test_nested_vec2_constructor_default_stays_vector_array_and_flat_vec2_stays_scalar(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))

        remap = snapshot["nodes"]["Node_Points_Remap"]["inputs"][0]
        self.assertEqual(remap["default"], "[[0,0]]")
        self.assertEqual(remap["array_depth"], 2)
        self.assertEqual(remap["effective_type"], "array")
        self.assertEqual(remap["array_element_type"], "vector2")
        self.assertIn("scripts/node_points_remap/node_points_remap.gml", snapshot["source_constructor_evidence"])

        triangulate = snapshot["nodes"]["Node_Points_Triangulate"]["inputs"][0]
        self.assertEqual(triangulate["default"], "[0,0]")
        self.assertEqual(triangulate["array_depth"], 2)
        self.assertNotIn("effective_type", triangulate)


if __name__ == "__main__":
    unittest.main()
