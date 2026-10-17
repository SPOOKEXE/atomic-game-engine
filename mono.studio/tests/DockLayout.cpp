#include "DockLayout.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

TEST_SUITE_ID("studio.dock-layout")

namespace {
	struct Context {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Handle = ImGui::CreateContext();
		Context() {
			ImGui::SetCurrentContext(Handle);
			ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::GetIO().LogFilename = nullptr;
			ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
			ImGui::GetIO().Fonts->AddFontDefault();
			ImGui::GetIO().Fonts->Build();
			ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
			ImGui::NewFrame();
		}
		~Context() {
			if (Handle->WithinFrameScope) ImGui::EndFrame();
			ImGui::DestroyContext(Handle);
			ImGui::SetCurrentContext(Previous);
		}
	};

	struct Layout {
		ImGuiID Root = 0x1000;
		ImGuiID Left = 0;
		ImGuiID Centre = 0;
		Layout() {
			ImGui::DockBuilderAddNode(Root, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(Root, ImVec2(1280.0f, 720.0f));
			Left = ImGui::DockBuilderSplitNode(Root, ImGuiDir_Left, 0.25f, nullptr, &Centre);
			ImGui::DockBuilderDockWindow("Explorer", Left);
			ImGui::DockBuilderDockWindow("Viewport 1", Centre);
			ImGui::DockBuilderFinish(Root);
		}
	};
}

TEST_CASE("attached panels fill the host after grow and shrink", "[studio][dock-layout]") {
	Context context;
	Layout layout;
	SECTION("saved panel belongs to a retired dockspace") {
		ImGui::DockBuilderAddNode(0x2000, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderDockWindow("Viewport 1", 0x2000);
		studio::detail::RepairPanelDock("Viewport 1", layout.Centre);
	}
	SECTION("saved panel already belongs to the active dockspace") {}
	ImGui::EndFrame();
	const auto draw = [&](ImVec2 size) {
		ImGui::GetIO().DisplaySize = size;
		ImGui::NewFrame();
		if (ImGui::BeginMainMenuBar()) ImGui::EndMainMenuBar();
		ImGui::DockSpaceOverViewport(layout.Root);
		ImGui::Begin("Explorer");
		ImGui::TextUnformatted("tree");
		ImGui::End();
		ImGui::Begin("Viewport 1");
		ImGui::TextUnformatted("scene");
		ImGui::End();
		ImGui::Render();
	};
	for (const ImVec2 size : std::array{ImVec2(1280, 720), ImVec2(900, 600), ImVec2(1920, 1080)}) {
		draw(size);
		draw(size);
		const ImGuiDockNode *root = ImGui::DockBuilderGetNode(layout.Root);
		REQUIRE(root != nullptr);
		const ImGuiViewport *viewport = ImGui::GetMainViewport();
		CHECK(root->Pos.x == viewport->WorkPos.x);
		CHECK(root->Pos.y == viewport->WorkPos.y);
		CHECK(root->Size.x == viewport->WorkSize.x);
		CHECK(root->Size.y == viewport->WorkSize.y);
		const ImGuiWindow *scene = ImGui::FindWindowByName("Viewport 1");
		REQUIRE(scene != nullptr);
		CHECK(scene->DockIsActive);
		CHECK(scene->Pos.x + scene->Size.x == root->Pos.x + root->Size.x);
		CHECK(scene->Pos.y + scene->Size.y == root->Pos.y + root->Size.y);
	}
}

TEST_CASE("startup gives missing and stale panels a surviving peer slot", "[studio][dock-layout]") {
	Context context;
	Layout layout;
	const std::array peers{"Explorer", "Worlds"};
	const ImGuiID home = studio::detail::PanelDockHome(layout.Root, peers);
	REQUIRE(home == layout.Left);
	studio::detail::RepairPanelDock("Worlds", home);
	CHECK(studio::detail::PanelDockId("Worlds") == layout.Left);

	ImGui::DockBuilderDockWindow("Properties", 0xdead);
	studio::detail::RepairPanelDock("Properties", home);
	CHECK(studio::detail::PanelDockId("Properties") == layout.Left);

	ImGui::DockBuilderDockWindow("Output", layout.Root);
	studio::detail::RepairPanelDock("Output", home);
	CHECK(studio::detail::PanelDockId("Output") == layout.Left);

	ImGui::DockBuilderAddNode(0x2000, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderDockWindow("History", 0x2000);
	studio::detail::RepairPanelDock("History", home);
	CHECK(studio::detail::PanelDockId("History") == layout.Left);
}

TEST_CASE("startup snaps hosted floaters and preserves chosen docks", "[studio][dock-layout]") {
	Context context;
	Layout layout;
	ImGui::DockBuilderDockWindow("Properties", layout.Centre);
	studio::detail::RepairPanelDock("Properties", layout.Left);
	CHECK(studio::detail::PanelDockId("Properties") == layout.Centre);

	ImGui::DockBuilderDockWindow("Output", 0);
	studio::detail::RepairPanelDock("Output", layout.Left);
	CHECK(studio::detail::PanelDockId("Output") == layout.Left);

	ImGui::DockBuilderAddNode(0x3000);
	ImGui::DockBuilderDockWindow("History", 0x3000);
	studio::detail::RepairPanelDock("History", layout.Left);
	CHECK(studio::detail::PanelDockId("History") == layout.Left);
}

TEST_CASE(
	"startup preserves external panels but snaps overlapping native floaters", "[studio][dock-layout]"
) {
	Context context;
	Layout layout;
	ImGui::DockBuilderDockWindow("External", 0);
	auto *external = ImGui::FindWindowSettingsByID(ImHashStr("External"));
	REQUIRE(external != nullptr);
	external->ViewportId = 0x1234;
	external->ViewportPos = ImVec2ih(3000, 500);
	external->Size = ImVec2ih(384, 355);
	studio::detail::RepairPanelDock("External", layout.Left);
	CHECK(studio::detail::PanelDockId("External") == 0);

	ImGui::DockBuilderDockWindow("Overlapping", 0);
	auto *overlapping = ImGui::FindWindowSettingsByID(ImHashStr("Overlapping"));
	REQUIRE(overlapping != nullptr);
	overlapping->ViewportId = 0x2345;
	overlapping->ViewportPos = ImVec2ih(1100, 500);
	overlapping->Size = ImVec2ih(384, 355);
	studio::detail::RepairPanelDock("Overlapping", layout.Left);
	CHECK(studio::detail::PanelDockId("Overlapping") == layout.Left);
}

TEST_CASE("closed peer settings choose homes after layout reload", "[studio][dock-layout]") {
	Context context;
	Layout layout;
	size_t size = 0;
	const std::string saved(ImGui::SaveIniSettingsToMemory(&size));
	ImGui::EndFrame();
	ImGui::DestroyContext(context.Handle);
	context.Handle = ImGui::CreateContext();
	ImGui::SetCurrentContext(context.Handle);
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::LoadIniSettingsFromMemory(saved.c_str(), saved.size());
	const std::array peers{"Explorer"};
	CHECK(studio::detail::PanelDockHome(layout.Root, peers) == layout.Left);
	studio::detail::RepairPanelDock("New Panel", layout.Left);
	CHECK(studio::detail::PanelDockId("New Panel") == layout.Left);
}

TEST_CASE("an empty central slot does not reserve a blank half below Output", "[studio][dock-layout]") {
	Context context;
	constexpr ImGuiID ROOT = 0x4000;
	ImGui::DockBuilderAddNode(ROOT, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(ROOT, ImVec2(1280, 720));
	ImGuiID lower = 0;
	const ImGuiID upper = ImGui::DockBuilderSplitNode(ROOT, ImGuiDir_Up, 0.5f, nullptr, &lower);
	ImGui::DockBuilderDockWindow("Output", upper);
	ImGui::DockBuilderDockWindow("Preview", lower);
	ImGui::DockBuilderFinish(ROOT);
	ImGui::EndFrame();
	const auto draw = [&](bool preview) {
		ImGui::NewFrame();
		studio::detail::FillEmptyCentralDock(ROOT);
		ImGui::DockSpaceOverViewport(ROOT);
		ImGui::Begin("Output");
		ImGui::TextUnformatted("log");
		ImGui::End();
		if (preview) {
			ImGui::Begin("Preview");
			ImGui::TextUnformatted("scene");
			ImGui::End();
		}
		ImGui::Render();
	};
	SECTION("central slot was never opened") {}
	SECTION("central panel was closed") {
		for (int frame = 0; frame < 3; frame++)
			draw(true);
		const ImGuiWindow *output = ImGui::FindWindowByName("Output");
		REQUIRE(output != nullptr);
		CHECK(output->Size.y < ImGui::GetMainViewport()->WorkSize.y);
	}
	for (int frame = 0; frame < 4; frame++) {
		draw(false);
	}
	const ImGuiWindow *output = ImGui::FindWindowByName("Output");
	REQUIRE(output != nullptr);
	CHECK(output->DockIsActive);
	CHECK(output->Size.y == ImGui::GetMainViewport()->WorkSize.y);
}
