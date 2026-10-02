import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "pixel-composer/update-matrix.py"
SPEC = importlib.util.spec_from_file_location("pixel_composer_update_matrix", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
UPDATE_MATRIX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UPDATE_MATRIX)


class PixelComposerUpdateMatrixTest(unittest.TestCase):
    def test_type_name_mention_is_candidate_not_implementation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executors = root / "nodes"
            tests = root / "tests"
            executors.mkdir()
            tests.mkdir()
            (executors / "Node.cpp").write_text('{"pc.fake", ExecuteFake}\n', encoding="utf-8")
            (tests / "Mention.cpp").write_text('constexpr auto type = "pc.fake";\n', encoding="utf-8")

            candidates = UPDATE_MATRIX.find_native_candidates(executors, tests)
            rows, implemented = UPDATE_MATRIX.refresh_rows(
                [{
                    "node_id": "FakeNode",
                    "status": UPDATE_MATRIX.BLOCKED,
                    "fixture": "unrecorded",
                    "prerequisite": "native validation pending",
                }],
                {"FakeNode": {"type": "pc.fake", "file": "scripts/node_fake/node_fake.gml", "inputs": []}},
                candidates,
            )

        self.assertEqual(implemented, 0)
        self.assertEqual(rows[0]["status"], UPDATE_MATRIX.BLOCKED)
        self.assertEqual(
            rows[0]["native_evidence_candidate"],
            "executor registry: pc.fake; test reference: mono.engine/imagegraph/tests/Mention.cpp",
        )
        self.assertEqual(rows[0]["fixture"], UPDATE_MATRIX.REVIEW_PENDING)

    def test_registry_candidates_accept_only_two_or_literal_boolean_fields_and_need_test_reference(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executors = root / "nodes"
            tests = root / "tests"
            executors.mkdir()
            tests.mkdir()
            (executors / "Node.cpp").write_text(
                "ExecutorEntry{\"pc.two_field\", ExecuteTwo},\n"
                "{ \"pc.true_flag\" , ExecuteTrue , true },\n"
                "{\"pc.false_flag\", ExecuteFalse, false},\n"
                "{\"pc.malformed_flag\", ExecuteBad, maybe},\n"
                "{\"pc.no_test\", ExecuteNoTest, true},\n",
                encoding="utf-8",
            )
            (tests / "Registry.cpp").write_text(
                '\"pc.two_field\"; \"pc.true_flag\"; '
                '\"pc.false_flag\"; \"pc.malformed_flag\";\n',
                encoding="utf-8",
            )

            candidates = UPDATE_MATRIX.find_native_candidates(executors, tests)
            rows, implemented = UPDATE_MATRIX.refresh_rows(
                [{
                    "node_id": "FlagNode",
                    "status": UPDATE_MATRIX.IMPLEMENTED,
                    "fixture": "unrecorded",
                    "prerequisite": "native validation pending",
                }],
                {
                    "FlagNode": {
                        "type": "pc.true_flag",
                        "file": "scripts/node_flag/node_flag.gml",
                        "inputs": [],
                    }
                },
                candidates,
            )

        self.assertEqual(set(candidates), {"pc.two_field", "pc.true_flag", "pc.false_flag"})
        self.assertNotIn("pc.malformed_flag", candidates)
        self.assertNotIn("pc.no_test", candidates)
        self.assertEqual(implemented, 0)
        self.assertEqual(rows[0]["status"], UPDATE_MATRIX.BLOCKED)
        self.assertIn("executor registry: pc.true_flag", rows[0]["native_evidence_candidate"])
        self.assertIn(
            "test reference: mono.engine/imagegraph/tests/Registry.cpp",
            rows[0]["native_evidence_candidate"],
        )

    def test_check_mode_does_not_write_outdated_matrix(self):
        with tempfile.TemporaryDirectory() as temporary:
            matrix = Path(temporary) / "matrix.csv"
            catalogue = Path(temporary) / "catalogue.inc"
            executors = Path(temporary) / "nodes"
            tests = Path(temporary) / "tests"
            executors.mkdir()
            tests.mkdir()
            catalogue.write_text("N\tpc.fake\tFakeNode\tunused\tunused\tfake.gml\n", encoding="utf-8")
            (executors / "Node.cpp").write_text('{"pc.fake", ExecuteFake}\n', encoding="utf-8")
            (tests / "Mention.cpp").write_text('"pc.fake"\n', encoding="utf-8")
            with matrix.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=["node_id", "defaults", "status", "prerequisite", "fixture"])
                writer.writeheader()
                writer.writerow({"node_id": "FakeNode", "status": UPDATE_MATRIX.BLOCKED})
            original = matrix.read_bytes()

            old_values = UPDATE_MATRIX.MATRIX, UPDATE_MATRIX.CATALOGUE, UPDATE_MATRIX.EXECUTORS, UPDATE_MATRIX.TESTS
            UPDATE_MATRIX.MATRIX, UPDATE_MATRIX.CATALOGUE, UPDATE_MATRIX.EXECUTORS, UPDATE_MATRIX.TESTS = (
                matrix, catalogue, executors, tests
            )
            try:
                result = UPDATE_MATRIX.main(["--check"])
            finally:
                UPDATE_MATRIX.MATRIX, UPDATE_MATRIX.CATALOGUE, UPDATE_MATRIX.EXECUTORS, UPDATE_MATRIX.TESTS = old_values
            self.assertEqual(result, 1)
            self.assertEqual(matrix.read_bytes(), original)


    def test_named_validation_fixture_keeps_independently_implemented_row(self):
        rows, implemented = UPDATE_MATRIX.refresh_rows(
            [{
                "node_id": "FakeNode",
                "status": UPDATE_MATRIX.IMPLEMENTED,
                "fixture": "case identity: exact output pixels in tests/NodeFake.cpp; 1 test passed",
                "prerequisite": "native controls validated; licensed executable comparison unavailable",
                "build_availability": "unverified: reference executable unavailable",
            }],
            {"FakeNode": {"type": "pc.fake", "file": "scripts/node_fake/node_fake.gml", "inputs": []}},
            {"pc.fake": ["executor registry: pc.fake", "test reference: mono.engine/imagegraph/tests/NodeFake.cpp"]},
        )

        self.assertEqual(implemented, 1)
        self.assertEqual(rows[0]["status"], UPDATE_MATRIX.IMPLEMENTED)
        self.assertEqual(rows[0]["prerequisite"], "native controls validated")
        self.assertEqual(rows[0]["build_availability"], "unverified: reference executable unavailable")

    def test_implemented_row_without_current_executor_is_blocked(self):
        rows, implemented = UPDATE_MATRIX.refresh_rows(
            [{
                "node_id": "FakeNode",
                "status": UPDATE_MATRIX.IMPLEMENTED,
                "fixture": "case identity: exact output pixels in tests/NodeFake.cpp; 1 test passed",
                "prerequisite": "native controls validated",
            }],
            {"FakeNode": {"type": "pc.fake", "file": "scripts/node_fake/node_fake.gml", "inputs": []}},
            {},
        )

        self.assertEqual(implemented, 0)
        self.assertEqual(rows[0]["status"], UPDATE_MATRIX.BLOCKED)
        self.assertEqual(rows[0]["fixture"], "case identity: exact output pixels in tests/NodeFake.cpp; 1 test passed")
        self.assertIn("native executor and named test validation evidence pending", rows[0]["prerequisite"])

    def test_engine_policy_prohibition_is_preserved(self):
        rows, _ = UPDATE_MATRIX.refresh_rows(
            [{
                "node_id": "FakeNode",
                "status": "prohibited by engine policy",
                "prerequisite": "prohibited by engine policy: feature is excluded",
                "fixture": "unrecorded",
            }],
            {"FakeNode": {"type": "pc.fake", "file": "scripts/node_fake/node_fake.gml", "inputs": []}},
            {"pc.fake": ["executor registry: pc.fake", "test reference: mono.engine/imagegraph/tests/NodeFake.cpp"]},
        )

        self.assertEqual(rows[0]["status"], "prohibited by engine policy")
        self.assertEqual(rows[0]["prerequisite"], "prohibited by engine policy: feature is excluded")

    def test_discrepancy_prerequisite_and_fixture_survive_candidate_discovery(self):
        discrepancy = "source/docs discrepancy: source clamps the end; docs omit the clamp"
        fixture = "docs and source differ on end clamp; see tests/NodeFake.cpp"
        original = [{
                "node_id": "FakeNode",
                "status": UPDATE_MATRIX.BLOCKED,
                "prerequisite": discrepancy,
                "fixture": fixture,
            }]
        entries = {"FakeNode": {"type": "pc.fake", "file": "scripts/node_fake/node_fake.gml", "inputs": []}}
        candidates = {"pc.fake": ["executor registry: pc.fake", "test reference: mono.engine/imagegraph/tests/NodeFake.cpp"]}
        rows, _ = UPDATE_MATRIX.refresh_rows(original, entries, candidates)
        once = [dict(row) for row in rows]
        rows, _ = UPDATE_MATRIX.refresh_rows(rows, entries, candidates)

        self.assertIn(discrepancy, rows[0]["prerequisite"])
        self.assertIn(UPDATE_MATRIX.REVIEW_PENDING, rows[0]["prerequisite"])
        self.assertEqual(rows[0]["fixture"], fixture)
        self.assertEqual(rows, once)

    def test_non_boilerplate_default_note_survives_refresh(self):
        note = "source extension uses denominator N and emits reversed half-spectrum"
        rows, _ = UPDATE_MATRIX.refresh_rows(
            [{
                "node_id": "FakeNode",
                "status": UPDATE_MATRIX.BLOCKED,
                "defaults": "pinned source old-pin fake.gml: width=i 4; " + note,
                "prerequisite": "native validation pending",
                "fixture": "unrecorded",
            }],
            {"FakeNode": {"type": "pc.fake", "file": "fake.gml", "inputs": [("width", "int", "4")]}},
            {},
        )

        self.assertIn(note, rows[0]["defaults"])

    def test_executor_and_test_path_boilerplate_does_not_validate_row(self):
        rows, implemented = UPDATE_MATRIX.refresh_rows(
            [{
                "node_id": "FakeNode",
                "status": UPDATE_MATRIX.IMPLEMENTED,
                "fixture": "native executor pc.fake; exact fixtures in mono.engine/imagegraph/tests/Mention.cpp",
                "prerequisite": "licensed executable comparison unavailable",
            }],
            {"FakeNode": {"type": "pc.fake", "file": "scripts/node_fake/node_fake.gml", "inputs": []}},
            {"pc.fake": ["executor registry: pc.fake", "test reference: mono.engine/imagegraph/tests/Mention.cpp"]},
        )

        self.assertEqual(implemented, 0)
        self.assertEqual(rows[0]["status"], UPDATE_MATRIX.BLOCKED)
        self.assertEqual(rows[0]["fixture"], UPDATE_MATRIX.REVIEW_PENDING)
        self.assertNotIn("licensed executable comparison", rows[0]["prerequisite"])


if __name__ == "__main__":
    unittest.main()
