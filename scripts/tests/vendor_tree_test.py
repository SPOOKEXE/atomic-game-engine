#!/usr/bin/env python3
"""Exercise vendor-tree's submodule guard with local repositories."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "vendor-tree.sh"
REAL_GIT = shutil.which("git")


def run(command: list[str], cwd: Path, *, check: bool = True, env: dict[str, str] | None = None):
    return subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True, check=check)


class VendorTreeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="vendor-tree-test-")
        self.root = Path(self.temp.name) / "root"
        child = Path(self.temp.name) / "child"
        vendor = Path(self.temp.name) / "vendor"
        self.vendor_origin = vendor
        self.root.mkdir()
        child.mkdir()
        vendor.mkdir()
        self.git(child, "init")
        self.git_config(child)
        (child / "child.txt").write_text("child\n")
        self.git(child, "add", "child.txt")
        self.git(child, "commit", "-m", "child")

        self.git(vendor, "init")
        self.git_config(vendor)
        self.git(vendor, "-c", "protocol.file.allow=always", "submodule", "add", str(child), "nested")
        (vendor / "vendor.txt").write_text("base\n")
        self.git(vendor, "add", "vendor.txt")
        self.git(vendor, "commit", "-m", "vendor")

        self.git(self.root, "init")
        self.git_config(self.root)
        (self.root / "mono.vendor/patches/fixture").mkdir(parents=True)
        patch = self.root / "mono.vendor/patches/fixture/change.patch"
        patch.write_text("--- a/vendor.txt\n+++ b/vendor.txt\n@@ -1 +1 @@\n-base\n+patched\n")
        self.git(self.root, "-c", "protocol.file.allow=always", "submodule", "add", str(vendor), "mono.vendor/fixture")
        self.git(self.root, "add", "mono.vendor/patches/fixture/change.patch")
        self.git(self.root, "commit", "-m", "fixture")
        self.git(self.root, "-c", "protocol.file.allow=always", "submodule", "update", "--init", "--recursive")

        self.log = Path(self.temp.name) / "updates.log"
        self.reject = Path(self.temp.name) / "reject-updates"
        bin_dir = Path(self.temp.name) / "bin"
        bin_dir.mkdir()
        wrapper = bin_dir / "git"
        wrapper.write_text(
            "#!/bin/sh\n"
            "if [ \"$1\" = submodule ] && [ \"$2\" = update ]; then\n"
            f"  printf 'update\\n' >> '{self.log}'\n"
            f"  [ ! -e '{self.reject}' ] || exit 97\n"
            "fi\n"
            f"exec '{REAL_GIT}' \"$@\"\n"
        )
        wrapper.chmod(0o755)
        self.env = os.environ.copy()
        self.env["PATH"] = f"{bin_dir}{os.pathsep}{self.env['PATH']}"
        self.env["GIT_ALLOW_PROTOCOL"] = "file"

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def git_config(repo: Path):
        run([REAL_GIT, "config", "user.name", "Fixture"], repo)
        run([REAL_GIT, "config", "user.email", "fixture@example.invalid"], repo)

    @staticmethod
    def git(repo: Path, *args: str):
        return run([REAL_GIT, *args], repo)

    def invoke(self, *, check=True):
        return run(["bash", str(SCRIPT), "fixture"], self.root, check=check, env=self.env)

    def update_log(self):
        return self.log.read_text() if self.log.exists() else ""

    def test_matching_checkout_skips_update_and_preserves_stamp(self):
        self.invoke()
        stamp = (self.root / ".cache/vendor/fixture/.stamp").read_text()
        output = self.root / ".cache/vendor/fixture/vendor.txt"
        self.assertEqual(output.read_text(), "patched\n")
        self.reject.touch()
        result = self.invoke()
        self.assertEqual(result.stdout.strip(), ".cache/vendor/fixture")
        self.assertEqual(self.update_log(), "")
        self.assertEqual((self.root / ".cache/vendor/fixture/.stamp").read_text(), stamp)

    def test_changed_gitlink_runs_update(self):
        vendor = self.root / "mono.vendor/fixture"
        (self.vendor_origin / "extra.txt").write_text("new file\n")
        self.git(self.vendor_origin, "add", "extra.txt")
        self.git(self.vendor_origin, "commit", "-m", "vendor update")
        pinned = self.git(self.vendor_origin, "rev-parse", "HEAD").stdout.strip()
        self.git(self.root, "update-index", "--cacheinfo", f"160000,{pinned},mono.vendor/fixture")
        result = self.invoke(check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.git(vendor, "rev-parse", "HEAD").stdout.strip(), pinned)
        self.assertEqual(self.update_log(), "update\n", result.stderr)

    def test_uninitialized_nested_submodule_runs_update(self):
        self.git(self.root / "mono.vendor/fixture", "submodule", "deinit", "-f", "--", "nested")
        self.invoke()
        self.assertTrue((self.root / "mono.vendor/fixture/nested/child.txt").is_file())
        self.assertEqual(self.update_log(), "update\n")

    def test_nested_commit_mismatch_runs_update(self):
        nested = self.root / "mono.vendor/fixture/nested"
        (nested / "child.txt").write_text("different\n")
        self.git(nested, "add", "child.txt")
        self.git(nested, "commit", "-m", "local nested commit")
        self.invoke()
        self.assertEqual((nested / "child.txt").read_text(), "child\n")
        self.assertEqual(self.update_log(), "update\n")

    def test_dirty_tracked_vendor_is_still_refused(self):
        vendor = self.root / "mono.vendor/fixture"
        (vendor / "vendor.txt").write_text("hand edit\n")
        result = self.invoke(check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("has local edits", result.stderr)

    def test_patch_content_changes_stamp_and_materialized_tree(self):
        self.invoke()
        stamp_path = self.root / ".cache/vendor/fixture/.stamp"
        old_stamp = stamp_path.read_text()
        patch = self.root / "mono.vendor/patches/fixture/change.patch"
        patch.write_text("--- a/vendor.txt\n+++ b/vendor.txt\n@@ -1 +1 @@\n-base\n+changed patch\n")
        self.invoke()
        self.assertNotEqual(stamp_path.read_text(), old_stamp)
        self.assertEqual((self.root / ".cache/vendor/fixture/vendor.txt").read_text(), "changed patch\n")


if __name__ == "__main__":
    unittest.main()
