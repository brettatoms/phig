#!/usr/bin/env python3
"""Load, check and render phig's rule records. Standard library only."""

import argparse
import dataclasses
import pathlib
import re
import sys
import tomllib

STATUSES = {"holds", "partial", "violated", "unenforced"}
# sanitizer has no members in the shipped set — 54 witness, 10 conformance,
# 6 subprocess, 0 sanitizer. It stays legal because rules/README.md defines it.
TEST_KINDS = {"witness", "conformance", "subprocess", "sanitizer"}
TIERS = {"engine", "boundary"}
SURFACES = {"abi", "cli", "http"}
NEEDS_OWNER = {"partial", "violated"}

REQUIRED = ("id", "tier", "concern", "statement", "rationale",
            "enforced_at", "protects", "tests", "test_kind", "status")


@dataclasses.dataclass
class Model:
    root: pathlib.Path
    rules: list
    operations: list
    concerns: list
    assets: list
    config: dict


def load(root):
    """Read every rules/*.toml under `root`. `root` is the repo root."""
    rules_dir = root / "rules"
    tax = tomllib.loads((rules_dir / "taxonomy.toml").read_text())
    config = tomllib.loads((rules_dir / "operations.toml").read_text())
    collected = []
    for path in sorted(rules_dir.glob("*.toml")):
        if path.name in ("taxonomy.toml", "operations.toml"):
            continue
        doc = tomllib.loads(path.read_text())
        for rule in doc.get("rule", []):
            rule["_file"] = path.name
            collected.append(rule)
    return Model(root=root, rules=collected, operations=config.get("operation", []),
                 concerns=tax["concerns"], assets=tax["asset"], config=config)


def check_schema(model):
    problems = []
    op_ids = {o["id"] for o in model.operations}
    asset_ids = {a["id"] for a in model.assets}
    seen = set()

    for rule in model.rules:
        rid = rule.get("id", "<no id>")
        for field in REQUIRED:
            if field not in rule:
                problems.append(f"{rid}: missing required field '{field}'")
        if rid in seen:
            problems.append(f"{rid}: duplicate rule id")
        seen.add(rid)

        if rule.get("tier") not in TIERS:
            problems.append(f"{rid}: tier must be one of {sorted(TIERS)}")
        if rule.get("concern") not in model.concerns:
            problems.append(f"{rid}: unknown concern '{rule.get('concern')}'")
        if rule.get("status") not in STATUSES:
            problems.append(f"{rid}: status must be one of {sorted(STATUSES)}")
        if rule.get("test_kind") not in TEST_KINDS:
            problems.append(f"{rid}: test_kind must be one of {sorted(TEST_KINDS)}")

        if rule.get("status") in NEEDS_OWNER and not rule.get("owner"):
            problems.append(f"{rid}: status '{rule.get('status')}' requires an owner")
        if rule.get("test_gap") and not rule.get("owner"):
            problems.append(f"{rid}: test_gap requires an owner")

        if rule.get("tier") == "engine":
            if "binds" not in rule:
                problems.append(f"{rid}: engine rule needs 'binds'")
            for op in rule.get("binds", []):
                if op not in op_ids:
                    problems.append(f"{rid}: binds unknown operation '{op}'")
            if "binds_surface" in rule:
                problems.append(f"{rid}: engine rule must not use 'binds_surface'")
        elif rule.get("tier") == "boundary":
            if "binds_surface" not in rule:
                problems.append(f"{rid}: boundary rule needs 'binds_surface'")
            for surface in rule.get("binds_surface", []):
                if surface not in SURFACES:
                    problems.append(f"{rid}: unknown surface '{surface}'")
            if "binds" in rule:
                problems.append(f"{rid}: boundary rule must not use 'binds'")

        for asset in rule.get("protects", []):
            if asset not in asset_ids:
                problems.append(f"{rid}: protects unknown asset '{asset}'")

    problems += _check_classification(model)
    return problems


def _check_classification(model):
    """operations.toml's own consistency: an id is declared once, and a name
    lands in exactly one of the three header buckets or on one allowlist.

    Every comparison here is on the whole name, never on the part after '::'.
    ScanEngine::run is core_internal while ThumbsEngine::run and
    FacesEngine::run are operation core_entry symbols; run is declared in three
    of the seven core_headers. A bare-name comparison merges them, which is how
    phase 1's Task 2 let ScanEngine::run hide behind ThumbsEngine::run.
    """
    problems = []
    config = model.config

    seen = set()
    for operation in model.operations:
        oid = operation.get("id", "<no id>")
        if oid in seen:
            problems.append(f"operation '{oid}': duplicate operation id")
        seen.add(oid)

    entries = {o["core_entry"].split(":", 1)[1]
               for o in model.operations if ":" in o.get("core_entry", "")}
    pairs = (
        (entries, set(config.get("core_internal", [])),
         "an operation's core_entry", "core_internal"),
        (entries, set(config.get("core_read_only", [])),
         "an operation's core_entry", "core_read_only"),
        (set(config.get("core_internal", [])), set(config.get("core_read_only", [])),
         "core_internal", "core_read_only"),
        ({s for o in model.operations for s in o.get("surfaces_abi", [])},
         set(config.get("read_only_abi", [])), "surfaces_abi", "read_only_abi"),
        ({s for o in model.operations for s in o.get("surfaces_cli", [])},
         set(config.get("read_only_cli", [])), "surfaces_cli", "read_only_cli"),
    )
    for left, right, left_name, right_name in pairs:
        for name in sorted(left & right):
            problems.append(
                f"'{name}' is in both {left_name} and {right_name} — "
                f"a name is classified once")
    return problems


CHECKS = [check_schema]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="run every check")
    parser.add_argument("--root", default=pathlib.Path(__file__).resolve().parent.parent)
    args = parser.parse_args(argv)

    model = load(pathlib.Path(args.root))
    problems = [p for check in CHECKS for p in check(model)]
    for problem in problems:
        print(f"rules: {problem}", file=sys.stderr)
    if problems:
        print(f"rules: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print(f"rules: {len(model.rules)} rules, {len(model.operations)} operations, OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
