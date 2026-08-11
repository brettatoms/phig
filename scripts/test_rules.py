import pathlib
import tempfile
import unittest

import rules


def write(root, name, text):
    (root / "rules").mkdir(exist_ok=True)
    (root / "rules" / name).write_text(text)


MINIMAL_TAXONOMY = """
concerns = ["conflict", "concurrency"]

[[asset]]
id = "original-file"
rebuildable = "backup-only"
note = "The bytes on disk."

[[asset]]
id = "client-process"
rebuildable = "no"
note = "A foreign caller's address space and its ability to keep running."
"""

MINIMAL_OPS = """
read_only_abi = []
read_only_cli = []
core_headers = ["core/organize_engine.h"]
core_internal = []
core_read_only = []

[[operation]]
id = "organize.move"
core_entry = "core/organize_engine.h:OrganizeEngine::apply"
destructive = true
surfaces_abi = []
surfaces_cli = ["mv"]
surfaces_http = []
"""

# Structurally a copy of rules/conflict.toml's R01, down to the real test name.
GOOD_RULE = """
[[rule]]
id = "R01"
tier = "engine"
concern = "conflict"
statement = "A protected destination is never overwritten under any on_conflict policy; the resolver renames around it."
rationale = "Overwriting destroys bytes phig cannot restore, so the guard outranks the policy the user asked for."
enforced_at = ["core/paths.cpp:resolve_conflict"]
binds = ["organize.move"]
protects = ["original-file"]
tests = ["OrganizeEngineTest.OverwritePolicyStillRenamesAroundAnExistingFile"]
test_kind = "witness"
status = "holds"
"""

# Structurally a copy of rules/boundary.toml's R63: tier = "boundary" carries
# binds_surface instead of binds, and protects the client-process asset.
GOOD_BOUNDARY_RULE = """
[[rule]]
id = "R63"
tier = "boundary"
concern = "concurrency"
statement = "phig_last_error returns the calling thread's own last message."
rationale = "The storage is a function-local static thread_local std::string."
enforced_at = ["libphig/context.cpp:phig_last_error"]
binds_surface = ["abi"]
protects = ["client-process"]
tests = ["AbiMutate.EachThreadSeesOnlyItsOwnError"]
test_kind = "witness"
status = "holds"
"""


class SchemaTest(unittest.TestCase):
    def build(self, rule_text, ops_text=MINIMAL_OPS):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        write(root, "taxonomy.toml", MINIMAL_TAXONOMY)
        write(root, "operations.toml", ops_text)
        write(root, "conflict.toml", rule_text)
        return rules.load(root)

    def test_clean_record_has_no_problems(self):
        self.assertEqual(rules.check_schema(self.build(GOOD_RULE)), [])

    def test_clean_boundary_rule_has_no_problems(self):
        self.assertEqual(rules.check_schema(self.build(GOOD_BOUNDARY_RULE)), [])

    def test_unknown_status_is_reported(self):
        model = self.build(GOOD_RULE.replace('status = "holds"', 'status = "maybe"'))
        problems = rules.check_schema(model)
        self.assertTrue(any("status" in p and "R01" in p for p in problems))

    def test_violated_without_owner_is_reported(self):
        model = self.build(GOOD_RULE.replace('status = "holds"', 'status = "violated"'))
        problems = rules.check_schema(model)
        self.assertTrue(any("owner" in p and "R01" in p for p in problems))

    def test_test_gap_without_owner_is_reported(self):
        model = self.build(GOOD_RULE + '\ntest_gap = "weaker than the claim"\n')
        problems = rules.check_schema(model)
        self.assertTrue(any("owner" in p and "R01" in p for p in problems))

    def test_duplicate_rule_id_is_reported(self):
        model = self.build(GOOD_RULE + GOOD_RULE)
        self.assertTrue(any("duplicate" in p.lower() for p in rules.check_schema(model)))

    def test_binds_unknown_operation_is_reported(self):
        model = self.build(GOOD_RULE.replace('"organize.move"', '"organize.teleport"'))
        problems = rules.check_schema(model)
        self.assertTrue(any("organize.teleport" in p for p in problems))

    def test_protects_unknown_asset_is_reported(self):
        model = self.build(GOOD_RULE.replace('"original-file"', '"morale"'))
        self.assertTrue(any("morale" in p for p in rules.check_schema(model)))

    def test_engine_rule_without_binds_is_reported(self):
        model = self.build(GOOD_RULE.replace('binds = ["organize.move"]\n', ""))
        self.assertTrue(any("binds" in p and "R01" in p for p in rules.check_schema(model)))

    def test_engine_rule_using_binds_surface_is_reported(self):
        model = self.build(GOOD_RULE + '\nbinds_surface = ["abi"]\n')
        self.assertTrue(any("binds_surface" in p for p in rules.check_schema(model)))

    def test_boundary_rule_without_binds_surface_is_reported(self):
        text = GOOD_BOUNDARY_RULE.replace('binds_surface = ["abi"]\n', "")
        self.assertTrue(any("binds_surface" in p for p in rules.check_schema(self.build(text))))

    def test_boundary_rule_using_binds_is_reported(self):
        text = GOOD_BOUNDARY_RULE + '\nbinds = ["organize.move"]\n'
        problems = rules.check_schema(self.build(text))
        self.assertTrue(any("must not use 'binds'" in p for p in problems))

    def test_unknown_surface_is_reported(self):
        text = GOOD_BOUNDARY_RULE.replace('["abi"]', '["grpc"]')
        self.assertTrue(any("grpc" in p for p in rules.check_schema(self.build(text))))

    def test_overlapping_classification_buckets_are_reported(self):
        ops = MINIMAL_OPS.replace('core_internal = []',
                                  'core_internal = ["carry_sidecars"]')
        ops = ops.replace('core_read_only = []',
                          'core_read_only = ["carry_sidecars"]')
        problems = rules.check_schema(self.build(GOOD_RULE, ops))
        self.assertTrue(any("carry_sidecars" in p for p in problems))

    def test_buckets_are_compared_on_the_qualified_name(self):
        # ScanEngine::run is core_internal in the real record; ThumbsEngine::run
        # and FacesEngine::run are operation core_entry symbols. They collide on
        # the bare name and must not be reported as an overlap.
        ops = MINIMAL_OPS.replace('core_internal = []',
                                  'core_internal = ["ScanEngine::run"]')
        ops = ops.replace(
            'core_entry = "core/organize_engine.h:OrganizeEngine::apply"',
            'core_entry = "core/thumbs_engine.h:ThumbsEngine::run"')
        self.assertEqual(rules.check_schema(self.build(GOOD_RULE, ops)), [])

    def test_an_allowlisted_symbol_may_not_also_be_claimed(self):
        ops = MINIMAL_OPS.replace('read_only_cli = []', 'read_only_cli = ["mv"]')
        problems = rules.check_schema(self.build(GOOD_RULE, ops))
        self.assertTrue(any("mv" in p for p in problems))


class SourceAnchorTest(unittest.TestCase):
    def core(self, root, name, text):
        """Append, so the operation's own core_entry header can coexist with
        whatever a test writes into the same file."""
        (root / "core").mkdir(exist_ok=True)
        with (root / "core" / name).open("a") as handle:
            handle.write(text)

    def fixture(self, root, rule_text):
        # check_sources also resolves every operation's core_entry, so the
        # fixture has to declare MINIMAL_OPS's OrganizeEngine::apply.
        self.core(root, "organize_engine.h",
                  "class OrganizeEngine {\n  OrganizeResult apply();\n};\n")
        write(root, "taxonomy.toml", MINIMAL_TAXONOMY)
        write(root, "operations.toml", MINIMAL_OPS)
        write(root, "conflict.toml", rule_text)
        return rules.load(root)

    def build(self, root, enforced_at):
        return self.fixture(
            root, GOOD_RULE.replace('["core/paths.cpp:resolve_conflict"]', enforced_at))

    def test_missing_file_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(root, '["core/nope.cpp:resolve_conflict"]')
            self.assertTrue(any("core/nope.cpp" in p for p in rules.check_sources(model)))

    def test_missing_symbol_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.core(root, "paths.cpp", "int unrelated() { return 0; }\n")
            model = self.build(root, '["core/paths.cpp:resolve_conflict"]')
            self.assertTrue(any("resolve_conflict" in p for p in rules.check_sources(model)))

    def test_enforcement_in_cli_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "cli").mkdir()
            (root / "cli" / "main.cpp").write_text("void cmd_organize() {}\n")
            model = self.build(root, '["cli/main.cpp:cmd_organize"]')
            problems = rules.check_sources(model)
            self.assertTrue(any("cli/" in p and "core/" in p for p in problems))

    def test_enforcement_in_cli_is_allowed_when_violated(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "cli").mkdir()
            (root / "cli" / "main.cpp").write_text("void cmd_organize() {}\n")
            model = self.fixture(
                root,
                GOOD_RULE
                .replace('["core/paths.cpp:resolve_conflict"]',
                         '["cli/main.cpp:cmd_organize"]')
                .replace('status = "holds"', 'status = "violated"\nowner = "043"'))
            self.assertEqual(rules.check_sources(model), [])

    def test_valid_anchor_is_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.core(root, "paths.cpp", "std::string resolve_conflict() {}\n")
            model = self.build(root, '["core/paths.cpp:resolve_conflict"]')
            self.assertEqual(rules.check_sources(model), [])

    def test_qualifier_is_stripped_when_the_member_is_declared_bare(self):
        # R09's shape: the anchor names OrganizePlan::set_resolution, the header
        # declares set_resolution inside the class body.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(
                root, '["core/organize_engine.h:OrganizePlan::set_resolution"]')
            self.core(root, "organize_engine.h",
                      "class OrganizePlan {\n  void set_resolution(int i);\n};\n")
            self.assertEqual(rules.check_sources(model), [])

    def test_a_symbol_does_not_resolve_through_a_longer_name(self):
        # ScanEngine::run must not resolve against run_workers/run_reconcile.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.core(root, "scan_engine.cpp",
                      "void ScanEngine::run_workers() {}\n"
                      "void ScanEngine::run_reconcile() {}\n")
            model = self.build(root, '["core/scan_engine.cpp:ScanEngine::run"]')
            self.assertTrue(any("ScanEngine::run" in p for p in rules.check_sources(model)))

    def test_an_operation_core_entry_is_checked_too(self):
        # An engine method renamed without its operations.toml row following it
        # is the same decay as an orphaned enforced_at.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.core(root, "paths.cpp", "std::string resolve_conflict() {}\n")
            self.core(root, "organize_engine.h",
                      "class OrganizeEngine {\n  OrganizeResult apply();\n};\n")
            write(root, "taxonomy.toml", MINIMAL_TAXONOMY)
            write(root, "operations.toml",
                  MINIMAL_OPS.replace("OrganizeEngine::apply", "OrganizeEngine::teleport"))
            write(root, "conflict.toml", GOOD_RULE)
            problems = rules.check_sources(rules.load(root))
            self.assertTrue(any("organize.move" in p and "teleport" in p for p in problems))

    def test_symbol_mentioned_only_in_a_comment_does_not_resolve(self):
        # Reproduces R56's real hazard: ScanEngine::run was renamed, and the
        # only remaining trace of "run" is a comment's "run's detections" —
        # a bare-word match must not treat that as the symbol still existing.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.core(root, "scan_engine.cpp",
                      "// run's detections would duplicate them. insert() cleared\n"
                      "void ScanEngine::run_all() {}\n")
            model = self.build(root, '["core/scan_engine.cpp:ScanEngine::run"]')
            self.assertTrue(any("ScanEngine::run" in p for p in rules.check_sources(model)))

    def test_symbol_mentioned_only_in_a_string_literal_does_not_resolve(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.core(root, "paths.cpp",
                      'const char *msg = "resolve_conflict failed";\n')
            model = self.build(root, '["core/paths.cpp:resolve_conflict"]')
            self.assertTrue(any("resolve_conflict" in p for p in rules.check_sources(model)))


INDEX = """# Plan Index

| # | Name | Status | File | Description |
|---|------|--------|------|-------------|
| 041 | Scan Reconcile | planned | [041-x.md](041-x.md) | words |
| 042 | Organize Engine | completed | [042-x.md](042-x.md) | words |
| 046 | Vec Faces | designing | [046-x.md](046-x.md) | words |
| 054 | Rules of Record | in-progress | [054-x.md](054-x.md) | words |
"""


def violated(owner):
    """GOOD_RULE with its status replaced, not appended.

    Appending `status = ...` would put a second `status` key in the same
    [[rule]] table, which TOML forbids — tomllib raises TOMLDecodeError and the
    test errors instead of asserting anything.
    """
    return GOOD_RULE.replace('status = "holds"',
                             f'status = "violated"\nowner = "{owner}"')


class OwnerTest(unittest.TestCase):
    def build(self, root, rule_text):
        write(root, "taxonomy.toml", MINIMAL_TAXONOMY)
        write(root, "operations.toml", MINIMAL_OPS)
        write(root, "conflict.toml", rule_text)
        (root / "plans").mkdir(exist_ok=True)
        (root / "plans" / "000-INDEX.md").write_text(INDEX)
        return rules.load(root)

    def test_index_is_parsed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "plans").mkdir()
            (root / "plans" / "000-INDEX.md").write_text(INDEX)
            self.assertEqual(rules.plan_statuses(root)["042"], "completed")

    def test_owner_that_is_completed_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            model = self.build(pathlib.Path(tmp), violated("042"))
            problems = rules.check_owners(model)
            self.assertTrue(any("042" in p and "completed" in p for p in problems))

    def test_owner_that_is_open_is_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            model = self.build(pathlib.Path(tmp), violated("041"))
            self.assertEqual(rules.check_owners(model), [])

    def test_owner_not_in_index_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            model = self.build(pathlib.Path(tmp), violated("099"))
            self.assertTrue(any("099" in p for p in rules.check_owners(model)))

    def test_test_gap_owner_is_checked_too(self):
        with tempfile.TemporaryDirectory() as tmp:
            # test_gap is a new key, so appending is safe here.
            model = self.build(pathlib.Path(tmp),
                               GOOD_RULE + '\ntest_gap = "weak"\nowner = "042"\n')
            self.assertTrue(any("042" in p for p in rules.check_owners(model)))

    def test_in_progress_is_an_open_owner(self):
        # 054 owns 26 of its own test gaps and is in-progress, not planned.
        with tempfile.TemporaryDirectory() as tmp:
            model = self.build(pathlib.Path(tmp),
                               GOOD_RULE + '\ntest_gap = "weak"\nowner = "054"\n')
            self.assertEqual(rules.check_owners(model), [])


CLOSURES_INDEX = """# Plan Index

| # | Name | Status | File | Description |
|---|------|--------|------|-------------|
| 039 | Cross-Platform | designing | [039-x.md](039-x.md) | words |
| 041 | Scan Reconcile | planned | [041-x.md](041-x.md) | words |
| 042 | Organize Engine | completed | [042-x.md](042-x.md) | words |
"""


def plan_file(root, filename, closes_rules=None):
    """Write a plan file with frontmatter, optionally carrying closes_rules
    in Task 15's uniform flow-style list."""
    (root / "plans").mkdir(exist_ok=True)
    text = "---\nname: Test\nstatus: whatever\ncreated: 2026-08-09\n"
    if closes_rules is not None:
        text += f"closes_rules: [{closes_rules}]\n"
    text += "description: >\n  words\n---\n\n# words\n"
    (root / "plans" / filename).write_text(text)


class PlanClosuresTest(unittest.TestCase):
    def test_flow_list_is_parsed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            plan_file(root, "041-x.md", closes_rules="R04, R25, R40, R59")
            self.assertEqual(
                rules.plan_closures(root)["041"], ["R04", "R25", "R40", "R59"])

    def test_single_id_is_still_a_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            plan_file(root, "039-x.md", closes_rules="R50")
            self.assertEqual(rules.plan_closures(root)["039"], ["R50"])

    def test_plan_with_no_closes_rules_is_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            plan_file(root, "041-x.md")
            self.assertEqual(rules.plan_closures(root), {})

    def test_index_file_itself_is_not_read_as_a_plan(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "plans").mkdir()
            (root / "plans" / "000-INDEX.md").write_text(CLOSURES_INDEX)
            self.assertEqual(rules.plan_closures(root), {})


class ClosuresTest(unittest.TestCase):
    def build(self, root, rule_text, plans_closures, index=CLOSURES_INDEX):
        write(root, "taxonomy.toml", MINIMAL_TAXONOMY)
        write(root, "operations.toml", MINIMAL_OPS)
        write(root, "conflict.toml", rule_text)
        (root / "plans").mkdir(exist_ok=True)
        (root / "plans" / "000-INDEX.md").write_text(index)
        for number, closes in plans_closures.items():
            plan_file(root, f"{number}-x.md", closes_rules=closes)
        return rules.load(root)

    def test_completed_plan_claiming_a_violated_rule_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(root, violated("041"), {"042": "R01"})
            problems = rules.check_closures(model)
            self.assertTrue(any("R01" in p and "042" in p for p in problems))

    def test_completed_plan_with_only_holds_rules_is_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(root, GOOD_RULE, {"042": "R01"})
            self.assertEqual(rules.check_closures(model), [])

    def test_violated_rule_claimed_by_nobody_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(root, violated("041"), {})
            problems = rules.check_closures(model)
            self.assertTrue(any("R01" in p for p in problems))

    def test_violated_rule_claimed_by_an_open_plan_is_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(root, violated("041"), {"041": "R01"})
            self.assertEqual(rules.check_closures(model), [])

    def test_claim_naming_a_nonexistent_rule_id_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(root, GOOD_RULE, {"041": "R99"})
            problems = rules.check_closures(model)
            self.assertTrue(any("R99" in p for p in problems))

    def test_plan_with_no_closes_rules_is_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(root, GOOD_RULE, {})
            plan_file(root, "041-x.md")
            self.assertEqual(rules.check_closures(model), [])


# Shapes taken from a probe binary built against the repo's gtest 1.17.0, and
# confirmed against build/test-results.xml. GTEST_SKIP() writes status="run"
# result="skipped" with a <skipped> child. A DISABLED_-prefixed test writes
# status="notrun" result="suppressed" with no <skipped> child at all, and
# never runs — status="notrun" belongs to DISABLED_, never to GTEST_SKIP.
GTEST_XML = """<?xml version="1.0"?>
<testsuites tests="4" failures="1" disabled="1">
  <testsuite name="OrganizeEngineTest" tests="2">
    <testcase name="OverwritePolicyStillRenamesAroundAnExistingFile" status="run"/>
    <testcase name="DISABLED_RenamesTheLegacyWay" status="notrun" result="suppressed"/>
  </testsuite>
  <testsuite name="DatabaseTest" tests="1">
    <testcase name="PurgeSnapshotMarksOmittedFaces" status="run">
      <failure message="SQL logic error"/>
    </testcase>
  </testsuite>
  <testsuite name="MoveFileTest" tests="1">
    <testcase name="CrossDeviceRefusesToOverwriteExistingDest" status="run"
              result="skipped">
      <skipped message="temp dir is on one filesystem"/>
    </testcase>
  </testsuite>
</testsuites>
"""

EMPTY_GTEST_XML = """<?xml version="1.0"?>
<testsuites tests="0" failures="0">
</testsuites>
"""

PASSING = '"OrganizeEngineTest.OverwritePolicyStillRenamesAroundAnExistingFile"'
DISABLED = '"OrganizeEngineTest.DISABLED_RenamesTheLegacyWay"'


class TestAnchorTest(unittest.TestCase):
    def results(self, root):
        path = root / "results.xml"
        path.write_text(GTEST_XML)
        return rules.load_test_results(path)

    def build(self, root, rule_text):
        write(root, "taxonomy.toml", MINIMAL_TAXONOMY)
        write(root, "operations.toml", MINIMAL_OPS)
        write(root, "conflict.toml", rule_text)
        return rules.load(root)

    def test_results_are_parsed(self):
        with tempfile.TemporaryDirectory() as tmp:
            res = self.results(pathlib.Path(tmp))
            self.assertEqual(
                res["OrganizeEngineTest.OverwritePolicyStillRenamesAroundAnExistingFile"],
                "passed")
            self.assertEqual(res["DatabaseTest.PurgeSnapshotMarksOmittedFaces"], "failed")
            self.assertEqual(
                res["MoveFileTest.CrossDeviceRefusesToOverwriteExistingDest"], "skipped")
            self.assertEqual(res["OrganizeEngineTest.DISABLED_RenamesTheLegacyWay"], "disabled")

    def test_empty_results_file_is_reported_distinctly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = root / "empty.xml"
            path.write_text(EMPTY_GTEST_XML)
            with self.assertRaises(rules.TestResultsError):
                rules.load_test_results(path)

    def test_malformed_results_file_is_reported_distinctly(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = root / "bad.xml"
            path.write_text("this is not xml at all")
            with self.assertRaises(rules.TestResultsError):
                rules.load_test_results(path)

    def test_nonexistent_results_path_is_reported_distinctly(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "does-not-exist.xml"
            with self.assertRaises(rules.TestResultsError):
                rules.load_test_results(path)

    def test_directory_as_results_path_is_reported_distinctly(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(rules.TestResultsError):
                rules.load_test_results(pathlib.Path(tmp))

    def test_holds_rule_with_passing_test_is_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(root, GOOD_RULE)
            self.assertEqual(rules.check_tests(model, self.results(root)), [])

    def test_holds_rule_with_failing_test_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            text = GOOD_RULE.replace(PASSING, '"DatabaseTest.PurgeSnapshotMarksOmittedFaces"')
            problems = rules.check_tests(self.build(root, text), self.results(root))
            self.assertTrue(any("failed" in p for p in problems))

    def test_a_cited_test_missing_from_the_run_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            text = GOOD_RULE.replace(PASSING, '"OrganizeEngineTest.RenamedAwayLastWeek"')
            problems = rules.check_tests(self.build(root, text), self.results(root))
            self.assertTrue(any("RenamedAwayLastWeek" in p for p in problems))

    def test_a_skipped_test_is_neither_a_pass_nor_a_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            text = GOOD_RULE.replace(
                PASSING, '"MoveFileTest.CrossDeviceRefusesToOverwriteExistingDest"')
            self.assertEqual(rules.check_tests(self.build(root, text), self.results(root)), [])

    def test_holds_rule_citing_a_disabled_test_is_reported(self):
        # A skip is a property of the machine; DISABLED_ is a permanent
        # code-level decision, so unlike a skip it must count against a
        # holds rule's only witness.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            text = GOOD_RULE.replace(PASSING, DISABLED)
            problems = rules.check_tests(self.build(root, text), self.results(root))
            self.assertTrue(any("DISABLED_" in p for p in problems))

    def test_violated_rule_citing_a_disabled_test_is_clean(self):
        # Existence-only: the test still exists in the binary, and outcome —
        # including never running at all — is not check 2's business for a
        # rule that isn't claiming to hold.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            text = (GOOD_RULE
                    .replace('status = "holds"', 'status = "violated"\nowner = "041"')
                    .replace(PASSING, DISABLED))
            self.assertEqual(rules.check_tests(self.build(root, text), self.results(root)), [])

    def test_holds_rule_with_no_test_and_no_gap_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            model = self.build(root, GOOD_RULE.replace(f"tests = [{PASSING}]", "tests = []"))
            problems = rules.check_tests(model, self.results(root))
            self.assertTrue(any("R01" in p and "no test" in p for p in problems))

    def test_holds_rule_with_no_test_but_a_gap_and_owner_is_clean(self):
        # R26's shape: the code satisfies the rule, nothing pins it, 054 owns it.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            text = (GOOD_RULE.replace(f"tests = [{PASSING}]", "tests = []")
                    + '\ntest_gap = "nothing calls it with an unknown path"\nowner = "054"\n')
            self.assertEqual(rules.check_tests(self.build(root, text), self.results(root)), [])

    def test_violated_rule_with_no_test_is_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            text = (GOOD_RULE
                    .replace('status = "holds"', 'status = "violated"\nowner = "041"')
                    .replace(f"tests = [{PASSING}]", "tests = []"))
            self.assertEqual(rules.check_tests(self.build(root, text), self.results(root)), [])

    def test_violated_rule_pinning_contrary_behaviour_may_fail(self):
        # R08's shape: its one test asserts what the code does today, not what
        # the rule says, so its outcome is not check 2's business.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            text = (GOOD_RULE
                    .replace('status = "holds"', 'status = "violated"\nowner = "059"')
                    .replace(PASSING, '"DatabaseTest.PurgeSnapshotMarksOmittedFaces"'))
            self.assertEqual(rules.check_tests(self.build(root, text), self.results(root)), [])


PHIG_H = """
#define PHIG_ABI_VERSION_MAJOR 1u
#define PHIG_ABI_VERSION_MINOR 3u
PHIG_API int phig_init(const char* db);
PHIG_API int phig_tag_add(phig_ctx* c, int64_t id, const char* tag);
PHIG_API int phig_op_move_start(phig_ctx* c, phig_op** out);
"""

# The three shapes cli/main.cpp actually dispatches on: a plain command, a
# command sharing a branch with another, and the help flags.
MAIN_CPP = """
    if (command == "--help" || command == "-h" || command == "help") { usage(); }
    else if (command == "cp" || command == "mv") { return cmd_organize(argc, argv); }
    else if (command == "search") { return cmd_search(argc, argv); }
"""

CMAKELISTS = """
set_target_properties(phig PROPERTIES
    VERSION 1.3.0
    SOVERSION 1
)
"""


class CoverageTest(unittest.TestCase):
    def build(self, root, ops_text, cmake=CMAKELISTS):
        write(root, "taxonomy.toml", MINIMAL_TAXONOMY)
        write(root, "operations.toml", ops_text)
        write(root, "conflict.toml", GOOD_RULE)
        (root / "libphig" / "include" / "phig").mkdir(parents=True)
        (root / "libphig" / "include" / "phig" / "phig.h").write_text(PHIG_H)
        (root / "libphig" / "CMakeLists.txt").write_text(cmake)
        (root / "cli").mkdir()
        (root / "cli" / "main.cpp").write_text(MAIN_CPP)
        return rules.load(root)

    def classified(self, extra_abi="", extra_cli=""):
        ops = MINIMAL_OPS.replace(
            'read_only_abi = []',
            'read_only_abi = ["phig_init", "phig_tag_add"' + extra_abi + ']')
        ops = ops.replace('surfaces_abi = []', 'surfaces_abi = ["phig_op_move_start"]')
        return ops.replace('read_only_cli = []',
                           'read_only_cli = ["search", "help"' + extra_cli + ']')

    def test_unclassified_abi_symbol_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            ops = MINIMAL_OPS.replace('read_only_abi = []',
                                      'read_only_abi = ["phig_init"]')
            ops = ops.replace('surfaces_abi = []',
                              'surfaces_abi = ["phig_op_move_start"]')
            problems = rules.check_coverage(self.build(pathlib.Path(tmp), ops))
            self.assertTrue(any("phig_tag_add" in p for p in problems))

    def test_unclassified_cli_command_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            ops = self.classified().replace('"search", "help"', '"help"')
            problems = rules.check_coverage(self.build(pathlib.Path(tmp), ops))
            self.assertTrue(any("search" in p for p in problems))

    def test_a_command_sharing_a_branch_is_claimed_separately(self):
        # cp and mv dispatch from one branch; the operations table claims mv
        # only, so cp must still be reported.
        with tempfile.TemporaryDirectory() as tmp:
            problems = rules.check_coverage(self.build(pathlib.Path(tmp), self.classified()))
            self.assertTrue(any("'cp'" in p for p in problems))

    def test_help_flags_are_not_read_as_commands(self):
        # command == "--help" and command == "-h" are the same branch as help;
        # a flag is not a top-level command and must not need classifying.
        with tempfile.TemporaryDirectory() as tmp:
            ops = self.classified(extra_cli=', "cp"')
            problems = rules.check_coverage(self.build(pathlib.Path(tmp), ops))
            self.assertEqual(problems, [])

    def test_abi_version_agreement_is_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            model = self.build(pathlib.Path(tmp), self.classified(extra_cli=', "cp"'))
            self.assertEqual(rules.check_abi_version(model), [])

    def test_a_minor_bump_without_the_cmake_version_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.build(root, self.classified(extra_cli=', "cp"'))
            header = root / "libphig" / "include" / "phig" / "phig.h"
            header.write_text(PHIG_H.replace("MINOR 3u", "MINOR 4u"))
            self.assertTrue(any("1.3.0" in p for p in rules.check_abi_version(rules.load(root))))

    def test_a_soversion_that_is_not_the_major_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            model = self.build(pathlib.Path(tmp), self.classified(extra_cli=', "cp"'),
                               cmake=CMAKELISTS.replace("SOVERSION 1", "SOVERSION 2"))
            self.assertTrue(any("SOVERSION" in p for p in rules.check_abi_version(model)))


class RenderTest(unittest.TestCase):
    def build(self, rule_text=GOOD_RULE, taxonomy=MINIMAL_TAXONOMY):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        write(root, "taxonomy.toml", taxonomy)
        write(root, "operations.toml", MINIMAL_OPS)
        write(root, "conflict.toml", rule_text)
        return rules.load(root)

    def test_three_views_are_produced(self):
        views = rules.render(self.build())
        self.assertEqual(set(views), {"by-invariant.md", "by-operation.md", "by-asset.md"})

    def test_invariant_view_carries_statement_and_anchors(self):
        text = rules.render(self.build())["by-invariant.md"]
        self.assertIn("R01", text)
        self.assertIn("the resolver renames around it", text)
        self.assertIn("core/paths.cpp:resolve_conflict", text)

    def test_operation_view_pivots_on_binds(self):
        text = rules.render(self.build())["by-operation.md"]
        self.assertIn("organize.move", text)
        self.assertIn("R01", text)

    def test_operation_view_lists_boundary_rules_by_surface(self):
        # A boundary rule binds no operation, so it appears in no cell. It must
        # still be visible in the view a reader calls the matrix.
        text = rules.render(self.build(GOOD_RULE + GOOD_BOUNDARY_RULE))["by-operation.md"]
        self.assertIn("R63", text)
        self.assertIn("abi", text)

    def test_asset_view_pivots_on_protects(self):
        text = rules.render(self.build())["by-asset.md"]
        self.assertIn("original-file", text)
        self.assertIn("R01", text)

    def test_asset_view_names_an_asset_no_rule_protects(self):
        # This is what replaces check 6: client-process is in the taxonomy and
        # GOOD_RULE protects only original-file.
        text = rules.render(self.build())["by-asset.md"]
        self.assertIn("client-process", text)
        self.assertIn("Protected by no rule.", text)

    def test_no_view_has_trailing_whitespace(self):
        for name, text in rules.render(self.build()).items():
            for n, line in enumerate(text.splitlines(), 1):
                self.assertEqual(line, line.rstrip(), f"{name}:{n} trailing whitespace")

    def test_views_carry_a_do_not_edit_banner(self):
        for text in rules.render(self.build()).values():
            self.assertIn("Generated by scripts/rules.py", text.splitlines()[2])


if __name__ == "__main__":
    unittest.main()
