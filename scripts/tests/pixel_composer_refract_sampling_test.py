"""Reviewed Refract constructor sampling overrides, with no source checkout dependency."""
import ast
import re
import unittest
from pathlib import Path

EXTRACTOR = Path(__file__).resolve().parents[1] / "pixel-composer/extract-source.py"


def helper():
    tree = ast.parse(EXTRACTOR.read_text(encoding="utf-8"))
    nodes = [node for node in tree.body if isinstance(node, ast.FunctionDef)
             and node.name in ("refract_sampling", "call_args")]
    scope = {"re": re, "NODE_ATTRIBUTE": re.compile(
        r"^\s*(?:self\.)?attributes\.([a-z_][a-z0-9_]*)\s*=\s*([^;\n]+)", re.M)}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(EXTRACTOR), "exec"), scope)
    return scope["refract_sampling"]


# Literal reviewed lines from node_refract.gml, including factory order.
SOURCE = '''function Node_Refract() constructor {
    attribute_oversample();
    attribute_interpolation(false, true);
    attributes.oversample = 3;
    static processData = function() { attributes.oversample = 99; }
}'''


class RefractSampling(unittest.TestCase):
    def test_constructor_final_override_and_extended_choice(self):
        self.assertEqual((True, "3"), helper()(SOURCE))

    def test_nonliteral_override_is_not_invented(self):
        with self.assertRaises(ValueError):
            helper()(SOURCE.replace("oversample = 3", "oversample = preference"))

    def test_later_competing_override_is_rejected(self):
        with self.assertRaises(ValueError):
            helper()(SOURCE.replace("static processData", "attributes.oversample = 4;\n    static processData"))

    def test_initialization_after_override_is_rejected(self):
        with self.assertRaises(ValueError):
            helper()(SOURCE.replace("attribute_interpolation(false, true);", "").replace(
                "attributes.oversample = 3;", "attributes.oversample = 3;\n    attribute_interpolation(false, true);"))

    def test_later_factory_reset_is_not_a_literal_default(self):
        with self.assertRaises(ValueError):
            helper()(SOURCE.replace("attributes.oversample = 3;", "attributes.oversample = 3;\n    attribute_oversample();"))

    def test_callback_only_override_does_not_supply_a_constructor_default(self):
        with self.assertRaises(ValueError):
            helper()(SOURCE.replace("    attributes.oversample = 3;\n", ""))

    def test_unverified_interpolation_options_are_rejected(self):
        with self.assertRaises(ValueError):
            helper()(SOURCE.replace("false, true", "false, false"))


if __name__ == "__main__":
    unittest.main()
