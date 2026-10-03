"""Extract node input and output declarations from a pinned Pixel Composer source checkout.

The result is the committed `docs/pixel-composer-m0/source-inputs.json` snapshot. Regenerating the engine
catalogue reads only that snapshot, so the external checkout is needed only when the pin moves.

Usage: uv run scripts/pixel-composer/extract-source.py <checkout> <node-parity-matrix.csv> <out.json> [fixture-dir] [verified-node-evidence.json]

With a fixture directory, node types used by the supplied .pxc projects but absent from the documentation
inventory are extracted too and marked "undocumented".
"""

import csv
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

from enum_values import parse_enum_members
from surface_depth import DEPTH_LABELS, depth_attribute
from array_depth import SourceIndex, apply_runtime_depth_mutations, declaration_depth
from source_selection import verified_source_nodes
from source_array_classification import SourceArrayClassification, apply_input_classification_mutations, _result
from source_behavior import (
    _matching_end,
    choice_count,
    choice_source_evidence,
    enum_behavior,
    node_condition_choice_source,
    node_gradient_choice_source,
    node_math_choice_source,
    node_vector_math_choice_source,
    source_choice_map,
)

root = Path(sys.argv[1])
matrix = list(csv.DictReader(open(sys.argv[2], encoding="utf-8")))
out = Path(sys.argv[3])

FUNCTION = re.compile(r"^\s*function\s+(Node_[A-Za-z0-9_]+)\s*\(([^)]*)\)\s*(?::\s*([A-Za-z0-9_]+)\s*\()?", re.M)
INPUT = re.compile(r"newInput\(\s*([^,]+?)\s*,\s*nodeValue_([A-Za-z0-9_]+)\s*\(")
GENERIC_INPUT = re.compile(r"newInput\(\s*([^,]+?)\s*,\s*nodeValue\s*\(")
OUTPUT = re.compile(r"newOutput\(\s*([^,]+?)\s*,\s*nodeValue_Output\s*\(")
SURFACE_OUTPUT = re.compile(r"newOutput\(\s*([^,]+?)\s*,\s*nodeValue_Surface\s*\(")
ACTIVE = re.compile(r"newActiveInput\(\s*([^)]+)\)")
# Constructors whose name argument has a default.
DEFAULT_NAMES = {"Dimension": "Dimension", "Anchor": "Anchor", "Pbbox": "PBbox"}
NODE_ATTRIBUTE = re.compile(r"^\s*(?:self\.)?attributes\.([a-z_][a-z0-9_]*)\s*=\s*([^;\n]+)", re.M)
EDITOR_ATTRIBUTES = {
    "hovering", "focusing", "file_checker", "cache_use", "cache_data", "cache", "timeline_override", "show_preview",
    "layer_visible", "layer_selectable", "layer_order", "display_name", "select_object", "temp_path",
    "inherit_name", "inherit_type", "mapped", "interpolate", "oversample", "use_project_dimension",
}
UNIT_SIMPLE = re.compile(r"\.setUnitSimple\(([^)]*)")
CHAIN = re.compile(r"\.(setMappableConst|setMappableRange|setMappable|setCurvable|addShift)\s*\(")
INPUT_START = re.compile(r"newInput\(|newActiveInput\(|newOutput\(|__init_mask_modifier\(")
SEED_INPUT = re.compile(r"newInput\(\s*([^,]+?)\s*,\s*nodeValueSeed(Float|Int)?\s*\(")
MASK_MODIFIER = re.compile(r"__init_mask_modifier\(\s*([^,)]+)\s*,\s*([^,)]+)\s*\)")
DYNAMIC_ASSIGNED_INPUT = re.compile(
    r"\binputs\s*\[\s*([^\]]+?)\s*\]\s*=\s*(nodeValue(?:_([A-Za-z0-9_]+))?)\s*\("
)
CONSTRUCTOR_DEFAULT_OVERRIDE = re.compile(
    r"if\s*\(\s*!LOADING\s*&&\s*!APPENDING\s*\)\s*inputs\s*\[\s*(\d+)\s*\]\.setValue\s*\("
)
# Only this inherited default has reviewed source evidence in the current pin.
CONSTRUCTOR_DEFAULT_OVERRIDE_NODES = {"Node_3D_Light"}
# These mapped controls are numeric endpoint pairs in the pinned source wrapper.
MAPPED_RANGE_TYPE_OVERRIDES = {
    ("Node_Noise_Simplex", "Iteration"): "vector2",
    ("Node_Noise_Simplex", "Scale"): "vector2",
    ("Node_Gradient", "Angle"): "vector2",
    ("Node_Gradient", "Radius"): "vector2",
    ("Node_Gradient", "Shift"): "vector2",
    ("Node_Gradient", "Scale"): "vector2",

}
# Dynamic assignments have only been schema-checked for Struct's key/value pair.
DYNAMIC_ASSIGNED_INPUT_NODES = {"Node_Struct"}
SCALAR_DEPTH_SOURCE_FILES = (
    "scripts/node_value/node_value.gml",
    "scripts/node_value_float/node_value_float.gml",
    "scripts/__node_value_number/__node_value_number.gml",
)
GLOBAL_ARRAY = re.compile(r"(?:^|;)\s*(?:global\.)?([A-Z_][A-Z0-9_]*)\s*=\s*\[", re.M)
STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
MACRO = re.compile(r"^\s*#macro\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+)$", re.M)
ENUM = re.compile(r"\benum\s+([A-Z_][A-Z0-9_]*)\s*\{([^}]*)\}")

files, bases, bodies = {}, {}, {}
arrays, choice_arrays, macros, enums, enum_values = {}, {}, {}, {}, {}
source_behavior_evidence = {}
source_constructor_evidence = {}


def strip_comments(text):
    """Blank line and block comments without shifting source offsets or editing strings."""
    output = list(text)
    state = "code"
    quote = ""
    index = 0
    while index < len(text):
        character = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if character in ('"', "'"):
                state = "string"
                quote = character
            elif character == "/" and following == "/":
                output[index] = output[index + 1] = " "
                state = "line_comment"
                index += 1
            elif character == "/" and following == "*":
                output[index] = output[index + 1] = " "
                state = "block_comment"
                index += 1
        elif state == "string":
            if character == "\\":
                index += 1
            elif character == quote:
                state = "code"
        elif state == "line_comment":
            if character in "\r\n":
                state = "code"
            else:
                output[index] = " "
        elif state == "block_comment":
            if character == "*" and following == "/":
                output[index] = output[index + 1] = " "
                state = "code"
                index += 1
            elif character not in "\r\n":
                output[index] = " "
        index += 1
    return "".join(output)


def record_constructor_source(name):
    """Keep hashes for source files that define recovered constructor metadata."""
    record_constructor_source_file(files[name])


def record_constructor_source_file(path):
    """Keep a hash for a pinned source file used to derive constructor metadata."""
    content = (root / path).read_bytes()
    source_constructor_evidence[path] = {
        "bytes": len(content),
        "sha256": hashlib.sha256(content).hexdigest(),
    }


def unquote(text):
    """Remove one matching string delimiter pair without trimming escaped quotes."""
    text = text.strip()
    if len(text) >= 2 and text[0] in ('"', "'") and text[-1] == text[0]:
        return text[1:-1]
    return text


def call_args(text, start):
    """Return the top-level argument strings of the call whose '(' is at `start`."""
    depth, args, current, quote, escaped = 0, [], [], None, False
    for index in range(start, len(text)):
        character = text[index]
        if quote:
            current.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
        elif character in "\"'":
            quote = character
            current.append(character)
        elif character in "([{":
            depth += 1
            if depth > 1:
                current.append(character)
        elif character in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(current).strip())
                return args
            current.append(character)
        elif character == "," and depth == 1:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(character)
    return args


def vector2_array_default(kind, raw):
    """Recognize a literal list of two-number rows passed to a Vec2 input."""
    if kind not in {"Vec2", "IVec2", "Vector", "Vec2Arr"} or not raw.strip().startswith("["):
        return False
    rows = [row for row in call_args(raw.strip(), 0) if row]
    if not rows:
        return False
    number = re.compile(r"-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$")
    for row in rows:
        if not row.startswith("["):
            return False
        coordinates = call_args(row, 0)
        if len(coordinates) != 2 or any(not number.fullmatch(value.strip()) for value in coordinates):
            return False
    return True


def wave_table_attribute(name, body):
    """Recover the pinned WaveTable's authored enum array after checking its editor mutation."""
    if name != "Node_Fn_WaveTable":
        return None
    members = enum_values.get("WAVETABLE_FN", {})
    expected = {"sine", "square", "tri", "saw"}
    if set(members) != expected or sorted(members.values()) != [0, 1, 2, 3]:
        raise ValueError("WaveTable enum source changed; review attribute_wavetable metadata")
    assignments = list(re.finditer(r"\battributes\.wavetable\s*=\s*\[", body))
    if len(assignments) != 1:
        raise ValueError("WaveTable source must have one literal wavetable constructor default")
    opening = body.find("[", assignments[0].start())
    values = [value for value in call_args(body, opening) if value]
    if values != ["WAVETABLE_FN.sine", "WAVETABLE_FN.square", "WAVETABLE_FN.tri"]:
        raise ValueError("WaveTable constructor default is not the reviewed sine, square, tri sequence")
    operation = re.search(
        r"\bwavetable_apply\s*=\s*function\s*\(\s*typ\s*\)\s*\{([\s\S]*?)\n\s*\}", body
    )
    if operation is None or not re.search(
        r"attributes\.wavetable\s*\[\s*wavetable_selecting\s*\]\s*=\s*typ\b", operation.group(1)
    ):
        raise ValueError("WaveTable editor mutation no longer writes the selected wavetable attribute")
    menu_values = set(re.findall(r"wavetable_apply\s*\(\s*WAVETABLE_FN\.([A-Za-z_][A-Za-z0-9_]*)\s*\)", body))
    if menu_values != expected:
        raise ValueError("WaveTable editor menu no longer exposes every reviewed enum value")
    record_constructor_source(name)
    return {
        "index": "-1",
        "kind": "AttributeArray",
        "name": "attribute wavetable",
        "default": "[WAVETABLE_FN.sine,WAVETABLE_FN.square,WAVETABLE_FN.tri]",
        "extra": [],
        "attribute": "wavetable",
        "array_depth": 1,
        "effective_type": "array",
        "array_element_type": "integer",
        "array_allowed_values": [members[member] for member in sorted(expected, key=members.get)],
    }


def _matching_call_end(text, opening):
    if opening < 0:
        return None
    depth, quote, escaped = 0, None, False
    for index in range(opening, len(text)):
        character = text[index]
        if quote:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
        elif character in "\"'":
            quote = character
        elif character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
            if depth == 0:
                return index + 1
    return None


def _enum_chain_span(text, match):
    opening = text.find("(", match.start())
    end = _matching_call_end(text, opening)
    if end is None:
        return None
    semicolon = text.find(";", end)
    if semicolon < 0:
        semicolon = len(text)
    return end, semicolon


for path in sorted(root.glob("scripts/**/*.gml")):
    text = strip_comments(path.read_text(errors="replace"))
    for match in MACRO.finditer(text):
        macros.setdefault(match.group(1), match.group(2).strip())
    for match in ENUM.finditer(text):
        name = match.group(1)
        if name not in enums:
            members, values = parse_enum_members(match.group(2), name)
            enums[name] = members
            enum_values[name] = values
    for match in GLOBAL_ARRAY.finditer(text):
        args = call_args(text, match.end() - 1)
        arrays.setdefault(match.group(1), args)
        array_end = _matching_call_end(text, match.end() - 1)
        if array_end is not None:
            choice_arrays.setdefault(match.group(1), text[match.end() - 1:array_end])
    matches = list(FUNCTION.finditer(text))
    for index, match in enumerate(matches):
        name = match.group(1)
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        # Older copies of some nodes linger in unrelated folders; the folder named after the node wins.
        if name in files and path.parent.name.lower() != name.lower():
            continue
        files[name] = str(path.relative_to(root))
        bases[name] = match.group(3) or ""
        bodies[name] = text[match.start():end]

surface_attribute_file = root / "scripts/node_attributes/node_attributes.gml"
surface_preference_file = root / "scripts/preferences/preferences.gml"
depth_preference = None
if surface_attribute_file.exists() and surface_preference_file.exists():
    surface_attributes = strip_comments(surface_attribute_file.read_text())
    surface_preferences = strip_comments(surface_preference_file.read_text())
    if not re.search(r"_useInput\s*=\s*!array_empty\(inputs\)\s*&&\s*inputs\[0\]\.type\s*==\s*VALUE_TYPE\.surface", surface_attributes) or not re.search(r"attributes\.color_depth\s*=\s*_useInput\?\s*0\s*:\s*PREFERENCES\.node_def_depth", surface_attributes):
        raise ValueError("source Color Depth default semantics are unrepresented")
    if any(not re.search(r'scrollItem\("' + re.escape(label) + r'"\s*\)', surface_attributes) for label in DEPTH_LABELS):
        raise ValueError("source Color Depth choice labels are unrepresented")
    depth_preference = re.search(r"PREFERENCES\.node_def_depth\s*=\s*(\d+)\s*;", surface_preferences)
    if depth_preference is None or int(depth_preference.group(1)) > 8:
        raise ValueError("source Color Depth preference default is unrepresented")

source_index = SourceIndex(root, macros)
source_classification = SourceArrayClassification(root, macros)
source_choice_evidence = choice_source_evidence(root)
math_choice_labels, math_choice_evidence = node_math_choice_source(root)
vector_math_choice_labels, vector_math_choice_evidence = node_vector_math_choice_source(root)
condition_choice_labels, condition_choice_evidence = node_condition_choice_source(root)
gradient_choice_labels, gradient_choice_evidence = node_gradient_choice_source(root)
source_choice_generated_evidence = {}
generated_choice_arrays = {}
if source_choice_evidence is not None:
    for expression, labels, evidence in (
        ("global.node_math_scroll", math_choice_labels, math_choice_evidence),
        ("global.node_vmath_scroll", vector_math_choice_labels, vector_math_choice_evidence),
    ):
        if labels is not None and evidence is not None:
            source_choice_generated_evidence[expression] = evidence
            generated_choice_arrays[expression] = labels
    if condition_choice_labels is not None and condition_choice_evidence is not None:
        source_choice_generated_evidence["cond_array"] = condition_choice_evidence
        generated_choice_arrays["cond_array"] = condition_choice_labels

    if gradient_choice_labels is not None and gradient_choice_evidence is not None:
        source_choice_generated_evidence["__gradTypes"] = gradient_choice_evidence
        generated_choice_arrays["__gradTypes"] = gradient_choice_labels


def array_process_metadata(name):
    current, seen = name, set()
    while current and current not in seen:
        if current == "Node_Processor":
            body = bodies[current]
            default = re.search(r"attributes\.array_process\s*=\s*ARRAY_PROCESS\.([A-Za-z_][A-Za-z0-9_]*)", body)
            choices_match = re.search(
                r'Node_Attribute\("Array Process Type"[\s\S]*?scrollBox\(\s*\[([^\]]*)\]', body
            )
            if default is None or choices_match is None:
                raise ValueError("Node_Processor array process source metadata is incomplete")
            value = enum_values.get("ARRAY_PROCESS", {}).get(default.group(1))
            if value is None:
                raise ValueError(f"unresolved ARRAY_PROCESS.{default.group(1)}")
            return {"default": value, "choices": STRING.findall(choices_match.group(1))}
        seen.add(current)
        current = bases.get(current)
    return None


def choices(argument, body):
    """Resolve an enum input's choice labels from a literal, a local list or a global list."""
    identifier = re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", argument)
    if identifier:
        local = re.search(r"\b" + re.escape(argument) + r"\s*=\s*\[", body)
        if local:
            return [label for arg in call_args(body, local.end() - 1) for label in STRING.findall(arg)[:1]]
        if argument in arrays:
            return [label for arg in arrays[argument] for label in STRING.findall(arg)[:1]]
        return None
    labels = STRING.findall(argument)
    return labels or None


symbols_of = {}
dynamic_of = {}
CREATE_NEW_INPUT = re.compile(r"(?:static\s+)?createNewInput\s*=?\s*function\s*\(\s*([A-Za-z_]*)|function\s+createNewInput\s*\(\s*([A-Za-z_]*)")
DYNAMIC_INPUT = re.compile(r"setDynamicInput\(\s*([^,)]*)")
LENGTH_SYMBOL = re.compile(r"\b(?:var\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*array_length\(\s*inputs\s*\)")
ALIAS_SYMBOL = re.compile(r"\bvar\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;")


def resolve_index(expression, symbols):
    """Evaluate an input index expression using known integer symbols, or None."""
    expression = expression.strip()
    if expression.isdigit():
        return int(expression)
    text = re.sub(r"[A-Za-z_][A-Za-z0-9_]*", lambda m: str(symbols[m.group(0)]) if m.group(0) in symbols else "?", expression)
    if not re.fullmatch(r"[0-9+\-*() ]+", text):
        return None
    return int(eval(text))


def parse(name, seen):
    if name in seen or name not in bodies:
        return [], []
    seen.add(name)
    body = bodies[name]
    inputs, outputs = [], []
    inherited_inputs = []
    direct_unclamp_spans = []
    direct_choice_spans = []
    for candidate in INPUT.finditer(body):
        candidate_kind = candidate.group(2)
        span = _enum_chain_span(body, candidate)
        if span:
            chain_source = body[span[0]:span[1]]
            if candidate_kind in ("EScroll", "Enum_Scroll") and ".setUnclamp" in chain_source:
                direct_unclamp_spans.append(span)
            if candidate_kind in ("EScroll", "EButton", "Enum_Scroll", "Enum_Button") and ".setChoices" in chain_source:
                direct_choice_spans.append(span)
    base = bases.get(name, "")
    if base.startswith("Node_") and base in bodies:
        inherited_inputs, inherited_outputs = parse(base, seen)
        inputs += [dict(item, inherited=base) for item in inherited_inputs]
        outputs += [dict(item, inherited=base) for item in inherited_outputs]
    declared = []
    for match in ACTIVE.finditer(body):
        declared.append((match.start(), {"index": match.group(1).strip(), "kind": "Active", "name": "Active", "default": "true", "extra": [], "array_depth": 0}))
    for match in INPUT.finditer(body):
        args = call_args(body, match.end() - 1)
        entry = {
            "index": match.group(1).strip(),
            "kind": match.group(2),
            "name": unquote(args[0].lstrip("$")) if args and args[0] else DEFAULT_NAMES.get(match.group(2), ""),
            "default": args[1] if len(args) > 1 else "",
            "extra": args[2:],
        }
        if vector2_array_default(entry["kind"], entry["default"]):
            entry["effective_type"] = "array"
            entry["array_element_type"] = "vector2"
            record_constructor_source(name)
        if entry["kind"] in ("EScroll", "EButton", "Enum_Scroll", "Enum_Button") and len(args) > 2:
            entry["choices"] = choices(args[2], body)
        declared.append((match.start(), entry))
        # Chained helpers add hidden inputs: setMappable a "<name> Map" surface (plus a gradient's "Map Range"),
        # setMappableConst a surface, setMappableRange a range, setCurvable a "<name> Curve" with its toggle.
        following = INPUT_START.search(body, match.end())
        statement = body[match.end():following.start() if following else len(body)][:1200]
        span = _enum_chain_span(body, match)
        input_end, chain_end = span if span else (None, -1)
        chain = body[input_end:chain_end] if input_end is not None else ""
        declaration_end = min(chain_end, following.start()) if following and chain_end >= 0 else chain_end
        declaration = body[match.start():declaration_end] if declaration_end >= 0 else ""
        entry["_source_classification_state"] = source_classification.state(entry["kind"], declaration)
        entry["source_array_classification"] = _result(entry["_source_classification_state"])
        choice_calls = list(re.finditer(r"\.setChoices\s*\(", chain))
        choice_mutations = list(re.finditer(r"\.setChoices\s*\(", body))
        dynamic_choices = any(
            not any(start <= call.start() < end for start, end in direct_choice_spans)
            for call in choice_mutations
        )
        choice_expression = args[2].strip() if len(args) > 2 and args[2].strip() else None
        if choice_calls:
            choice_args = call_args(chain, choice_calls[-1].end() - 1) if len(choice_calls) == 1 else []
            if dynamic_choices or not choice_args or not re.fullmatch(r"(?:\s*\.\s*[A-Za-z_]\w*\s*\([^{};]*\)\s*)+", chain):
                choice_expression = None
                dynamic_choices = True
            else:
                choice_expression = choice_args[0]
        source_choices = None
        if entry["kind"] in ("EScroll", "EButton", "Enum_Scroll", "Enum_Button"):
            if choice_expression and not dynamic_choices:
                source_choices = source_choice_map(
                    choice_expression,
                    body,
                    choice_arrays,
                    array_map_verified=source_choice_evidence is not None,
                    scroll_item_verified=source_choice_evidence is not None,
                    separator_verified=source_choice_evidence is not None,
                    allowlisted_arrays=generated_choice_arrays,
                )
                if choice_expression in generated_choice_arrays and source_choices is not None:
                    entry["choices"] = [choice["label"] for choice in source_choices]
            entry["source_choices"] = {
                "status": "resolved" if source_choices is not None else "unknown",
                "entries": source_choices,
            }
        unclamp_calls = list(re.finditer(r"\.setUnclamp\s*\(", body))
        dynamic_unclamp = any(
            not any(start <= call.start() < end for start, end in direct_unclamp_spans)
            for call in unclamp_calls
        )
        dynamic_connectability = bool(re.search(r"\.isConnectable(?:Strict)?\s*=", body))
        resolved_choice_count = choice_count(choice_expression, body, choice_arrays) if choice_expression and not dynamic_choices else None
        if choice_expression in generated_choice_arrays and source_choices is not None:
            resolved_choice_count = len(source_choices)
        behavior, evidence = enum_behavior(
            root,
            entry["kind"],
            resolved_choice_count,
            chain,
            dynamic_unclamp,
            dynamic_connectability,
        )
        if behavior is not None:
            entry["source_behavior"] = behavior
            if evidence is not None:
                source_behavior_evidence[entry["kind"]] = evidence
        entry["array_depth"] = declaration_depth(source_index.type_array(entry["kind"]), statement)
        if ".setArrayDepth" in statement:
            record_constructor_source(name)
        unit = UNIT_SIMPLE.search(statement)
        if unit:
            entry["unit"] = "constant" if unit.group(1).strip().startswith("false") else "reference"
        for order, chain in enumerate(CHAIN.finditer(statement)):
            chain_args = call_args(statement, chain.end() - 1)
            if not chain_args or not chain_args[0]:
                continue
            helper, index, position = chain.group(1), chain_args[0], match.start() + 1 + order
            suffix = lambda at, fallback: chain_args[at].strip('"') if len(chain_args) > at and chain_args[at].startswith('"') else fallback
            if helper == "setMappable":
                entry["mapped"] = "range"
                mapped_range_type = MAPPED_RANGE_TYPE_OVERRIDES.get((name, entry["name"]))
                if mapped_range_type is not None:
                    entry["mapped_range_type"] = mapped_range_type
                    record_constructor_source(name)
                declared.append((position, {"index": index, "kind": "Surface", "name": f"{entry['name']} Map", "default": "", "extra": [], "map_of": entry["name"], "array_depth": 0}))
                if entry["kind"] == "Gradient":
                    declared.append((position + 0.5, {"index": f"({index})+1", "kind": "Vec4", "name": f"{entry['name']} Map Range", "default": "[0,0,1,0]", "extra": [], "map_of": entry["name"], "array_depth": 1}))
            elif helper == "setMappableConst":
                entry["mapped"] = "const"
                declared.append((position, {"index": index, "kind": "Surface", "name": f"{entry['name']} {suffix(1, 'Map')}", "default": "", "extra": [], "map_of": entry["name"], "array_depth": 0}))
            elif helper == "setMappableRange":
                entry["mapped"] = "const"
                declared.append((position, {"index": index, "kind": "Range", "name": f"{entry['name']} {suffix(1, 'Map')}", "default": "[0,0]", "extra": [], "map_of": entry["name"], "array_depth": 1}))
            elif helper == "addShift":
                declared.append((position, {"index": index, "kind": "Slider", "name": "Shift", "default": "0", "extra": ["[-1,1,.01]"], "shift_of": entry["name"], "array_depth": 0}))
            elif helper == "setCurvable":
                key = suffix(3, "curved")
                entry.setdefault("curves", []).append(key)
                default = chain_args[1] if len(chain_args) > 1 else "CURVE_DEF_11"
                declared.append((position, {"index": index, "kind": "Curve", "name": f"{entry['name']} {suffix(2, 'Curve')}", "default": default, "extra": [], "curve_of": entry["name"], "curve_key": key, "array_depth": 1}))
    for match in SEED_INPUT.finditer(body):
        args = call_args(body, match.end() - 1)
        label = "Seed"
        if match.group(2) and args and args[0].startswith('"'):
            label = args[0].strip('"')
        elif not match.group(2) and len(args) > 1 and args[1].startswith('"'):
            label = args[1].strip('"')
        integer = match.group(2) == "Int" or (args and args[0] == "VALUE_TYPE.integer")
        declared.append((match.start(), {"index": match.group(1).strip(), "kind": "SeedInt" if integer else "Seed", "name": label, "default": "", "extra": []}))
        declared[-1][1]["array_depth"] = 0
    for match in GENERIC_INPUT.finditer(body):
        args = call_args(body, match.end() - 1)
        declared.append((match.start(), {
            "index": match.group(1).strip(),
            "kind": args[3].replace("VALUE_TYPE.", "Generic_") if len(args) > 3 else "Generic",
            "name": unquote(args[0]) if args else "",
            "default": args[4] if len(args) > 4 else "",
            "extra": args[5:],
        }))
        following = INPUT_START.search(body, match.end())
        statement = body[match.end():following.start() if following else len(body)][:1200]
        declared[-1][1]["array_depth"] = declaration_depth(
            source_index.type_array(declared[-1][1]["kind"]), statement
        )
        if ".setArrayDepth" in statement:
            record_constructor_source(name)
        span = _enum_chain_span(body, match)
        end = min(span[1], following.start()) if span and following else (span[1] if span else match.end())
        entry = declared[-1][1]
        entry["_source_classification_state"] = source_classification.state(entry["kind"], body[match.start():end])
        entry["source_array_classification"] = _result(entry["_source_classification_state"])
    # __init_mask_modifier(mask, first) declares Invert mask and Mask feather at first and first + 1.
    for match in MASK_MODIFIER.finditer(body):
        if not match.group(2).isdigit():
            continue
        first = int(match.group(2))
        declared.append((match.start(), {"index": str(first), "kind": "Bool", "name": "Invert mask", "default": "false", "extra": [], "array_depth": 0}))
        declared.append((match.start() + 1, {"index": str(first + 1), "kind": "Slider", "name": "Mask feather", "default": "0", "extra": ["[0,32,.1]"], "array_depth": 0}))
    # Inputs declared inside createNewInput are the template of one dynamic group, not fixed inputs.
    template_span = None
    creator = CREATE_NEW_INPUT.search(body)
    if creator:
        brace = body.find("{", creator.end())
        depth, cursor = 0, brace
        while cursor < len(body):
            depth += {"{": 1, "}": -1}.get(body[cursor], 0)
            if depth == 0:
                break
            cursor += 1
        template_span = (creator.start(), cursor)
        assigned_inputs = [
            match for match in DYNAMIC_ASSIGNED_INPUT.finditer(body)
            if template_span[0] <= match.start() <= template_span[1]
        ] if name in DYNAMIC_ASSIGNED_INPUT_NODES else []
        for match in assigned_inputs:
            args = call_args(body, match.end() - 1)
            if not args:
                continue
            kind = match.group(3)
            if kind:
                input_name = unquote(args[0].lstrip("$"))
                default = args[1] if len(args) > 1 else ""
                extra = args[2:]
            else:
                input_name = unquote(args[0])
                kind = args[3].replace("VALUE_TYPE.", "Generic_") if len(args) > 3 else "Generic"
                default = args[4] if len(args) > 4 else ""
                extra = args[5:]
            statement_end = body.find("\n", match.start())
            statement = body[match.start():statement_end if statement_end >= 0 else len(body)]
            state = source_classification.state(kind, statement)
            entry = {
                "index": match.group(1).strip(),
                "kind": kind,
                "name": input_name,
                "default": default,
                "extra": extra,
                "array_depth": source_index.type_array(kind),
                "_source_classification_state": state,
                "source_array_classification": _result(state),
            }
            template_entry = (match.start(), entry)
            declared.append(template_entry)
        if assigned_inputs:
            record_constructor_source(name)
    # Resolve symbolic indices such as "i+4" where `x = array_length(inputs)` recorded the input count
    # and `var i = x;` aliased it. Symbols from the base constructor carry over.
    symbols = dict(symbols_of.get(base, {}))
    events = [(m.start(), "length", m.group(1)) for m in LENGTH_SYMBOL.finditer(body)]
    events += [(m.start(), "alias", (m.group(1), m.group(2))) for m in ALIAS_SYMBOL.finditer(body)]
    events += [
        (position, "input", entry)
        for position, entry in declared
        if not (template_span and template_span[0] <= position <= template_span[1])
    ]
    resolved = [int(item["index"]) for item in inputs if item["index"].isdigit()]
    for _, kind, data in sorted(events, key=lambda event: (event[0], event[1] != "input")):
        if kind == "length":
            symbols[data] = max(resolved, default=-1) + 1
        elif kind == "alias":
            if data[1] in symbols:
                symbols[data[0]] = symbols[data[1]]
        else:
            index = resolve_index(data["index"], symbols) if isinstance(data, dict) else None
            if index is not None:
                data["index"] = str(index)
                resolved.append(index)
    symbols_of[name] = symbols
    fixed, template = [], []
    for position, entry in sorted(declared, key=lambda item: item[0]):
        if template_span and template_span[0] <= position <= template_span[1]:
            local = creator.group(1) or creator.group(2) or "index"
            offset = resolve_index(entry["index"], {local: 0, "index": 0, "i": 0, "_index": 0})
            template.append(dict(entry, index=str(offset) if offset is not None else entry["index"]))
        else:
            fixed.append(entry)
    # Look At replaces the inherited physical slots through Node.newInput's inputs[i] assignment.
    # Other constructors retain their current extraction until their replacement semantics are audited.
    if name == "Node_Quarternion_Lookat":
        replaced = {item["index"] for item in fixed if item["index"].isdigit()}
        inputs = [item for item in inputs if item["index"] not in replaced]
        record_constructor_source(name)
        record_constructor_source_file("scripts/node_data/node_data.gml")
    inputs += fixed
    overrides = CONSTRUCTOR_DEFAULT_OVERRIDE.finditer(body) if name in CONSTRUCTOR_DEFAULT_OVERRIDE_NODES else ()
    for match in overrides:
        prefix = body[:match.start()]
        if prefix.count("{") - prefix.count("}") != 1:
            continue
        args = call_args(body, match.end() - 1)
        if not args:
            continue
        targets = [item for item in inputs if str(item.get("index")) == match.group(1)]
        if len(targets) != 1:
            continue
        targets[0]["default"] = args[0]
        targets[0]["constructor_default_override"] = {
            "source_node": name,
            "method": "setValue",
            "guard": "!LOADING && !APPENDING",
        }
        record_constructor_source(name)
    dynamic = DYNAMIC_INPUT.search(body)
    if dynamic:
        numbers = [int(item["index"]) for item in inputs if item["index"].isdigit()]
        length = resolve_index(dynamic.group(1), symbols) if dynamic.group(1).strip() else 1
        dynamic_of[name] = {"data_length": length, "fixed_length": max(numbers, default=-1) + 1, "template": template}
    elif base in dynamic_of:
        dynamic_of[name] = dynamic_of[base]
    depth = depth_attribute(name, bodies, bases, inherited_inputs, declared, source_index, int(depth_preference.group(1))) if depth_preference is not None else None
    if depth is not None:
        inputs = [item for item in inputs if item["name"] != "attribute color_depth"]
        inputs.append(depth)
    # Constructor-level literal attributes are saved in the node's "attri" record and change processing,
    # such as Outline's nine-direction filter. Editor-only keys are skipped.
    for match in NODE_ATTRIBUTE.finditer(body):
        # Only constructor top level counts; region blocks indent without opening a brace.
        prefix = body[:match.start()]
        if prefix.count("{") - prefix.count("}") != 1:
            continue
        key, raw = match.group(1), match.group(2).strip().rstrip(";").strip()
        if raw.startswith("["):
            opening = body.index("[", match.start(2))
            end = _matching_end(body, opening)
            if end is None:
                continue
            raw = " ".join(body[opening:end].split())
        if key in EDITOR_ATTRIBUTES or any(item["name"] == f"attribute {key}" for item in inputs):
            continue
        created = re.fullmatch(r"array_create\(\s*(\d+)\s*,\s*([^)]+)\)", raw)
        if created:
            raw = "[" + ",".join([created.group(2).strip()] * int(created.group(1))) + "]"
        # Nested numeric lists, such as Path's [[position, weight], ...], are kept flattened.
        if re.fullmatch(r"\[[-0-9., \[\]]*\]", raw) and "[" in raw[1:]:
            raw = "[" + ",".join(re.findall(r"-?[0-9.]+", raw)) + "]"
        if not re.fullmatch(r"(true|false|-?[0-9.]+|\[[-0-9., truefals]*\])", raw):
            continue
        kind = "Bool" if raw in ("true", "false") else ("AttributeArray" if raw.startswith("[") else "Float")
        inputs.append({"index": "-1", "kind": kind, "name": f"attribute {key}", "default": raw, "extra": [], "attribute": key,
                       "array_depth": 0 if kind in ("Bool", "Float", "Attribute") else None})
    custom_attribute = wave_table_attribute(name, body)
    if custom_attribute is not None:
        inputs.append(custom_attribute)
    # attribute_oversample and attribute_interpolation both add the two sampling attributes. Value 0
    # inherits the project attribute plus one; separators keep their slot so labels line up with values.
    sampling = re.search(r"attribute_(?:oversample|interpolation)\(([^)]*)\)", body)
    if sampling and not any(item["name"] == "Interpolate" for item in inputs):
        extended = "true" in sampling.group(1).split(",")[-1] if "," in sampling.group(1) else False
        interpolation = ["Inherited", "Pixel", "Bilinear", "Bicubic", "Lanczos3"] + (["", "CleanEdge"] if extended else [])
        oversample = ["Inherited", "Empty", "Black", "Clamp", "Repeat XY", "", "Repeat X Empty Y", "Repeat X Black Y",
                      "Repeat X Clamp Y", "", "Repeat Y Empty X", "Repeat Y Black X", "Repeat Y Clamp X"]
        inputs.append({"index": "-1", "kind": "Attribute", "name": "Interpolate", "default": "0", "extra": [], "choices": interpolation, "array_depth": 0})
        inputs.append({"index": "-1", "kind": "Attribute", "name": "Oversample", "default": "0", "extra": [], "choices": oversample, "array_depth": 0})
    for match in OUTPUT.finditer(body):
        args = call_args(body, match.end() - 1)
        outputs.append({
            "index": match.group(1).strip(),
            "name": unquote(args[0]) if args else "",
            "type": args[1] if len(args) > 1 else "",
            "default": args[2] if len(args) > 2 else "",
        })
    for match in SURFACE_OUTPUT.finditer(body):
        record_constructor_source(name)
        args = call_args(body, match.end() - 1)
        outputs.append({
            "index": match.group(1).strip(),
            "name": unquote(args[0]) if args else "",
            "type": "surface",
            "default": "",
        })
    for entry in inputs + template:
        if "_source_classification_state" not in entry:
            entry["_source_classification_state"] = source_classification.state(entry["kind"])
            entry["source_array_classification"] = _result(entry["_source_classification_state"])
    apply_input_classification_mutations(inputs, body)
    if apply_runtime_depth_mutations(inputs, body):
        record_constructor_source(name)
        for path in SCALAR_DEPTH_SOURCE_FILES:
            if (root / path).is_file():
                record_constructor_source_file(path)
    return inputs, outputs


commit = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
nodes = {}
for row in matrix:
    node = row["node_id"]
    if not node or node in nodes:
        continue
    if node not in bodies:
        nodes[node] = {"file": None, "base": None, "inputs": [], "outputs": []}
        continue
    inputs, outputs = parse(node, set())
    nodes[node] = {"file": files[node], "base": bases[node], "inputs": inputs, "outputs": outputs}
    processor = array_process_metadata(node)
    if processor is not None:
        nodes[node]["array_process"] = processor
    if node in dynamic_of:
        nodes[node]["dynamic"] = dynamic_of[node]

# Published pages can be missing while their official pinned declarations are available.
# Select only reviewed evidence rows; recognizing a source helper does not make it a product node.
if len(sys.argv) > 5:
    evidence = json.loads(Path(sys.argv[5]).read_text(encoding="utf-8"))
    for node, origin in verified_source_nodes(evidence, commit, root, files).items():
        if node not in nodes:
            inputs, outputs = parse(node, set())
            nodes[node] = {"file": files[node], "base": bases[node], "inputs": inputs, "outputs": outputs}
            processor = array_process_metadata(node)
            if processor is not None:
                nodes[node]["array_process"] = processor
            if node in dynamic_of:
                nodes[node]["dynamic"] = dynamic_of[node]
        nodes[node].update(origin)

if len(sys.argv) > 4:
    import struct
    import zlib

    for project in sorted(Path(sys.argv[4]).glob("*.pxc")):
        data = project.read_bytes()
        offset = struct.unpack("<I", data[4:8])[0]
        graph = json.loads(zlib.decompress(data[offset:]).rstrip(b"\0"))
        for record in graph["nodes"]:
            node = record["type"]
            if node in nodes or node not in bodies:
                continue
            inputs, outputs = parse(node, set())
            nodes[node] = {"file": files[node], "base": bases[node], "inputs": inputs, "outputs": outputs, "undocumented": True}
            processor = array_process_metadata(node)
            if processor is not None:
                nodes[node]["array_process"] = processor
            if node in dynamic_of:
                nodes[node]["dynamic"] = dynamic_of[node]

for node in nodes.values():
    for item in node["inputs"] + node.get("dynamic", {}).get("template", []):
        item.pop("_source_classification_state", None)

json.dump(
    {
        "source_commit": commit,
        "nodes": nodes,
        "macros": macros,
        "enums": enums,
        "enum_values": enum_values,
        "source_behavior_evidence": source_behavior_evidence,
        "source_constructor_evidence": source_constructor_evidence,
        "source_array_classification_evidence": source_classification.evidence,
        "source_choice_evidence": source_choice_evidence,
        "source_choice_generated_evidence": source_choice_generated_evidence,
    },
    open(out, "w", encoding="utf-8"),
    indent=1,
    sort_keys=True,
)
print(f"{len(nodes)} nodes from {len(matrix)} matrix rows at {commit}")
