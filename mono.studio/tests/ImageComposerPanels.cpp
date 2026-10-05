#include "ImageComposerPanels.hpp"

#include "ImageComposerInternal.hpp"

#include <engine/core/Name.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("studio.image-composer-panels")

namespace {
	struct Context {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Handle = ImGui::CreateContext();
		Context() {
			ImGui::SetCurrentContext(Handle);
			auto &io = ImGui::GetIO();
			io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(1280, 720);
			io.DeltaTime = 1.0f / 60.0f;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Context() {
			if (Handle->WithinFrameScope) ImGui::EndFrame();
			ImGui::DestroyContext(Handle);
			ImGui::SetCurrentContext(Previous);
		}
	};

	constexpr ImGuiID ROOT = 0x2200;
	void MakeHost() {
		ImGui::DockBuilderAddNode(ROOT, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(ROOT, ImVec2(1280, 720));
		ImGui::DockBuilderDockWindow("Image Composer", ROOT);
		ImGui::DockBuilderFinish(ROOT);
	}
}

TEST_CASE("composer panes start inside their owner without splitting Studio", "[studio][composer-panels]") {
	Context context;
	ImGui::NewFrame();
	MakeHost();
	studio::detail::ImageComposerPanels panels;
	studio::detail::InitializeImageComposerPanels(panels, ImVec2(40, 80), ImVec2(900, 500));
	REQUIRE(panels.LayoutInitialized);
	CHECK(ImGui::DockBuilderGetNode(ROOT)->IsLeafNode());
	CHECK(studio::detail::PanelDockId("Image Composer") == ROOT);
	const ImGuiID graph = studio::detail::PanelDockId(studio::detail::IMAGE_COMPOSER_GRAPH);
	const ImGuiID home = studio::detail::PanelDockId(studio::detail::IMAGE_COMPOSER_PANELS[0].Window);
	REQUIRE(home != 0);
	CHECK(home != graph);
	CHECK(
		ImGui::DockNodeGetRootNode(ImGui::DockBuilderGetNode(home))->ID ==
		studio::detail::ImageComposerDockspaceId()
	);
	for (size_t index = 0; index < panels.Open.size(); ++index) {
		CHECK(panels.Open[index] == (index < 2));
		CHECK(studio::detail::PanelDockId(studio::detail::IMAGE_COMPOSER_PANELS[index].Window) == home);
	}
	ImGui::DockBuilderDockWindow(studio::detail::IMAGE_COMPOSER_PANELS[2].Window, graph);
	const std::string saved = ImGui::SaveIniSettingsToMemory();
	ImGui::EndFrame();
	ImGui::LoadIniSettingsFromMemory(saved.c_str());
	ImGui::NewFrame();
	panels.LayoutInitialized = false;
	studio::detail::InitializeImageComposerPanels(panels, ImVec2(40, 80), ImVec2(900, 500));
	CHECK(studio::detail::PanelDockId(studio::detail::IMAGE_COMPOSER_PANELS[2].Window) == graph);
}

TEST_CASE(
	"composer migrates hosted panes inside while preserving detached groups", "[studio][composer-panels]"
) {
	Context context;
	ImGui::NewFrame();
	MakeHost();
	const char *nodes = studio::detail::IMAGE_COMPOSER_PANELS[0].Window;
	const char *preview = studio::detail::IMAGE_COMPOSER_PANELS[4].Window;
	ImGui::DockBuilderDockWindow(nodes, ROOT);
	ImGui::DockBuilderAddNode(0x4400);
	ImGui::DockBuilderSetNodeSize(0x4400, ImVec2(300, 600));
	ImGui::DockBuilderDockWindow(preview, 0x4400);
	studio::detail::ImageComposerPanels panels;
	studio::detail::InitializeImageComposerPanels(panels, ImVec2(0, 0), ImVec2(900, 500));
	CHECK(
		ImGui::DockNodeGetRootNode(ImGui::DockBuilderGetNode(studio::detail::PanelDockId(nodes)))->ID ==
		studio::detail::ImageComposerDockspaceId()
	);
	CHECK(studio::detail::PanelDockId(preview) == 0x4400);
	panels.LayoutInitialized = false;
	ImGui::DockBuilderDockWindow(nodes, ROOT);
	studio::detail::InitializeImageComposerPanels(panels, ImVec2(0, 0), ImVec2(900, 500));
	CHECK(studio::detail::PanelDockId(nodes) != ROOT);
}

TEST_CASE("composer first hidden owner still builds a split when shown", "[studio][composer-panels]") {
	Context context;
	ImGui::NewFrame();
	ImGui::Begin("Image Composer");
	const auto composerClass = studio::detail::ImageComposerWindowClass();
	ImGui::DockSpace(
		studio::detail::ImageComposerDockspaceId(),
		ImVec2(0, 0),
		ImGuiDockNodeFlags_KeepAliveOnly,
		&composerClass
	);
	ImGui::End();
	ImGui::EndFrame();
	ImGui::NewFrame();
	studio::detail::ImageComposerPanels panels;
	studio::detail::InitializeImageComposerPanels(panels, ImVec2(0, 0), ImVec2(900, 500));
	CHECK(ImGui::DockBuilderGetNode(studio::detail::ImageComposerDockspaceId())->IsSplitNode());
	CHECK(
		studio::detail::PanelDockId(studio::detail::IMAGE_COMPOSER_GRAPH) !=
		studio::detail::PanelDockId(studio::detail::IMAGE_COMPOSER_PANELS[0].Window)
	);
}

TEST_CASE("composer layout reset reunites detached panes inside owner", "[studio][composer-panels]") {
	Context context;
	ImGui::NewFrame();
	studio::detail::ImageComposerPanels panels;
	studio::detail::InitializeImageComposerPanels(panels, ImVec2(0, 0), ImVec2(900, 500));
	const char *preview = studio::detail::IMAGE_COMPOSER_PANELS[4].Window;
	ImGui::DockBuilderAddNode(0x6600);
	ImGui::DockBuilderSetNodeSize(0x6600, ImVec2(300, 600));
	ImGui::DockBuilderDockWindow(preview, 0x6600);
	panels.LayoutInitialized = false;
	panels.ResetRequested = true;
	studio::detail::InitializeImageComposerPanels(panels, ImVec2(0, 0), ImVec2(900, 500));
	const ImGuiID home = studio::detail::PanelDockId(studio::detail::IMAGE_COMPOSER_PANELS[0].Window);
	CHECK(studio::detail::PanelDockId(preview) == home);
	CHECK_FALSE(panels.ResetRequested);
}

TEST_CASE("composer owned panes resize inside owner and disappear on close", "[studio][composer-panels]") {
	Context context;
	engine::render::Renderer renderer;
	bool open = true;
	studio::ResetImageComposerPanelLayout();
	for (int frame = 0; frame < 5; ++frame) {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(frame < 2 ? ImVec2(900, 600) : ImVec2(1100, 650), ImGuiCond_Always);
		ImGui::SetNextWindowCollapsed(frame == 0, ImGuiCond_Always);
		studio::DrawImageComposer(renderer, engine::core::Name{}, open);
		CHECK(open);
		if (frame == 0) {
			CHECK(ImGui::FindWindowByName(studio::detail::IMAGE_COMPOSER_GRAPH) == nullptr);
			ImGui::Render();
			continue;
		}
		const ImGuiWindow *owner = ImGui::FindWindowByName("Image Composer");
		for (size_t index = 0; index < 2; ++index) {
			const ImGuiWindow *window =
				ImGui::FindWindowByName(studio::detail::IMAGE_COMPOSER_PANELS[index].Window);
			REQUIRE(window != nullptr);
			CHECK(window->Active);
			CHECK(window->DockId != 0);
			if (frame % 2 == 1) {
				CHECK(window->Pos.x >= owner->Pos.x);
				CHECK(window->Pos.y >= owner->Pos.y);
				CHECK(window->Pos.x + window->Size.x <= owner->Pos.x + owner->Size.x);
				CHECK(window->Pos.y + window->Size.y <= owner->Pos.y + owner->Size.y);
			}
		}
		for (size_t index = 2; index < studio::detail::IMAGE_COMPOSER_PANELS.size(); ++index) {
			const ImGuiWindow *window =
				ImGui::FindWindowByName(studio::detail::IMAGE_COMPOSER_PANELS[index].Window);
			CHECK((window == nullptr || !window->Active));
		}
		ImGui::Render();
	}
	ImGui::NewFrame();
	const char *nodes = studio::detail::IMAGE_COMPOSER_PANELS[0].Window;
	ImGui::DockBuilderDockWindow(nodes, 0);
	ImGui::SetNextWindowCollapsed(true, ImGuiCond_Always);
	studio::DrawImageComposer(renderer, engine::core::Name{}, open);
	CHECK(ImGui::FindWindowByName(nodes)->Active);
	CHECK(ImGui::FindWindowByName("Image Composer")->Collapsed);
	CHECK(studio::detail::PanelDockId(studio::detail::IMAGE_COMPOSER_PANELS[1].Window) != 0);
	ImGui::Render();
	ImGui::NewFrame();
	open = false;
	studio::DrawImageComposer(renderer, engine::core::Name{}, open);
	CHECK_FALSE(ImGui::FindWindowByName(nodes)->Active);
	CHECK_FALSE(ImGui::FindWindowByName(studio::detail::IMAGE_COMPOSER_GRAPH)->Active);
	CHECK_FALSE(ImGui::FindWindowByName(studio::detail::IMAGE_COMPOSER_PANELS[1].Window)->Active);
	ImGui::Render();
}
