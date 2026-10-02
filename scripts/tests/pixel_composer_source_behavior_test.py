import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pixel-composer"))

from source_behavior import (
    choice_count,
    choice_source_evidence,
    enum_behavior,
    node_math_choice_source,
    node_vector_math_choice_source,
    source_choice_map,
)


def source_fixture(root: Path, name: str, source_kind: str, class_name: str, display: str) -> None:
    source = root / "scripts" / name / f"{name}.gml"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text(
        f"""#macro nodeValue_{source_kind} nodeValue_{class_name}
function nodeValue_{class_name}(_name, _value, _data) {{ return new __NodeValue_{class_name}(_name, self, _value, _data); }}
function __NodeValue_{class_name}(_name, _node, _value, _data) : NodeValue(_name, _node, CONNECT_TYPE.input, VALUE_TYPE.integer, _value, "") constructor {{
    {display}
    static isConnectableStrict = function() {{return false}};
    static getValue = function() {{
        if(!clamp_range) return val;
        if(is_real(val)) val = clamp(val, 0, choicesAmount - 1);
        return val;
    }}
    static lerpAnimKeys = function(from, to, rat) {{ return lerp(from.value, to.value, rat); }}
}}
""",
        encoding="utf-8",
    )
    source.parent.mkdir(parents=True, exist_ok=True)


class PixelComposerSourceBehaviorTest(unittest.TestCase):
    def make_root(self) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source_fixture(root, "node_value_enum_button", "EButton", "Enum_Button", "")
        source_fixture(root, "node_value_enum_scroll", "EScroll", "Enum_Scroll", "clamp_range = true;\n    static setUnclamp = function() { clamp_range = false; }")
        base = root / "scripts/node_value/node_value.gml"
        base.parent.mkdir(parents=True)
        base.write_text(
            "static isConnectable = function(value) { if(!typeCompatible(value.type, type)) return -4; if(value.searchNodeBackward(node)) return -7; return connect_type; }",
            encoding="utf-8",
        )
        types = root / "scripts/node_value_types/node_value_types.gml"
        types.parent.mkdir(parents=True)
        types.write_text(
            "function typeCompatible(fromType, toType) { if(value_bit(fromType) & value_bit(toType)) return true; return value_type_directional(fromType, toType); }",
            encoding="utf-8",
        )
        suggestions = root / "scripts/panel_graph/panel_graph.gml"
        suggestions.parent.mkdir(parents=True)
        suggestions.write_text("if (!input.isConnectableStrict(value)) continue;", encoding="utf-8")
        choices = root / "scripts/scrollBox/scrollBox.gml"
        choices.parent.mkdir(parents=True)
        choices.write_text(
            "function __enum_array_gen(arr, spr) { return array_map(arr, function(v,i) { return new scrollItem(v, spr, i); }); }\n"
            "function scrollItem(_name) constructor { name = _name; }\n"
            "function scrollBox(_data) : widget() constructor { until(data_list[ind] != -1 || ind == curr_val); }",
            encoding="utf-8",
        )
        return root

    def test_button_and_scroll_keep_integer_behavior_separate_from_suggestion(self):
        root = self.make_root()
        button, _ = enum_behavior(root, "EButton", 3, "", False)
        scroll, _ = enum_behavior(root, "EScroll", 3, "", False)

        self.assertEqual(False, button["strict_suggestion"])
        self.assertEqual("general", button["actual_connectability"])
        self.assertTrue(button["fractional_interpolation"])
        self.assertEqual({"mode": "always", "choice_count": 3}, button["choice_clamp"])
        self.assertEqual(False, scroll["strict_suggestion"])
        self.assertEqual("general", scroll["actual_connectability"])
        self.assertTrue(scroll["fractional_interpolation"])
        self.assertEqual({"mode": "default", "choice_count": 3}, scroll["choice_clamp"])

    def test_scroll_unclamp_is_recorded_only_for_a_static_chain(self):
        root = self.make_root()
        static, _ = enum_behavior(root, "EScroll", 4, ".setUnclamp()", False)
        conditional, _ = enum_behavior(root, "EScroll", 4, "if (_mode) { .setUnclamp() }", False)
        later_mutation, _ = enum_behavior(root, "EScroll", 4, ".setUnclamp()", True)

        self.assertEqual("disabled", static["choice_clamp"]["mode"])
        self.assertEqual("unknown", conditional["choice_clamp"]["mode"])
        self.assertEqual("unknown", later_mutation["choice_clamp"]["mode"])

    def test_choice_count_requires_an_exact_resolved_array_length(self):
        globals_ = {"GLOBAL_CHOICES": '["A", /* ignored, comma */ VALUE_X, -1,]'}
        self.assertEqual(3, choice_count('["A", nested(1, 2), -1]', "", {}))
        self.assertEqual(2, choice_count('["A", /* comma, ignored */ "B", // trailing entry\n]', "", {}))
        self.assertEqual(3, choice_count("__enum_array_gen([\"A\", \"B\", \"C\"], ENUM)", "", {}))
        self.assertEqual(3, choice_count("array_create(3, VALUE_X)", "", {}))
        self.assertEqual(3, choice_count("GLOBAL_CHOICES", "", globals_))
        self.assertEqual(2, choice_count("local_choices", "var local_choices = [\"A\", 4];", {}))
        self.assertIsNone(choice_count("runtime_choices", "", {}))
        self.assertIsNone(choice_count("[\"A\",", "", {}))

    def test_unrelated_integer_kinds_do_not_get_enum_metadata(self):
        root = self.make_root()
        self.assertEqual((None, None), enum_behavior(root, "Int", 3, "", False))

    def test_unproven_or_mutated_connectability_stays_unknown(self):
        root = self.make_root()
        source = root / "scripts/node_value/node_value.gml"
        source.write_text("static isConnectable = function(value) { return false; }", encoding="utf-8")
        unverified, evidence = enum_behavior(root, "EButton", 3, "", False)
        self.assertIsNone(evidence)
        self.assertIsNone(unverified["strict_suggestion"])
        self.assertEqual("unknown", unverified["actual_connectability"])

        root = self.make_root()
        mutated, _ = enum_behavior(root, "EButton", 3, "", False, dynamic_connectability=True)
        self.assertEqual("unknown", mutated["actual_connectability"])

    def test_source_choice_indices_keep_negative_separator_slots(self):
        root = self.make_root()
        evidence = choice_source_evidence(root)
        self.assertIsNotNone(evidence)
        choices = source_choice_map(
            '["Normal", "Replace", -1, "Multiply", "Color Burn"]',
            "",
            {},
            array_map_verified=True,
            scroll_item_verified=True,
            separator_verified=True,
        )
        self.assertEqual(
            [
                {"choice_index": 0, "label": "Normal"},
                {"choice_index": 1, "label": "Replace"},
                {"choice_index": 2, "separator": -1},
                {"choice_index": 3, "label": "Multiply"},
                {"choice_index": 4, "label": "Color Burn"},
            ],
            choices,
        )

    def test_comment_entries_are_not_source_choices(self):
        root = self.make_root()
        choices = source_choice_map(
            '["Shape", /*"Mesh"*/ ]',
            "",
            {},
            array_map_verified=True,
            scroll_item_verified=True,
            separator_verified=True,
        )
        self.assertEqual([{"choice_index": 0, "label": "Shape"}], choices)

    def test_dynamic_array_entries_and_unverified_wrappers_stay_unknown(self):
        root = self.make_root()
        self.assertIsNone(
            source_choice_map(
                '["Known", runtime_label]',
                "",
                {},
                array_map_verified=True,
                scroll_item_verified=True,
                separator_verified=True,
            )
        )
        self.assertIsNone(
            source_choice_map(
                '__enum_array_gen(["A", "B"], enum)',
                "",
                {},
                array_map_verified=False,
                scroll_item_verified=True,
                separator_verified=True,
            )
        )

    def test_verified_scroll_item_names_map_by_source_array_position(self):
        choices = source_choice_map(
            '[new scrollItem("Sphere", sprite, 4), new scrollItem("Plane", sprite, 8)]',
            "",
            {},
            array_map_verified=True,
            scroll_item_verified=True,
            separator_verified=True,
        )
        self.assertEqual(
            [
                {"choice_index": 0, "label": "Sphere"},
                {"choice_index": 1, "label": "Plane"},
            ],
            choices,
        )

    def test_node_math_generated_scroll_resolves_only_verified_literal_labels(self):
        root = self.make_root()
        math = root / "scripts/node_math/node_math.gml"
        math.parent.mkdir(parents=True)
        labels = [
            "Add", "Subtract", "Multiply", "Divide", "Power", "Root", "Sin", "Cos", "Tan", "Modulo",
            "Floor", "Ceil", "Round", "Lerp", "Abs", "Clamp", "Snap", "Fract", "Map", "Log", "Max", "Min",
        ]
        literals = ", ".join(f'"{label}"' for label in labels)
        math.write_text(
            f'global.node_math_names = [{literals}];\n'
            "global.node_math_scroll = array_create_ext(array_length(global.node_math_names), "
            "function(i) /*=>*/ {return new scrollItem(global.node_math_names[i], s_node_math_operators, i)});\n"
            'function Node_Math() { newInput(0, nodeValue_EScroll("Type", 0, global.node_math_scroll)).rejectArray(); }',
            encoding="utf-8",
        )

        found_labels, evidence = node_math_choice_source(root)
        self.assertEqual(labels, found_labels)
        self.assertEqual(22, evidence["count"])
        self.assertEqual(
            "node_math_scroll copies node_math_names[i] in array_create_ext index order",
            evidence["mapping"],
        )
        mapped = source_choice_map(
            "global.node_math_scroll",
            "",
            {},
            array_map_verified=True,
            scroll_item_verified=True,
            separator_verified=True,
            allowlisted_arrays={"global.node_math_scroll": found_labels},
        )
        self.assertEqual(22, len(mapped))
        self.assertEqual({"choice_index": 2, "label": "Multiply"}, mapped[2])
        self.assertEqual({"choice_index": 18, "label": "Map"}, mapped[18])
        self.assertIsNone(
            source_choice_map(
                "global.other_scroll",
                "",
                {},
                array_map_verified=True,
                scroll_item_verified=True,
                separator_verified=True,
                allowlisted_arrays={"global.node_math_scroll": found_labels},
            )
        )

    def test_node_math_scroll_mapping_rejects_dynamic_labels_or_changed_schema(self):
        root = self.make_root()
        math = root / "scripts/node_math/node_math.gml"
        math.parent.mkdir(parents=True)
        math.write_text(
            'global.node_math_names = ["Add", runtime_name];\n'
            "global.node_math_scroll = array_create_ext(array_length(global.node_math_names), "
            "function(i) {return new scrollItem(global.node_math_names[i], s_node_math_operators, i)});\n"
            'function Node_Math() { newInput(0, nodeValue_EScroll("Type", 0, global.node_math_scroll)).rejectArray(); }',
            encoding="utf-8",
        )
        self.assertEqual((None, None), node_math_choice_source(root))

    def test_node_vector_math_generated_scroll_preserves_eight_source_indices(self):
        root = self.make_root()
        vector_math = root / "scripts/node_vector_math/node_vector_math.gml"
        vector_math.parent.mkdir(parents=True)
        labels = ["Add", "Subtract", "Multiply", "Divide", "Power", "Root", "Length", "Distance"]
        literals = ", ".join(f'"{label}"' for label in labels)
        vector_math.write_text(
            f'global.node_vmath_names = [{literals}];\n'
            "global.node_vmath_scroll = array_create_ext(array_length(global.node_vmath_names), "
            "function(i) {return new scrollItem(global.node_vmath_names[i], s_node_vmath_operators, i)});\n"
            'function Node_Vector_Math() { newInput(0, nodeValue_EScroll("Type", 0, '
            'global.node_vmath_scroll)).rejectArray(); }',
            encoding="utf-8",
        )

        found_labels, evidence = node_vector_math_choice_source(root)
        self.assertEqual(labels, found_labels)
        self.assertEqual(8, evidence["count"])
        self.assertEqual(
            "node_vmath_scroll copies node_vmath_names[i] in array_create_ext index order",
            evidence["mapping"],
        )
        mapped = source_choice_map(
            "global.node_vmath_scroll",
            "",
            {},
            array_map_verified=True,
            scroll_item_verified=True,
            separator_verified=True,
            allowlisted_arrays={"global.node_vmath_scroll": found_labels},
        )
        self.assertEqual({"choice_index": 6, "label": "Length"}, mapped[6])
        self.assertEqual({"choice_index": 7, "label": "Distance"}, mapped[7])

    def test_node_vector_math_scroll_rejects_dynamic_labels_or_nonidentity_mapping(self):
        root = self.make_root()
        vector_math = root / "scripts/node_vector_math/node_vector_math.gml"
        vector_math.parent.mkdir(parents=True)
        vector_math.write_text(
            'global.node_vmath_names = ["Add", runtime_name];\n'
            "global.node_vmath_scroll = array_create_ext(array_length(global.node_vmath_names), "
            "function(i) {return new scrollItem(global.node_vmath_names[7 - i], s_node_vmath_operators, i)});\n"
            'function Node_Vector_Math() { newInput(0, nodeValue_EScroll("Type", 0, '
            'global.node_vmath_scroll)).rejectArray(); }',
            encoding="utf-8",
        )
        self.assertEqual((None, None), node_vector_math_choice_source(root))


if __name__ == "__main__":
    unittest.main()
