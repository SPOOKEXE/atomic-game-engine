"""Extract constructor Color Depth attributes without assigning a foreign input slot."""
import re


DEPTH_LABELS = ["Input", "Inherited", "4 bit RGBA", "8 bit RGBA", "16 bit RGBA",
                "32 bit RGBA", "8 bit Greyscale", "16 bit Greyscale", "32 bit Greyscale"]


def surface_kind(kind, source_index):
    if kind.startswith("Generic_"):
        return kind == "Generic_surface"
    current, seen = "nodeValue_" + kind, set()
    while current and current not in seen:
        seen.add(current)
        if current in source_index.factories:
            current = source_index.factories[current][0]
        elif current in source_index.classes:
            parent, arguments, _ = source_index.classes[current]
            types = set(re.findall(r"\bVALUE_TYPE\.([A-Za-z_][A-Za-z0-9_]*)", arguments))
            if len(types) == 1:
                return next(iter(types)) == "surface"
            if types:
                return None
            current = parent
        else:
            return None
    return None


def depth_attribute(name, bodies, bases, inherited, declarations, source_index, preference):
    current, seen = name, set()
    while current and current not in seen and current != "Node_Processor":
        seen.add(current)
        current = bases.get(current)
    # Time Remap declares the same attribute explicitly on the plain source Node base.
    if current != "Node_Processor" and not (name == "Node_Time_Remap" and bases.get(name) == "Node"):
        return None
    body = bodies[name]
    body = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/',
                  lambda match: "".join("\n" if ch == "\n" else " " for ch in match.group()), body)
    calls = [match for match in re.finditer(r"\battribute_surface_depth\s*\(", body)
             if body[:match.start()].count("{") - body[:match.start()].count("}") == 1]
    if not calls:
        return None
    # A later constructor call recomputes the default from the junctions present at that point.
    call = calls[-1]
    inputs = [item for item in inherited if item["index"] != "-1"]
    inputs += [item for position, item in declarations if position < call.start()]
    zero = [item for item in inputs if item["index"] == "0"]
    unknown_index = any(not re.fullmatch(r"-?\d+", item["index"]) for item in inputs)
    is_surface = surface_kind(zero[-1]["kind"], source_index) if zero else False
    default = "" if unknown_index or is_surface is None else "0" if is_surface else str(preference)
    return {"index": "-1", "kind": "Attribute", "name": "attribute color_depth",
            "default": default, "extra": [], "attribute": "color_depth", "display_name": "Color Depth", "choices": DEPTH_LABELS,
            "array_depth": 0, "source_array_classification": False,
            "source_depth_input_enabled": is_surface if not unknown_index else None}
