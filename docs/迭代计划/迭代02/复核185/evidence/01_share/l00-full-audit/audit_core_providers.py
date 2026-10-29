#!/usr/bin/env python3
"""Read-only inventory of duplicate wtp classes and final CoverageTests providers."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import shlex
import subprocess
from pathlib import Path


CORE_NAMES = ("WtCore", "WtUftCore", "WtBtCore", "WtDtCore")
SYMBOL_TYPES = set("ABCDGIRSTUVWabcdgirstuvw")
OWNER_RE = re.compile(r"\bwtp::([A-Za-z_][A-Za-z_0-9]*)(?:::|<|$)")
IMPORT_RE = re.compile(r"^\s*(?:0x[0-9A-Fa-f]+\s+)?(\S+)\s+\(from (.+)\)\s*$")


def run(args: list[str], *, cwd: Path, input_text: str | None = None) -> str:
    proc = subprocess.run(args, cwd=cwd, input=input_text, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {shlex.join(args)}\n{proc.stderr[-3000:]}"
        )
    return proc.stdout


def demangle_many(symbols: list[str], cwd: Path) -> dict[str, str]:
    if not symbols:
        return {}
    unique = list(dict.fromkeys(symbols))
    outputs: list[str] = []
    # Bound argv/stdin size and keep one-to-one output mapping.
    for start in range(0, len(unique), 4000):
        chunk = unique[start:start + 4000]
        outputs.extend(run(["xcrun", "llvm-cxxfilt"], cwd=cwd,
                           input_text="\n".join(chunk) + "\n").splitlines())
    if len(outputs) != len(unique):
        raise RuntimeError(f"cxxfilt cardinality mismatch: {len(unique)} in, {len(outputs)} out")
    return dict(zip(unique, outputs))


def owner_class(symbol: str) -> str | None:
    s = symbol.strip()
    for prefix in ("non-virtual thunk to ", "virtual thunk to ",
                   "covariant return thunk to ", "vtable for ",
                   "VTT for ", "typeinfo for ", "typeinfo name for ",
                   "construction vtable for "):
        if s.startswith(prefix):
            s = s[len(prefix):]
            break
    match = OWNER_RE.search(s)
    return match.group(1) if match else None


def parse_nm_record(line: str, *, prefixed: bool) -> tuple[str | None, str, str] | None:
    """Return (object_prefix, type, demangled_symbol) for an nm output line."""
    line = line.rstrip()
    prefix = None
    record = line
    if prefixed:
        if ": " not in line:
            return None
        prefix, record = line.rsplit(": ", 1)
    parts = record.strip().split(maxsplit=2)
    if len(parts) >= 2 and parts[0] in SYMBOL_TYPES:
        kind, symbol = parts[0], " ".join(parts[1:])
    elif len(parts) >= 3 and parts[1] in SYMBOL_TYPES:
        kind, symbol = parts[1], parts[2]
    else:
        return None
    return prefix, kind, symbol


def source_family(path_text: str) -> str | None:
    normalized = path_text.replace("\\", "/")
    for name in CORE_NAMES:
        if f"/{name}/" in normalized or f"{name}.cpp.o" in normalized:
            return name
    return None


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def link_command(target: str, ninja: Path, build_dir: Path) -> str | None:
    output = run([str(ninja), "-C", str(build_dir), "-t", "commands", target], cwd=build_dir.parent)
    lines = [line for line in output.splitlines() if line.strip()]
    # The final link command has the target output path and an -o option.
    needle = f"/bin/TestUnits/{target}"
    for line in reversed(lines):
        if needle in line and re.search(r"\s-o\s", line):
            return line
    return None


def parse_link_inputs(line: str | None, ninja_build: Path) -> tuple[list[str], list[str]]:
    if not line:
        return [], []
    try:
        tokens = shlex.split(line)
    except ValueError:
        tokens = line.split()
    core_order: list[str] = []
    object_paths: list[str] = []
    core_re = re.compile(r"lib(WtCore|WtUftCore|WtBtCore|WtDtCore)\.a$")
    for token in tokens:
        match = core_re.search(token)
        if match:
            core_order.append(match.group(1))
        elif token in {f"-l{name}" for name in CORE_NAMES}:
            core_order.append(token[2:])
        if token.endswith(".o"):
            candidate = Path(token)
            if not candidate.is_absolute():
                candidate = ninja_build / candidate
            if candidate.is_file():
                object_paths.append(str(candidate.resolve()))
    return core_order, list(dict.fromkeys(object_paths))


def parse_imports(text: str) -> list[tuple[str, str]]:
    result = []
    for line in text.splitlines():
        match = IMPORT_RE.match(line)
        if match:
            result.append((match.group(1), match.group(2)))
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="CMake/Ninja build directory, defaults to build-cov")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()

    repo = args.repo.resolve()
    build_dir = (args.build_dir or (repo / "build-cov")).resolve()
    output_dir = args.output_dir.resolve()
    ninja = Path("/opt/homebrew/bin/ninja")
    bin_dir = build_dir / "build_x64/Debug/bin/TestUnits"
    lib_dir = build_dir / "build_x64/Debug/libs"
    if not ninja.is_file():
        raise SystemExit(f"ninja not found: {ninja}")
    if not bin_dir.is_dir():
        raise SystemExit(f"CoverageTests output directory not found: {bin_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)

    archive_data: dict[str, dict[str, set[str]]] = {}
    for core in CORE_NAMES:
        archive = lib_dir / f"lib{core}.a"
        if not archive.is_file():
            raise SystemExit(f"core archive missing: {archive}")
        symbols_by_class: dict[str, set[str]] = collections.defaultdict(set)
        text = run(["xcrun", "nm", "-A", "-g", "-C", str(archive)], cwd=repo)
        for line in text.splitlines():
            parsed = parse_nm_record(line, prefixed=True)
            if not parsed:
                continue
            prefix, kind, symbol = parsed
            owner = owner_class(symbol)
            if owner and kind.upper() != "U":
                symbols_by_class[owner].add(symbol)
        archive_data[core] = symbols_by_class

    class_archives: dict[str, list[str]] = collections.defaultdict(list)
    for core in CORE_NAMES:
        for class_name in archive_data[core]:
            class_archives[class_name].append(core)
    duplicate_classes = sorted(name for name, archives in class_archives.items()
                               if len(set(archives)) > 1)
    duplicate_class_set = set(duplicate_classes)

    exact_definitions: dict[str, list[str]] = collections.defaultdict(list)
    for core in CORE_NAMES:
        for class_name in duplicate_classes:
            for symbol in archive_data[core].get(class_name, set()):
                exact_definitions[symbol].append(core)
    exact_duplicate_symbols = {
        symbol: sorted(set(cores)) for symbol, cores in exact_definitions.items()
        if len(set(cores)) > 1
    }

    (output_dir / "archive-class-inventory.json").write_text(
        json.dumps({
            "core_archives": {
                core: {
                    "path": str(lib_dir / f"lib{core}.a"),
                    "class_count": len(archive_data[core]),
                    "classes": {
                        cls: sorted(archive_data[core][cls])
                        for cls in sorted(archive_data[core])
                    },
                } for core in CORE_NAMES
            },
            "duplicate_classes": [
                {
                    "class": cls,
                    "archives": [core for core in CORE_NAMES if cls in archive_data[core]],
                    "exact_symbol_overlaps": {
                        symbol: exact_duplicate_symbols[symbol]
                        for symbol in sorted(exact_duplicate_symbols)
                        if owner_class(symbol) == cls
                    },
                } for cls in duplicate_classes
            ],
            "exact_duplicate_symbol_count": len(exact_duplicate_symbols),
            "exact_duplicate_symbols": {
                symbol: exact_duplicate_symbols[symbol]
                for symbol in sorted(exact_duplicate_symbols)
            },
        }, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")

    binary_paths = sorted(p for p in bin_dir.glob("CoverageTests*") if p.is_file())
    target_specs = []
    unique_objects: set[str] = set()
    for binary in binary_paths:
        target = binary.name
        command = link_command(target, ninja, build_dir)
        core_order, objects = parse_link_inputs(command, build_dir)
        unique_objects.update(objects)
        target_specs.append({
            "target": target,
            "binary": binary,
            "link_command": command,
            "core_archive_order": core_order,
            "direct_objects": objects,
        })

    object_definitions: dict[str, dict[str, list[dict[str, str]]]] = collections.defaultdict(
        lambda: collections.defaultdict(list)
    )
    obj_list = sorted(unique_objects)
    # Keep each nm invocation below a conservative argv bound.
    for start in range(0, len(obj_list), 80):
        chunk = obj_list[start:start + 80]
        text = run(["xcrun", "nm", "-A", "-g", "-C", *chunk], cwd=repo)
        for line in text.splitlines():
            parsed = parse_nm_record(line, prefixed=True)
            if not parsed:
                continue
            prefix, kind, symbol = parsed
            owner = owner_class(symbol)
            if owner in duplicate_class_set and kind.upper() != "U":
                object_path = str(Path(prefix).resolve())
                object_definitions[object_path][symbol].append({
                    "type": kind,
                    "source_family_from_object_path": source_family(object_path) or "unknown",
                })

    targets = []
    provider_counts: collections.Counter[str] = collections.Counter()
    for spec in target_specs:
        binary: Path = spec["binary"]
        nm_text = run(["xcrun", "nm", "-g", "-C", str(binary)], cwd=repo)
        nm_by_class: dict[str, dict[str, list[str]]] = collections.defaultdict(
            lambda: {"definitions": [], "undefined": []}
        )
        for line in nm_text.splitlines():
            parsed = parse_nm_record(line, prefixed=False)
            if not parsed:
                continue
            _prefix, kind, symbol = parsed
            owner = owner_class(symbol)
            if owner in duplicate_class_set:
                key = "undefined" if kind.upper() == "U" else "definitions"
                nm_by_class[owner][key].append({"symbol": symbol, "type": kind})

        raw_imports = parse_imports(run(["dyld_info", "-imports", str(binary)], cwd=repo))
        demangled = demangle_many([raw for raw, _ in raw_imports], cwd=repo)
        imports_by_class: dict[str, list[dict[str, str]]] = collections.defaultdict(list)
        for raw, provider in raw_imports:
            symbol = demangled.get(raw, raw)
            owner = owner_class(symbol)
            if owner in duplicate_class_set:
                imports_by_class[owner].append({
                    "symbol": symbol,
                    "mangled_symbol": raw,
                    "provider": provider,
                })
                provider_counts[provider] += 1

        direct_by_class: dict[str, list[dict[str, str]]] = collections.defaultdict(list)
        for object_path in spec["direct_objects"]:
            for symbol, definitions in object_definitions.get(object_path, {}).items():
                owner = owner_class(symbol)
                if owner in duplicate_class_set:
                    for definition in definitions:
                        direct_by_class[owner].append({
                            "symbol": symbol,
                            "type": definition["type"],
                            "object": object_path,
                            "source_family_from_object_path": definition["source_family_from_object_path"],
                        })

        class_rows = []
        observed_classes = set(nm_by_class) | set(imports_by_class) | set(direct_by_class)
        for class_name in duplicate_classes:
            nm_row = nm_by_class.get(class_name, {"definitions": [], "undefined": []})
            class_rows.append({
                "class": class_name,
                "archive_candidates": [core for core in CORE_NAMES
                                       if class_name in archive_data[core]],
                "nm_definitions_in_final_image": sorted(nm_row["definitions"], key=lambda x: x["symbol"]),
                "nm_undefined": sorted(nm_row["undefined"], key=lambda x: x["symbol"]),
                "dyld_imports": sorted(imports_by_class.get(class_name, []),
                                        key=lambda x: (x["symbol"], x["provider"])),
                "definitions_in_direct_link_objects": sorted(
                    direct_by_class.get(class_name, []), key=lambda x: (x["symbol"], x["object"])),
                "observation": ("symbols_observed" if class_name in observed_classes
                                else "no_global_symbol_observed"),
            })
        targets.append({
            "target": spec["target"],
            "binary": str(binary),
            "binary_sha256": sha256(binary),
            "core_archive_link_order": spec["core_archive_order"],
            "link_command": spec["link_command"],
            "class_provider_matrix": class_rows,
        })

    matrix = {
        "scope": {
            "core_archives": list(CORE_NAMES),
            "coverage_test_target_count": len(targets),
            "duplicate_wtp_class_count": len(duplicate_classes),
            "exact_duplicate_symbol_count": len(exact_duplicate_symbols),
            "nm_scope": "external/global symbols (-g); T/W/V/etc are final-image definitions, U are undefined",
            "dyld_scope": "Mach-O import table; provider is the image name reported by dyld_info",
            "limits": [
                "A final-image definition is attributed to the executable image, but nm alone cannot prove which archive member supplied it.",
                "Direct object definitions are mapped by link-input object path; local archive origin remains a candidate set unless unique among inspected direct objects and four core archives.",
                "No global symbol observed does not prove the class is unused: inline, hidden/local, stripped, or dynamically loaded symbols may not appear here.",
                "This inventory does not execute targets or prove runtime call reachability; weak-def-coalesce and flat-namespace imports do not identify a unique family.",
                "Only the four named core archives and direct object inputs are inspected for static source candidates; other archives are not expanded by this script.",
            ],
        },
        "dyld_import_counts_by_reported_provider": dict(sorted(provider_counts.items())),
        "targets": targets,
    }
    (output_dir / "target-provider-matrix.json").write_text(
        json.dumps(matrix, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    lines = [
        "# BUG-L00-005 静态 provider 清单",
        "",
        "由 `audit_core_providers.py` 只读生成。脚本没有运行 GTest、没有构建或更改 CMake。",
        "",
        f"- CoverageTests 最终二进制：{len(targets)} 个。",
        f"- 四核心归档中重复定义的 `wtp` 类名：{len(duplicate_classes)} 个。",
        f"- 在至少两个核心归档中出现的完全相同 demangled 外部符号：{len(exact_duplicate_symbols)} 个。",
        "- `archive-class-inventory.json` 给出类名、每归档定义符号与精确符号重叠。",
        "- `target-provider-matrix.json` 给出每个最终二进制的核心静态归档链接顺序、全局定义/U 符号、dyld 实际 import image、直接 `.o` 输入中的定义来源候选及二进制 SHA-256。",
        "",
        "## 直接 dyld provider 汇总",
        "",
    ]
    for provider, count in sorted(provider_counts.items(), key=lambda item: (-item[1], item[0])):
        lines.append(f"- `{provider}`: {count} 个重复类方法/对象 import 项。")
    lines += [
        "",
        "## 解读边界",
        "",
        "`dyld_info` 指向 dylib 的 import 是当前 Mach-O import provider；`nm` 的本地 `T`/`W` 等仅证明符号落在最终 executable image，无法单靠 nm 判断静态归档成员。脚本因此保留链接顺序、精确符号候选与直接对象源路径，并把多候选本地定义标为不可归因；它不把归档重复自动判为错绑。弱合并、flat namespace、未观察到 global symbol、运行时 `dlopen`、以及全 37 类的运行期调用路径均不由该静态矩阵证明。",
        "",
    ]
    (output_dir / "summary.md").write_text("\n".join(lines), encoding="utf-8")

    print(json.dumps({
        "output_dir": str(output_dir),
        "coverage_test_targets": len(targets),
        "duplicate_wtp_classes": len(duplicate_classes),
        "exact_duplicate_symbols": len(exact_duplicate_symbols),
        "unique_direct_objects": len(unique_objects),
        "dyld_import_counts_by_provider": dict(sorted(provider_counts.items())),
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
