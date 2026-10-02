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
    def run_generator(self, root: Path) -> list[str]:
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
        (docs / "source-inputs.json").write_text(json.dumps(snapshot), encoding="utf-8")
        (docs / "node-parity-matrix.csv").write_text(
            "node_id,display_name,family\nNode_Test,Fixture node,fixture\n",
            encoding="utf-8",
        )
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


if __name__ == "__main__":
    unittest.main()
