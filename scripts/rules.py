#!/usr/bin/env python3
"""Load, check and render phig's rule records. Standard library only."""

import argparse
import dataclasses
import pathlib
import re
import sys
import tomllib
import xml.etree.ElementTree as ET

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


ENFORCEMENT_ROOTS = ("core/", "libphig/")

# Comments and string/char literals, in an order where a string wins over a
# line comment even when the string contains "//" (e.g. a URL): re.sub tries
# alternatives left to right at each position, so the string alternative
# claims an opening quote before the "//" inside it is ever considered as a
# comment start. Matched spans are replaced with a space, not deleted, so a
# stripped span can't accidentally splice two identifiers into one.
_NOISE = re.compile(
    r"/\*.*?\*/"
    r'|//[^\n]*'
    r'|"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)


def _strip_noise(text):
    """Blank out C++ comments and string/char literals.

    A symbol named only in prose or a string must not satisfy an anchor —
    ScanEngine::run stripped to run matches the comment "run's detections"
    otherwise, exactly as if run still existed. This only knows C++ syntax;
    a file like libphig/CMakeLists.txt that uses '#' comments passes through
    unchanged, which is fine because its anchors are never hidden by '#'.
    """
    return _NOISE.sub(" ", text)


def _resolves(text, symbol):
    """True when `symbol` is declared or defined in `text`.

    Tries the qualified form first, then the bare member name. Both are matched
    on a word boundary: a substring test lets ScanEngine::run resolve against
    run_workers, so the anchor survives the symbol being deleted. Comments and
    string/char literals are blanked out first, so mentioning a symbol in
    prose cannot resolve it.
    """
    text = _strip_noise(text)
    for candidate in (symbol, symbol.split("::")[-1]):
        if re.search(r"\b" + re.escape(candidate) + r"\b", text):
            return True
    return False


def check_sources(model):
    """Check 1: every enforced_at, and every operation's core_entry, names a
    real file and a symbol found in it. Check 8, folded in: enforcement lives
    in core/ or libphig/ unless the rule is violated, in which case the
    location itself is what the rule records."""
    problems = []
    for rule in model.rules:
        rid = rule.get("id", "<no id>")
        misplacement_is_the_defect = rule.get("status") == "violated"
        for site in rule.get("enforced_at", []):
            if ":" not in site:
                problems.append(f"{rid}: enforced_at '{site}' is not 'path:symbol'")
                continue
            rel, symbol = site.split(":", 1)
            if not rel.startswith(ENFORCEMENT_ROOTS) and not misplacement_is_the_defect:
                problems.append(
                    f"{rid}: enforced_at '{rel}' is outside core/ and libphig/ — "
                    f"a rule enforced there is unreachable by a client, so it is "
                    f"status = \"violated\" with an owner, not status = \"holds\"")
                continue
            path = model.root / rel
            if not path.is_file():
                problems.append(f"{rid}: enforced_at file '{rel}' does not exist")
            elif not _resolves(path.read_text(errors="replace"), symbol):
                problems.append(f"{rid}: symbol '{symbol}' not found in '{rel}'")

    for operation in model.operations:
        oid = operation.get("id", "<no id>")
        entry = operation.get("core_entry", "")
        if ":" not in entry:
            problems.append(f"operation '{oid}': core_entry '{entry}' is not 'path:symbol'")
            continue
        rel, symbol = entry.split(":", 1)
        path = model.root / rel
        if not path.is_file():
            problems.append(f"operation '{oid}': core_entry file '{rel}' does not exist")
        elif not _resolves(path.read_text(errors="replace"), symbol):
            problems.append(f"operation '{oid}': core_entry symbol '{symbol}' "
                            f"not found in '{rel}'")
    return problems


OPEN_PLAN_STATUSES = {"designing", "planned", "in-progress"}
INDEX_ROW = re.compile(r"^\|\s*(\d{3})\s*\|[^|]*\|\s*([a-z-]+)\s*\|")


def plan_statuses(root):
    """Map plan number to status, read from plans/000-INDEX.md."""
    index = pathlib.Path(root) / "plans" / "000-INDEX.md"
    statuses = {}
    for line in index.read_text().splitlines():
        match = INDEX_ROW.match(line)
        if match:
            statuses[match.group(1)] = match.group(2)
    return statuses


def check_owners(model):
    """Check 3: a violated or partial rule names an open plan, and so does any
    test_gap (which is what the design numbered check 10)."""
    problems = []
    statuses = plan_statuses(model.root)
    for rule in model.rules:
        owner = rule.get("owner")
        if not owner:
            continue
        rid = rule.get("id", "<no id>")
        why = ("status " + rule.get("status", "?")
               if rule.get("status") in NEEDS_OWNER else "test_gap")
        status = statuses.get(owner)
        if status is None:
            problems.append(f"{rid}: owner '{owner}' ({why}) is not in plans/000-INDEX.md")
        elif status not in OPEN_PLAN_STATUSES:
            problems.append(
                f"{rid}: owner '{owner}' ({why}) is '{status}', not an open plan — "
                f"either the debt is paid and the rule should change, or it needs a new owner")
    return problems


class TestResultsError(Exception):
    """The gtest XML at a given path is not a usable test run.

    Raised instead of returning an empty dict, so a broken artifact — a wrong
    path, a build that produced no binary, an interrupted run — is reported as
    itself, distinctly from a citation that genuinely does not resolve. Fed to
    check_tests as an empty dict, either failure mode looks identical to every
    cited test having been renamed or deleted: measured, a well-formed but
    empty `<testsuites/>` against the real record produces 166 "is in no test
    run" reports, one per citation, none of them a real decay.
    """


def load_test_results(xml_path):
    """Map 'Suite.Name' to 'passed', 'failed', 'skipped' or 'disabled', from
    gtest XML.

    GTEST_SKIP() writes status="run" result="skipped" with a <skipped> child.
    A DISABLED_-prefixed test writes status="notrun" result="suppressed" with
    no <skipped> child, and never runs at all — measured against a probe
    binary built with the repo's gtest 1.17.0. The two are bucketed
    separately: a skip is a property of the machine and can start passing
    without any code change (a model file appearing, a second filesystem
    becoming available), but DISABLED_ is a permanent code-level decision, so
    a rule's witness sitting there gives that rule zero evidence forever, not
    just on this run.
    """
    try:
        root = ET.parse(xml_path).getroot()
    except OSError as exc:
        # ET.parse opens the path itself, so a wrong path, a directory, or a
        # permissions problem raises here — a different hierarchy from
        # ET.ParseError, and one that ET.parse does not itself distinguish
        # from a parse failure in its own message.
        raise TestResultsError(f"'{xml_path}' could not be read: {exc}") from exc
    except ET.ParseError as exc:
        raise TestResultsError(f"'{xml_path}' is not valid gtest XML: {exc}") from exc

    results = {}
    for suite in root.iter("testsuite"):
        for case in suite.iter("testcase"):
            name = f"{suite.get('name')}.{case.get('name')}"
            if case.get("status") == "notrun":
                results[name] = "disabled"
            elif case.get("result") == "skipped" or case.find("skipped") is not None:
                results[name] = "skipped"
            elif case.find("failure") is not None or case.find("error") is not None:
                results[name] = "failed"
            else:
                results[name] = "passed"

    if not results:
        raise TestResultsError(
            f"'{xml_path}' names no test case — the run that produced it "
            f"likely failed before writing results")
    return results


def check_tests(model, results):
    """Check 2: a 'holds' rule's tests exist and pass.

    Four deliberate limits, each settled from a measurement — see this task's
    preamble. A skipped test is neither a pass nor a failure. A DISABLED_ test
    is reported for a 'holds' rule, because unlike a skip it can never turn
    into evidence without a code change, and a rule quietly held up by one
    would mean 'holds' forever with nothing behind it. A violated or partial
    rule's tests are checked for existence only, because R08's and R40's pin
    the contrary behaviour on purpose. And a 'holds' rule may name no test at
    all when it carries a test_gap and an owner, which is what rules/README.md
    defines that shape to mean.
    """
    problems = []
    for rule in model.rules:
        rid = rule.get("id", "<no id>")
        names = rule.get("tests", [])

        for name in names:
            if name not in results:
                problems.append(f"{rid}: test '{name}' is in no test run — "
                                f"renamed, deleted, or never written")

        if rule.get("status") != "holds":
            continue
        if not names and not (rule.get("test_gap") and rule.get("owner")):
            problems.append(f"{rid}: status 'holds' but no test named, and no "
                            f"test_gap with an owner saying why")
        for name in names:
            if results.get(name) == "failed":
                problems.append(f"{rid}: test '{name}' failed")
            elif results.get(name) == "disabled":
                problems.append(f"{rid}: test '{name}' is DISABLED_ — it never "
                                f"runs, so it cannot pin a holds rule")
    return problems


ABI_HEADER = "libphig/include/phig/phig.h"
ABI_CMAKE = "libphig/CMakeLists.txt"
CLI_MAIN = "cli/main.cpp"
ABI_SYMBOL = re.compile(r"\b(phig_[a-z0-9_]+)\s*\(")
# [a-z] first, so `command == "--help"` and `command == "-h"` — the same branch
# as `help` — are not read as top-level commands needing classification.
CLI_COMMAND = re.compile(r'command == "([a-z][a-z0-9-]*)"')


def check_coverage(model):
    """Check 4: every phig_* symbol in phig.h and every CLI command is claimed
    by an operation's surfaces or is on an explicit allowlist."""
    problems = []

    claimed_abi = {s for o in model.operations for s in o.get("surfaces_abi", [])}
    allowed_abi = set(model.config.get("read_only_abi", []))
    header = model.root / ABI_HEADER
    if header.is_file():
        for symbol in sorted(set(ABI_SYMBOL.findall(header.read_text()))):
            if symbol not in claimed_abi and symbol not in allowed_abi:
                problems.append(
                    f"ABI symbol '{symbol}' is claimed by no operation and is not in "
                    f"read_only_abi — classify it in rules/operations.toml")

    claimed_cli = {c for o in model.operations for c in o.get("surfaces_cli", [])}
    allowed_cli = set(model.config.get("read_only_cli", []))
    main = model.root / CLI_MAIN
    if main.is_file():
        for command in sorted(set(CLI_COMMAND.findall(main.read_text()))):
            if command not in claimed_cli and command not in allowed_cli:
                problems.append(
                    f"CLI command '{command}' is claimed by no operation and is not in "
                    f"read_only_cli — classify it in rules/operations.toml")

    return problems


VERSION_MACRO = re.compile(r"#define\s+PHIG_ABI_VERSION_(MAJOR|MINOR)\s+(\d+)")
CMAKE_VERSION = re.compile(r"^\s*VERSION\s+(\d+)\.(\d+)\.(\d+)\s*$", re.M)
CMAKE_SOVERSION = re.compile(r"^\s*SOVERSION\s+(\d+)\s*$", re.M)


def check_abi_version(model):
    """R64's replacement: the header's MAJOR/MINOR, the CMake VERSION and
    SOVERSION are hand-maintained in two files and nothing compared them.

    R64 says a symbol bump moves MINOR and VERSION and never SOVERSION. Its own
    test only checks that phig_abi_version()'s high half equals the header
    macro, which passes with the CMake values set to anything at all. This is
    strictly stronger, and it is why R64 can leave the record set.
    """
    header = model.root / ABI_HEADER
    cmake = model.root / ABI_CMAKE
    if not header.is_file() or not cmake.is_file():
        return [f"cannot compare ABI versions: {ABI_HEADER} or {ABI_CMAKE} is missing"]

    macros = {k: int(v) for k, v in VERSION_MACRO.findall(header.read_text())}
    text = cmake.read_text()
    version = CMAKE_VERSION.search(text)
    soversion = CMAKE_SOVERSION.search(text)
    if len(macros) != 2 or not version or not soversion:
        return [f"cannot compare ABI versions: expected MAJOR and MINOR in "
                f"{ABI_HEADER} and VERSION and SOVERSION in {ABI_CMAKE}"]

    problems = []
    major, minor = macros["MAJOR"], macros["MINOR"]
    if (int(version.group(1)), int(version.group(2))) != (major, minor):
        problems.append(
            f"{ABI_CMAKE} VERSION is {version.group(0).strip()} but {ABI_HEADER} "
            f"says MAJOR {major}, MINOR {minor} — a symbol bump moves both")
    if int(soversion.group(1)) != major:
        problems.append(
            f"{ABI_CMAKE} SOVERSION is {soversion.group(1)} but the ABI major is "
            f"{major} — SOVERSION is the major alone and moves only on a break")
    return problems


CHECKS = [check_schema, check_sources, check_owners, check_coverage, check_abi_version]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="run every check")
    parser.add_argument("--root", default=pathlib.Path(__file__).resolve().parent.parent)
    parser.add_argument("--test-results", dest="test_results",
                        help="gtest XML from --gtest_output=xml:PATH")
    args = parser.parse_args(argv)

    model = load(pathlib.Path(args.root))
    problems = [p for check in CHECKS for p in check(model)]
    if args.test_results:
        try:
            problems += check_tests(model, load_test_results(args.test_results))
        except TestResultsError as exc:
            problems.append(str(exc))
    for problem in problems:
        print(f"rules: {problem}", file=sys.stderr)
    if problems:
        print(f"rules: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print(f"rules: {len(model.rules)} rules, {len(model.operations)} operations, OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
