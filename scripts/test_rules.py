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


if __name__ == "__main__":
    unittest.main()
