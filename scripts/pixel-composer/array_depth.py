"""Resolve Pixel Composer input array depth from source value constructors."""

import re
from pathlib import Path


ARRAY_DISPLAYS = frozenset({
    "range", "vector_range", "rotation_range", "rotation_random", "slider_range", "path_anchor",
    "gradient_range", "vector", "padding", "area", "puppet_control", "grid_anchor", "matrix",
    "transform", "boolean_grid", "corner", "number_array", "curve", "path_array", "palette",
    "text_array", "d3vertex", "d3quarternion",
})


class SourceIndex:
    def __init__(self, root: Path, macros: dict[str, str]) -> None:
        self.macros = macros
        self.factories: dict[str, tuple[str | None, int | None]] = {}
        self.classes: dict[str, tuple[str | None, str, str]] = {}
        for path in sorted(root.glob("scripts/**/*.gml")):
            source = path.read_text(errors="replace")
            self._read_functions(source)

    def _read_functions(self, source: str) -> None:
        for match in re.finditer(r"\bfunction\s+(nodeValue_[A-Za-z0-9_]+)\b", source):
            end = _signature_end(source, match.end())
            start = source.find("{", end) if end is not None else -1
            if start < 0:
                continue
            body = _block(source, start)
            if body is None:
                continue
            factory_name = match.group(1)
            value_class = re.search(r"\bnew\s+(__NodeValue_[A-Za-z0-9_]+)\s*\(", body)
            forwarded = re.search(r"\bnodeValue_([A-Za-z0-9_]+)\s*\(", body)
            direct_type = re.search(r"\bVALUE_TYPE\.([A-Za-z0-9_]+)", body)
            if value_class:
                self.factories[factory_name] = (value_class.group(1), None)
            elif forwarded:
                self.factories[factory_name] = ("nodeValue_" + forwarded.group(1), None)
            elif direct_type:
                self.factories[factory_name] = (None, int(direct_type.group(1) == "curve"))
        for match in re.finditer(r"\bfunction\s+(__NodeValue_[A-Za-z0-9_]+)\b", source):
            end = _signature_end(source, match.end())
            header_end = source.find("{", end) if end is not None else -1
            if header_end < 0:
                continue
            header = source[match.end():header_end]
            parent = re.search(r"\:\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(", header)
            body = _block(source, header_end)
            if body is None:
                continue
            type_depth = re.search(r"\btype_array\s*=\s*([^;\n]+)", body)
            display = re.search(r"\bsetDisplay\s*\(\s*VALUE_DISPLAY\.([A-Za-z_][A-Za-z0-9_]*)", body)
            parent_args = header if parent else ""
            own_body = body.split("\n\tstatic ", 1)[0]
            self.classes[match.group(1)] = (
                parent.group(1) if parent else None,
                parent_args,
                own_body,
            )

    def type_array(self, kind: str) -> int | None:
        return self._type_array(kind, set())

    def _type_array(self, kind: str, seen_factories: set[str]) -> int | None:
        if kind in {"Active", "Attribute", "Seed", "SeedInt"}:
            return 0
        if kind == "AttributeArray" or kind == "Generic":
            return None
        if kind.startswith("Generic_"):
            return int(kind == "Generic_curve")
        factory = "nodeValue_" + kind
        while factory in self.macros and factory not in seen_factories:
            seen_factories.add(factory)
            replacement = self.macros[factory].strip()
            if not re.fullmatch(r"nodeValue_[A-Za-z0-9_]+", replacement):
                return None
            factory = replacement
        if factory in seen_factories:
            return None
        seen_factories.add(factory)
        factory_row = self.factories.get(factory)
        if factory_row is None:
            return None
        class_name, direct_depth = factory_row
        if direct_depth is not None:
            return direct_depth
        if class_name is None:
            return None
        if class_name.startswith("nodeValue_"):
            return self._type_array(class_name.removeprefix("nodeValue_"), seen_factories)
        return self._class_depth(class_name, set())

    def _class_depth(self, class_name: str, seen: set[str]) -> int | None:
        if class_name in seen:
            return None
        seen.add(class_name)
        row = self.classes.get(class_name)
        if row is None:
            return None
        parent, parent_args, body = row
        assignment = re.search(r"\btype_array\s*=\s*([^;\n]+)", body)
        depth = self._parent_depth(parent, parent_args, seen)
        if assignment:
            raw = assignment.group(1).strip()
            if not raw.isdigit():
                return None
            depth = int(raw)
        display = re.search(r"\bsetDisplay\s*\(\s*VALUE_DISPLAY\.([A-Za-z_][A-Za-z0-9_]*)", body)
        if display:
            depth = int(display.group(1) in ARRAY_DISPLAYS)
        return depth

    def _parent_depth(self, parent: str | None, args: str, seen: set[str]) -> int | None:
        if parent is None:
            return None
        if parent == "NodeValue":
            return int(bool(re.search(r"\bVALUE_TYPE\.curve\b", args)))
        return self._class_depth(parent, seen)


def declaration_depth(base_depth: int | None, statement: str) -> int | None:
    """Resolve constructor plus literal setArrayDepth; unknown expressions stay unknown."""
    overrides = re.findall(r"\.setArrayDepth\s*\(([^)]*)\)", statement)
    if "setArrayDepth" in statement and not overrides:
        return None
    if len(overrides) > 1:
        return None
    array_depth = 0
    if overrides:
        expression = overrides[0].strip()
        if not re.fullmatch(r"\d+", expression):
            return None
        array_depth = int(expression)
    display_calls = re.findall(r"\.setDisplay\s*\(\s*([^,)]*)", statement)
    if len(display_calls) > 1:
        return None
    if display_calls:
        display = re.fullmatch(r"VALUE_DISPLAY\.([A-Za-z_][A-Za-z0-9_]*)", display_calls[0].strip())
        if display is None:
            return None
        base_depth = int(display.group(1) in ARRAY_DISPLAYS)
    if base_depth is None:
        return None
    return base_depth + array_depth


def apply_runtime_depth_mutations(inputs: list[dict], body: str) -> set[str]:
    """Resolve literal scalar display changes and leave unproven mutations unknown."""
    patterns = (
        re.compile(r"inputs\s*\[\s*([^\]]+)\s*\]\.setArrayDepth\s*\(([^)]*)\)"),
        re.compile(r"inputs\s*\[\s*([^\]]+)\s*\]\.array_depth\s*=\s*([^;\n]+)"),
    )
    for pattern in patterns:
        for match in pattern.finditer(body):
            index = match.group(1).strip()
            if index.isdigit():
                targets = [item for item in inputs if str(item.get("index", "")) == index]
            else:
                targets = inputs
            for item in targets:
                item["array_depth"] = None

    resolved_displays: set[str] = set()
    display_pattern = re.compile(r"inputs\s*\[\s*([^\]]+)\s*\]\.setDisplay\s*\(([^)]*)\)")
    for match in display_pattern.finditer(body):
        index = match.group(1).strip()
        expression = match.group(2).strip()
        if index.isdigit():
            targets = [item for item in inputs if str(item.get("index", "")) == index]
        else:
            targets = inputs
        if index.isdigit() and _known_scalar_display_mutation(targets, expression):
            resolved_displays.add(index)
            continue
        for item in targets:
            item["array_depth"] = None
    return resolved_displays


def _known_scalar_display_mutation(inputs: list[dict], expression: str) -> bool:
    """Preserve numeric scalar depth when source only switches between scalar displays."""
    if not inputs or any(item.get("kind") not in {"Slider", "Float"} for item in inputs):
        return False
    return re.fullmatch(r"VALUE_DISPLAY\.(?:slider|_default)(?:\s*,[\s\S]*)?", expression) is not None


def _block(source: str, opening: int) -> str | None:
    depth = 0
    quote = None
    line_comment = False
    block_comment = False
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
        elif block_comment:
            if char == "*" and next_char == "/":
                block_comment = False
                index += 1
        elif quote:
            if char == quote and (index == 0 or source[index - 1] != "\\"):
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "/" and next_char == "/":
            line_comment = True
            index += 1
        elif char == "/" and next_char == "*":
            block_comment = True
            index += 1
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
        index += 1
    return None


def _signature_end(source: str, start: int) -> int | None:
    opening = source.find("(", start)
    if opening < 0:
        return None
    depth = 0
    quote = None
    for index in range(opening, len(source)):
        char = source[index]
        if quote:
            if char == quote and (index == 0 or source[index - 1] != "\\"):
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index + 1
    return None
