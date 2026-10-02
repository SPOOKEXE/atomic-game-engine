"""Extract bounded enum input behavior from the pinned Pixel Composer source."""

import hashlib
import json
import re
from pathlib import Path


ENUM_SOURCES = {
    "EButton": "scripts/node_value_enum_button/node_value_enum_button.gml",
    "EScroll": "scripts/node_value_enum_scroll/node_value_enum_scroll.gml",
}
BASE_SOURCE = "scripts/node_value/node_value.gml"
TYPE_SOURCE = "scripts/node_value_types/node_value_types.gml"
SUGGESTION_SOURCE = "scripts/panel_graph/panel_graph.gml"
CHOICE_SOURCE = "scripts/scrollBox/scrollBox.gml"
MATH_CHOICE_SOURCE = "scripts/node_math/node_math.gml"
VECTOR_MATH_CHOICE_SOURCE = "scripts/node_vector_math/node_vector_math.gml"
CONDITION_CHOICE_SOURCE = "scripts/node_condition/node_condition.gml"
CONDITION_CHOICES = ["Equal", "Not equal", "Less ", "Less or equal ", "Greater ", "Greater or equal"]


def _matching_end(text: str, opening: int) -> int | None:
    depth = 0
    quote = None
    line_comment = False
    block_comment = False
    for index in range(opening, len(text)):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
        elif block_comment:
            if char == "*" and next_char == "/":
                block_comment = False
        elif quote:
            if char == quote and text[index - 1] != "\\":
                quote = None
        elif char == "/" and next_char == "/":
            line_comment = True
        elif char == "/" and next_char == "*":
            block_comment = True
        elif char in "\"'":
            quote = char
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
            if depth == 0:
                return index + 1
    return None


def _split_top_level(text: str) -> list[str]:
    text = _without_comments(text)
    parts, start, depth, quote = [], 0, 0, None
    index = 0
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if quote:
            if char == quote and text[index - 1] != "\\":
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "/" and next_char == "/":
            newline = text.find("\n", index + 2)
            index = len(text) if newline < 0 else newline
            continue
        elif char == "/" and next_char == "*":
            close = text.find("*/", index + 2)
            if close < 0:
                return []
            index = close + 2
            continue
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
            if depth < 0:
                return []
        elif char == "," and depth == 0:
            parts.append(text[start:index].strip())
            start = index + 1
        index += 1
    if quote or depth != 0:
        return []
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def _without_comments(text: str) -> str:
    result, index, quote = [], 0, None
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if quote:
            result.append(char)
            if char == quote and text[index - 1] != "\\":
                quote = None
            index += 1
        elif char in "\"'":
            quote = char
            result.append(char)
            index += 1
        elif char == "/" and next_char == "/":
            newline = text.find("\n", index + 2)
            result.append(" ")
            index = len(text) if newline < 0 else newline
        elif char == "/" and next_char == "*":
            close = text.find("*/", index + 2)
            if close < 0:
                return ""
            result.append(" ")
            index = close + 2
        else:
            result.append(char)
            index += 1
    return "".join(result)


def _array_count(expression: str, body: str, global_arrays: dict[str, str], seen: set[str]) -> int | None:
    expression = expression.strip()
    if expression.startswith("["):
        end = _matching_end(expression, 0)
        if end != len(expression):
            return None
        return len(_split_top_level(expression[1:-1]))

    call = re.match(r"([A-Za-z_]\w*)\s*\(", expression)
    if call:
        opening = expression.find("(", call.start())
        end = _matching_end(expression, opening)
        if end != len(expression):
            return None
        function = call.group(1)
        arguments = _split_top_level(expression[opening + 1:end - 1])
        if function not in {"__enum_array_gen", "array_create"}:
            return None
        if arguments:
            if function == "array_create" and arguments[0].isdigit():
                return int(arguments[0])
            return _array_count(arguments[0], body, global_arrays, seen)

    identifier = re.fullmatch(r"[A-Za-z_]\w*", expression)
    if not identifier or expression in seen:
        return None
    if expression in global_arrays:
        seen.add(expression)
        return _array_count(global_arrays[expression], body, global_arrays, seen)

    assignment = re.search(r"\b" + re.escape(expression) + r"\s*=\s*\[", body)
    if assignment is None or re.search(r"\b" + re.escape(expression) + r"\s*(?:\[[^]]*\]\s*)?=|array_push\(\s*" + re.escape(expression) + r"\b", body[assignment.end():]):
        return None
    end = _matching_end(body, assignment.end() - 1)
    if end is None:
        return None
    seen.add(expression)
    return _array_count(body[assignment.end() - 1:end], body, global_arrays, seen)


def choice_count(argument: str, body: str, global_arrays: dict[str, str]) -> int | None:
    """Resolve only literal or source-array choice lengths; names are never ordinals."""
    expression = argument.strip()
    if expression.startswith("{") and expression.endswith("}"):
        fields = _split_top_level(expression[1:-1])
        data = next((field.split(":", 1)[1].strip() for field in fields if ":" in field and field.split(":", 1)[0].strip() == "data"), None)
        if data is None:
            return None
        expression = data
    return _array_count(expression, body, global_arrays, set())


def _call_arguments(expression: str) -> tuple[str, list[str]] | None:
    match = re.match(r"(new\s+)?([A-Za-z_]\w*)\s*\(", expression.strip())
    if match is None:
        return None
    text = expression.strip()
    opening = text.find("(", match.start())
    end = _matching_end(text, opening)
    if end != len(text):
        return None
    name = f"new {match.group(2)}" if match.group(1) else match.group(2)
    return name, _split_top_level(text[opening + 1:end - 1])


def choice_source_evidence(root: Path) -> dict | None:
    """Verify ordered array mapping, ScrollItem names, and the -1 separator sentinel."""
    path = root / CHOICE_SOURCE
    source = path.read_text(encoding="utf-8")
    array_map = re.search(r"function\s+__enum_array_gen\([^)]*\)\s*\{", source)
    scroll_item = re.search(r"function\s+scrollItem\([^)]*\)\s+constructor\s*\{", source)
    scroll_box = re.search(r"function\s+scrollBox\([^)]*\)\s*:\s*widget\(\)\s+constructor\s*\{", source)
    if not (array_map and scroll_item and scroll_box):
        return None
    array_body_start = source.find("{", array_map.start())
    array_body_end = _matching_end(source, array_body_start)
    item_body_start = source.find("{", scroll_item.start())
    item_body_end = _matching_end(source, item_body_start)
    box_body_start = source.find("{", scroll_box.start())
    box_body_end = _matching_end(source, box_body_start)
    if not all((array_body_end, item_body_end, box_body_end)):
        return None
    array_body = source[array_body_start + 1:array_body_end - 1]
    item_body = source[item_body_start + 1:item_body_end - 1]
    box_body = source[box_body_start + 1:box_body_end - 1]
    ordered_mapping = bool(re.search(r"return\s+array_map\(arr,\s*function\(v,i\)[\s\S]*?return\s+new\s+scrollItem\(v,", array_body))
    scroll_name = bool(re.search(r"^\s*name\s*=\s*_name\s*;", item_body, re.M))
    separator = bool(re.search(r"until\(data_list\[ind\]\s*!=\s*-1\s*\|\|\s*ind\s*==\s*curr_val\)", box_body))
    if not (ordered_mapping and scroll_name and separator):
        return None
    return {"path": CHOICE_SOURCE, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def _generated_scroll_choice_source(
    root: Path,
    *,
    source_path: str,
    names_global: str,
    scroll_global: str,
    node_name: str,
    sprite_name: str,
) -> tuple[list[str] | None, dict | None]:
    """Resolve one allowlisted generated menu when its rows preserve literal name indexes."""
    path = root / source_path
    source = path.read_text(encoding="utf-8")
    code = _without_comments(source)
    names_assignments = list(re.finditer(re.escape(names_global) + r"\s*=\s*\[", code))
    scroll_assignments = list(re.finditer(re.escape(scroll_global) + r"\s*=(?!=)", code))
    if len(names_assignments) != 1 or len(scroll_assignments) != 1:
        return None, None

    array_open = code.find("[", names_assignments[0].start())
    array_end = _matching_end(code, array_open)
    if array_end is None:
        return None, None
    labels = []
    for expression in _split_top_level(code[array_open + 1:array_end - 1]):
        match = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', expression.strip())
        if match is None:
            return None, None
        try:
            labels.append(json.loads('"' + match.group(1) + '"'))
        except json.JSONDecodeError:
            return None, None

    scroll_mapping = re.compile(
        re.escape(scroll_global) + r"\s*=\s*array_create_ext\s*\(\s*"
        r"array_length\s*\(\s*" + re.escape(names_global) + r"\s*\)\s*,\s*"
        r"function\s*\(\s*i\s*\)\s*(?:/\*[\s\S]*?\*/\s*)?\{\s*"
        r"return\s+new\s+scrollItem\s*\(\s*" + re.escape(names_global) + r"\s*\[\s*i\s*\]\s*,\s*"
        + re.escape(sprite_name) + r"\s*,\s*i\s*\)\s*;?\s*\}\s*\)\s*;"
    )
    node_declarations = list(re.finditer(r"\bfunction\s+" + re.escape(node_name) + r"\s*\(", code))
    if len(node_declarations) != 1:
        return None, None
    node_open = code.find("{", node_declarations[0].end())
    node_end = _matching_end(code, node_open)
    if node_open < 0 or node_end is None:
        return None, None
    node_body = code[node_open + 1:node_end - 1]
    typed_input = re.compile(
        r"newInput\s*\(\s*0\s*,\s*nodeValue_EScroll\s*\(\s*\"Type\"\s*,\s*0\s*,\s*"
        + re.escape(scroll_global)
        + r"\s*\)\s*\)\s*\.\s*rejectArray\s*\(\s*\)\s*;"
    )
    if not labels or not scroll_mapping.search(code) or not typed_input.search(node_body):
        return None, None
    if re.search(re.escape(names_global) + r"\s*\[[^]]*\]\s*=", code):
        return None, None
    return labels, {
        "path": source_path,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "count": len(labels),
        "mapping": (
            f"{scroll_global.rsplit('.', 1)[-1]} copies "
            f"{names_global.rsplit('.', 1)[-1]}[i] in array_create_ext index order"
        ),
    }


def node_math_choice_source(root: Path) -> tuple[list[str] | None, dict | None]:
    return _generated_scroll_choice_source(
        root,
        source_path=MATH_CHOICE_SOURCE,
        names_global="global.node_math_names",
        scroll_global="global.node_math_scroll",
        node_name="Node_Math",
        sprite_name="s_node_math_operators",
    )


def node_vector_math_choice_source(root: Path) -> tuple[list[str] | None, dict | None]:
    return _generated_scroll_choice_source(
        root,
        source_path=VECTOR_MATH_CHOICE_SOURCE,
        names_global="global.node_vmath_names",
        scroll_global="global.node_vmath_scroll",
        node_name="Node_Vector_Math",
        sprite_name="s_node_vmath_operators",
    )


def node_condition_choice_source(root: Path) -> tuple[list[str] | None, dict | None]:
    """Verify Condition's authored labels and their six numeric comparator cases."""
    path = root / CONDITION_CHOICE_SOURCE
    if not path.is_file():
        return None, None
    source = path.read_text(encoding="utf-8")
    node = re.search(r"function\s+Node_Condition\([^)]*\)(?:\s*:[^{]+)?\s*\{", source)
    if node is None:
        return None, None
    opening = source.find("{", node.start())
    end = _matching_end(source, opening)
    if end is None:
        return None, None
    body = _without_comments(source[opening + 1:end - 1])
    labels = re.search(r'cond_array\s*=\s*__enum_array_gen\(\s*\[([^]]*)\]\s*,\s*s_node_condition_type\s*\)', body)
    declaration = re.search(
        r'newInput\(\s*1\s*,\s*nodeValue_EScroll\(\s*"Condition"\s*,\s*0\s*,\s*cond_array\s*\)\s*\)\s*\.\s*rejectArray\(\s*\)\s*;',
        body,
    )
    cases = re.search(
        r"switch\s*\(\s*_cond\s*\)\s*\{\s*case\s+0\s*:\s*res\s*=\s*_chck\s*==\s*_valu\s*;\s*break\s*;\s*"
        r"case\s+1\s*:\s*res\s*=\s*_chck\s*!=\s*_valu\s*;\s*break\s*;\s*"
        r"case\s+2\s*:\s*res\s*=\s*_chck\s*<\s*_valu\s*;\s*break\s*;\s*"
        r"case\s+3\s*:\s*res\s*=\s*_chck\s*<=\s*_valu\s*;\s*break\s*;\s*"
        r"case\s+4\s*:\s*res\s*=\s*_chck\s*>\s*_valu\s*;\s*break\s*;\s*"
        r"case\s+5\s*:\s*res\s*=\s*_chck\s*>=\s*_valu\s*;\s*break\s*;\s*\}",
        body,
    )
    if labels is None or declaration is None or cases is None:
        return None, None
    found = re.findall(r'"((?:[^"\\]|\\.)*)"', labels.group(1))
    if found != CONDITION_CHOICES:
        return None, None
    return found, {
        "path": CONDITION_CHOICE_SOURCE,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "count": 6,
        "mapping": "cond_array source order maps indices 0..5 to ==, !=, <, <=, >, >=",
    }


def _resolved_source_array(
    expression: str,
    body: str,
    global_arrays: dict[str, str],
    seen: set[str],
    array_map_verified: bool,
) -> list[str] | None:
    expression = expression.strip()
    if expression.startswith("{") and expression.endswith("}"):
        fields = _split_top_level(expression[1:-1])
        data = next((field.split(":", 1)[1].strip() for field in fields if ":" in field and field.split(":", 1)[0].strip() == "data"), None)
        return _resolved_source_array(data, body, global_arrays, seen, array_map_verified) if data is not None else None

    call = _call_arguments(expression)
    if call is not None:
        function, arguments = call
        if function == "__enum_array_gen" and array_map_verified and arguments:
            return _resolved_source_array(arguments[0], body, global_arrays, seen, array_map_verified)
        return None

    if expression.startswith("["):
        end = _matching_end(expression, 0)
        if end != len(expression):
            return None
        return _split_top_level(expression[1:-1])

    if re.fullmatch(r"[A-Za-z_]\w*", expression):
        if expression in seen:
            return None
        if expression in global_arrays:
            seen.add(expression)
            return _resolved_source_array(global_arrays[expression], body, global_arrays, seen, array_map_verified)
        assignment = re.search(r"\b" + re.escape(expression) + r"\s*=\s*\[", body)
        if assignment is None:
            return None
        end = _matching_end(body, assignment.end() - 1)
        if end is None:
            return None
        if re.search(r"\b" + re.escape(expression) + r"\s*(?:\[[^]]*\]\s*)?=|array_push\(\s*" + re.escape(expression) + r"\b", body[end:]):
            return None
        seen.add(expression)
        return _split_top_level(body[assignment.end():end - 1])
    return None


def source_choice_map(
    expression: str,
    body: str,
    global_arrays: dict[str, str],
    *,
    array_map_verified: bool,
    scroll_item_verified: bool,
    separator_verified: bool,
    allowlisted_arrays: dict[str, list[str]] | None = None,
) -> list[dict] | None:
    """Map exact labels to array positions, retaining verified separator slots."""
    expression = expression.strip()
    labels = (allowlisted_arrays or {}).get(expression)
    if labels is not None:
        if not array_map_verified or not all(isinstance(label, str) for label in labels):
            return None
        return [{"choice_index": index, "label": label} for index, label in enumerate(labels)]
    items = _resolved_source_array(expression, body, global_arrays, set(), array_map_verified)
    if items is None:
        return None
    mapped = []
    for index, item in enumerate(items):
        value = item.strip()
        label_match = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', value)
        if label_match:
            try:
                label = json.loads('"' + label_match.group(1) + '"')
            except json.JSONDecodeError:
                return None
            mapped.append({"choice_index": index, "label": label})
            continue
        if value == "-1" and separator_verified:
            mapped.append({"choice_index": index, "separator": -1})
            continue
        call = _call_arguments(value)
        if call is not None and call[0] == "new scrollItem":
            if not scroll_item_verified or not call[1]:
                return None
            name = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', call[1][0].strip())
            if name is None:
                return None
            try:
                label = json.loads('"' + name.group(1) + '"')
            except json.JSONDecodeError:
                return None
            mapped.append({"choice_index": index, "label": label})
            continue
        return None
    return mapped


def _constructor_behavior(root: Path, kind: str) -> tuple[dict, dict] | None:
    source_path = ENUM_SOURCES.get(kind)
    if source_path is None:
        return None
    path = root / source_path
    source = path.read_text(encoding="utf-8")
    class_name = "Enum_Button" if kind == "EButton" else "Enum_Scroll"
    macro = re.search(r"#macro\s+nodeValue_" + kind + r"\s+nodeValue_" + class_name + r"\b", source)
    constructor = re.search(
        r"function\s+__NodeValue_" + class_name + r"\b[\s\S]*?:\s*NodeValue\([^\n]*VALUE_TYPE\.integer[^\n]*\)\s*constructor\s*\{",
        source,
    )
    if macro is None or constructor is None:
        return None
    opening = source.find("{", constructor.start())
    end = _matching_end(source, opening)
    if end is None:
        return None
    body = source[opening + 1:end - 1]
    strict = bool(re.search(r"static\s+isConnectableStrict\s*=\s*function\([^)]*\)\s*(?:/\*[\s\S]*?\*/\s*)?\{\s*return\s+false\s*;?\s*\}", body))
    interpolation = bool(re.search(r"static\s+lerpAnimKeys\s*=\s*function[\s\S]*?return\s+lerp\s*\(", body))
    clamp = bool(re.search(r"if\s*\(is_real\(val\)\)\s*val\s*=\s*clamp\(val\s*,\s*0\s*,\s*choicesAmount\s*-\s*1\s*\)", body))
    scroll_default = kind != "EScroll" or bool(re.search(r"clamp_range\s*=\s*true\s*;", body))
    unclamp = kind != "EScroll" or bool(re.search(r"static\s+setUnclamp\s*=\s*function[\s\S]*?clamp_range\s*=\s*false", body))

    base_path = root / BASE_SOURCE
    base_source = base_path.read_text(encoding="utf-8")
    base_method = re.search(r"static\s+isConnectable\s*=\s*function\([^)]*\)\s*\{", base_source)
    if base_method is None:
        general_method = False
    else:
        method_open = base_source.find("{", base_method.start())
        method_end = _matching_end(base_source, method_open)
        method_body = base_source[method_open + 1:method_end - 1] if method_end else ""
        general_method = all(token in method_body for token in ("typeCompatible(", "searchNodeBackward(", "connect_type"))
    type_path = root / TYPE_SOURCE
    type_source = type_path.read_text(encoding="utf-8")
    type_compatibility = bool(re.search(r"function\s+typeCompatible\([^)]*\)\s*\{[\s\S]*?value_bit\(fromType\)[\s\S]*?value_type_directional\(fromType,\s*toType\)", type_source))
    suggestion_path = root / SUGGESTION_SOURCE
    suggestion_source = suggestion_path.read_text(encoding="utf-8")
    suggestion_callsite = bool(re.search(r"if\s*\(\s*!\s*\w+\.isConnectableStrict\(", suggestion_source))

    inherits_general_connectability = not re.search(r"static\s+isConnectable\s*=", body)
    if not (
        strict
        and interpolation
        and clamp
        and scroll_default
        and unclamp
        and general_method
        and inherits_general_connectability
        and type_compatibility
        and suggestion_callsite
    ):
        return None
    evidence = {
        "constructor": {"path": source_path, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()},
        "base_connectability": {"path": BASE_SOURCE, "sha256": hashlib.sha256(base_path.read_bytes()).hexdigest()},
        "type_compatibility": {"path": TYPE_SOURCE, "sha256": hashlib.sha256(type_path.read_bytes()).hexdigest()},
        "strict_suggestion_use": {"path": SUGGESTION_SOURCE, "sha256": hashlib.sha256(suggestion_path.read_bytes()).hexdigest()},
    }
    return ({
        "strict_suggestion": False,
        "actual_connectability": "general",
        "fractional_interpolation": True,
    }, evidence)


def enum_behavior(
    root: Path,
    kind: str,
    count: int | None,
    chain: str,
    dynamic_mutation: bool,
    dynamic_connectability: bool = False,
) -> tuple[dict | None, dict | None]:
    """Return source behavior; general means inherited checks, not guaranteed acceptance."""
    if kind not in ENUM_SOURCES:
        return None, None
    verified = _constructor_behavior(root, kind)
    if verified is None:
        return ({
            "strict_suggestion": None,
            "actual_connectability": "unknown",
            "fractional_interpolation": None,
            "choice_clamp": {"mode": "unknown", "choice_count": count},
        }, None)
    behavior, evidence = verified
    if dynamic_connectability:
        behavior["actual_connectability"] = "unknown"
    if kind == "EButton":
        mode = "always"
    elif dynamic_mutation:
        mode = "unknown"
    elif ".setUnclamp" not in chain:
        mode = "default"
    elif chain.count(".setUnclamp") == 1 and re.fullmatch(
        r"(?:\s*\.\s*[A-Za-z_]\w*\s*\([^{};]*\)\s*)+", chain
    ):
        mode = "disabled"
    else:
        mode = "unknown"
    behavior["choice_clamp"] = {"mode": mode, "choice_count": count}
    return behavior, evidence
