#pragma once

#include <engine/imagegraph/Document.hpp>

#include <span>
#include <vector>

namespace studio {

	struct Vector2PadView {
		double MinimumX = -1;
		double MaximumX = 1;
		double MinimumY = -1;
		double MaximumY = 1;
	};

	struct Vector2EditorRect {
		double X = 0;
		double Y = 0;
		double Width = 0;
		double Height = 0;
	};

	using Vector2PreviewGuide = engine::imagegraph::PreviewRulerGuide;

	struct Vector2PreviewSnap {
		bool ShowGrid = false;
		bool SnapGrid = false;
		engine::imagegraph::Vector2 GridSize{16, 16};
		bool ShowRulers = false;
		std::span<const Vector2PreviewGuide> Guides;
	};

	// The pad uses upward Y; preview overlays use downward Y, as in the source editor.
	bool Vector2PadCoordinate(
		const Vector2PadView &view,
		const Vector2EditorRect &rect,
		engine::imagegraph::Vector2 mouse,
		bool control,
		engine::imagegraph::Vector2 &coordinate
	);
	bool
	PanVector2Pad(Vector2PadView &view, engine::imagegraph::Vector2 delta, const Vector2EditorRect &rect);
	bool ZoomVector2Pad(Vector2PadView &view, double wheel);
	bool FocusVector2Pad(Vector2PadView &view, engine::imagegraph::Vector2 value);
	double Vector2EditorRound(double value);
	bool SnapVector2Preview(
		engine::imagegraph::Vector2 &value, const Vector2PreviewSnap &snap, double previewScale, bool control
	);
	bool Vector2OverlayPosition(
		engine::imagegraph::Vector2 value,
		engine::imagegraph::Vector2 factor,
		engine::imagegraph::Vector2 offset,
		engine::imagegraph::Vector2 origin,
		double scale,
		engine::imagegraph::Vector2 &position
	);
	bool Vector2OverlayDrag(
		engine::imagegraph::Vector2 start,
		engine::imagegraph::Vector2 mouseDelta,
		engine::imagegraph::Vector2 factor,
		double scale,
		const Vector2PreviewSnap &snap,
		bool control,
		engine::imagegraph::Vector2 &value
	);
	bool HitVector2Overlay(
		engine::imagegraph::Vector2 mouse,
		engine::imagegraph::Vector2 position,
		double style,
		engine::imagegraph::Vector2 size,
		double gizmoScale,
		double previewScale
	);
	// Charges simultaneous CPU, upload and texture payloads without overflowing byte counters.
	bool ReserveVector2Sprite(
		size_t heldBytes,
		size_t imageBytes,
		size_t simultaneousCopies,
		size_t budget = engine::imagegraph::Limits::MaximumEvaluationBytes
	);

	// Both source components land together; a refused component preserves the entire document.
	bool SetImageGraphVector2Coordinates(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		engine::imagegraph::Vector2 value,
		engine::imagegraph::Diagnostic &diagnostic
	);
	// Animated axes author the exact signed frame; unanimated axes keep their base values.
	bool SetImageGraphVector2CoordinatesAtFrame(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		engine::imagegraph::Vector2 value,
		uint64_t tick,
		double subframe,
		engine::imagegraph::Diagnostic &diagnostic,
		bool negativeFrame = false
	);
}
