"""Generate the imagegraph source catalogue from the committed Pixel Composer source snapshot.

Reads docs/pixel-composer-m0/source-inputs.json and node-parity-matrix.csv and writes
mono.engine/imagegraph/src/SourceCatalogue.inc. Each record is one tab-separated line:

    N <type> <source node> <title> <family> <source file>
    I <id> <name> <source index> <source kind> <value type> <default or empty> <choices joined by ';'>

A Dimension or unit-bearing input is followed by its "<id>_unit" enum. A mappable input is followed by its "<id>_mapped" toggle, which the source keeps as an input attribute, and
for setMappable numeric inputs its "<id>_map_range" [low, high] pair. A curvable input is followed by its
"<id>_<key>" toggle.
    O <id> <name> <source index> <value type> <constructor ValueText or empty> <constructor expression as ValueText text>
    D <fixed input count> <inputs per dynamic group> [maximum groups]
    T <id> <name> <offset in group> <source kind> <value type> <default or empty> <choices>
    S <I|T> <id> <source typeArray classification: 0|1|?>
    B <I|T> <id> <strict suggestion> <actual connectability> <fractional interpolation> <clamp mode> <choice count>
    C <I|T> <id> <resolved|unknown>
    Q <I|T> <id> <source index> <separator flag> <label as ValueText text>

Defaults use the document value text form, so one reader types authored and catalogue values alike.

Usage: uv run scripts/pixel-composer/generate-catalogue.py
"""

import ast
import csv
import json
import math
import re
from fractions import Fraction
from pathlib import Path

from enum_values import enum_member_value

REPO = Path(__file__).resolve().parents[2]
with (REPO / "docs/pixel-composer-m0/source-inputs.json").open(encoding="utf-8") as snapshot_file:
    SNAPSHOT = json.load(snapshot_file)
with (REPO / "docs/pixel-composer-m0/node-parity-matrix.csv").open(encoding="utf-8") as matrix_file:
    MATRIX = list(csv.DictReader(matrix_file))
OUT = REPO / "mono.engine/imagegraph/src/SourceCatalogue.inc"
PINNED_ENUM_SOURCE = "b69eca232217360cf1502ef0223523d818606652"
if SNAPSHOT.get("source_commit") == PINNED_ENUM_SOURCE and "enum_values" not in SNAPSHOT:
    raise ValueError("pinned source snapshot needs enum_values; re-run extract-source.py before catalogue generation")

# Source constructor kind to engine value type. Array-valued numeric defaults refine Scalar and Integer below.
KIND_TYPES = {
    "Bool": "boolean", "Active": "boolean", "Trigger": "boolean",
    "Slider": "scalar", "Float": "scalar", "Rotation": "scalar", "Rot": "scalar", "Float_Simple": "scalar",
    "Int": "integer", "ISlider": "integer", "Toggle": "integer",
    "EScroll": "enum", "EButton": "enum", "Enum_Scroll": "enum", "Enum_Button": "enum",
    "Color": "colour",
    "Vec2": "vector2", "IVec2": "vector2", "Dimension": "vector2", "Range": "vector2", "SliRange": "vector2",
    "Slider_Range": "vector2", "RotRange": "vector2", "Rotation_Range": "vector2", "Anchor": "vector2",
    "Grid_Anchor": "vector2", "Vector": "vector2",
    "Vec3": "vector3", "IVec3": "vector3",
    "Vec4": "vector4", "Padding": "vector4", "IPadding": "vector4", "Corner": "vector4", "Vec2_Range": "vector4",
    "Range2": "vector4",
    "RotRand": "array", "Rotation_Random": "array", "Vec3_Range": "array", "Range3": "array", "Vec2Arr": "array",
    "IArray": "array", "Path_Anchor": "array", "Path_Anchor_3D": "array",
    "Quaternion": "quaternion", "Quat": "quaternion",
    "Text": "text", "FPath": "text", "Font": "text", "EString": "text", "Generic_font": "text",
    "Gradient": "gradient", "Curve": "curve", "Area": "area", "Palette": "array",
    "Path": "path2d", "Generic_path": "path2d",
    "Surface": "image",
    "D3Mesh": "mesh", "Mesh": "mesh2d", "AudioBit": "audiobit",
    "Matrix": "matrix", "Particle": "particle", "Generic_rigid": "rigid", "Fdomain": "fluid_domain",
    "Sdomain": "smoke_domain", "Generic_sdomain": "smoke_domain", "Strand": "strand", "SDF": "sdf",
    "Armature": "armature", "Bone": "armature", "Atlas": "atlas", "Tileset": "tileset", "Pbbox": "pixel_box",
    "D3Scene": "scene3d", "D3Material": "material3d", "3DMat": "material3d", "Buffer": "buffer",
    "Struct": "struct", "Any": "any", "Generic_any": "any", "Generic_node": "node_ref", "Generic_PCXnode": "pcx_node",
    "Generic_object": "object", "Transform": "struct", "Puppet": "struct", "Generic_float": "scalar", "_typ": "enum",
    "Attribute": "enum", "Seed": "scalar", "SeedInt": "integer", "AttributeArray": "array",
}
OUTPUT_TYPES = {
    "surface": "image", "float": "scalar", "integer": "integer", "boolean": "boolean", "text": "text",
    "color": "colour", "gradient": "gradient", "curve": "curve", "path": "text", "pathnode": "path2d",
    "d3Mesh": "mesh", "mesh": "mesh2d", "audioBit": "audiobit", "particle": "particle", "struct": "struct",
    "any": "any", "atlas": "atlas", "trigger": "boolean", "pbBox": "pixel_box", "rigid": "rigid",
    "PCXnode": "pcx_node", "fdomain": "fluid_domain", "buffer": "buffer", "node": "node_ref",
    "sdomain": "smoke_domain", "dynaSurface": "dynamic_surface", "d3Scene": "scene3d", "armature": "armature",
    "tileset": "tileset", "strands": "strand", "object": "object", "d3Light": "light3d", "sdf": "sdf",
    "font": "text", "d3Material": "material3d",
}
# Vector kinds whose empty-list default makes them an array of 2D points.
VECTOR_ARRAY_KINDS = {"Vec2", "IVec2", "Vector", "Vec2Arr"}
# Types with an authored document value. Everything else is a runtime-only typed socket.
AUTHORED = {"boolean", "scalar", "integer", "enum", "colour", "vector2", "vector3", "vector4", "array",
            "quaternion", "text", "gradient", "curve", "area", "path2d", "matrix"}
COLOURS = {
    "c_white": (255, 255, 255), "c_black": (0, 0, 0), "c_dkgrey": (64, 64, 64), "c_dkgray": (64, 64, 64),
    "c_ltgray": (192, 192, 192), "c_ltgrey": (192, 192, 192), "c_grey": (128, 128, 128), "c_gray": (128, 128, 128),
    "c_red": (255, 0, 0), "c_yellow": (255, 255, 0), "c_lime": (0, 255, 0), "c_blue": (0, 0, 255),
    "c_aqua": (0, 255, 255), "c_fuchsia": (255, 0, 255), "c_orange": (255, 160, 64), "c_green": (0, 128, 0),
}
MACROS = SNAPSHOT["macros"]
ENUMS = SNAPSHOT["enums"]
ENUM_VALUES = SNAPSHOT.get("enum_values", {})
PROJECT_SURFACE = [32, 32]
# project_data.gml PROJECT_ATTRIBUTES.palette, used by palettes declared without a default.
PROJECT_PALETTE = "[ca_white, ca_black]"


def snake(text):
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", text.strip())
    text = re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_").lower()
    return text or "value"


def expand(text, depth=0):
    """Replace known macros and enum members with literal values."""
    text = text.strip()
    if depth > 6:
        return text
    text = re.sub(r"/\*.*?\*/", "", text)
    for name in ("PROJ_SURF", "DEF_SURF"):
        text = re.sub(r"\b" + name + r"\b", "[32,32]", text)
    text = re.sub(r"\b(PROJ_SURF_W|PROJ_SURF_H|DEF_SURF_W|DEF_SURF_H)\b", "32", text)

    def enum_member(match):
        value = enum_member_value(
            match.group(1),
            match.group(2),
            ENUMS,
            ENUM_VALUES,
            allow_legacy=SNAPSHOT.get("source_commit") != PINNED_ENUM_SOURCE,
        )
        return str(value) if value is not None else match.group(0)

    text = re.sub(r"\b([A-Z_][A-Z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\b", enum_member, text)
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", text) and text in MACROS and text not in COLOURS:
        return expand(MACROS[text], depth + 1)
    return text


def constant_number(text):
    """Fold bounded square-root constructor expressions without executing source code."""
    if len(text) > 256 or not re.search(r"\bsqrt\s*\(", text):
        return None
    try:
        tree = ast.parse(text, mode="eval")
        if sum(1 for _ in ast.walk(tree)) > 64:
            return None

        def fold(node, depth=0):
            if depth > 16:
                raise ValueError("constant expression depth")
            if isinstance(node, ast.Constant) and type(node.value) in (int, float):
                value = float(node.value)
            elif isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
                value = fold(node.operand, depth + 1)
                if isinstance(node.op, ast.USub):
                    value = -value
            elif isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Sub, ast.Mult, ast.Div)):
                left, right = fold(node.left, depth + 1), fold(node.right, depth + 1)
                if isinstance(node.op, ast.Add):
                    value = left + right
                elif isinstance(node.op, ast.Sub):
                    value = left - right
                elif isinstance(node.op, ast.Mult):
                    value = left * right
                else:
                    value = left / right
            elif (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                  and node.func.id == "sqrt" and len(node.args) == 1 and not node.keywords):
                value = math.sqrt(fold(node.args[0], depth + 1))
            else:
                raise ValueError("unsupported constant expression")
            if not math.isfinite(value):
                raise ValueError("nonfinite constant expression")
            return value

        return fold(tree.body)
    except (SyntaxError, ValueError, ZeroDivisionError, OverflowError, RecursionError):
        return None


def number(text):
    text = text.strip()
    if re.fullmatch(r"-?0b[01]+", text):
        return int(text.replace("0b", ""), 2) * (-1 if text.startswith("-") else 1)
    if re.fullmatch(r"-?0x[0-9a-fA-F]+", text):
        return int(text, 16)
    if re.fullmatch(r"-?\d+/\d+", text):
        return float(Fraction(text))
    if re.fullmatch(r"-?(\d+\.?\d*|\.\d+)", text):
        return float(text)
    if text in ("true", "false"):
        return 1.0 if text == "true" else 0.0
    return constant_number(text)


def items(text):
    """Split a bracketed literal into top-level items, or None when it is not one."""
    text = text.strip()
    if not (text.startswith("[") and text.endswith("]")):
        return None
    depth, current, out = 0, [], []
    for character in text[1:-1]:
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        if character == "," and depth == 0:
            out.append("".join(current).strip())
            current = []
        else:
            current.append(character)
    tail = "".join(current).strip()
    if tail:
        out.append(tail)
    return out


def numbers(text):
    parts = items(expand(text))
    if parts is None:
        return None
    values = [number(expand(part)) for part in parts]
    return None if any(value is None for value in values) else values


def vector2_rows(text):
    """Parse an authored outer array whose rows are two-component vectors."""
    parts = items(expand(text))
    if not parts:
        return None
    rows = []
    for part in parts:
        values = numbers(part)
        if values is None or len(values) != 2:
            return None
        rows.append(values)
    return rows


def validate_array_metadata(source_node, item, value_type):
    element_type = item.get("array_element_type")
    if element_type is None:
        return
    if value_type != "array":
        raise ValueError(f"array element metadata requires array value type: {source_node}.{item['name']}")
    if element_type == "vector2":
        if item["kind"] not in VECTOR_ARRAY_KINDS or vector2_rows(item["default"]) is None:
            raise ValueError(f"invalid source vector2 array default: {source_node}.{item['name']}")
        return
    if element_type == "integer":
        allowed = item.get("array_allowed_values")
        values = numbers(item["default"])
        if (
            item["kind"] != "AttributeArray"
            or not isinstance(allowed, list)
            or not allowed
            or any(type(value) is not int for value in allowed)
            or len(set(allowed)) != len(allowed)
            or values is None
            or any(not value.is_integer() or int(value) not in allowed for value in values)
        ):
            raise ValueError(f"invalid bounded integer array metadata: {source_node}.{item['name']}")
        return
    raise ValueError(f"unsupported array element type {element_type!r}: {source_node}.{item['name']}")


def colour(text):
    text = expand(text)
    if text in ("ca_white", "ca_black", "ca_zero"):
        text = expand(MACROS.get(text, text))
    match = re.fullmatch(r"cola\(\s*([A-Za-z_]+)\s*(?:,\s*([^)]+))?\)", text)
    if match and match.group(1) in COLOURS:
        alpha = number(match.group(2)) if match.group(2) else 1.0
        if alpha is None:
            return None
        return (*COLOURS[match.group(1)], round(alpha * 255))
    if text in COLOURS:
        return (*COLOURS[text], 255)
    value = number(text)
    if value is not None:
        integer = int(value)
        return (integer & 255, (integer >> 8) & 255, (integer >> 16) & 255, 255)
    return None


def fmt(value):
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return repr(float(value))


def default_text(kind, value_type, raw, extra, array_element_type=None):
    raw = raw.strip()
    if value_type == "boolean":
        if kind == "Trigger":
            return "b 0"
        value = number(expand(raw)) if raw else None
        return None if value is None else f"b {1 if value else 0}"
    if value_type == "scalar":
        value = number(expand(raw))
        return None if value is None else f"d {fmt(value)}"
    if value_type in ("integer", "enum"):
        value = number(expand(raw))
        if value is None:
            return None
        return f"{'i' if value_type == 'integer' else 'e'} {int(value)}"
    if value_type == "colour":
        rgba = colour(raw)
        return None if rgba is None else "c " + " ".join(str(part) for part in rgba)
    if value_type in ("vector2", "vector3", "vector4", "quaternion"):
        size = {"vector2": 2, "vector3": 3, "vector4": 4, "quaternion": 4}[value_type]
        if not raw and kind == "Dimension":
            # preferences.gml node_def_dim_unit = 1: a new Dimension is [1, 1] times the project surface.
            values = [1, 1]
        elif not raw and kind == "Anchor":
            values = [0.5, 0.5]
        elif not raw and kind in ("Quaternion", "Quat"):
            values = [0, 0, 0, 1]
        else:
            values = numbers(raw)
        if values is None or len(values) != size:
            return None
        tag = {"vector2": "v", "vector3": "3", "vector4": "w", "quaternion": "h"}[value_type]
        return tag + " " + " ".join(fmt(float(v)) for v in values)
    if value_type == "text":
        if raw in ("", '""', "noone"):
            return 's ""'
        match = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', raw)
        return None if not match else 's "' + match.group(1) + '"'
    if value_type == "array":
        if kind == "Palette":
            if raw in ("", "DEF_PALETTE", "PROJ_PALETTE", "array_clone(PROJ_PALETTE)"):
                raw = PROJECT_PALETTE
            parts = items(expand(raw))
            if parts is None:
                return None
            colours = [colour(part) for part in parts]
            if any(c is None for c in colours):
                return None
            return f"a colour {len(colours)}" + "".join(" c " + " ".join(str(v) for v in c) for c in colours)
        if value_type == "array" and array_element_type == "vector2" and kind in VECTOR_ARRAY_KINDS and raw.strip().startswith("[["):
            vectors = vector2_rows(raw)
            if vectors is None:
                return None
            return f"a vector2 {len(vectors)}" + "".join(
                " v " + " ".join(fmt(float(value)) for value in vector) for vector in vectors
            )
        if kind in VECTOR_ARRAY_KINDS and raw.strip() in ("[]", ""):
            return "a vector2 0"
        if kind == "Text" and raw.strip() == "[]":
            return "a text 0"
        values = numbers(raw)
        if values is None:
            return None
        return f"a scalar {len(values)}" + "".join(f" d {fmt(float(v))}" for v in values)
    if value_type == "gradient":
        match = re.fullmatch(r"new gradientObject\((.*)\)", expand(raw))
        if not match:
            return None
        parts = items(match.group(1)) or [match.group(1)]
        colours = [colour(part) for part in parts]
        if any(c is None for c in colours):
            return None
        if len(colours) == 1:
            return "g 0 1 0 " + " ".join(str(v) for v in colours[0])
        step = 1 / (len(colours) - 1)
        keys = " ".join(fmt(index * step) + " " + " ".join(str(v) for v in c) for index, c in enumerate(colours))
        return f"g 0 {len(colours)} {keys}"
    if value_type == "curve":
        values = numbers(raw)
        if values is None or len(values) < 18 or (len(values) - 6) % 6:
            return None
        return f"q {(len(values) - 6) // 6} " + " ".join(fmt(v) for v in values)
    if value_type == "area":
        values = numbers(raw) if raw else numbers(MACROS.get("DEF_AREA", ""))
        if values is None or len(values) != 6:
            return None
        return "r " + " ".join(fmt(v) for v in values)
    if value_type == "path2d":
        return "p 0 0 0"
    if value_type == "matrix":
        # new Matrix(n) is an n by n zero matrix; setArray fills it row-major.
        match = re.fullmatch(r"new Matrix\((\d+)\)(?:\.setArray\(\[([^\]]*)\]\))?", raw.strip())
        if not match:
            return None
        size = int(match.group(1))
        values = [0.0] * (size * size)
        for index, part in enumerate((match.group(2) or "").split(",") if match.group(2) else []):
            values[index] = float(part)
        return f"m {size} {size} " + " ".join(fmt(v) for v in values)
    return None


def refine(kind, value_type, raw):
    """Numeric inputs with array defaults are vectors or arrays in the source."""
    if value_type in ("vector2", "vector3", "vector4") and (raw.strip() == "[]" or (kind == "Vector" and not raw.strip())):
        return "array"
    if value_type == "text" and raw.strip() == "[]":
        return "array"
    if value_type not in ("scalar", "integer"):
        return value_type
    values = numbers(raw) if raw.strip().startswith("[") else None
    if values is None:
        return "array" if raw.strip() == "[]" else value_type
    return {2: "vector2", 3: "vector3", 4: "vector4"}.get(len(values), "array")


def output_type(item):
    if item.get("effective_type"):
        return item["effective_type"]
    kind = item["type"].replace("VALUE_TYPE.", "")
    if "Matrix(" in item["default"]:
        return "matrix"
    value_type = OUTPUT_TYPES.get(kind, "any")
    if value_type in ("scalar", "integer"):
        return refine("Float", value_type, item["default"])
    return value_type


def value_text_string(text):
    """Match the document reader's escapes without losing source whitespace."""
    return 's "' + text.replace("\\", "\\\\").replace('"', '\\"').replace(
        "\n", "\\n").replace("\r", "\\r").replace("\t", "\\t") + '"'


def output_constructor_default(raw, value_type):
    """Resolve bounded source literals and reviewed constructors without executing update code."""
    if len(raw) > 256 or not raw.strip():
        return None
    try:
        expression = raw.strip()
        matrix = re.fullmatch(r"new Matrix\(([1-4])\)", expression)
        if matrix:
            size = int(matrix.group(1))
            return f"m {size} {size} " + " ".join(["0"] * (size * size))
        if expression == "CURVE_DEF_01":
            return default_text("Curve", "curve", expand(expression), {})
        if re.fullmatch(r"new gradientObject\((?:ca_white|ca_black)\)", expression):
            return default_text("Gradient", "gradient", expression, {})
        if "#" in re.sub(r'"(?:[^"\\]|\\.)*"', "", expression):
            return None
        tree = ast.parse(expression, mode="eval")
        if (sum(1 for _ in ast.walk(tree)) > 64
                or ast.get_source_segment(expression, tree.body) != expression):
            return None

        def encode(node, declared="any", depth=0):
            if depth > 16:
                raise ValueError("constructor literal depth")
            if isinstance(node, ast.Name) and node.id in (*COLOURS, "ca_white", "ca_black", "ca_zero"):
                if ast.get_source_segment(expression, node) != node.id:
                    raise ValueError("unsupported source colour identifier")
                value = colour(node.id)
                if value is None:
                    raise ValueError("unresolved source colour")
                return "c " + " ".join(str(channel) for channel in value)
            if isinstance(node, ast.Name) and node.id in ("noone", "true", "false"):
                if ast.get_source_segment(expression, node) != node.id:
                    raise ValueError("unsupported source identifier")
                return {"noone": "i -4", "true": "b 1", "false": "b 0"}[node.id]
            if isinstance(node, ast.Constant) and type(node.value) is str:
                segment = ast.get_source_segment(expression, node)
                if not re.fullmatch(r'"(?:[^"\\\r\n]|\\[\\"nrt])*"', segment or ""):
                    raise ValueError("unsupported source string literal")
                return value_text_string(node.value)
            if isinstance(node, ast.List):
                tags = {"vector2": (2, "v"), "vector3": (3, "3"),
                        "vector4": (4, "w"), "quaternion": (4, "h")}
                elements = [encode(child, depth=depth + 1) for child in node.elts]
                vector = tags.get(declared)
                if vector and len(elements) == vector[0] and all(v.startswith("d ") for v in elements):
                    return vector[1] + " " + " ".join(v[2:] for v in elements)
                return f"a any {len(elements)}" + "".join(" " + v for v in elements)
            sign = 1
            if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
                sign = -1 if isinstance(node.op, ast.USub) else 1
                node = node.operand
            if isinstance(node, ast.Constant) and type(node.value) in (int, float):
                segment = ast.get_source_segment(expression, node)
                if not re.fullmatch(r'(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?', segment or ""):
                    raise ValueError("unsupported source number literal")
                value = sign * node.value
                if not math.isfinite(value):
                    raise ValueError("nonfinite constructor literal")
                if declared == "integer" and type(value) is int:
                    if not -(2 ** 63) <= value < 2 ** 63:
                        raise ValueError("constructor integer range")
                    return f"i {value}"
                if type(value) is int and abs(value) > 2 ** 53:
                    raise ValueError("constructor number is not exactly representable")
                numeric = sign * float(node.value)
                if numeric == 0 and math.copysign(1, numeric) < 0:
                    return "d -0"
                return f"d {fmt(numeric)}"
            raise ValueError("unresolved constructor expression")

        return encode(tree.body, value_type)
    except (SyntaxError, ValueError, OverflowError, RecursionError):
        return None


def clean(text):
    return re.sub(r"[\t\n\r]+", " ", text).strip()


def boolean_field(value):
    if value is None:
        return "?"
    if type(value) is not bool:
        raise ValueError(f"expected a boolean metadata value, got {value!r}")
    return "1" if value else "0"


def source_behavior_record(record_kind, identifier, item):
    behavior = item.get("source_behavior")
    if behavior is None:
        return None
    clamp = behavior.get("choice_clamp")
    if not isinstance(clamp, dict):
        raise TypeError(f"incomplete source behavior metadata: {identifier}")
    actual = behavior.get("actual_connectability")
    if actual not in ("general", "unknown"):
        raise ValueError(f"unknown source connectability metadata for {identifier}: {actual!r}")
    clamp_mode = clamp.get("mode")
    if clamp_mode not in ("always", "default", "disabled", "unknown"):
        raise ValueError(f"unknown source clamp metadata for {identifier}: {clamp_mode!r}")
    choice_count = clamp.get("choice_count")
    if choice_count is None:
        choice_count_text = "?"
    elif type(choice_count) is int and choice_count >= 0:
        choice_count_text = str(choice_count)
    else:
        raise ValueError(f"invalid source choice count for {identifier}: {choice_count!r}")
    return "\t".join([
        "B", record_kind, identifier,
        boolean_field(behavior.get("strict_suggestion")),
        actual,
        boolean_field(behavior.get("fractional_interpolation")),
        clamp_mode,
        choice_count_text,
    ])


def source_choice_records(record_kind, identifier, item):
    choices = item.get("source_choices")
    if choices is None:
        return []
    status = choices.get("status")
    entries = choices.get("entries")
    if status not in ("resolved", "unknown"):
        raise ValueError(f"invalid source choices status for {identifier}: {status!r}")
    if status == "unknown":
        if entries is not None:
            raise ValueError(f"unknown source choices include fabricated entries: {identifier}")
        return [f"C\t{record_kind}\t{identifier}\tunknown"]
    if not isinstance(entries, list):
        raise TypeError(f"resolved source choices lack entries: {identifier}")

    records = [f"C\t{record_kind}\t{identifier}\tresolved"]
    for expected_index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise TypeError(f"invalid source choice entry for {identifier}: {entry!r}")
        source_index = entry.get("choice_index")
        if type(source_index) is not int or source_index != expected_index:
            raise ValueError(f"nonsequential source choice index for {identifier}: {source_index!r}")
        if "separator" in entry:
            if entry.get("separator") != -1 or "label" in entry:
                raise ValueError(f"invalid source choice separator for {identifier}: {entry!r}")
            separator, label = "1", ""
        else:
            label = entry.get("label")
            if not isinstance(label, str):
                raise ValueError(f"invalid source choice label for {identifier}: {label!r}")
            separator = "0"
        value_text = "s " + json.dumps(label, ensure_ascii=False)
        records.append("\t".join(["Q", record_kind, identifier, str(source_index), separator, value_text]))
    return records


families = {}
titles = {}
for row in MATRIX:
    if row["node_id"]:
        families.setdefault(row["node_id"], row["family"])
        titles.setdefault(row["node_id"], row["display_name"])

lines = []
source_classifications = {}
types = set()
for source_node, node in sorted(SNAPSHOT["nodes"].items()):
    if not node["file"]:
        continue
    missing_depth = [item["name"] for item in node["inputs"] if "array_depth" not in item]
    if SNAPSHOT.get("source_commit") == PINNED_ENUM_SOURCE and missing_depth:
        raise ValueError(f"pinned source inputs need array_depth metadata: {source_node}: {missing_depth[:3]}")
    native = "pc." + snake(source_node[len("Node_"):])
    assert native not in types, native
    types.add(native)
    title = titles.get(source_node, source_node[len("Node_"):].replace("_", " "))
    family = families.get(source_node, "undocumented")
    lines.append("\t".join(["N", native, source_node, clean(title), family, node["file"]]))
    seen, owners = set(), {}
    for item in node["inputs"]:
        base = snake(item["name"]) if item["name"] else "input_" + snake(item["index"])
        owner = item.get("map_of")
        if owner in owners and item["name"] in (owner + " Map", owner + " Map Range"):
            # A helper-declared map is named after its owner, so a renamed duplicate keeps "<id>_map".
            base = owners[owner] + ("_map" if item["name"] == owner + " Map" else "_map_range")
        identifier, suffix = base, 2
        while identifier in seen:
            identifier, suffix = f"{base}_{suffix}", suffix + 1
        seen.add(identifier)
        owners[item["name"]] = identifier
        value_type = item.get("effective_type") or refine(item["kind"], KIND_TYPES.get(item["kind"], "any"), item["default"])
        validate_array_metadata(source_node, item, value_type)
        default = default_text(item["kind"], value_type, item["default"], item["extra"], item.get("array_element_type")) if value_type in AUTHORED else None
        index = item["index"] if re.fullmatch(r"\d+", item["index"]) else "-1"
        labels = ";".join(clean(label).replace(";", ",") for label in (item.get("choices") or []))
        if labels and not labels.replace(";", ""):
            labels = ""
        lines.append("\t".join(["I", identifier, clean(item.get("display_name", item["name"])), index, item["kind"], value_type, default or "", labels]))
        source_classifications[(native, "I", identifier)] = item.get("source_array_classification")
        array_depth = item.get("array_depth", 0)
        if array_depth is None or array_depth != 0:
            lines.append("\t".join(["A", "I", identifier, "?" if array_depth is None else str(array_depth)]))
        behavior_record = source_behavior_record("I", identifier, item)
        if behavior_record is not None:
            lines.append(behavior_record)
        lines.extend(source_choice_records("I", identifier, item))
        if item["kind"] == "Surface" and item["name"] == "Mask":
            # mask_apply_input reads the Mask junction's mask_alpha_only attribute.
            seen.add("mask_alpha_only")
            lines.append("I\tmask_alpha_only\tMask Alpha Only\t-1\tMaskAlphaOnly\tboolean\tb 0\t")
        if item.get("array_select"):
            selector = item["array_select"]
            lines.append("\t".join([
                "I", identifier + "_select", clean(item["name"]) + " Array Select", "-1",
                "SourceArraySelect", "enum", f"e {selector['default']}", ";".join(selector["choices"])
            ]))
        if item["kind"] == "Dimension" or item.get("unit"):
            # Dimension keeps use_project_dimension; setUnitSimple keeps a pixel or surface-relative unit.
            toggle = identifier + "_unit"
            seen.add(toggle)
            if item["kind"] == "Dimension":
                lines.append("\t".join(["I", toggle, clean(item["name"]) + " Unit", "-1", "DimensionUnit", "enum", "e 1", "Pixel;Project;Mask"]))
            else:
                mode = 1 if item["unit"] == "reference" else 0
                unit_kind = item.get("unit_source_kind", "ValueUnit")
                unit_choices = item.get("unit_choices", ["Pixel", "Reference"])
                lines.append("\t".join(["I", toggle, clean(item["name"]) + " Unit", "-1", unit_kind, "enum", f"e {mode}", ";".join(unit_choices)]))
        if item.get("mapped"):
            # The source stores the map toggle as an input attribute. While setMappable is on, a numeric
            # input's value becomes a [low, high] range mixed by the map, recorded here as its own input.
            toggle = identifier + "_mapped"
            seen.add(toggle)
            lines.append("\t".join(["I", toggle, clean(item["name"]) + " Mapped", "-1", "MapToggle", "boolean", "b 0", ""]))
            ranged = {"scalar": ("vector2", "v"), "vector2": ("vector4", "w")}.get(value_type)
            # Bevel Height uses the source Int getter but maps the same two numeric endpoints.
            if source_node == "Node_Bevel" and identifier == "height" and item["index"] == "1" and item["kind"] == "Int":
                ranged = ("vector2", "v")
            mapped_range_type = item.get("mapped_range_type")
            if mapped_range_type is not None:
                markers = {"vector2": "v", "vector4": "w"}
                marker = markers.get(mapped_range_type)
                if marker is None:
                    raise ValueError(f"invalid mapped range type: {source_node}.{item['name']}")
                ranged = (mapped_range_type, marker)
            if ranged and item["mapped"] == "range":
                current = (default or "").split(" ")[1:]
                endpoint_count = {"vector2": 2, "vector4": 4}[ranged[0]]
                span = ["0"] * max(0, endpoint_count - len(current)) + current if current else []
                range_default = f"{ranged[1]} " + " ".join(span) if span else ""
                seen.add(identifier + "_map_range")
                lines.append("\t".join(["I", identifier + "_map_range", clean(item["name"]) + " Map Range", "-1", "MapRange", ranged[0], range_default, ""]))
        for key in item.get("curves", []):
            # setCurvable keeps its on/off switch as an input attribute named by the key.
            toggle = identifier + "_" + snake(key)
            seen.add(toggle)
            lines.append("\t".join(["I", toggle, clean(item["name"]) + " " + key, "-1", "CurveToggle", "boolean", "b 0", ""]))
    processor = node.get("array_process")
    if processor is not None:
        default = processor.get("default")
        choices = ";".join(clean(label).replace(";", ",") for label in processor.get("choices", []))
        if not isinstance(default, int) or not choices:
            raise ValueError(f"incomplete source array process metadata: {source_node}")
        lines.append("\t".join([
            "I", "attribute_array_process", "Array Process Type", "-1", "EButton", "enum", f"e {default}", choices
        ]))
    dynamic = node.get("dynamic")
    if dynamic:
        # One dynamic group repeats the template after the fixed inputs. Instances are named "<id>_<group>".
        dynamic_fields = ["D", str(dynamic["fixed_length"]), str(dynamic["data_length"] or 1)]
        if dynamic.get("max_groups") is not None:
            dynamic_fields.append(str(dynamic["max_groups"]))
        lines.append("\t".join(dynamic_fields))
        template_ids = set()
        for item in dynamic["template"]:
            base = item.get("id") or (snake(item["name"]) if item["name"] else "input_" + snake(item["index"]))
            identifier, suffix = base, 2
            while identifier in template_ids:
                identifier, suffix = f"{base}_{suffix}", suffix + 1
            template_ids.add(identifier)
            value_type = item.get("effective_type") or refine(item["kind"], KIND_TYPES.get(item["kind"], "any"), item["default"])
            validate_array_metadata(source_node, item, value_type)
            default = default_text(item["kind"], value_type, item["default"], item["extra"], item.get("array_element_type")) if value_type in AUTHORED else None
            index = item["index"] if re.fullmatch(r"\d+", item["index"]) else "-1"
            labels = ";".join(clean(label).replace(";", ",") for label in (item.get("choices") or []))
            if labels and not labels.replace(";", ""):
                labels = ""
            lines.append("\t".join(["T", identifier, clean(item.get("display_name", item["name"])), index, item["kind"], value_type, default or "", labels]))
            source_classifications[(native, "T", identifier)] = item.get("source_array_classification")
            array_depth = item.get("array_depth", 0)
            if array_depth is None or array_depth != 0:
                lines.append("\t".join(["A", "T", identifier, "?" if array_depth is None else str(array_depth)]))
            behavior_record = source_behavior_record("T", identifier, item)
            if behavior_record is not None:
                lines.append(behavior_record)
            lines.extend(source_choice_records("T", identifier, item))
            if item.get("unit"):
                mode = 1 if item["unit"] == "reference" else 0
                lines.append("\t".join(["T", identifier + "_unit", clean(item.get("display_name", item["name"])) + " Unit", "-1", "ValueUnit", "enum", f"e {mode}", "Pixel;Reference"]))
    outputs = set()
    for item in node["outputs"]:
        base = snake(item["name"]) if item["name"] else "output_" + snake(item["index"])
        identifier, suffix = base, 2
        while identifier in outputs:
            identifier, suffix = f"{base}_{suffix}", suffix + 1
        outputs.add(identifier)
        index = item["index"] if re.fullmatch(r"\d+", item["index"]) else "-1"
        value_type = output_type(item)
        expression = item["default"]
        lines.append("\t".join(["O", identifier, clean(item["name"]), index, value_type,
                                output_constructor_default(expression, value_type) or "",
                                value_text_string(expression)]))

# Every input has a distinct declaration classification record, including unknown host attributes.
classified_lines = []
source_type = ""
for line in lines:
    fields = line.split("\t")
    classified_lines.append(line)
    if fields[0] == "N":
        source_type = fields[1]
    elif fields[0] in {"I", "T"}:
        value = source_classifications.get((source_type, fields[0], fields[1]))
        if value is not None and type(value) is not bool:
            raise ValueError(f"source_array_classification must be Boolean or unknown: {source_type}.{fields[1]}")
        classified_lines.append("\t".join(["S", fields[0], fields[1], "?" if value is None else str(int(value))]))
lines = classified_lines

header = (
    "// Generated by scripts/pixel-composer/generate-catalogue.py from docs/pixel-composer-m0/source-inputs.json.\n"
    f"// Pinned Pixel Composer source commit {SNAPSHOT['source_commit']}. Do not edit by hand.\n"
)
body = "\n".join(lines)
assert ")CATALOGUE\"" not in body
generated = header + 'R"CATALOGUE(' + body + '\n)CATALOGUE"\n'
if not OUT.exists() or OUT.read_text(encoding="utf-8") != generated:
    OUT.write_text(generated, encoding="utf-8")
inputs = [line for line in lines if line.startswith("I")]
typed = [line for line in inputs if line.split("\t")[5] in AUTHORED]
defaulted = [line for line in typed if line.split("\t")[6]]
print(f"{len(types)} nodes, {len(inputs)} inputs, {len(defaulted)} of {len(typed)} authored defaults resolved")
