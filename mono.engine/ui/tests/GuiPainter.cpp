#include <engine/gui/DrawList.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/ui/GuiPainter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

TEST_SUITE_ID("engine.ui.guipainter")

namespace {
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
