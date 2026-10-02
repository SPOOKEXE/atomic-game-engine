import importlib.util
import unittest
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "pixel-composer/enum_values.py"
SPEC = importlib.util.spec_from_file_location("pixel_composer_enum_values", MODULE)
assert SPEC is not None and SPEC.loader is not None
ENUM_VALUES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ENUM_VALUES)


class PixelComposerEnumValuesTest(unittest.TestCase):
    def test_explicit_source_values_override_declaration_ordinals(self):
        members, values = ENUM_VALUES.parse_enum_members(
            "smooth=0, none=1, hue=2, hueInv=5, oklab=3, srgb=4, cmyk=6",
            "GRADIENT_INTER",
        )

        self.assertEqual(members, ["smooth", "none", "hue", "hueInv", "oklab", "srgb", "cmyk"])
        self.assertEqual(values["hueInv"], 5)
        self.assertEqual(
            ENUM_VALUES.enum_member_value("GRADIENT_INTER", "hueInv", {"GRADIENT_INTER": members}, {"GRADIENT_INTER": values}),
            5,
        )

    def test_implicit_values_follow_explicit_and_expression_values(self):
        _, values = ENUM_VALUES.parse_enum_members(
            "first=0x10, next, negative=-0x2, afterNegative, bits=1 << 2, alias=bits, afterAlias"
        )

        self.assertEqual(values, {
            "first": 16,
            "next": 17,
            "negative": -2,
            "afterNegative": -1,
            "bits": 4,
            "alias": 4,
            "afterAlias": 5,
        })

    def test_unsupported_arithmetic_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unsupported enum expression"):
            ENUM_VALUES.parse_enum_members("base=2, derived=base+1")

    def test_comments_are_ignored_between_and_after_members(self):
        members, values = ENUM_VALUES.parse_enum_members(
            "// heading\nfirst = 1, /* explicit gap */ next, // tail\nlast = first,"
        )

        self.assertEqual(members, ["first", "next", "last"])
        self.assertEqual(values, {"first": 1, "next": 2, "last": 1})

    def test_unresolved_expression_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unresolved enum name 'missing'"):
            ENUM_VALUES.parse_enum_members("first=2, second=missing", "BROKEN")

    def test_unresolved_member_does_not_fall_back_to_label_ordinal(self):
        value = ENUM_VALUES.enum_member_value(
            "GRADIENT_INTER",
            "unknown",
            {"GRADIENT_INTER": ["smooth", "none", "hue", "hueInv"]},
            {"GRADIENT_INTER": {"smooth": 0, "none": 1, "hue": 2, "hueInv": 5}},
        )

        self.assertIsNone(value)

    def test_legacy_snapshot_uses_ordered_labels_when_no_value_map_exists(self):
        value = ENUM_VALUES.enum_member_value(
            "Legacy",
            "third",
            {"Legacy": ["first", "second", "third"]},
            {},
            allow_legacy=True,
        )

        self.assertEqual(value, 2)

    def test_snapshot_without_explicit_values_never_uses_ordinal_by_default(self):
        value = ENUM_VALUES.enum_member_value(
            "Pinned",
            "hueInv",
            {"Pinned": ["smooth", "none", "hue", "hueInv"]},
            {},
        )

        self.assertIsNone(value)


if __name__ == "__main__":
    unittest.main()
