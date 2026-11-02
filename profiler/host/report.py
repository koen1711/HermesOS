import json
import math
from pathlib import Path

_TEMPLATE = Path(__file__).with_name("report_template.html")
_MARKER = "/*__REPORT_DATA__*/"
_ESCAPES = {"<": "\\u003c", ">": "\\u003e", "&": "\\u0026", "/": "\\/", "\u2028": "\\u2028", "\u2029": "\\u2029"}


def _finite(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {k: _finite(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_finite(v) for v in value]
    return value


def _embed(data: dict) -> str:
    text = json.dumps(_finite(data), ensure_ascii=True, allow_nan=False, separators=(",", ":"))
    return "".join(_ESCAPES.get(c, c) for c in text)


def render_report(data: dict, out_path: str) -> None:
    html = _TEMPLATE.read_text(encoding="utf-8").replace(_MARKER, _embed(data))
    Path(out_path).write_text(html, encoding="utf-8")


def top_functions(data: dict, n: int = 20) -> list[tuple[str, int, int]]:
    names = data["functions"]
    stacks = data["stacks"]
    self_counts: dict[int, int] = {}
    total_counts: dict[int, int] = {}
    for stack_id, _cpu, count in data["samples"]:
        stack = stacks[stack_id]
        if not stack:
            continue
        self_counts[stack[-1]] = self_counts.get(stack[-1], 0) + count
        for fn in set(stack):
            total_counts[fn] = total_counts.get(fn, 0) + count
    rows = [(names[fn], self_counts.get(fn, 0), total) for fn, total in total_counts.items()]
    rows.sort(key=lambda r: (-r[1], -r[2], r[0]))
    return rows[:n]
