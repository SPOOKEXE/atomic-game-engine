#include <engine/gui/DrawList.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/ui/GuiPainter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

TEST_SUITE_ID("engine.ui.guipainter")

namespace {
	void SetLinearSampler(const ImDrawList *, const ImDrawCmd *) {}
	void SetNearestSampler(const ImDrawList *, const ImDrawCmd *) {}

	struct ImGuiContextScope {
		ImGuiContextScope() {
			ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			ImGui::NewFrame();
		}

		~ImGuiContextScope() {
			ImGui::EndFrame();
			ImGui::DestroyContext();
		}
	};

	engine::gui::DrawCommand Command(engine::gui::DrawKind kind) {
		engine::gui::DrawCommand command;
		command.Kind = kind;
		command.Bounds = engine::core::Rect{{10.0f, 20.0f}, {210.0f, 80.0f}};
		command.Clip = engine::core::Rect{{0.0f, 0.0f}, {800.0f, 600.0f}};
		return command;
	}
}

TEST_CASE("the studio painter resolves viewport images by element", "[ui][guipainter]") {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Viewport);
	command.Source = engine::ecs::Entity{17};
	list.Commands.push_back(command);

	bool resolved = false;
	engine::ui::ImageSource images;
	images.ResolveViewport = [&](engine::ecs::Entity source) {
		resolved = source == command.Source;
		engine::ui::ImageSource::Resolved image;
		image.Texture = static_cast<ImTextureID>(1);
		image.Size = ImVec2(200.0f, 60.0f);
		return image;
	};

	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	CHECK(resolved);
	CHECK(target->VtxBuffer.Size == 4);
}

TEST_CASE("text stroke and rotation are emitted by the studio painter", "[ui][guipainter]") {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Text);
	command.Text = "a long label that truncates";
	command.TextSize = 16;
	command.Truncate = engine::gui::TextTruncate::AtEnd;
	command.StrokeTransparency = 0.0f;
	command.Rotation = 25.0f;
	list.Commands.push_back(command);

	CHECK(engine::ui::PaintGui(list, target, {}) == 9);
	CHECK(target->VtxBuffer.Size > 4);
	CHECK(target->VtxBuffer[0].pos.y != command.Bounds.Min.Y);
}

TEST_CASE("the studio painter carries every compiled primitive and image resampling", "[ui][guipainter]") {
	const ImGuiContextScope context;
	ImGuiPlatformIO &platform = ImGui::GetPlatformIO();
	platform.DrawCallback_SetSamplerLinear = SetLinearSampler;
	platform.DrawCallback_SetSamplerNearest = SetNearestSampler;

	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;

	auto fill = Command(engine::gui::DrawKind::Rectangle);
	fill.Gradient = 0;
	list.Commands.push_back(fill);

	auto outline = Command(engine::gui::DrawKind::Outline);
	outline.Thickness = 3.0f;
	list.Commands.push_back(outline);

	auto image = Command(engine::gui::DrawKind::Image);
	image.Image = engine::core::Name("pixel-art");
	image.Resample = engine::gui::ResampleMode::Pixelated;
	list.Commands.push_back(image);

	auto text = Command(engine::gui::DrawKind::Text);
	text.Text = "bold text";
	text.TextSize = 16;
	engine::gui::DrawSpan bold;
	bold.Begin = 0;
	bold.End = 4;
	bold.Font = engine::gui::FontFace::Bold;
	text.Spans.push_back(bold);
	list.Commands.push_back(text);

	engine::gui::DrawGradient gradient;
	gradient.Color = engine::core::ColorSequence{
		engine::core::Color3{1.0f, 0.0f, 0.0f}, engine::core::Color3{0.0f, 0.0f, 1.0f}
	};
	gradient.Origin = engine::core::Vector2{10.0f, 20.0f};
	gradient.Axis = engine::core::Vector2{200.0f, 0.0f};
	list.Gradients.push_back(gradient);

	engine::ui::ImageSource images;
	images.Resolve = [](const engine::core::Name &) {
		engine::ui::ImageSource::Resolved resolved;
		resolved.Texture = static_cast<ImTextureID>(1);
		resolved.Size = ImVec2{32.0f, 32.0f};
		return resolved;
	};

	CHECK(engine::ui::PaintGui(list, target, {}, images) == 5);
	CHECK(target->VtxBuffer.Size > 12);

	bool sawNearest = false;
	bool sawLinear = false;
	for (const ImDrawCmd &draw : target->CmdBuffer) {
		sawNearest = sawNearest || draw.UserCallback == SetNearestSampler;
		sawLinear = sawLinear || draw.UserCallback == SetLinearSampler;
	}
	CHECK(sawNearest);
	CHECK(sawLinear);

	// The fill's left and right vertices were tinted by opposite gradient ends.
	CHECK(target->VtxBuffer[0].col != target->VtxBuffer[1].col);
}
