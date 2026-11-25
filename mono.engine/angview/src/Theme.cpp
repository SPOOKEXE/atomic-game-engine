#include <engine/angview/Theme.hpp>

#include <algorithm>

namespace engine::angview {

	Rgba Mix(const Rgba &from, const Rgba &to, float amount) {
		const float t = std::clamp(amount, 0.0f, 1.0f);
		return {
			from.R + (to.R - from.R) * t,
			from.G + (to.G - from.G) * t,
			from.B + (to.B - from.B) * t,
			from.A + (to.A - from.A) * t,
		};
	}

	Rgba FromColor3(const core::Color3 &colour, float alpha) {
		return {colour.R, colour.G, colour.B, alpha};
	}

	Theme Theme::Dark() {
		Theme theme;
		theme.Name = "Dark";
		return theme;
	}

	Theme Theme::HighContrast() {
		Theme theme;
		theme.Name = "HighContrast";
		theme.Grid = {1.0f, 1.0f, 1.0f, 0.12f};
		theme.GridMajor = {1.0f, 1.0f, 1.0f, 0.25f};
		theme.NodeBody = {0.0f, 0.0f, 0.0f, 1.0f};
		theme.NodeHeader = {0.0f, 0.0f, 0.0f, 1.0f};
		theme.NodeBorder = {1.0f, 1.0f, 1.0f, 1.0f};
		theme.NodeBorderHovered = {1.0f, 1.0f, 0.0f, 1.0f};
		theme.NodeBorderSelected = {0.0f, 1.0f, 1.0f, 1.0f};
		theme.TextDim = {1.0f, 1.0f, 1.0f, 1.0f};
		theme.BorderWidth = 2.0f;
		theme.SelectedBorderWidth = 3.0f;
		theme.WireWidth = 3.0f;
		theme.PortRadius = 6.0f;
		theme.FontScale = 1.15f;
		return theme;
	}

	Theme Theme::Blueprint() {
		Theme theme;
		theme.Name = "Blueprint";
		theme.Background = {0.05f, 0.18f, 0.42f, 1.0f};
		theme.Grid = {1.0f, 1.0f, 1.0f, 0.10f};
		theme.GridMajor = {1.0f, 1.0f, 1.0f, 0.22f};
		theme.NodeBody = {0.06f, 0.22f, 0.50f, 0.95f};
		theme.NodeHeader = {0.10f, 0.30f, 0.62f, 1.0f};
		theme.NodeBorder = {0.85f, 0.92f, 1.0f, 0.8f};
		theme.NodeBorderHovered = {1.0f, 1.0f, 1.0f, 1.0f};
		theme.NodeBorderSelected = {1.0f, 0.85f, 0.3f, 1.0f};
		theme.TextDim = {0.80f, 0.88f, 1.0f, 1.0f};
		theme.Wires = WireStyle::Orthogonal;
		theme.WireFallback = {0.92f, 0.96f, 1.0f, 1.0f};
		theme.CornerRadius = 0.0f;
		theme.DefaultPortShape = PortShape::Square;
		return theme;
	}

	Theme Theme::Minimal() {
		Theme theme;
		theme.Name = "Minimal";
		theme.ShowGrid = false;
		theme.ShowHeaders = false;
		theme.HeaderHeight = 18.0f;
		theme.NodeHeader = theme.NodeBody;
		theme.NodeBorder = {0.18f, 0.18f, 0.18f, 1.0f};
		theme.Wires = WireStyle::Straight;
		theme.WireWidth = 1.5f;
		theme.ShowTimings = false;
		return theme;
	}

	std::optional<Theme> Theme::Preset(std::string_view name) {
		if (name == "Dark") {
			return Dark();
		}
		if (name == "HighContrast") {
			return HighContrast();
		}
		if (name == "Blueprint") {
			return Blueprint();
		}
		if (name == "Minimal") {
			return Minimal();
		}
		return std::nullopt;
	}

	std::vector<std::string> Theme::PresetNames() {
		return {"Dark", "HighContrast", "Blueprint", "Minimal"};
	}

	PortShape Theme::ShapeFor(const ang::TypeId &type) const {
		const auto found = PortShapes.find(type);
		return found == PortShapes.end() ? DefaultPortShape : found->second;
	}
}
