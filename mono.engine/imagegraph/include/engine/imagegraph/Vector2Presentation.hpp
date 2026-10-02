#pragma once

// Resolved source Vector2 controls contain no editor, device or wall-clock state.
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	// An owned snapshot of one source Vector2 node after timeline and processor execution.
	struct Vector2Presentation {
		// Computed horizontal component, after optional source integer rounding.
		double X = 0;
		// Computed vertical component, after optional source integer rounding.
		double Y = 0;
		// Source integer rounding is enabled for the selected row.
		bool Integer = false;
		// Source display choice: zero is Number and one is Coordinate; fractions are retained.
		double DisplayType = 0;
		// The source permits the gizmo in the global preview.
		bool ShowOnGlobal = false;
		// Gizmo translation in project pixels, before relative component scaling.
		Vector2 Offset{};
		// Source gizmo visual scale, independent of computed vector components.
		double Scale = 1;
		// Source style choice: zero is Default, one is Shapes and two is Sprite.
		double Style = 0;
		// Source shape choice: zero is Rectangle and one is Ellipse.
		double Shape = 0;
		// Shape or sprite display dimensions in project pixels.
		Vector2 Size{32, 32};
		// Computed components use project width and height for gizmo placement.
		bool RelativeUnit = false;
		// Resolved project surface width in pixels.
		uint32_t ProjectWidth = 32;
		// Resolved project surface height in pixels.
		uint32_t ProjectHeight = 32;
		// Number of successful executions in the actual source processor schedule.
		uint64_t ProcessorCount = 0;
		// Horizontal component has an effective incoming graph link.
		bool XLinked = false;
		// Vertical component has an effective incoming graph link.
		bool YLinked = false;
		// Owned sprite pixels, present only for one processor row with Sprite style and an image input.
		std::optional<Image> Sprite;
	};
	// Uses actual timeline, link resolution and processor execution. Failure leaves result unchanged.
	Status ResolveVector2Presentation(
		const Document &document,
		const std::string &nodeId,
		const EvaluationRequest &request,
		Vector2Presentation &result,
		Diagnostic &diagnostic
	);
}
