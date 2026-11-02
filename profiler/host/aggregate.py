from typing import Iterable, Iterator

from .samples import Sample

_BAD_RIP = -1


def _lookups(s: Sample, trap_return: int | None) -> Iterator[int]:
    yield s.rip
    precise = False
    for addr in s.frames:
        yield addr if precise else addr - 1
        precise = addr == trap_return


def build_report_data(samples: Iterable[Sample], symbols, meta: dict) -> dict:
    trap_return = symbols.address_of("trap_return")
    unique: dict[tuple, list] = {}
    for s in samples:
        key = (s.cpu,) if s.user else (s.cpu, s.truncated, s.rip, *s.frames)
        entry = unique.get(key)
        if entry is None:
            unique[key] = [s, 1]
        else:
            entry[1] += 1

    addrs: set[int] = set()
    plans: list[list[int | None] | None] = []
    for s, _ in unique.values():
        if s.user:
            plans.append(None)
            continue
        plan: list[int | None] = []
        for i, lookup in enumerate(_lookups(s, trap_return)):
            if symbols.is_code(lookup):
                addrs.add(lookup)
                plan.append(lookup)
            else:
                plan.append(_BAD_RIP if i == 0 else None)
        plans.append(plan)
    resolved = symbols.resolve(sorted(addrs))

    function_ids: dict[str, int] = {}
    functions: list[str] = []

    def fid(name: str) -> int:
        if name not in function_ids:
            function_ids[name] = len(functions)
            functions.append(name)
        return function_ids[name]

    stack_ids: dict[tuple[int, ...], int] = {}
    stacks: list[list[int]] = []
    counts: dict[tuple[int, int], int] = {}
    total = user = truncated = depth_zero = rejected = 0

    for (s, n), plan in zip(unique.values(), plans):
        total += n
        if plan is None:
            user += n
            leaf_first = ["[userspace]"]
        else:
            if not s.frames:
                depth_zero += n
            leaf_first = []
            for lookup in plan:
                if lookup is None:
                    rejected += n
                elif lookup == _BAD_RIP:
                    rejected += n
                    leaf_first.append(f"[unknown {s.rip:#x}]")
                else:
                    leaf_first.extend(f.function for f in resolved[lookup])
            if s.truncated:
                truncated += n
                leaf_first.append("[truncated]")
        key = tuple(fid(name) for name in reversed(leaf_first))
        if key not in stack_ids:
            stack_ids[key] = len(stacks)
            stacks.append(list(key))
        pair = (stack_ids[key], s.cpu)
        counts[pair] = counts.get(pair, 0) + n

    return {
        "meta": meta,
        "functions": functions,
        "stacks": stacks,
        "samples": [[stack, cpu, n] for (stack, cpu), n in counts.items()],
        "summary": {
            "samples": total,
            "dropped": meta.get("dropped", 0),
            "truncated": truncated,
            "user_samples": user,
            "user_share": user / total if total else 0,
            "frames_rejected": rejected,
            "kernel_depth_zero": depth_zero,
        },
    }
