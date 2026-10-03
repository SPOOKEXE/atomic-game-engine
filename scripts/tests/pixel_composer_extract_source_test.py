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
            "scripts/node_herringbone_tile/node_herringbone_tile.gml": """
function Node_Herringbone_Tile(_x, _y) : Node(_x, _y) constructor {
    newInput(2, nodeValue_Vec2("Scale", [.25,.25])).setUnitSimple().setMappable(11);
}
""",
            "scripts/node_gabor_noise/node_gabor_noise.gml": """
function Node_Gabor_Noise(_x, _y) : Node(_x, _y) constructor {
    newInput(2, nodeValue_Vec2("Scale", [4,4])).setShaderProp("scale").setMappable(8);
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
        files["scripts/node_value_enum_button/node_value_enum_button.gml"] = ""
        files["scripts/node_data/node_data.gml"] = "static newInput = function(i,j) { inputs[i] = j; }\n"
        files["scripts/node_3d_object/node_3d_object.gml"] = """
function Node_3D_Object(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    newInput(0, nodeValue_Vec3("Position", [0,0,0]));
    newInput(1, nodeValue_Quaternion("Rotation", [0,0,0,1]));
    newInput(2, nodeValue_Vec3("Scale", [1,1,1]));
    newInput(3, nodeValue_Vec3("Anchor", [0,0,0]));
    attributes.process = true;
}
"""
        files["scripts/node_quarternion_lookat/node_quarternion_lookat.gml"] = """
function Node_Quarternion_Lookat(_x, _y, _group = noone) : Node_3D_Object(_x, _y, _group) constructor {
    newInput(0, nodeValue_Vec3("Origin", [0,0,0])).setVisible(true,true);
    newInput(1, nodeValue_Vec3("Target", [1,0,0])).setVisible(true,true);
    newInput(2, nodeValue_Vec3("Up", [0,0,-1]));
    newInput(3, nodeValue_EButton("Unit", 0, ["Quaternion", "Euler"]));
    newOutput(0, nodeValue_Output("Rotation", VALUE_TYPE.float, [0,0,0,1])).setDisplay(VALUE_DISPLAY.vector);
    static processData = function(_outSurf,_data,_array_index=0) {
        if (_for.LengthSqr()==0) return [0,0,0,1];
        if (_unit==0) return q.ToArray();
        return q.ToEuler(true);
    }
}
"""
        files["scripts/node_switch/node_switch.gml"] = """
function Node_Switch(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    newInput(0, nodeValue_Text("Index")).rejectArray();
    newInput(1, nodeValue("Default value", self, CONNECT_TYPE.input, VALUE_TYPE.any, 0));
    function createNewInput(index = array_length(inputs)) {
        inputs[index + 0] = nodeValue_Text("Case").setDisplay(VALUE_DISPLAY.text_box, { side_button : bDel }).setAnimable(false);
        inputs[index + 1] = nodeValue("Value", self, CONNECT_TYPE.input, VALUE_TYPE.any, 0).setVisible(false, false);
        postCreateNewInput(index);
    }
    setDynamicInput(2, false);
    newOutput(0, nodeValue_Output("Result", VALUE_TYPE.any, 0));
}
"""
        files["scripts/node_threshold_switch/node_threshold_switch.gml"] = """
function Node_Threshold_Switch(_x, _y, _group = noone) : Node(_x, _y, _group) constructor {
    newInput(2, nodeValue_EButton("Type", 0, ["Number", "Frame"]));
    newInput(0, nodeValue_Float("Index")).rejectArray();
    newInput(1, nodeValue("Default Value", self, CONNECT_TYPE.input, VALUE_TYPE.any, 0));
    function createNewInput(index = array_length(inputs)) {
        inputs[index + 0] = nodeValue_Float("Value", 0).setSideButton(bDel).setAnimable(false);
        inputs[index + 1] = nodeValue("Value", self, CONNECT_TYPE.input, VALUE_TYPE.any, 0).setVisible(false, false);
        postCreateNewInput(index);
    }
    setDynamicInput(2, false);
    newOutput(0, nodeValue_Output("Result", VALUE_TYPE.any, 0));
}
"""
        files["scripts/node_value_enum_scroll/node_value_enum_scroll.gml"] = ""
        files["scripts/node_path_shape_3d/node_path_shape_3d.gml"] = """
function Node_Path_Shape_3D(_x, _y, _group=noone) : Node(_x, _y, _group) constructor {
    shape_types=["Rectangle","Ellipse","Regular Polygon",-1,"Star",-1,"Spring","Spring Sphere","Spiral"];
    __ind=0; shapeScroll=array_map(shape_types,function(v,i) { return v==-1 ? -1 : new scrollItem(v,s_node_path_3d_shape,__ind++); });
    newInput(0,nodeValue_Vec3("Position",[0,0,0]));
    newInput(1,nodeValue_Vec3("Half Size",[.5,.5,.5]));
    newInput(2,nodeValue_EScroll("Shape",0,{data:shapeScroll,horizontal:1,text_pad:ui(8)}));
    newOutput(0,nodeValue_Output("Path data",VALUE_TYPE.pathnode,self));
    static getPointRatio=function(_rat,_ind=0,out=undefined) { if(!is(out,__vec3P)) out=new __vec3P(); return out; }
}
"""
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
                "Node_Quarternion_Lookat",
                "Node_3D_Light_Point",
                "Node_3D_Light_Directional",
                "Node_Switch",
                "Node_Threshold_Switch",
                "Node_Path_Shape_3D",
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
                "Node_Herringbone_Tile",
                "Node_Gabor_Noise",
                "Node_Scatter_Point_Fibonacci",
                *(('Node_Condition',) if include_condition else ()),
                *(('Node_Gradient',) if include_gradient else ()),
            ):
                writer.writerow({"node_id": node})

        output = root / "source-inputs.json"
        subprocess.run([sys.executable, str(EXTRACTOR), str(script_root), str(matrix), str(output)], check=True)
        return json.loads(output.read_text(encoding="utf-8"))

    def test_lookat_replaces_inherited_physical_slots_and_preserves_source_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))
        node = snapshot["nodes"]["Node_Quarternion_Lookat"]
        physical = [item for item in node["inputs"] if item["index"] != "-1"]
        self.assertEqual([(item["index"], item["name"]) for item in physical],
                         [("0", "Origin"), ("1", "Target"), ("2", "Up"), ("3", "Unit")])
        self.assertEqual([item["default"] for item in physical],
                         ["[0,0,0]", "[1,0,0]", "[0,0,-1]", "0"])
        self.assertTrue(any(item["name"] == "attribute process" for item in node["inputs"]))
        self.assertEqual(node["outputs"][0]["default"], "[0,0,0,1]")
        self.assertEqual(node["outputs"][0]["type"], "VALUE_TYPE.float")
        light = snapshot["nodes"]["Node_3D_Light_Point"]
        self.assertTrue(any(item["name"] == "Position" for item in light["inputs"]))
        for path in ("scripts/node_quarternion_lookat/node_quarternion_lookat.gml",
                     "scripts/node_data/node_data.gml"):
            self.assertRegex(snapshot["source_constructor_evidence"][path]["sha256"], r"^[0-9a-f]{64}$")

    def test_switch_direct_assignments_preserve_dynamic_pair_order_and_duplicate_names(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))
        for name, fixed, selector_kind, selector_name, selector_default in (
            ("Node_Switch", 2, "Text", "Case", ""),
            ("Node_Threshold_Switch", 3, "Float", "Value", "0"),
        ):
            node = snapshot["nodes"][name]
            self.assertEqual(node["dynamic"]["fixed_length"], fixed)
            self.assertEqual(node["dynamic"]["data_length"], 2)
            template = node["dynamic"]["template"]
            self.assertEqual([(item["index"], item["kind"], item["name"]) for item in template],
                             [("0", selector_kind, selector_name), ("1", "Generic_any", "Value")])
            self.assertEqual([item["default"] for item in template], [selector_default, "0"])
            self.assertEqual(template[1]["array_depth"], 0)
            self.assertEqual(len([item for item in node["inputs"] if item["index"] != "-1"]), fixed)
            self.assertEqual(node["outputs"][0]["type"], "VALUE_TYPE.any")
            evidence = snapshot["source_constructor_evidence"][node["file"]]
            self.assertRegex(evidence["sha256"], r"^[0-9a-f]{64}$")

    def test_path_shape_spatial_output_and_physical_menu_positions_are_source_backed(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary), include_gradient=True)
        node = snapshot["nodes"]["Node_Path_Shape_3D"]
        self.assertEqual(node["base"], "Node")
        self.assertNotIn("array_process", node)
        shape = next(item for item in node["inputs"] if item["name"] == "Shape")
        self.assertEqual(shape["default"], "0")
        self.assertEqual(shape["source_choices"]["status"], "resolved")
        self.assertEqual([x["choice_index"] for x in shape["source_choices"]["entries"]], list(range(9)))
        self.assertEqual([x["choice_index"] for x in shape["source_choices"]["entries"] if "separator" in x], [3,5])
        self.assertEqual(shape["source_behavior"]["choice_clamp"], {"mode":"default", "choice_count":9})
        self.assertEqual(node["outputs"][0]["effective_type"], "path3d")
        self.assertEqual(node["outputs"][0]["type"], "VALUE_TYPE.pathnode")
        self.assertEqual(node["outputs"][0]["default"], "self")
        self.assertRegex(snapshot["source_constructor_evidence"][node["file"]]["sha256"], r"^[0-9a-f]{64}$")

    def test_fibonacci_square_root_constructor_default_keeps_exact_source_expression(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))
        item = snapshot["nodes"]["Node_Scatter_Point_Fibonacci"]["inputs"][0]
        self.assertEqual("4", item["index"])
        self.assertEqual("Float", item["kind"])
        self.assertEqual("(1 + sqrt(5)) / 2", item["default"])
        self.assertEqual("scripts/node_scatter_point_fibo/node_scatter_point_fibo.gml",
                         snapshot["nodes"]["Node_Scatter_Point_Fibonacci"]["file"])

    def test_reviewed_vec2_mapped_patterns_keep_the_existing_two_endpoint_tuple(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = self.extract(Path(temporary))
        for name, default in (("Node_Herringbone_Tile", "[.25,.25]"), ("Node_Gabor_Noise", "[4,4]")):
            item = next(v for v in snapshot["nodes"][name]["inputs"] if v["name"] == "Scale")
            self.assertEqual("Vec2", item["kind"])
            self.assertEqual(default, item["default"])
            self.assertEqual("range", item["mapped"])
            self.assertEqual("vector2", item["mapped_range_type"])
            evidence = snapshot["source_constructor_evidence"][snapshot["nodes"][name]["file"]]
            self.assertRegex(evidence["sha256"], r"^[0-9a-f]{64}$")
            self.assertGreater(evidence["bytes"], 0)

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
