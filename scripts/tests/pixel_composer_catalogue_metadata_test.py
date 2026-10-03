import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
GENERATOR = REPOSITORY / "scripts/pixel-composer/generate-catalogue.py"
ENUM_VALUES = REPOSITORY / "scripts/pixel-composer/enum_values.py"


class PixelComposerCatalogueMetadataTest(unittest.TestCase):
    def run_generator(self, root: Path, extra_nodes: dict | None = None) -> list[str]:
        script = root / "scripts/pixel-composer/generate-catalogue.py"
        script.parent.mkdir(parents=True)
        shutil.copy2(GENERATOR, script)
        shutil.copy2(ENUM_VALUES, script.parent / "enum_values.py")
        docs = root / "docs/pixel-composer-m0"
        docs.mkdir(parents=True)
        snapshot = {
            "source_commit": "fixture",
            "nodes": {
                "Node_Test": {
                    "file": "scripts/node_test/node_test.gml",
                    "base": None,
                    "inputs": [
                        {
                            "index": "0",
                            "kind": "EScroll",
                            "name": "Fixed Choice",
                            "source_array_classification": False,
                            "default": "0",
                            "extra": [],
                            "choices": ["Alpha; beta", "Gamma"],
                            "array_depth": 0,
                            "source_behavior": {
                                "strict_suggestion": False,
                                "actual_connectability": "general",
                                "fractional_interpolation": True,
                                "choice_clamp": {"mode": "default", "choice_count": 3},
                            },
                            "source_choices": {
                                "status": "resolved",
                                "entries": [
                                    {"choice_index": 0, "label": "Alpha; beta"},
                                    {"choice_index": 1, "separator": -1},
                                    {"choice_index": 2, "label": 'Gamma "quoted"\tline'},
                                ],
                            },
                        },
                        {
                            "index": "1",
                            "kind": "EButton",
                            "name": "Dynamic Choice",
                            "default": "0",
                            "extra": [],
                            "choices": [],
                            "array_depth": 0,
                            "source_behavior": {
                                "strict_suggestion": None,
                                "actual_connectability": "unknown",
                                "fractional_interpolation": None,
                                "choice_clamp": {"mode": "unknown", "choice_count": None},
                            },
                            "source_choices": {"status": "unknown", "entries": None},
                        },
                    ],
                    "outputs": [],
                    "array_process": {"default": 0, "choices": ["Loop", "Hold"]},
                    "dynamic": {
                        "fixed_length": 2,
                        "data_length": 1,
                        "template": [
                            {
                                "index": "0",
                                "kind": "EButton",
                                "name": "Template Choice",
                                "source_array_classification": True,
                                "default": "0",
                                "extra": [],
                                "choices": ["First", "Second"],
                                "array_depth": 0,
                                "source_behavior": {
                                    "strict_suggestion": False,
                                    "actual_connectability": "general",
                                    "fractional_interpolation": True,
                                    "choice_clamp": {"mode": "always", "choice_count": 2},
                                },
                                "source_choices": {
                                    "status": "resolved",
                                    "entries": [
                                        {"choice_index": 0, "label": "First"},
                                        {"choice_index": 1, "label": "Second"},
                                    ],
                                },
                            }
                        ],
                    },
                }
            },
            "macros": {},
            "enums": {},
            "enum_values": {},
        }
        snapshot["nodes"].update(extra_nodes or {})
        (docs / "source-inputs.json").write_text(json.dumps(snapshot), encoding="utf-8")
        matrix_rows = ["node_id,display_name,family", "Node_Test,Fixture node,fixture"]
        matrix_rows.extend(
            f"{node_id},{metadata['display_name']},{metadata['family']}"
            for node_id, metadata in (extra_nodes or {}).items()
        )
        (docs / "node-parity-matrix.csv").write_text("\n".join(matrix_rows) + "\n", encoding="utf-8")
        (root / "mono.engine/imagegraph/src").mkdir(parents=True)
        subprocess.run([sys.executable, str(script)], cwd=root, check=True)
        output = root / "mono.engine/imagegraph/src/SourceCatalogue.inc"
        return output.read_text(encoding="utf-8").splitlines()

    def test_square_root_constructor_defaults_are_bounded_and_source_only(self):
        expressions = {
            "Fibonacci": "(1 + sqrt(5)) / 2",
            "Nested": "-sqrt(4) + sqrt(sqrt(16)) * 3 / 2",
            "Zero division": "sqrt(5) / 0",
            "Negative root": "sqrt(-1)",
            "Unknown function": "abs(sqrt(5))",
            "Source variable": "sqrt(PROJECT_WIDTH)",
            "Attribute call": "math.sqrt(5)",
            "Keywords": "sqrt(x=5)",
            "Two arguments": "sqrt(4, 5)",
            "Power": "sqrt(2 ** 3)",
            "Boolean": "sqrt(True)",
            "Nonfinite": "sqrt(1e309)",
            "Too long": "sqrt(" + "1" * 257 + ")",
            "Too deep": "sqrt(" * 18 + "1" + ")" * 18,
            "Too many nodes": "sqrt(1)" + "+1" * 33,
            "Unrelated arithmetic": "1 + 2",
        }
        node = {
            "display_name": "Constant defaults", "family": "fixture",
            "file": "scripts/node_constant/node_constant.gml", "base": None,
            "inputs": [{"index": str(index), "kind": "Float", "name": name,
                        "default": expression, "extra": [], "array_depth": 0}
                       for index, (name, expression) in enumerate(expressions.items())],
            "outputs": [],
        }
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary), {"Node_Constants": node})
        defaults = {parts[1]: parts[6] for line in lines if (parts := line.split("\t"))[0] == "I"}
        self.assertEqual("d 1.618033988749895", defaults["fibonacci"])
        self.assertEqual("d 1", defaults["nested"])
        for name in expressions:
            if name in ("Fibonacci", "Nested"):
                continue
            with self.subTest(expression=name):
                self.assertEqual("", defaults[name.lower().replace(" ", "_")])

    def test_bevel_mapped_integer_height_emits_source_endpoint_pair_default(self):
        node = {"display_name": "Bevel", "family": "fixture", "base": None,
                "file": "scripts/node_bevel/node_bevel.gml", "outputs": [],
                "inputs": [{"index": "1", "kind": "Int", "name": "Height", "default": "4",
                            "extra": [], "array_depth": 0, "mapped": "range"}]}
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary), {"Node_Bevel": node})
        self.assertIn("I\theight\tHeight\t1\tInt\tinteger\ti 4\t", lines)
        self.assertIn("I\theight_map_range\tHeight Map Range\t-1\tMapRange\tvector2\tv 0 4\t", lines)
        self.assertIn("I\theight_mapped\tHeight Mapped\t-1\tMapToggle\tboolean\tb 0\t", lines)

    def test_fixed_input_emits_behavior_and_exact_choice_indices(self):
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary))

        self.assertIn("I\tfixed_choice\tFixed Choice\t0\tEScroll\tenum\te 0\tAlpha, beta;Gamma", lines)
        self.assertIn("B\tI\tfixed_choice\t0\tgeneral\t1\tdefault\t3", lines)
        self.assertIn("C\tI\tfixed_choice\tresolved", lines)
        self.assertIn('Q\tI\tfixed_choice\t0\t0\ts "Alpha; beta"', lines)
        self.assertIn('Q\tI\tfixed_choice\t1\t1\ts ""', lines)
        self.assertIn('Q\tI\tfixed_choice\t2\t0\ts "Gamma \\"quoted\\"\\tline"', lines)

    def test_unknown_choices_and_behavior_stay_explicit_and_templates_use_t_ids(self):
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary))

        self.assertIn("B\tI\tdynamic_choice\t?\tunknown\t?\tunknown\t?", lines)
        self.assertIn("C\tI\tdynamic_choice\tunknown", lines)
        self.assertFalse(any(line.startswith("Q\tI\tdynamic_choice\t") for line in lines))
        self.assertIn("B\tT\ttemplate_choice\t0\tgeneral\t1\talways\t2", lines)
        self.assertIn("C\tT\ttemplate_choice\tresolved", lines)
        self.assertIn('Q\tT\ttemplate_choice\t1\t0\ts "Second"', lines)
        self.assertTrue(any(line.startswith("I\tattribute_array_process\t") for line in lines))
        self.assertFalse(any(line.startswith(("B\tI\tattribute_array_process\t", "C\tI\tattribute_array_process\t", "Q\tI\tattribute_array_process\t")) for line in lines))

    def test_classification_is_separate_and_unknown_is_not_payload_inference(self):
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary))
        self.assertIn("S\tI\tfixed_choice\t0", lines)
        self.assertIn("S\tI\tdynamic_choice\t?", lines)
        self.assertIn("S\tT\ttemplate_choice\t1", lines)
        self.assertIn("S\tI\tattribute_array_process\t?", lines)
        self.assertEqual(sum(line.startswith(("I\t", "T\t")) for line in lines),
                         sum(line.startswith("S\t") for line in lines))

    def test_pinned_single_shapes_keep_process_disabled_in_catalogue(self):
        source_path = REPOSITORY / "docs/pixel-composer-m0/source-inputs.json"
        source = json.loads(source_path.read_text(encoding="utf-8"))
        self.assertEqual("b69eca232217360cf1502ef0223523d818606652", source["source_commit"])
        shapes = {node_id: source["nodes"][node_id] for node_id in (
            "Node_Shape_Ellipse", "Node_Shape_Rectangle", "Node_Shape_Half",
        )}
        for node in shapes.values():
            process = next(item for item in node["inputs"] if item.get("attribute") == "process")
            self.assertEqual("false", process["default"])

        fixture = {
            "display_name": "Process fixture", "family": "fixture",
            "file": "scripts/node_process_fixture/node_process_fixture.gml",
            "base": None, "outputs": [],
            "inputs": [{
                "index": "-1", "kind": "Bool", "name": "attribute process", "attribute": "process",
                "default": "false", "extra": [], "array_depth": 0, "source_array_classification": False,
            }],
        }
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary), {"Node_Shape_Process_Fixture": fixture})
        self.assertIn("I\tattribute_process\tattribute process\t-1\tBool\tboolean\tb 0\t", lines)

        catalogue_path = REPOSITORY / "mono.engine/imagegraph/src/SourceCatalogue.inc"
        catalogue = catalogue_path.read_text(encoding="utf-8").splitlines()
        for node_id, catalogue_id in (
            ("Node_Shape_Ellipse", "pc.shape_ellipse"),
            ("Node_Shape_Rectangle", "pc.shape_rectangle"),
            ("Node_Shape_Half", "pc.shape_half"),
        ):
            with self.subTest(node=node_id):
                start = catalogue.index(next(line for line in catalogue if line.startswith(f"N\t{catalogue_id}\t")))
                end = next(
                    (index for index in range(start + 1, len(catalogue)) if catalogue[index].startswith("N\t")),
                    len(catalogue),
                )
                self.assertIn("I\tattribute_process\tattribute process\t-1\tBool\tboolean\tb 0\t", catalogue[start:end])

    def test_simplex_mapped_ranges_keep_source_dimensions(self):
        source = json.loads(
            (REPOSITORY / "docs/pixel-composer-m0/source-inputs.json").read_text(encoding="utf-8")
        )
        self.assertEqual("b69eca232217360cf1502ef0223523d818606652", source["source_commit"])
        simplex = source["nodes"]["Node_Noise_Simplex"]
        mapped_types = {
            item["name"]: item.get("mapped_range_type")
            for item in simplex["inputs"]
            if item.get("mapped")
        }
        self.assertEqual({"Iteration": "vector2", "Scale": "vector2"}, mapped_types)
        simplex["display_name"] = "Simplex Noise"
        simplex["family"] = "generate"
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary), {"Node_Noise_Simplex": simplex})

        self.assertIn("I\titeration_mapped\tIteration Mapped\t-1\tMapToggle\tboolean\tb 0\t", lines)
        self.assertIn("I\titeration_map\tIteration Map\t9\tSurface\timage\t\t", lines)
        self.assertIn("I\titeration_map_range\tIteration Map Range\t-1\tMapRange\tvector2\tv 0 1\t", lines)
        self.assertIn("I\tscale_mapped\tScale Mapped\t-1\tMapToggle\tboolean\tb 0\t", lines)
        self.assertIn("I\tscale_map\tScale Map\t8\tSurface\timage\t\t", lines)
        self.assertIn("I\tscale_map_range\tScale Map Range\t-1\tMapRange\tvector2\tv 0.25 0.25\t", lines)

    def test_source_only_constructor_records_preserve_values_and_dynamic_templates(self):
        extra_nodes = {
            "Node_3D_Light_Point": {
                "display_name": "Point Light", "family": "3d",
                "file": "scripts/node_3d_light_point/node_3d_light_point.gml", "base": "Node_3D_Light",
                "inputs": [{
                    "index": "0", "kind": "Vec3", "name": "Position", "default": "[0,0,1]",
                    "extra": [], "array_depth": 1, "source_array_classification": True,
                }],
                "outputs": [],
            },
            "Node_Struct": {
                "display_name": "Struct", "family": "values",
                "file": "scripts/node_struct/node_struct.gml", "base": "Node",
                "inputs": [], "outputs": [],
                "dynamic": {
                    "fixed_length": 0, "data_length": 2,
                    "template": [
                        {"index": "0", "kind": "Text", "name": "Key", "default": "", "extra": [],
                         "array_depth": 0, "source_array_classification": False},
                        {"index": "1", "kind": "Generic_any", "name": "value", "default": "0", "extra": [],
                         "array_depth": 0, "source_array_classification": False},
                    ],
                },
            },
            "Node_String_Insert": {
                "display_name": "Insert Text", "family": "undocumented",
                "file": "scripts/node_string_insert/node_string_insert.gml", "base": "Node_Processor",
                "source_only": True,
                "inputs": [
                    {"index": "0", "kind": "Text", "name": "Text", "default": "", "extra": [],
                     "array_depth": 0, "source_array_classification": False},
                    {"index": "1", "kind": "Text", "name": "Insert Text", "default": "", "extra": [],
                     "array_depth": 0, "source_array_classification": False},
                    {"index": "2", "kind": "Int", "name": "Position", "default": "0", "extra": [],
                     "array_depth": 0, "source_array_classification": False},
                ],
                "outputs": [{"index": "0", "name": "Text", "type": "VALUE_TYPE.text", "default": '""'}],
            },
            "Node_Spout_Receive": {
                "display_name": "Spout Receive", "family": "undocumented",
                "file": "scripts/node_spout_receive/node_spout_receive.gml", "base": "Node",
                "source_only": True,
                "inputs": [
                    {"index": "0", "kind": "Text", "name": "Receiver name", "default": '"PixelComposer"',
                     "extra": [], "array_depth": 0, "source_array_classification": False},
                    {"index": "1", "kind": "Bool", "name": "Animated", "default": "true",
                     "extra": [], "array_depth": 0, "source_array_classification": False},
                ],
                "outputs": [{"index": "0", "name": "Surface", "type": "VALUE_TYPE.surface", "default": "noone"}],
            },
            "Node_Points_Remap": {
                "display_name": "Points Remap", "family": "transform",
                "file": "scripts/node_points_remap/node_points_remap.gml", "base": "Node",
                "inputs": [{
                    "index": "0", "kind": "Vec2", "name": "Points", "default": "[[0,0]]",
                    "extra": [], "array_depth": 2, "effective_type": "array", "array_element_type": "vector2",
                    "source_array_classification": True,
                }],
                "outputs": [],
            },
            "Node_Points_Triangulate": {
                "display_name": "Points Triangulate", "family": "transform",
                "file": "scripts/node_points_triangulate/node_points_triangulate.gml", "base": "Node",
                "inputs": [{
                    "index": "0", "kind": "Vec2", "name": "Points", "default": "[0,0]",
                    "extra": [], "array_depth": 2, "source_array_classification": True,
                }],
                "outputs": [],
            },
            "Node_Fn_WaveTable": {
                "display_name": "Wave Table", "family": "animation",
                "file": "scripts/node_fn_wave_table/node_fn_wave_table.gml", "base": "Node_Fn",
                "inputs": [{
                    "index": "-1", "kind": "AttributeArray", "name": "attribute wavetable",
                    "default": "[0,1,2]", "extra": [], "attribute": "wavetable", "array_depth": 1,
                    "effective_type": "array", "array_element_type": "integer", "array_allowed_values": [0,1,2,3],
                    "source_array_classification": None,
                }],
                "outputs": [],
            },
        }
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary), extra_nodes)

        self.assertIn("N\tpc.string_insert\tNode_String_Insert\tInsert Text\tundocumented\tscripts/node_string_insert/node_string_insert.gml", lines)
        self.assertIn('I\ttext\tText\t0\tText\ttext\ts ""\t', lines)
        self.assertIn("I\tposition\tPosition\t2\tInt\tinteger\ti 0\t", lines)
        self.assertIn("O\ttext\tText\t0\ttext", lines)
        self.assertIn("N\tpc.spout_receive\tNode_Spout_Receive\tSpout Receive\tundocumented\tscripts/node_spout_receive/node_spout_receive.gml", lines)
        self.assertIn('I\treceiver_name\tReceiver name\t0\tText\ttext\ts "PixelComposer"\t', lines)
        self.assertIn("I\tanimated\tAnimated\t1\tBool\tboolean\tb 1\t", lines)
        self.assertIn("O\tsurface\tSurface\t0\timage", lines)
        self.assertIn("D\t0\t2", lines)
        self.assertIn('T\tkey\tKey\t0\tText\ttext\ts ""\t', lines)
        self.assertIn("T\tvalue\tvalue\t1\tGeneric_any\tany\t\t", lines)
        self.assertIn("I\tposition\tPosition\t0\tVec3\tvector3\t3 0 0 1\t", lines)
        self.assertIn("I\tpoints\tPoints\t0\tVec2\tarray\ta vector2 1 v 0 0\t", lines)
        self.assertIn("A\tI\tpoints\t2", lines)
        self.assertIn("I\tpoints\tPoints\t0\tVec2\tvector2\tv 0 0\t", lines)
        self.assertIn("A\tI\tpoints\t2", lines)
        self.assertIn("I\tattribute_wavetable\tattribute wavetable\t-1\tAttributeArray\tarray\ta scalar 3 d 0 d 1 d 2\t", lines)
        self.assertIn("A\tI\tattribute_wavetable\t1", lines)
        self.assertFalse(any("Node_VerletSim_Simple" in line for line in lines))

    def test_bounded_integer_array_rejects_values_outside_its_source_enum(self):
        node = {
            "display_name": "Wave Table", "family": "animation",
            "file": "scripts/node_fn_wave_table/node_fn_wave_table.gml", "base": "Node_Fn",
            "inputs": [{
                "index": "-1", "kind": "AttributeArray", "name": "attribute wavetable",
                "default": "[0,1,2]", "extra": [], "attribute": "wavetable", "array_depth": 1,
                "effective_type": "array", "array_element_type": "integer", "array_allowed_values": [0,1],
                "source_array_classification": None,
            }],
            "outputs": [],
        }
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(subprocess.CalledProcessError):
                self.run_generator(Path(temporary), {"Node_Fn_WaveTable": node})


if __name__ == "__main__":
    unittest.main()
