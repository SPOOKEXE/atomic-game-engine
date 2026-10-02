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
