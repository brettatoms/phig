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


if __name__ == "__main__":
    unittest.main()
