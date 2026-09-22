#!/usr/bin/env python3
"""Focused checks for the PortalSeam capture report."""

from __future__ import annotations

from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import json
import sys
import tempfile
import unittest

from PIL import Image


ROOT = Path(__file__).resolve().parents[2]
SPEC = spec_from_file_location("portal_seam_report", ROOT / "scripts/demos/portal-seam-report.py")
if SPEC is None or SPEC.loader is None:
	raise RuntimeError("could not load scripts/demos/portal-seam-report.py")
MODULE = module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class PortalSeamReportTest(unittest.TestCase):
	def test_sequence_records_temporal_and_topology_metadata(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			sequence = Path(temporary) / "portal-seam-side"
			sequence.mkdir()
			image = Image.new("RGB", (20, 20), (0, 0, 0))
			for x in range(10):
				for y in range(10):
					image.putpixel((x, y), (255, 214, 0))
			for x in range(10, 20):
				for y in range(10):
					image.putpixel((x, y), (120, 230, 120))
			image.save(sequence / "7.bmp")
			(sequence / "7.json").write_text(
				json.dumps(
					{
						"frame": 7,
						"seconds": 2.0,
						"tick": 120,
						"alpha": 0.5,
						"eye_capture": {"seam_revision": 3},
						"portal_views": [{"capture": {"seam_revision": 5}}],
					}
				),
				encoding="utf-8",
			)

			recorded = MODULE.report([sequence / "7.bmp"], "PortalSeam", "vulkan", "pipeline-9", 0.75)
			row = recorded["captures"][0]
			self.assertEqual(recorded["fixture"], "PortalSeam")
			self.assertEqual(recorded["backend"], "vulkan")
			self.assertEqual(recorded["pipeline_revision"], "pipeline-9")
			self.assertEqual(row["presentation_time"], {
				"frame": 7,
				"capture_seconds": 2.0,
				"simulation_tick": 120,
				"simulation_alpha": 0.5,
			})
			self.assertEqual(row["topology_revisions"], [3, 5])
			self.assertEqual(row["side"]["ratio"], 1.0)
			self.assertTrue(recorded["sequence"]["metadata_complete"])

	def test_reference_comparison_and_moving_front_seam(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			root = Path(temporary)
			portal = root / "portal-seam-front"
			reference = root / "portal-seam-reference-front"
			portal.mkdir()
			reference.mkdir()
			for frame, left in ((0, 4), (1, 10)):
				portal_image = Image.new("RGB", (24, 16), (0, 0, 0))
				reference_image = Image.new("RGB", (24, 16), (0, 0, 0))
				for x in range(left, left + 4):
					for y in range(5, 10):
						portal_image.putpixel((x, y), (255, 214, 0))
						reference_image.putpixel((x + 1, y), (255, 214, 0))
				portal_image.save(portal / f"{frame}.bmp")
				reference_image.save(reference / f"{frame}.bmp")
				for sequence in (portal, reference):
					(sequence / f"{frame}.json").write_text(
						json.dumps({"frame": frame, "tick": 120 + frame, "alpha": 0.5}), encoding="utf-8"
					)

			recorded = MODULE.report(
				MODULE.capture_images([portal, reference]),
				"PortalSeam",
				"vulkan",
				"pipeline-9",
				0.75,
				1,
				2.0,
			)
			self.assertEqual(len(recorded["full_reference"]), 2)
			self.assertTrue(all(entry["comparable"] for entry in recorded["full_reference"]))
			self.assertTrue(
				all(entry["uncovered_body_samples"] == 0 and entry["duplicate_body_samples"] == 0 for entry in recorded["full_reference"])
			)
			self.assertTrue(all(entry["presentation_time"]["matched"] for entry in recorded["full_reference"]))
			self.assertTrue(MODULE.full_reference_passes(recorded, 0))
			self.assertTrue(recorded["moving_front_seam"]["observed"])
			self.assertGreater(recorded["moving_front_seam"]["centroid_x_span_pixels"], 2.0)

	def test_missing_body_over_an_opaque_floor_is_a_reference_mismatch(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			root = Path(temporary)
			portal = root / "portal-seam-front"
			reference = root / "portal-seam-reference-front"
			portal.mkdir()
			reference.mkdir()
			floor = (40, 60, 90)
			portal_image = Image.new("RGB", (24, 16), floor)
			reference_image = Image.new("RGB", (24, 16), floor)
			for x in range(8, 12):
				for y in range(5, 10):
					reference_image.putpixel((x, y), (255, 214, 0))
			portal_image.save(portal / "0.bmp")
			reference_image.save(reference / "0.bmp")
			for sequence in (portal, reference):
				(sequence / "0.json").write_text(json.dumps({"frame": 0, "tick": 120, "alpha": 0.5}), encoding="utf-8")

			recorded = MODULE.report(
				MODULE.capture_images([portal, reference]), "PortalSeam", "vulkan", "pipeline-9", 0.75
			)
			comparison = recorded["full_reference"][0]
			self.assertGreater(comparison["uncovered_body_samples"], 0)
			self.assertEqual(comparison["portal_body_samples"], 0)
			self.assertFalse(MODULE.full_reference_passes(recorded, 0))

	def test_reference_requires_the_same_fixed_step_phase(self) -> None:
		portal = {"presentation_time": {"simulation_tick": 120, "simulation_alpha": 0.5}}
		reference = {"presentation_time": {"simulation_tick": 121, "simulation_alpha": 0.5}}
		comparison = MODULE.matched_presentation_time(portal, reference, 0.05)
		self.assertFalse(comparison["matched"])


if __name__ == "__main__":
	unittest.main()
