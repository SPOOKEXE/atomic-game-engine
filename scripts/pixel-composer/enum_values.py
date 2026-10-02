"""Parse and resolve integer enum declarations from pinned GML source."""

import ast
import operator
import re


_BINARY_OPERATORS = {
    ast.LShift: operator.lshift,
}
_UNARY_OPERATORS = {ast.UAdd: operator.pos, ast.USub: operator.neg}


def _integer_expression(expression: str, previous: dict[str, int]) -> int:
    if len(expression) > 256:
        raise ValueError("enum expression exceeds 256 characters")
    try:
        tree = ast.parse(expression, mode="eval")
    except SyntaxError as error:
        raise ValueError(f"invalid enum expression {expression!r}") from error

    def evaluate(node: ast.AST) -> int:
        if isinstance(node, ast.Expression):
            return evaluate(node.body)
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.Name):
            if node.id not in previous:
                raise ValueError(f"unresolved enum name {node.id!r} in {expression!r}")
            return previous[node.id]
        if isinstance(node, ast.UnaryOp) and type(node.op) in _UNARY_OPERATORS:
            return _UNARY_OPERATORS[type(node.op)](evaluate(node.operand))
        if isinstance(node, ast.BinOp) and type(node.op) in _BINARY_OPERATORS:
            left = evaluate(node.left)
            right = evaluate(node.right)
            if not 0 <= right <= 63:
                raise ValueError(f"enum shift count outside 0..63 in {expression!r}")
            return _BINARY_OPERATORS[type(node.op)](left, right)
        raise ValueError(f"unsupported enum expression {expression!r}")

    return evaluate(tree)


def parse_enum_members(body: str, enum_name: str = "<enum>") -> tuple[list[str], dict[str, int]]:
    """Return labels and exact values, rejecting expressions that cannot be resolved safely."""
    uncommented = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.DOTALL)
    members: list[str] = []
    values: dict[str, int] = {}
    next_value = 0
    for item in uncommented.split(","):
        item = item.strip()
        if not item:
            continue
        member, separator, expression = item.partition("=")
        member = member.strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", member):
            raise ValueError(f"invalid member {member!r} in enum {enum_name}")
        if member in values:
            raise ValueError(f"duplicate member {member!r} in enum {enum_name}")
        if separator:
            try:
                next_value = _integer_expression(expression.strip(), values)
            except ValueError as error:
                raise ValueError(f"enum {enum_name}.{member}: {error}") from error
        values[member] = next_value
        members.append(member)
        next_value += 1
    return members, values


def enum_member_value(
    enum_name: str,
    member: str,
    labels: dict[str, list[str]],
    explicit_values: dict[str, dict[str, int]],
    *,
    allow_legacy: bool = False,
) -> int | None:
    """Use exact source values, with explicit opt-in for old ordered snapshots."""
    if enum_name in explicit_values:
        return explicit_values[enum_name].get(member)
    if not allow_legacy:
        return None
    enum_members = labels.get(enum_name)
    if enum_members is None or member not in enum_members:
        return None
    return enum_members.index(member)
