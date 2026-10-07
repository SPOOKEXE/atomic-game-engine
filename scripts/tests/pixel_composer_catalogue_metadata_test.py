import json
import os
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
    def run_generator(self, root: Path, extra_nodes: dict | None = None, macros: dict | None = None) -> list[str]:
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
            "macros": macros or {},
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

    def test_output_constructor_values_are_independent_of_socket_types_and_keep_expressions(self):
        expressions = [
            ("Absent surface", "surface", "noone", "i -4"),
            ("Empty surfaces", "surface", "[]", "a any 0"),
            ("Boolean", "any", "false", "b 0"),
            ("Real", "float", "-2.5", "d -2.5"),
            ("Negative zero", "float", "-0.0", "d -0"),
            ("Negated zero", "float", "-0", "d -0"),
            ("Signed vector", "float", "[-0.0, 0]", "v -0 0"),
            ("Integer", "integer", "7", "i 7"),
            ("Vector", "float", "[0, 1]", "v 0 1"),
            ("Nested", "any", "[[], [noone, true, 2]]", "a any 2 a any 0 a any 3 i -4 b 1 d 2"),
            ("Text", "text", '"line\\n\\t\\\\\\\"end"', 's "line\\n\\t\\\\\\\"end"'),
            ("White", "color", "c_white", "c 255 255 255 255"),
            ("Black array", "color", "[c_black]", "a any 1 c 0 0 0 255"),
            ("Matrix", "any", "new Matrix(3)", "m 3 3 0 0 0 0 0 0 0 0 0"),
            ("Large matrix", "any", "new Matrix(999999)", ""),
            ("Edited matrix", "any", "new Matrix(3).setArray([1])", ""),
            ("Unknown gradient", "gradient", "new gradientObject(user_colour)", ""),
            ("Runtime", "any", "  new Thing(\n\tself)  ", ""),
            ("Unknown", "any", "PROJECT_WIDTH", ""),
            ("Arithmetic", "any", "1 + 2", ""),
            ("Call", "any", "run()", ""),
            ("Division", "any", "1/0", ""),
            ("Nonfinite", "any", "1e309", ""),
            ("Large integer", "integer", "9223372036854775808", ""),
            ("Inexact real", "float", "9007199254740993", ""),
            ("String escape", "text", '"\\x41"', ""),
            ("Python boolean", "any", "True", ""),
            ("Python number", "any", "1_000", ""),
            ("Python comment", "any", "1 # comment", ""),
            ("Nested comment", "any", "[1, # comment\n2]", ""),
            ("Hash text", "text", '"#keep"', 's "#keep"'),
            ("Normalized identifier", "any", "ｎｏｏｎｅ", ""),
            ("Too long", "any", "1" * 257, ""),
            ("Too deep", "any", "[" * 18 + "0" + "]" * 18, ""),
            ("Too many nodes", "any", "[" + ",".join(["0"] * 65) + "]", ""),
        ]
        node = {"display_name": "Output defaults", "family": "fixture", "base": None,
                "file": "scripts/node_output_defaults/node_output_defaults.gml", "inputs": [],
                "outputs": [{"name": name, "type": "VALUE_TYPE." + kind,
                             "index": str(index), "default": expression}
                            for index, (name, kind, expression, _) in enumerate(expressions)]}
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary), {"Node_Output_Defaults": node})
        outputs = {parts[2]: parts for line in lines if (parts := line.split("\t"))[0] == "O"}
        for name, _, expression, expected in expressions:
            with self.subTest(output=name):
                record = outputs[name]
                self.assertEqual(7, len(record))
                self.assertEqual(expected, record[5])
                self.assertEqual(expression, json.loads(record[6][2:]))

    def test_committed_output_records_preserve_every_selected_source_expression(self):
        snapshot = json.loads((REPOSITORY / "docs/pixel-composer-m0/source-inputs.json").read_text())
        catalogue = (REPOSITORY / "mono.engine/imagegraph/src/SourceCatalogue.inc").read_text()
        body = catalogue.split('R"CATALOGUE(', 1)[1].rsplit(')CATALOGUE"', 1)[0]
        outputs = []
        index = 0
        for line in body.splitlines():
            fields = line.split("\t")
            if fields[0] == "N":
                self.assertEqual(len(outputs), index)
                outputs = snapshot["nodes"][fields[2]]["outputs"]
                index = 0
            elif fields[0] == "O":
                self.assertEqual(7, len(fields))
                self.assertEqual(outputs[index]["default"], json.loads(fields[6][2:]))
                index += 1
        self.assertEqual(len(outputs), index)

    def test_unchanged_generation_keeps_catalogue_build_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.run_generator(root)
            output = root / "mono.engine/imagegraph/src/SourceCatalogue.inc"
            original = output.read_bytes()
            timestamp = 1_700_000_000_000_000_000
            os.utime(output, ns=(timestamp, timestamp))
            retained_timestamp = output.stat().st_mtime_ns
            subprocess.run([sys.executable, str(root / "scripts/pixel-composer/generate-catalogue.py")],
                           cwd=root, check=True)
            self.assertEqual(original, output.read_bytes())
            self.assertEqual(retained_timestamp, output.stat().st_mtime_ns)

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

    def test_mapped_scale_preserves_the_two_source_components(self):
        snapshot = json.loads((REPOSITORY / "docs/pixel-composer-m0/source-inputs.json").read_text())
        for path, evidence in (
            (
                "scripts/node_perlin/node_perlin.gml",
                {
                    "bytes": 4993,
                    "sha256": "9b659974876c3142c1c0ed02ca6133a62b93b83576f7ee8173efe9822cf7afe3",
                },
            ),
            (
                "scripts/node_voronoi_extra/node_voronoi_extra.gml",
                {
                    "bytes": 2125,
                    "sha256": "be8b7a4b549c95466081151406da9041a95f970f1333cee3b5e23e9f9fd0bda7",
                },
            ),
            (
                "scripts/node_shard_noise/node_shard_noise.gml",
                {
                    "bytes": 1869,
                    "sha256": "8a8543c7fee2732fae85a338b7097e254b3d80eda8293f45b4eadd4146a48ffb",
                },
            ),
            (
                "scripts/node_noise_strand/node_noise_strand.gml",
                {
                    "bytes": 2412,
                    "sha256": "87fb3005df645ebb638a0f310ccd1bb07e0c711169fa8b3554277cb90f69dc0c",
                },
            ),
            (
                "scripts/node_weave/node_weave.gml",
                {
                    "bytes": 2923,
                    "sha256": "be8890bee5d7c4629ae04e6f79cad9f5ef734639259355ff1c7f662f0ea3e4fa",
                },
            ),
            (
                "scripts/node_pytagorean_tile/node_pytagorean_tile.gml",
                {
                    "bytes": 5194,
                    "sha256": "6fae8079aaa9785e778df6b8cdeff8e528ea740d46f039cd326997476ac54964",
                },
            ),
            (
                "scripts/node_perlin_extra/node_perlin_extra.gml",
                {
                    "bytes": 3671,
                    "sha256": "c4aa6685ad6c0d6da8b48ce9019276453075138dcce5b6195e911088e46e10ee",
                },
            ),
            (
                "scripts/node_perlin_cube/node_perlin_cube.gml",
                {
                    "bytes": 1778,
                    "sha256": "ccb69433c608f35af4f015ed9811ed7a65ab765c40c9000a71d0c82069bc3e1e",
                },
            ),
            (
                "scripts/node_cellular_cube/node_cellular_cube.gml",
                {
                    "bytes": 1792,
                    "sha256": "385b6fe46d838809562eb3cfb4bd7bcbb4dbd58afea1554334d4d3eff6cd17b8",
                },
            ),
        ):
            self.assertEqual(evidence, snapshot["source_constructor_evidence"][path])
        for name, expected in (
            ("Node_Herringbone_Tile", "v 0.25 0.25"),
            ("Node_Gabor_Noise", "v 4 4"),
            ("Node_Perlin", "v 4 4"),
            ("Node_Shard_Noise", "v 4 4"),
            ("Node_Pytagorean_Tile", "v 0.25 0.25"),
            ("Node_Perlin_Extra", "v 4 4"),
        ):
            node = snapshot["nodes"][name]
            control = next(v for v in node["inputs"] if v["name"] == "Scale")
            self.assertEqual("Vec2", control["kind"])
            self.assertEqual("vector2", control["mapped_range_type"])
            node["display_name"] = name
            node["family"] = "generate"
            with tempfile.TemporaryDirectory() as temporary:
                lines = self.run_generator(Path(temporary), {name: node})
            self.assertIn(f"I\tscale_map_range\tScale Map Range\t-1\tMapRange\tvector2\t{expected}\t", lines)
            self.assertIn("A\tI\tscale\t1", lines)

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
        self.assertIn('O\ttext\tText\t0\ttext\ts ""\ts "\\\"\\\""', lines)
        self.assertIn("N\tpc.spout_receive\tNode_Spout_Receive\tSpout Receive\tundocumented\tscripts/node_spout_receive/node_spout_receive.gml", lines)
        self.assertIn('I\treceiver_name\tReceiver name\t0\tText\ttext\ts "PixelComposer"\t', lines)
        self.assertIn("I\tanimated\tAnimated\t1\tBool\tboolean\tb 1\t", lines)
        self.assertIn('O\tsurface\tSurface\t0\timage\ti -4\ts "noone"', lines)
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

    def test_vec2_constructor_provenance_tracks_surface_dependencies(self):
        node = {
            "display_name": "Vector Defaults", "family": "values",
            "file": "scripts/node_vector_defaults/node_vector_defaults.gml", "base": "Node",
            "inputs": [
                {"index": "0", "kind": "Vec2", "name": "Constant", "default": "[2,3]", "extra": []},
                {"index": "1", "kind": "Vec2", "name": "Fixed Macro", "default": "UNIT_PAIR", "extra": []},
                {"index": "2", "kind": "Vec2", "name": "Project Surface", "default": "PROJ_SURF", "extra": []},
                {"index": "3", "kind": "Vec2", "name": "Nested Surface", "default": "NESTED_SURFACE", "extra": []},
                {"index": "4", "kind": "Dimension", "name": "Default Dimension", "default": "", "extra": []},
                {"index": "-1", "kind": "Vec2", "name": "Synthetic", "default": "[2,3]", "extra": []},
            ],
            "outputs": [],
            "dynamic": {
                "fixed_length": 6, "data_length": 1,
                "template": [{"index": "5", "kind": "Range", "name": "Template", "default": "UNIT_PAIR", "extra": []}],
            },
        }
        macros = {
            "UNIT_PAIR": "[1,1]",
            "NESTED_SURFACE": "SURFACE_ALIAS",
            "SURFACE_ALIAS": "[PROJ_SURF_W,PROJ_SURF_H]",
        }
        with tempfile.TemporaryDirectory() as temporary:
            lines = self.run_generator(Path(temporary), {"Node_Vector_Defaults": node}, macros)

        self.assertIn("I\tconstant\tConstant\t0\tVec2\tvector2\tv 2 3\t", lines)
        self.assertIn("V\tI\tconstant\t1", lines)
        self.assertIn("I\tfixed_macro\tFixed Macro\t1\tVec2\tvector2\tv 1 1\t", lines)
        self.assertIn("V\tI\tfixed_macro\t1", lines)
        self.assertIn("I\tproject_surface\tProject Surface\t2\tVec2\tvector2\tv 32 32\t", lines)
        self.assertIn("V\tI\tproject_surface\t0", lines)
        self.assertIn("I\tnested_surface\tNested Surface\t3\tVec2\tvector2\tv 32 32\t", lines)
        self.assertIn("V\tI\tnested_surface\t0", lines)
        self.assertIn("I\tdefault_dimension\tDefault Dimension\t4\tDimension\tvector2\tv 1 1\t", lines)
        self.assertIn("V\tI\tdefault_dimension\t1", lines)
        self.assertNotIn("V\tI\tsynthetic\t1", lines)
        self.assertIn("V\tT\ttemplate\t1", lines)

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
        with tempfile.TemporaryDirectory() as temporary, self.assertRaises(subprocess.CalledProcessError):
            self.run_generator(Path(temporary), {"Node_Fn_WaveTable": node})


if __name__ == "__main__":
    unittest.main()
