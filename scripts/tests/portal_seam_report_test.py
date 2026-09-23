#!/usr/bin/env python3
"""Focused checks for the PortalSeam capture report."""

from __future__ import annotations

from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import json
import subprocess
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

	def test_capture_rate_uses_recorded_times(self) -> None:
		rows = [
			{"route": "portal", "view": "front", "presentation_time": {"capture_seconds": seconds}}
			for seconds in (10.0, 10.02, 10.04)
		]
		rates = MODULE.measured_capture_rates(rows)
		self.assertEqual(len(rates), 1)
		self.assertEqual(rates[0]["captured_frames"], 3)
		self.assertAlmostEqual(rates[0]["measured_fps"], 50.0)

	def test_cli_rejects_a_different_capture_phase(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			image = Path(temporary) / "0.bmp"
			Image.new("RGB", (2, 2), (0, 0, 0)).save(image)
			image.with_suffix(".json").write_text(
				json.dumps({"frame": 0, "tick": 0, "alpha": 0.25}), encoding="utf-8"
			)
			command = [sys.executable, str(ROOT / "scripts/demos/portal-seam-report.py")]
			accepted = subprocess.run(
				[*command, "--expected-alpha", "0.25", str(image)], capture_output=True, text=True
			)
			refused = subprocess.run(
				[*command, "--expected-alpha", "0.5", str(image)], capture_output=True, text=True
			)
			self.assertEqual(accepted.returncode, 0)
			self.assertNotEqual(refused.returncode, 0)
			self.assertIn("capture interpolation phase differed", refused.stderr)

	def test_aperture_comparison_excludes_body_behind_the_wall(self) -> None:
		with tempfile.TemporaryDirectory() as temporary:
			root = Path(temporary)
			portal = root / "portal-seam-front"
			reference = root / "portal-seam-reference-front"
			portal.mkdir()
			reference.mkdir()
			for sequence in (portal, reference):
				picture = Image.new("RGB", (100, 100), (0, 0, 0))
				picture.putpixel((50, 50), (255, 214, 0))
				if sequence == reference:
					picture.putpixel((10, 50), (255, 214, 0))
				picture.save(sequence / "0.bmp")
			frame = {
				"camera": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1]},
				"field_of_view": 1.5707963267948966,
				"portal_views": [{
					"external": False,
					"centre": [0, 0, -5],
					"first": [1, 0, 0],
					"second": [0, 1, 0],
					"normal": [0, 0, 1],
				}],
			}
			(portal / "0.json").write_text(json.dumps(frame), encoding="utf-8")
			bounded = MODULE.full_reference_measurement(portal / "0.bmp", reference / "0.bmp", 0, True)
			full = MODULE.full_reference_measurement(portal / "0.bmp", reference / "0.bmp", 0)
			self.assertEqual(bounded["comparison_region"], "projected_aperture")
			self.assertGreater(bounded["aperture_pixels"], 0)
			self.assertEqual(bounded["uncovered_body_samples"], 0)
			self.assertEqual(full["uncovered_body_samples"], 1)


if __name__ == "__main__":
	unittest.main()
