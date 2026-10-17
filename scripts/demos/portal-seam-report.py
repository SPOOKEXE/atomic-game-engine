#!/usr/bin/env python3
"""Summarise the temporal reference captures from ``PortalSeam.luau``.

The client writes a BMP and camera-state JSON sidecar for each frame passed to
``--capture-sequence``. This report keeps the image measurements from the old
still helper, and makes captured time, route revisions, and run settings
reviewable as one JSON document.
"""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter
from pathlib import Path
from typing import Any

try:
	from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageMath, ImageStat
except ImportError:  # pragma: no cover - a helper, not a dependency
	raise SystemExit("PIL is not installed; open the .bmp instead")


STRADDLER = (255, 214, 0)
CONTROL = (120, 230, 120)


def classify(pixel: tuple[int, int, int]) -> str | None:
	r, g, b = pixel[:3]
	total = r + g + b
	if total < 60:
		return None

	def near(colour: tuple[int, int, int]) -> bool:
		colour_total = sum(colour)
		return all(abs(value / total - expected / colour_total) < 0.06 for value, expected in zip((r, g, b), colour))

	if near(STRADDLER):
		return "straddler"
	if near(CONTROL):
		return "control"
	return None


def magenta(pixel: tuple[int, int, int]) -> bool:
	r, g, b = pixel[:3]
	return r > 90 and b > 70 and g < r * 0.7 and g < b * 0.8


def side_measurement(image: Image.Image) -> dict[str, Any]:
	counts = Counter(found for pixel in image.getdata() if (found := classify(pixel)))
	straddler = counts["straddler"]
	control = counts["control"]
	return {
		"straddler_pixels": straddler,
		"control_pixels": control,
		"ratio": straddler / control if control else None,
	}


def far_measurement(image: Image.Image) -> dict[str, Any]:
	width, height = image.size
	patch = [
		image.getpixel((x, y))
		for x in range(int(width * 0.55), int(width * 0.70))
		for y in range(int(height * 0.65), int(height * 0.78))
	]
	if not patch:
		return {"floor_red": None, "floor_blue": None, "sparks": 0}
	return {
		"floor_red": sum(pixel[0] for pixel in patch) / len(patch),
		"floor_blue": sum(pixel[2] for pixel in patch) / len(patch),
		"sparks": sum(1 for pixel in image.getdata() if magenta(pixel)),
	}


def front_measurement(image: Image.Image) -> dict[str, Any]:
	maximum_sample_width = 512
	scale = min(1.0, maximum_sample_width / image.width)
	width = max(1, round(image.width * scale))
	height = max(1, round(image.height * scale))
	sampled = image.resize((width, height)) if (width, height) != image.size else image
	positions = [
		(x, y)
		for y in range(sampled.height)
		for x in range(sampled.width)
		if classify(sampled.getpixel((x, y))) == "straddler"
	]
	return {
		"straddler_pixels": len(positions),
		"centroid_x": sum(position[0] for position in positions) / len(positions) / scale if positions else None,
		"centroid_y": sum(position[1] for position in positions) / len(positions) / scale if positions else None,
	}


def sidecar_for(image: Path) -> dict[str, Any] | None:
	path = image.with_suffix(".json")
	if not path.is_file():
		return None
	try:
		loaded = json.loads(path.read_text(encoding="utf-8"))
	except (OSError, json.JSONDecodeError):
		return None
	return loaded if isinstance(loaded, dict) else None


def seam_revisions(frame: dict[str, Any] | None) -> list[int]:
	if frame is None:
		return []
	revisions: set[int] = set()
	for portal in frame.get("portal_views", []):
		capture = portal.get("capture") if isinstance(portal, dict) else None
		if isinstance(capture, dict) and isinstance(capture.get("seam_revision"), int):
			revisions.add(capture["seam_revision"])
	eye = frame.get("eye_capture")
	if isinstance(eye, dict) and isinstance(eye.get("seam_revision"), int):
		revisions.add(eye["seam_revision"])
	return sorted(revisions)


def capture_route_and_view(image: Path) -> tuple[str, str]:
	name = image.parent.name
	reference_prefix = "portal-seam-reference-"
	portal_prefix = "portal-seam-"
	if name.startswith(reference_prefix):
		return "reference", name[len(reference_prefix) :]
	if name.startswith(portal_prefix):
		return "portal", name[len(portal_prefix) :]
	return "still", "still"


def straddler_mask(image: Image.Image) -> Image.Image:
	# Use yellow hue and low blue chroma so lighting does not hide the same body
	# in one capture. The red-to-green ratio excludes the orange floor.
	red, green, blue = image.convert("RGB").split()
	return ImageMath.eval(
		"convert(((r * 100 > g * 55) & (r * 100 < g * 155) & (b * 2 < r) & (b * 2 < g)) * 255, 'L')",
		r=red,
		g=green,
		b=blue,
	)


def nearby_opaque(mask: Image.Image, position_tolerance_pixels: int) -> Image.Image:
	if position_tolerance_pixels == 0:
		return mask
	return mask.filter(ImageFilter.MaxFilter(position_tolerance_pixels * 2 + 1))


def nonzero_pixels(image: Image.Image) -> int:
	return sum(image.histogram()[1:])


def projected_aperture_mask(size: tuple[int, int], frame: dict[str, Any] | None) -> Image.Image | None:
	if frame is None or not isinstance(frame.get("camera"), dict):
		return None
	camera = frame["camera"]
	position = camera.get("position")
	rotation = camera.get("rotation")
	fov = frame.get("field_of_view")
	if not (
		isinstance(position, list) and len(position) == 3
		and isinstance(rotation, list) and len(rotation) == 4
		and isinstance(fov, (int, float)) and 0 < fov < math.pi
	):
		return None
	qx, qy, qz, qw = (-rotation[0], -rotation[1], -rotation[2], rotation[3])
	tangent = math.tan(fov / 2)
	width, height = size
	mask = Image.new("L", size, 0)
	draw = ImageDraw.Draw(mask)
	projected = 0
	for portal in frame.get("portal_views", []):
		if not isinstance(portal, dict) or portal.get("external"):
			continue
		centre, first, second, normal = (
			portal.get("centre"), portal.get("first"), portal.get("second"), portal.get("normal")
		)
		if not all(isinstance(value, list) and len(value) == 3 for value in (centre, first, second, normal)):
			continue
		if sum(normal[index] * (position[index] - centre[index]) for index in range(3)) <= 0:
			continue
		corners: list[tuple[float, float]] = []
		for along, up in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
			world = [centre[i] + along * first[i] + up * second[i] - position[i] for i in range(3)]
			t = [
				2 * (qy * world[2] - qz * world[1]),
				2 * (qz * world[0] - qx * world[2]),
				2 * (qx * world[1] - qy * world[0]),
			]
			view = [
				world[0] + qw * t[0] + qy * t[2] - qz * t[1],
				world[1] + qw * t[1] + qz * t[0] - qx * t[2],
				world[2] + qw * t[2] + qx * t[1] - qy * t[0],
			]
			if view[2] >= -0.001:
				corners = []
				break
			corners.append((
				(1 + view[0] / (-view[2] * tangent * width / height)) * width / 2,
				(1 - view[1] / (-view[2] * tangent)) * height / 2,
			))
		if corners:
			draw.polygon(corners, fill=255)
			projected += 1
	if not projected:
		return None
	mask = mask.filter(ImageFilter.MinFilter(3))
	return mask if nonzero_pixels(mask) else None


def full_reference_measurement(
	portal_path: Path, reference_path: Path, position_tolerance_pixels: int, aperture_only: bool = False
) -> dict[str, Any]:
	portal = Image.open(portal_path).convert("RGB")
	reference = Image.open(reference_path).convert("RGB")
	if portal.size != reference.size:
		return {
			"reference_image": str(reference_path),
			"comparable": False,
			"reason": "resolution mismatch",
			"portal_resolution": {"width": portal.width, "height": portal.height},
			"reference_resolution": {"width": reference.width, "height": reference.height},
		}

	portal_mask = straddler_mask(portal)
	reference_mask = straddler_mask(reference)
	aperture = projected_aperture_mask(portal.size, sidecar_for(portal_path)) if aperture_only else None
	if aperture_only and aperture is None:
		return {"reference_image": str(reference_path), "comparable": False, "reason": "no projected portal aperture"}
	if aperture is not None:
		portal_mask = ImageChops.multiply(portal_mask, aperture)
		reference_mask = ImageChops.multiply(reference_mask, aperture)
	portal_near = nearby_opaque(portal_mask, position_tolerance_pixels)
	reference_near = nearby_opaque(reference_mask, position_tolerance_pixels)
	uncovered = ImageChops.subtract(reference_mask, portal_near)
	duplicate = ImageChops.subtract(portal_mask, reference_near)
	colour_delta = ImageChops.difference(portal, reference)
	colour_stats = ImageStat.Stat(colour_delta, aperture)
	return {
		"reference_image": str(reference_path),
		"comparable": True,
		"position_tolerance_pixels": position_tolerance_pixels,
		"comparison_region": "projected_aperture" if aperture_only else "full_frame",
		"aperture_pixels": nonzero_pixels(aperture) if aperture is not None else None,
		"reference_body_samples": nonzero_pixels(reference_mask),
		"portal_body_samples": nonzero_pixels(portal_mask),
		"uncovered_body_samples": nonzero_pixels(uncovered),
		"duplicate_body_samples": nonzero_pixels(duplicate),
		"mean_channel_delta": list(colour_stats.mean),
		"maximum_channel_delta": [channel[1] for channel in colour_stats.extrema],
	}


def measurement(image_path: Path) -> dict[str, Any]:
	image = Image.open(image_path).convert("RGB")
	frame = sidecar_for(image_path)
	route, view = capture_route_and_view(image_path)
	result: dict[str, Any] = {
		"image": str(image_path),
		"route": route,
		"view": view,
		"resolution": {"width": image.width, "height": image.height},
		"presentation_time": {
			"frame": frame.get("frame") if frame else None,
			"capture_seconds": frame.get("seconds") if frame else None,
			"simulation_tick": frame.get("tick") if frame else None,
			"simulation_alpha": frame.get("alpha") if frame else None,
		},
		"topology_revisions": seam_revisions(frame),
		"pipeline": frame.get("pipeline") if frame else None,
		"render_metrics": {
			"previous_render_uploaded_bytes": frame.get("previous_render_uploaded_bytes"),
			"portal_import": frame.get("portal_import"),
			"portal_inbox": frame.get("portal_inbox"),
			"portal_capture_ages_ms": [
				capture["accepted_age_ms"]
				for portal in frame.get("portal_views", [])
				if isinstance(portal, dict)
				for capture in [portal.get("capture")]
				if isinstance(capture, dict) and isinstance(capture.get("accepted_age_ms"), (int, float))
			],
		} if frame else None,
		"metadata_available": frame is not None,
	}
	result["straddler_body_pixels"] = nonzero_pixels(straddler_mask(image))
	if view == "side":
		result["side"] = side_measurement(image)
	elif view == "far":
		result["far"] = far_measurement(image)
	elif view == "front":
		result["front"] = front_measurement(image)
	return result


def capture_images(inputs: list[Path]) -> list[Path]:
	images: set[Path] = set()
	for entry in inputs:
		if entry.is_dir():
			images.update(entry.glob("*.bmp"))
		elif entry.suffix.lower() == ".bmp" and entry.is_file():
			images.add(entry)
	return sorted(
		images,
		key=lambda path: (*capture_route_and_view(path), int(path.stem) if path.stem.isdecimal() else path.stem),
	)


def matched_presentation_time(
	portal: dict[str, Any], reference: dict[str, Any], alpha_tolerance: float
) -> dict[str, Any]:
	portal_time = portal["presentation_time"]
	reference_time = reference["presentation_time"]
	portal_tick = portal_time["simulation_tick"]
	reference_tick = reference_time["simulation_tick"]
	portal_alpha = portal_time["simulation_alpha"]
	reference_alpha = reference_time["simulation_alpha"]
	valid = (
		isinstance(portal_tick, int)
		and isinstance(reference_tick, int)
		and isinstance(portal_alpha, (int, float))
		and isinstance(reference_alpha, (int, float))
	)
	alpha_difference = abs(portal_alpha - reference_alpha) if valid else None
	return {
		"portal_tick": portal_tick,
		"reference_tick": reference_tick,
		"alpha_difference": alpha_difference,
		"alpha_tolerance": alpha_tolerance,
		"matched": valid and portal_tick == reference_tick and alpha_difference <= alpha_tolerance,
	}


def reference_pairs(
	rows: list[dict[str, Any]], position_tolerance_pixels: int, alpha_tolerance: float, aperture_only: bool = False
) -> list[dict[str, Any]]:
	references = {
		(row["view"], row["presentation_time"]["frame"]): row
		for row in rows
		if row["route"] == "reference" and isinstance(row["presentation_time"]["frame"], int)
	}
	pairs: list[dict[str, Any]] = []
	for row in rows:
		frame = row["presentation_time"]["frame"]
		if row["route"] != "portal" or not isinstance(frame, int):
			continue
		reference = references.get((row["view"], frame))
		if reference is None:
			continue
		pairs.append(
			{
				"view": row["view"],
				"frame": frame,
				"portal_image": row["image"],
				"presentation_time": matched_presentation_time(row, reference, alpha_tolerance),
				**full_reference_measurement(Path(row["image"]), Path(reference["image"]), position_tolerance_pixels, aperture_only),
			}
		)
	return pairs


def moving_front_seam(rows: list[dict[str, Any]], minimum_pixels: float) -> dict[str, Any]:
	samples = [
		row["front"]
		for row in rows
		if row["route"] == "portal" and row["view"] == "front" and row.get("front", {}).get("centroid_x") is not None
	]
	centres = [sample["centroid_x"] for sample in samples]
	span = max(centres) - min(centres) if centres else 0.0
	return {
		"sample_count": len(samples),
		"centroid_x_span_pixels": span,
		"minimum_motion_pixels": minimum_pixels,
		"observed": len(samples) >= 2 and span >= minimum_pixels,
	}


def measured_capture_rates(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
	sequences: dict[tuple[str, str], list[float]] = {}
	for row in rows:
		seconds = row["presentation_time"]["capture_seconds"]
		if isinstance(seconds, (int, float)):
			sequences.setdefault((row["route"], row["view"]), []).append(float(seconds))
	return [
		{
			"route": route,
			"view": view,
			"captured_frames": len(times),
			"elapsed_seconds": times[-1] - times[0] if len(times) >= 2 else None,
			"measured_fps": (len(times) - 1) / (times[-1] - times[0]) if len(times) >= 2 and times[-1] > times[0] else None,
		}
		for (route, view), times in sorted(sequences.items())
	]


def full_reference_passes(recorded: dict[str, Any], body_mismatch_maximum: int) -> bool:
	expected = sum(
		1 for row in recorded["captures"] if row["route"] == "portal" and row["view"] == "front"
	)
	paired = recorded["full_reference"]
	return (
		expected != 0
		and len(paired) == expected
		and all(entry["comparable"] and entry["presentation_time"]["matched"] for entry in paired)
		and all(
			entry["uncovered_body_samples"] + entry["duplicate_body_samples"] <= body_mismatch_maximum
			for entry in paired
		)
	)


def measured_render_usage(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
	sequences: dict[tuple[str, str], list[dict[str, Any]]] = {}
	for row in rows:
		if isinstance(row.get("render_metrics"), dict):
			sequences.setdefault((row["route"], row["view"]), []).append(row["render_metrics"])
	result = []
	for (route, view), samples in sorted(sequences.items()):
		uploads = [
			sample["previous_render_uploaded_bytes"] for sample in samples
			if isinstance(sample.get("previous_render_uploaded_bytes"), int)
		]
		imports = [sample["portal_import"] for sample in samples if isinstance(sample.get("portal_import"), dict)]
		inboxes = [sample["portal_inbox"] for sample in samples if isinstance(sample.get("portal_inbox"), dict)]
		ages = [age for sample in samples for age in sample["portal_capture_ages_ms"]]
		result.append({
			"route": route,
			"view": view,
			"previous_render_uploaded_bytes_mean": sum(uploads) / len(uploads) if uploads else None,
			"previous_render_uploaded_bytes_max": max(uploads) if uploads else None,
			"portal_import_uploaded_bytes_start": imports[0].get("uploaded_bytes") if imports else None,
			"portal_import_uploaded_bytes_end": imports[-1].get("uploaded_bytes") if imports else None,
			"portal_inbox_pending_count_max": max((entry.get("pending_count", 0) for entry in inboxes), default=None),
			"portal_inbox_held_count_max": max((entry.get("held_count", 0) for entry in inboxes), default=None),
			"portal_inbox_decoded_bytes_start": inboxes[0].get("decoded_bytes") if inboxes else None,
			"portal_inbox_decoded_bytes_end": inboxes[-1].get("decoded_bytes") if inboxes else None,
			"portal_inbox_stale_rejections_end": inboxes[-1].get("stale_rejections") if inboxes else None,
			"portal_inbox_pending_capacity_max": max((entry.get("pending_capacity", 0) for entry in inboxes), default=None),
			"portal_capture_age_ms_max": max(ages) if ages else None,
		})
	return result


def report(
	images: list[Path],
	fixture: str,
	backend: str,
	pipeline_revision: str,
	side_ratio_maximum: float,
	position_tolerance_pixels: int = 1,
	front_motion_minimum_pixels: float = 2.0,
	alpha_tolerance: float = 0.05,
	target_fps: int | None = None,
	aperture_only: bool = False,
) -> dict[str, Any]:
	rows = [measurement(image) for image in images]
	frames = [row["presentation_time"]["frame"] for row in rows]
	valid_pipelines = [
		row["pipeline"] for row in rows
		if isinstance(row["pipeline"], dict)
		and isinstance(row["pipeline"].get("name"), str)
		and isinstance(row["pipeline"].get("revision"), int)
	]
	identities = {
		(pipeline["name"], pipeline["revision"])
		for pipeline in valid_pipelines
	}
	pipeline_identity_complete = len(rows) > 0 and len(valid_pipelines) == len(rows) and len(identities) == 1
	if pipeline_identity_complete:
		observed_revision = str(next(iter(identities))[1])
		if pipeline_revision not in ("unknown", observed_revision):
			raise ValueError("declared pipeline revision differs from capture sidecars")
		pipeline_revision = observed_revision
	return {
		"fixture": fixture,
		"backend": backend,
		"pipeline_revision": pipeline_revision,
		"pipeline_identities": [
			{"name": name, "revision": revision} for name, revision in sorted(identities)
		],
		"target_fps": target_fps,
		"tolerance": {
			"side_ratio_maximum": side_ratio_maximum,
			"position_tolerance_pixels": position_tolerance_pixels,
			"front_motion_minimum_pixels": front_motion_minimum_pixels,
			"presentation_alpha_tolerance": alpha_tolerance,
		},
		"captures": rows,
		"full_reference": reference_pairs(rows, position_tolerance_pixels, alpha_tolerance, aperture_only),
		"moving_front_seam": moving_front_seam(rows, front_motion_minimum_pixels),
		"capture_rates": measured_capture_rates(rows),
		"render_usage": measured_render_usage(rows),
		"sequence": {
			"capture_count": len(rows),
			"metadata_complete": all(row["metadata_available"] for row in rows),
			"pipeline_identity_complete": pipeline_identity_complete,
			"first_frame": min((frame for frame in frames if isinstance(frame, int)), default=None),
			"last_frame": max((frame for frame in frames if isinstance(frame, int)), default=None),
		},
	}


def print_row(row: dict[str, Any], side_ratio_maximum: float) -> None:
	time = row["presentation_time"]
	print(f"  {row['image']} frame={time['frame']} tick={time['simulation_tick']} revisions={row['topology_revisions']}")
	if side := row.get("side"):
		ratio = side["ratio"]
		verdict = "missing control"
		if ratio is not None:
			verdict = "cut" if ratio < side_ratio_maximum else "WHOLE - the seam is drawing twice"
		print(f"    straddler {side['straddler_pixels']:>7} px control {side['control_pixels']:>7} px ratio {ratio!s:>7} {verdict}")
	if far := row.get("far"):
		print(f"    far floor R {far['floor_red']!s:>5} B {far['floor_blue']!s:>5} sparks {far['sparks']}")
	if front := row.get("front"):
		print(f"    front straddler {front['straddler_pixels']:>7} px centroid x {front['centroid_x']!s}")


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("captures", nargs="+", type=Path, help="BMP files or capture-sequence directories")
	parser.add_argument("--fixture", default="PortalSeam")
	parser.add_argument("--backend", default="unknown", help="runtime backend name, or unknown")
	parser.add_argument("--pipeline-revision", default="unknown", help="runtime pipeline revision, or unknown")
	parser.add_argument("--target-fps", type=int, help="requested presentation cap; not measured output FPS")
	parser.add_argument("--expected-alpha", type=float, help="required capture interpolation phase")
	parser.add_argument("--side-ratio-maximum", type=float, default=0.75)
	parser.add_argument("--position-tolerance-pixels", type=int, default=1)
	parser.add_argument("--front-motion-minimum-pixels", type=float, default=2.0)
	parser.add_argument("--presentation-alpha-tolerance", type=float, default=0.05)
	parser.add_argument("--body-mismatch-maximum", type=int, default=0)
	parser.add_argument("--require-moving-front", action="store_true")
	parser.add_argument("--require-full-reference", action="store_true")
	parser.add_argument("--require-pipeline-identity", action="store_true")
	parser.add_argument("--aperture-only", action="store_true", help="compare the body within the projected portal aperture")
	parser.add_argument("--output", type=Path, help="write the complete machine-readable report here")
	args = parser.parse_args()
	if not 0.0 < args.side_ratio_maximum <= 1.0:
		parser.error("--side-ratio-maximum must be within (0, 1]")
	if args.position_tolerance_pixels < 0:
		parser.error("--position-tolerance-pixels must be non-negative")
	if args.front_motion_minimum_pixels < 0.0:
		parser.error("--front-motion-minimum-pixels must be non-negative")
	if args.presentation_alpha_tolerance < 0.0:
		parser.error("--presentation-alpha-tolerance must be non-negative")
	if args.body_mismatch_maximum < 0:
		parser.error("--body-mismatch-maximum must be non-negative")
	if args.target_fps is not None and args.target_fps <= 0:
		parser.error("--target-fps must be positive")
	if args.expected_alpha is not None and not 0.0 <= args.expected_alpha < 1.0:
		parser.error("--expected-alpha must be within [0, 1)")
	images = capture_images(args.captures)
	if not images:
		parser.error("no BMP captures found")
	recorded = report(
		images,
		args.fixture,
		args.backend,
		args.pipeline_revision,
		args.side_ratio_maximum,
		args.position_tolerance_pixels,
		args.front_motion_minimum_pixels,
		args.presentation_alpha_tolerance,
		args.target_fps,
		args.aperture_only,
	)
	if args.expected_alpha is not None:
		recorded["capture_alpha"] = args.expected_alpha
		recorded["sequence"]["fixed_alpha_match"] = all(
			isinstance((alpha := row["presentation_time"]["simulation_alpha"]), (int, float))
			and abs(alpha - args.expected_alpha) <= 0.0001
			for row in recorded["captures"]
		)
	for row in recorded["captures"]:
		print_row(row, args.side_ratio_maximum)
	print(json.dumps(recorded["sequence"], sort_keys=True))
	if args.output:
		args.output.write_text(json.dumps(recorded, indent=2, sort_keys=True) + "\n", encoding="utf-8")
	if args.expected_alpha is not None and not recorded["sequence"]["fixed_alpha_match"]:
		raise SystemExit("capture interpolation phase differed from --expected-alpha")
	if args.require_full_reference:
		if not full_reference_passes(recorded, args.body_mismatch_maximum):
			raise SystemExit("portal body silhouette or fixed-step phase differed from the matched-room reference")
	if args.require_pipeline_identity and not recorded["sequence"]["pipeline_identity_complete"]:
		raise SystemExit("capture pipeline identity was absent or changed within the sequence")
	if args.require_moving_front and not recorded["moving_front_seam"]["observed"]:
		raise SystemExit("front seam did not move far enough in the portal capture sequence")


if __name__ == "__main__":
	main()
