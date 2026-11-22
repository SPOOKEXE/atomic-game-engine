#pragma once

// How a node graph view looks, as plain data. Painters read it; nothing here draws.
//
// Presets ship as functions returning data so a host or script can start from
// one and override fields. Changing a theme marks the view dirty once; nothing
// animates.
// @tier L4 · shared

#include <engine/ang/Value.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace engine::angview {

	// Straight sRGB colour with alpha, in [0, 1].
	struct Rgba {
		float R = 0.0f;
		float G = 0.0f;
		float B = 0.0f;
		float A = 1.0f;

		friend bool operator==(const Rgba &, const Rgba &) = default;
	};

	enum class PortShape : uint8_t { Circle, Square, Diamond };
	enum class WireStyle : uint8_t { Bezier, Straight, Orthogonal };

	struct Theme {
		std::string Name{};

		Rgba Background{0.0f, 0.0f, 0.0f, 1.0f};
		bool ShowGrid = true;
		Rgba Grid{1.0f, 1.0f, 1.0f, 0.06f};
		Rgba GridMajor{1.0f, 1.0f, 1.0f, 0.12f};
		float GridSpacing = 24.0f;
		// Every Nth grid line is major.
		int GridMajorEvery = 8;

		bool ShowHeaders = true;
		Rgba NodeBody{0.08f, 0.08f, 0.08f, 1.0f};
		Rgba NodeHeader{0.16f, 0.16f, 0.16f, 1.0f};
		Rgba NodeBorder{0.30f, 0.30f, 0.30f, 1.0f};
		Rgba NodeBorderHovered{0.60f, 0.60f, 0.60f, 1.0f};
		Rgba NodeBorderSelected{1.0f, 1.0f, 1.0f, 1.0f};
		Rgba NodeFailed{0.90f, 0.25f, 0.25f, 1.0f};
		Rgba NodeWaiting{0.90f, 0.70f, 0.20f, 1.0f};
		Rgba NodeBypassed{0.45f, 0.45f, 0.45f, 1.0f};
		Rgba GroupFill{1.0f, 1.0f, 1.0f, 0.04f};
		Rgba GroupBorder{1.0f, 1.0f, 1.0f, 0.20f};

		Rgba Text{1.0f, 1.0f, 1.0f, 1.0f};
		Rgba TextDim{0.65f, 0.65f, 0.65f, 1.0f};
		float FontScale = 1.0f;
		float FontSize = 13.0f;

		float NodeWidth = 160.0f;
		float HeaderHeight = 24.0f;
		float RowHeight = 20.0f;
		float NodePadding = 6.0f;
		float CornerRadius = 4.0f;
		float BorderWidth = 1.0f;
		float SelectedBorderWidth = 2.0f;

		float PortRadius = 5.0f;
		PortShape DefaultPortShape = PortShape::Circle;
		// Per data type, e.g. {"image", Square}.
		std::map<ang::TypeId, PortShape> PortShapes{};

		WireStyle Wires = WireStyle::Bezier;
		float WireWidth = 2.0f;
		// Used when a type has no registered colour or a wire is invalid.
		Rgba WireFallback{0.8f, 0.8f, 0.8f, 1.0f};
		Rgba WireInvalid{0.9f, 0.25f, 0.25f, 1.0f};
		Rgba WireHighlight{1.0f, 1.0f, 1.0f, 1.0f};
		// Highlight on ports that accept the wire being dragged.
		Rgba PortAccepts{0.3f, 0.9f, 0.4f, 1.0f};

		Rgba BoxSelectFill{1.0f, 1.0f, 1.0f, 0.06f};
		Rgba BoxSelectBorder{1.0f, 1.0f, 1.0f, 0.5f};

		// Timing overlay: green at zero, red at TimingSlowMilliseconds or more.
		bool ShowTimings = true;
		Rgba TimingFast{0.3f, 0.85f, 0.4f, 1.0f};
		Rgba TimingSlow{0.95f, 0.3f, 0.25f, 1.0f};
		double TimingSlowMilliseconds = 4.0;
		float TimelineHeight = 36.0f;

		Rgba PaletteBackground{0.05f, 0.05f, 0.05f, 0.98f};
		Rgba PaletteHighlight{0.20f, 0.20f, 0.20f, 1.0f};
		float PaletteWidth = 240.0f;
		size_t PaletteRows = 10;

		// True black background, white text. The default.
		static Theme Dark();
		// Stronger borders and pure colours for low vision.
		static Theme HighContrast();
		// Blue paper, white grid.
		static Theme Blueprint();
		// No headers, no grid.
		static Theme Minimal();

		static std::optional<Theme> Preset(std::string_view name);
		static std::vector<std::string> PresetNames();

		PortShape ShapeFor(const ang::TypeId &type) const;
	};

	Rgba Mix(const Rgba &from, const Rgba &to, float amount);
	Rgba FromColor3(const core::Color3 &colour, float alpha = 1.0f);
}
