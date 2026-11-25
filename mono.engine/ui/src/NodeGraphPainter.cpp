#include <engine/ui/NodeGraphPainter.hpp>
#include <engine/ui/Theme.hpp>

#include <array>
#include <utility>

namespace engine::ui {

	namespace {
		ImU32 Pack(const angview::Rgba &colour) {
			return ImGui::ColorConvertFloat4ToU32({colour.R, colour.G, colour.B, colour.A});
		}

		angview::Rgba Unpack(unsigned int packed) {
			const ImVec4 colour = ImGui::ColorConvertU32ToFloat4(packed);
			return {colour.x, colour.y, colour.z, colour.w};
		}

		bool Visible(const angview::Rgba &colour) {
			return colour.A > 0.0f;
		}

		ImVec2 At(ImVec2 origin, core::Vector2 point) {
			return {origin.x + point.X, origin.y + point.Y};
		}

		angview::Modifiers ModifiersOf(const ImGuiIO &io) {
			return {.Shift = io.KeyShift, .Control = io.KeyCtrl, .Alt = io.KeyAlt};
		}
	}

	void PaintNodeGraph(
		const angview::DrawList &list, ImDrawList &out, ImVec2 origin, const NodeGraphImages &images
	) {
		for (const angview::DrawCommand &command : list) {
			const ImVec2 a = At(origin, command.A);
			const ImVec2 b = At(origin, command.B);
			switch (command.Kind) {
			case angview::DrawKind::Rect:
				if (Visible(command.Fill)) {
					out.AddRectFilled(a, b, Pack(command.Fill), command.Radius);
				}
				if (Visible(command.Stroke)) {
					out.AddRect(a, b, Pack(command.Stroke), command.Radius, 0, command.Thickness);
				}
				break;
			case angview::DrawKind::Line:
				out.AddLine(a, b, Pack(command.Stroke), command.Thickness);
				break;
			case angview::DrawKind::Bezier:
				out.AddBezierCubic(
					a,
					At(origin, command.C),
					At(origin, command.D),
					b,
					Pack(command.Stroke),
					command.Thickness
				);
				break;
			case angview::DrawKind::Circle:
				out.AddCircleFilled(a, command.Radius, Pack(command.Fill));
				break;
			case angview::DrawKind::Diamond:
				out.AddQuadFilled(
					{a.x, a.y - command.Radius},
					{a.x + command.Radius, a.y},
					{a.x, a.y + command.Radius},
					{a.x - command.Radius, a.y},
					Pack(command.Fill)
				);
				break;
			case angview::DrawKind::Text: {
				// A clip width keeps a long title inside its node.
				const bool clipped = command.B.X > 0.0f;
				if (clipped) {
					out.PushClipRect(a, {a.x + command.B.X, a.y + command.TextSize * 1.5f}, true);
				}
				out.AddText(nullptr, command.TextSize, a, Pack(command.Fill), command.Text.c_str());
				if (clipped) {
					out.PopClipRect();
				}
				break;
			}
			case angview::DrawKind::Image: {
				const ImTextureID texture = images ? images(command.Image) : ImTextureID{};
				if (texture != ImTextureID{}) {
					out.AddImage(texture, a, b);
				}
				break;
			}
			case angview::DrawKind::PushClip:
				out.PushClipRect(a, b, true);
				break;
			case angview::DrawKind::PopClip:
				out.PopClipRect();
				break;
			}
		}
	}

	bool NodeGraphCanvas(const char *id, angview::View &view, ImVec2 size, const NodeGraphImages &images) {
		const ImVec2 available = ImGui::GetContentRegionAvail();
		if (size.x <= 0.0f) {
			size.x = available.x;
		}
		if (size.y <= 0.0f) {
			size.y = available.y;
		}
		size = {std::max(size.x, 1.0f), std::max(size.y, 1.0f)};
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton(
			id,
			size,
			ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
				ImGuiButtonFlags_MouseButtonMiddle
		);
		const bool hovered = ImGui::IsItemHovered();
		const bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive() || hovered;
		view.SetViewport({size.x, size.y});

		ImGuiIO &io = ImGui::GetIO();
		const core::Vector2 local{io.MousePos.x - origin.x, io.MousePos.y - origin.y};
		const angview::Modifiers modifiers = ModifiersOf(io);
		bool consumed = false;
		// A drag that left the canvas keeps following the pointer until release.
		const bool tracking = hovered || view.Dragging() != angview::DragKind::None;
		if (tracking) {
			if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
				consumed |= view.PointerMove(local, modifiers);
			}
			constexpr std::array<std::pair<ImGuiMouseButton, angview::PointerButton>, 3> BUTTONS{{
				{ImGuiMouseButton_Left, angview::PointerButton::Left},
				{ImGuiMouseButton_Middle, angview::PointerButton::Middle},
				{ImGuiMouseButton_Right, angview::PointerButton::Right},
			}};
			for (const auto &[imgui, button] : BUTTONS) {
				if (hovered && ImGui::IsMouseClicked(imgui)) {
					consumed |= view.PointerDown(button, local, modifiers);
				}
				if (ImGui::IsMouseReleased(imgui)) {
					consumed |= view.PointerUp(button, local, modifiers);
				}
			}
		}
		if (hovered && io.MouseWheel != 0.0f) {
			consumed |= view.Wheel(io.MouseWheel, local);
		}
		if (focused && !io.WantTextInput) {
			constexpr std::array<std::pair<ImGuiKey, angview::Key>, 11> KEYS{{
				{ImGuiKey_Delete, angview::Key::Delete},
				{ImGuiKey_Backspace, angview::Key::Backspace},
				{ImGuiKey_Escape, angview::Key::Escape},
				{ImGuiKey_Enter, angview::Key::Enter},
				{ImGuiKey_UpArrow, angview::Key::Up},
				{ImGuiKey_DownArrow, angview::Key::Down},
				{ImGuiKey_F, angview::Key::F},
				{ImGuiKey_A, angview::Key::A},
				{ImGuiKey_Space, angview::Key::Space},
				{ImGuiKey_Tab, angview::Key::Tab},
				{ImGuiKey_D, angview::Key::D},
			}};
			const bool typing = view.AddPalette().Open;
			for (const auto &[imgui, key] : KEYS) {
				// While the palette is open, letters are text, not shortcuts.
				const bool letter =
					key == angview::Key::F || key == angview::Key::A || key == angview::Key::Space;
				if (typing && letter) {
					continue;
				}
				if (ImGui::IsKeyPressed(imgui, false)) {
					consumed |= view.KeyDown(key, modifiers);
				}
				if (key == angview::Key::Space && ImGui::IsKeyReleased(imgui)) {
					consumed |= view.KeyUp(key);
				}
			}
			if (typing) {
				for (const ImWchar character : io.InputQueueCharacters) {
					if (character >= 32 && character < 127) {
						const char text[2]{static_cast<char>(character), '\0'};
						consumed |= view.TextInput(text);
					}
				}
			}
		}

		ImDrawList &out = *ImGui::GetWindowDrawList();
		out.PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
		PaintNodeGraph(view.Paint(), out, origin, images);
		out.PopClipRect();
		return consumed;
	}

	angview::Theme NodeGraphThemeFromPalette() {
		angview::Theme theme = angview::Theme::Dark();
		theme.Name = "Editor";
		const angview::Rgba surface = Unpack(ColourOf(ThemeColour::Surface));
		const angview::Rgba accent = Unpack(ColourOf(ThemeColour::Accent));
		theme.Background = surface;
		theme.NodeBody = angview::Mix(surface, {1.0f, 1.0f, 1.0f, 1.0f}, 0.06f);
		theme.NodeHeader = angview::Mix(surface, {1.0f, 1.0f, 1.0f, 1.0f}, 0.12f);
		theme.NodeBorderSelected = accent;
		theme.NodeBorderHovered = Unpack(ColourOf(ThemeColour::AccentHot));
		theme.PortAccepts = accent;
		theme.Text = Unpack(ColourOf(ThemeColour::Text));
		theme.TextDim = Unpack(ColourOf(ThemeColour::TextMuted));
		theme.NodeFailed = Unpack(ColourOf(ThemeColour::Error));
		theme.NodeWaiting = Unpack(ColourOf(ThemeColour::Warning));
		theme.PaletteBackground = angview::Mix(surface, {0.0f, 0.0f, 0.0f, 1.0f}, 0.3f);
		return theme;
	}
}
