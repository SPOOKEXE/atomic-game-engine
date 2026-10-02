import copy
import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "pixel-composer/source_selection.py"
SPEC = importlib.util.spec_from_file_location("pixel_composer_source_selection", MODULE)
assert SPEC is not None and SPEC.loader is not None
SOURCE_SELECTION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SOURCE_SELECTION)


class PixelComposerSourceSelectionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.path = "scripts/node_example/node_example.gml"
        self.content = b"function Node_Example() {}\n"
        (self.root / self.path).parent.mkdir(parents=True)
        (self.root / self.path).write_bytes(self.content)
        self.node = {
            "source_node_id": "Node_Example", "source_node_id_verified": True,
            "source": {"archive_member": f"commit/{self.path}", "bytes": len(self.content),
                       "sha256": hashlib.sha256(self.content).hexdigest(), "immutable_url": "pinned-url"},
            "published_documentation": {"status": 404},
        }
        self.evidence = {"source_commit": "commit", "nodes": [self.node]}
        self.source_files = {"Node_Example": self.path}

    def tearDown(self):
        self.temp.cleanup()

    def test_selects_verified_source_and_marks_undocumented(self):
        result = SOURCE_SELECTION.verified_source_nodes(self.evidence, "commit", self.root, self.source_files)
        self.assertEqual(result["Node_Example"]["source_origin"], "official pinned source")
        self.assertEqual(result["Node_Example"]["source_evidence"]["source_url"], "pinned-url")
        self.assertTrue(result["Node_Example"]["undocumented"])

    def test_skips_unknown_and_unverified_ids(self):
        self.node["source_node_id_verified"] = False
        self.node["source_node_id"] = "Node_Unknown"
        self.assertEqual(SOURCE_SELECTION.verified_source_nodes(self.evidence, "commit", self.root, {}), {})
        self.node.update(source_node_id=None, source_node_id_verified=False)
        self.assertEqual(SOURCE_SELECTION.verified_source_nodes(self.evidence, "commit", self.root, {}), {})

    def test_rejects_commit_declaration_path_hash_and_size_mismatches(self):
        for field, value, message in [
            (None, None, "commit"),
            ("declaration", None, "unknown verified"),
            ("archive_member", "commit/wrong.gml", "path mismatch"),
            ("sha256", "bad", "hash mismatch"),
            ("bytes", 1, "size mismatch"),
        ]:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                evidence = copy.deepcopy(self.evidence)
                if field == "declaration":
                    mapping = {}
                else:
                    mapping = self.source_files
                if field in ("archive_member", "sha256", "bytes"):
                    evidence["nodes"][0]["source"][field] = value
                commit = "other" if field is None else "commit"
                SOURCE_SELECTION.verified_source_nodes(evidence, commit, self.root, mapping)

    def test_rejects_conflicting_duplicate_verified_ids(self):
        duplicate = {**self.node, "source": {**self.node["source"], "immutable_url": "other-url"}}
        self.evidence["nodes"].append(duplicate)
        with self.assertRaisesRegex(ValueError, "conflicting"):
            SOURCE_SELECTION.verified_source_nodes(self.evidence, "commit", self.root, self.source_files)


if __name__ == "__main__":
    unittest.main()
