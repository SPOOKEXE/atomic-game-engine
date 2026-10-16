#include <engine/ecs/Store.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <studio/ProceduralPanel.hpp>

TEST_SUITE_ID("studio.proceduralpanel")

namespace {
	struct ProceduralPanelContext {
		ImGuiContext *Handle = ImGui::CreateContext();
		ProceduralPanelContext() {
			ImGui::SetCurrentContext(Handle);
			auto &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1000, 800);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			io.Fonts->SetTexID(ImTextureID{1});
		}
		~ProceduralPanelContext() {
			ImGui::DestroyContext(Handle);
		}
		void Frame(
			studio::ProceduralPanelState &state,
			bool &open,
			engine::ecs::Store &store,
			ImVec2 mouse,
			bool down,
			void *image = nullptr
		) {
			ImGui::SetCurrentContext(Handle);
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(mouse.x, mouse.y);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(40, 30));
			ImGui::SetNextWindowSize(ImVec2(640, 640));
			studio::DrawProceduralPanel(state, open, &store, nullptr, image);
			ImGui::Render();
		}
	};
}
TEST_CASE(
	"procedural panel queues the clicked author action before changing ECS", "[studio][procedural][panel]"
) {
	engine::script::RegisterProceduralComponents();
	engine::ecs::Store store{"procedural-panel"};
	studio::ProceduralPanelState panel;
	bool open = true;
	ProceduralPanelContext context;
	context.Frame(panel, open, store, ImVec2(-1, -1), false);
	auto *window = ImGui::FindWindowByName("Procedural Generation");
	REQUIRE(window);
	constexpr const char *BUTTON_LABEL = "Create forest and village";
	const ImGuiID buttonId = window->GetID(BUTTON_LABEL);
	const ImVec2 textSize = ImGui::CalcTextSize(BUTTON_LABEL);
	const ImVec2 padding = ImGui::GetStyle().FramePadding;
	const ImVec2 button{
		window->DC.CursorStartPos.x + padding.x + textSize.x * 0.5f,
		window->DC.CursorStartPos.y + padding.y + textSize.y * 0.5f
	};
	// Establish hover before pressing so a refusal identifies hit testing or
	// activation separately from the queued action.
	context.Frame(panel, open, store, button, false);
	REQUIRE(ImGui::GetCurrentContext()->HoveredId == buttonId);
	context.Frame(panel, open, store, button, true);
	REQUIRE(ImGui::GetCurrentContext()->ActiveId == buttonId);
	context.Frame(panel, open, store, button, false);
	REQUIRE(panel.Pending.size() == 1);
	CHECK(panel.Pending.front().Kind == studio::ProceduralActionKind::Initialize);
	CHECK_FALSE(store.Resource<engine::script::ProceduralAuthority>());
	engine::procedural::Diagnostic diagnostic;
	REQUIRE(studio::ApplyProceduralAuthoringAction(store, panel.Pending.front(), diagnostic));
	const auto *authority = store.Resource<engine::script::ProceduralAuthority>();
	REQUIRE(authority);
	CHECK(authority->Sources.size() == 2);
	CHECK(authority->Regions.size() == 2);
	panel.Pending.clear();
	open = false;
	context.Frame(panel, open, store, button, true);
	CHECK(panel.Pending.empty());
}
TEST_CASE("procedural panel bounds queued owned action payloads", "[studio][procedural][panel]") {
	studio::ProceduralPanelState panel;
	studio::ProceduralAction action;
	action.Source.Name.assign(129, 'x');
	CHECK_FALSE(panel.Queue(action));
	CHECK(panel.Pending.empty());
	action.Source.Name.clear();
	for (size_t index = 0; index < 64; ++index)
		REQUIRE(panel.Queue(action));
	CHECK_FALSE(panel.Queue(action));
	CHECK(panel.Pending.size() == 64);
}

TEST_CASE(
	"retained procedural pictures are drawn only for their recorded region", "[studio][procedural][panel]"
) {
	engine::script::RegisterProceduralComponents();
	engine::ecs::Store store{"procedural-picture-scope"};
	studio::ProceduralAction initialize{studio::ProceduralActionKind::Initialize};
	engine::procedural::Diagnostic diagnostic;
	REQUIRE(studio::ApplyProceduralAuthoringAction(store, initialize, diagnostic));
	studio::ProceduralPanelState panel;
	panel.HasImageScope = true;
	panel.ImageScope = store.Resource<engine::script::ProceduralAuthority>()->Regions[0].Scope;
	const auto image = reinterpret_cast<void *>(uintptr_t{0x12345678});
	const auto texture = ImTextureRef(image).GetTexID();
	ProceduralPanelContext context;
	bool open = true;
	const auto containsImage = [&] {
		const auto *draw = ImGui::GetDrawData();
		for (const auto *list : draw->CmdLists)
			for (const auto &command : list->CmdBuffer)
				if (command.ElemCount && command.GetTexID() == texture) return true;
		return false;
	};
	context.Frame(panel, open, store, ImVec2(-1, -1), false, image);
	CHECK(containsImage());
	panel.RegionIndex = 1;
	context.Frame(panel, open, store, ImVec2(-1, -1), false, image);
	CHECK_FALSE(containsImage());
	panel.RegionIndex = 0;
	context.Frame(panel, open, store, ImVec2(-1, -1), false, image);
	CHECK(containsImage());
}
