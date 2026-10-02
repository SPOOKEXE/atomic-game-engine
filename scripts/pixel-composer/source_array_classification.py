"""Prove source typeArray declarations independently of processor array depth and payload shape."""

import hashlib
import re
from pathlib import Path

from array_depth import ARRAY_DISPLAYS, _block, _signature_end


UNKNOWN = (None, None)


def _code(source: str) -> str:
    """Blank comments and strings so quoted examples cannot become declaration evidence."""
    return re.sub(r"//[^\n]*|/\*[\s\S]*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
                  lambda match: "".join("\n" if c == "\n" else " " for c in match.group()), source)


def _arguments(text: str) -> list[str]:
    values, start, depth, quote = [], 0, 0, None
    for index, character in enumerate(text):
        if quote:
            if character == quote and (index == 0 or text[index - 1] != "\\"):
                quote = None
        elif character in "\"'":
            quote = character
        elif character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        elif character == "," and depth == 0:
            values.append(text[start:index].strip())
            start = index + 1
    values.append(text[start:].strip())
    return values


def _without_methods(body: str) -> str:
    # Nested function bodies are runtime methods, not constructor mutations.
    chars = list(body)
    for match in re.finditer(r"\bfunction\b", body):
        end = _signature_end(body, match.end())
        opening = body.find("{", end) if end is not None else -1
        if opening < 0:
            continue
        nested = _block(body, opening)
        if nested is None:
            continue
        for index in range(match.start(), opening + len(nested) + 2):
            if chars[index] != "\n":
                chars[index] = " "
    return "".join(chars)


def _literal(expression: str, family: str, environment: dict[str, str]) -> str | None:
    expression = expression.strip()
    for _ in range(12):
        if expression not in environment:
            break
        replacement = environment[expression].strip()
        if replacement == expression:
            break
        expression = replacement
    match = re.fullmatch(r"VALUE_" + family + r"\.([A-Za-z_]\w*)", expression)
    return match.group(1) if match else None


def _classification_value(expression: str, family: str, environment: dict[str, str]) -> bool | None:
    for _ in range(12):
        replacement = environment.get(expression.strip())
        if replacement is None or replacement == expression:
            break
        expression = replacement
    literal = _literal(expression, family, {})
    if literal is not None:
        return literal in ARRAY_DISPLAYS if family == "DISPLAY" else literal == "curve"
    branches = re.fullmatch(r"[^?:{};]+\?\s*(VALUE_" + family + r"\.\w+)\s*:\s*(VALUE_" + family + r"\.\w+)", expression.strip())
    if branches:
        left = _classification_value(branches.group(1), family, {})
        right = _classification_value(branches.group(2), family, {})
        return left if left == right else None
    return None


def _local_environment(body: str) -> dict[str, str]:
    assignments = {}
    for match in re.finditer(r"\b(?:var\s+)?(\w+)\s*=(?!=)\s*([^;\n]+)", body):
        assignments.setdefault(match.group(1), set()).add(match.group(2).strip())
    return {name: next(iter(values)) for name, values in assignments.items() if len(values) == 1}


def _result(state: tuple[bool | None, bool | None]) -> bool | None:
    curve, display = state
    if curve is True or display is True:
        return True
    return False if curve is False and display is False else None


def _effects(state, body: str, environment: dict[str, str], chained=False, factory=False):
    body = _code(body)
    pattern = r"(?:(\b\w+)\s*\.\s*|(?<![\w.])(?:\.\s*)?)set(Display|Type)\s*\(\s*([^,)]*)"
    aliases = {match.group(1) for match in re.finditer(r"\b(\w+)\s*=\s*(?:new\s+)?(?:__NodeValue_\w+|nodeValue\w*)\s*\(", body)} if factory else set()
    for match in re.finditer(pattern, body):
        if match.group(1) and match.group(1) not in aliases | {"self"}:
            continue
        if not chained and not factory and body[match.start():].startswith("."):
            continue
        curve, display = state
        family = "DISPLAY" if match.group(2) == "Display" else "TYPE"
        value = _classification_value(match.group(3), family, environment)
        # An unresolved branch may or may not run. Keep a classification only if both paths agree.
        prefix = body[:match.start()]
        conditional = not chained and (prefix.count("{") != prefix.count("}") or bool(re.search(r"\b(?:if|else|switch|for|while)\b[^;{}]*$", prefix)))
        if family == "DISPLAY":
            display = value if not conditional or display == value else None
        else:
            curve = value if not conditional or curve == value else None
        state = curve, display
    return state


class SourceArrayClassification:
    """Resolve constructor aliases and inherited display defaults, retaining unknown branches."""

    def __init__(self, root: Path, macros: dict[str, str]):
        self.macros = macros
        self.functions = {}
        self.evidence = {}
        for path in sorted(root.glob("scripts/**/*.gml")):
            source = _code(path.read_text(errors="replace"))
            for match in re.finditer(r"\bfunction\s+(nodeValue(?:_\w+|Seed(?:Float|Int)?)?|__NodeValue_\w+)\s*\(", source):
                end = _signature_end(source, match.start())
                opening = source.find("{", end) if end is not None else -1
                if opening < 0:
                    continue
                body = _block(source, opening)
                if body is None:
                    continue
                signature = source[match.end():end - 1]
                params = _arguments(signature)
                header = source[end:opening]
                parent = re.search(r":\s*(\w+)\s*\(", header)
                parent_call = header[parent.end():] if parent else ""
                if parent:
                    closing = _signature_end(header, parent.start())
                    parent_call = header[parent.end():closing - 1] if closing else ""
                self.functions[match.group(1)] = (params, parent.group(1) if parent else None, parent_call, _without_methods(body))
                self.evidence[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
        # These declarations control the classification, rather than the unrelated type_array cache.
        for relative in ("scripts/node_value_types/node_value_types.gml", "scripts/node_value/node_value.gml"):
            path = root / relative
            if path.is_file():
                self.evidence[relative] = hashlib.sha256(path.read_bytes()).hexdigest()

    def classification(self, kind: str, statement: str = "") -> bool | None:
        return _result(self.state(kind, statement))

    def state(self, kind: str, statement: str = ""):
        if kind.startswith("Generic_"):
            source_type = kind.removeprefix("Generic_")
            state = (source_type == "curve", source_type in {"curve", "d3vertex"})
        elif kind in {"Attribute", "AttributeArray"}:
            return UNKNOWN
        elif kind in {"Seed", "SeedInt"}:
            state = self._resolve("nodeValueSeed", ["VALUE_TYPE.integer" if kind == "SeedInt" else "VALUE_TYPE.float"], set())
        else:
            state = self._resolve("nodeValue_" + kind, [], set())
        return _effects(state, statement, {}, chained=True)

    def _resolve(self, name: str, args: list[str], seen: set[str]):
        if name in {"NodeValue", "nodeValue"}:
            typ = _literal(args[3], "TYPE", {}) if len(args) > 3 else None
            return (typ == "curve", typ in {"curve", "d3vertex"}) if typ else UNKNOWN
        if name in seen:
            return UNKNOWN
        seen = seen | {name}
        if name in self.macros:
            target = self.macros[name].strip()
            return self._resolve(target, args, seen) if re.fullmatch(r"nodeValue_\w+", target) else UNKNOWN
        row = self.functions.get(name)
        if row is None:
            return UNKNOWN
        params, parent, parent_call, body = row
        environment = {}
        for index, parameter in enumerate(params):
            pair = parameter.split("=", 1)
            environment[pair[0].strip()] = args[index] if index < len(args) else (pair[1].strip() if len(pair) == 2 else "?")
        substitute = lambda text: environment.get(text.strip(), text.strip())
        if parent:
            inherited_args = [substitute(arg) for arg in _arguments(parent_call)]
            if parent == "NodeValue":
                typ = _literal(inherited_args[3], "TYPE", environment) if len(inherited_args) > 3 else None
                state = (typ == "curve", typ in {"curve", "d3vertex"}) if typ else UNKNOWN
            else:
                state = self._resolve(parent, inherited_args, seen)
        else:
            candidates = []
            for call in re.finditer(r"(?:\bnew\s+)?(__NodeValue_\w+|nodeValue(?:_\w+|Seed(?:Float|Int)?)?|NodeValue)\s*\(", body):
                end = _signature_end(body, call.start())
                if end is None:
                    continue
                arguments = [substitute(arg) for arg in _arguments(body[call.end():end - 1])]
                candidates.append(self._resolve(call.group(1), arguments, seen))
            state = candidates[0] if candidates and all(item == candidates[0] for item in candidates) else UNKNOWN
        return _effects(state, body, environment, factory=parent is None)


def apply_input_classification_mutations(inputs: list[dict], body: str) -> None:
    """Resolve literal input setters; conditional or unresolved targets keep explicit unknowns."""
    body = _code(body)
    environment = _local_environment(body)
    events = []
    receiver = r"inputs\s*\[\s*([^\]]+)\s*\]"
    for match in re.finditer(receiver + r"\s*\.set(Display|Type)\s*\(\s*([^,)]*)", body):
        events.append((match.start(), match.group(1).strip(), match.group(2), match.group(3)))
    for match in re.finditer(receiver + r"\s*\.(display_type|type)\s*=\s*([^;\n]+)", body):
        events.append((match.start(), match.group(1).strip(), "Display" if match.group(2) == "display_type" else "Type", match.group(3)))
    bindings = list(re.finditer(r"\b(?:var\s+)?(\w+)\s*=(?!=)\s*([^;\n]+)", body))
    aliases = {}
    for match in bindings:
        if re.match(r"(?:newInput\s*\(|inputs\s*\[)", match.group(2)):
            aliases.setdefault(match.group(1), match.start())
    for binding in bindings:
        name = binding.group(1)
        if name not in aliases or binding.start() < aliases[name]:
            continue
        target = re.match(r"(?:newInput\s*\(\s*([^,]+)|inputs\s*\[\s*([^\]]+)\s*\])", binding.group(2))
        index = (target.group(1) or target.group(2)).strip() if target else "?"
        prefix = body[:binding.start()]
        if prefix.count("{") - prefix.count("}") != 1 or re.search(r"\b(?:if|else|switch|for|while)\b[^;{}]*$", prefix):
            index = "?"
        # A setter uses the current binding, never an earlier assignment of this alias.
        end = next((match.start() for match in bindings
                    if match.start() > binding.start() and match.group(1) == name), len(body))
        for mutation in re.finditer(r"\b" + re.escape(name) + r"\s*\.set(Display|Type)\s*\(\s*([^,)]*)", body[binding.end():end]):
            events.append((binding.end() + mutation.start(), index, mutation.group(1), mutation.group(2)))
    for position, index, setter, argument in sorted(events):
        targets = [item for item in inputs if str(item.get("index")) == index] if index.isdigit() else inputs
        prefix = body[:position]
        conditional = prefix.count("{") - prefix.count("}") != 1 or bool(re.search(r"\b(?:if|else|switch|for|while)\b[^;{}]*$", prefix))
        value = _classification_value(argument, "DISPLAY" if setter == "Display" else "TYPE", environment)
        for item in targets:
            curve, display = item.get("_source_classification_state", UNKNOWN)
            previous = display if setter == "Display" else curve
            resolved = value if not conditional or previous == value else None
            if not index.isdigit() and previous != value:
                resolved = None
            state = (curve, resolved) if setter == "Display" else (resolved, display)
            item["_source_classification_state"] = state
            item["source_array_classification"] = _result(state)
