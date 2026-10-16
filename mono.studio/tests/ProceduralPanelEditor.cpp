#include "ProceduralPanel.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/script/ProceduralScene.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <string_view>
#include <studio/Editor.hpp>
#include <studio/ProceduralEditor.hpp>
#include <vector>

TEST_SUITE_ID("studio.proceduralpaneladapter")
TEST_DEPENDS("studio.proceduraleditor")
TEST_DEPENDS("studio.commands")

namespace {
	struct ProceduralAdapterContext {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Owned = ImGui::CreateContext();
		ProceduralAdapterContext() {
			ImGui::SetCurrentContext(Owned);
			auto &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1100, 1400);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			io.Fonts->SetTexID(ImTextureID{1});
		}
		~ProceduralAdapterContext() {
			ImGui::DestroyContext(Owned);
			ImGui::SetCurrentContext(Previous);
		}
	};
	void InitializeAdapterEditor(studio::Editor &editor, std::string_view worldName) {
		using namespace engine;
		scene::RegisterSceneClasses();
		script::RegisterProceduralComponents();
		editor.Universe = std::make_unique<world::Universe>();
		editor.Commands = std::make_unique<studio::CommandLog>(*editor.Universe);
		const auto world = editor.Universe->Create({.Name = core::Name(worldName)});
		REQUIRE(world.IsValid());
		editor.Active = world;
		editor.ProceduralWorld = world;
		editor.ShowProcedural = true;
		editor.ProceduralPanel = std::make_unique<studio::ProceduralPanelState>();
		editor.ProceduralPreview = std::make_unique<studio::ProceduralEditor>(parallel::JobContext::Serial);
	}
	std::string DrawProceduralEditor(studio::Editor &editor) {
		ImGui::NewFrame();
		ImGui::LogToBuffer();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(900, 1200), ImGuiCond_Always);
		editor.DrawProceduralGeneration();
		const std::string text = GImGui->LogBuffer.c_str();
		ImGui::LogFinish();
		ImGui::Render();
		return text;
	}
	studio::ProceduralAction VillageAt(int64_t x) {
		studio::ProceduralAction action{studio::ProceduralActionKind::SetSource};
		action.Source = {
			engine::procedural::SourceKind::Village,
			"oakstead",
			{x * engine::procedural::SUBCELLS_PER_CELL, 8 * engine::procedural::SUBCELLS_PER_CELL},
			""
		};
		return action;
	}
}

TEST_CASE(
	"Studio queued edits and undo fence activation against the current authoring store",
	"[studio][procedural][adapter]"
) {
	using namespace engine;
	ProceduralAdapterContext context;
	scene::RegisterSceneClasses();
	script::RegisterProceduralComponents();
	studio::Editor editor;
	editor.Universe = std::make_unique<world::Universe>();
	editor.Commands = std::make_unique<studio::CommandLog>(*editor.Universe);
	const auto world = editor.Universe->Create({.Name = core::Name("procedural-panel-adapter")});
	REQUIRE(world.IsValid());
	editor.ProceduralWorld = world;
	editor.ShowProcedural = true;
	editor.ProceduralPanel = std::make_unique<studio::ProceduralPanelState>();
	editor.ProceduralPreview = std::make_unique<studio::ProceduralEditor>(parallel::JobContext::Serial);
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Initialize}));
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Preview}));
	editor.ApplyProceduralPending();
	editor.ApplyProceduralPending();
	REQUIRE(editor.ProceduralPreview->Progress().size() == 2);
	REQUIRE(
		std::all_of(
			editor.ProceduralPreview->Progress().begin(),
			editor.ProceduralPreview->Progress().end(),
			[](const auto &row) { return row.Status == script::ProceduralJobStatus::Ready; }
		)
	);
	const auto targets = editor.ProceduralPreview->Targets();
	REQUIRE(targets.size() == 2);
	const auto nearMesh = targets[0].Mesh;
	const auto farMesh = targets[1].Mesh;

	REQUIRE(editor.ProceduralPanel->Queue(VillageAt(9)));
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Activate}));
	editor.ApplyProceduralPending();
	const auto &previewStore = editor.ProceduralPreview->PreviewStore();
	const auto *published = previewStore.Resource<script::ProceduralPublished>();
	REQUIRE(published);
	REQUIRE(published->Regions.size() == 1);
	CHECK(published->Regions.front().Scope.X == 20);
	CHECK(previewStore.Get<scene::EditableMesh>(nearMesh)->Revision == 0);
	CHECK(previewStore.Get<scene::EditableMesh>(farMesh)->Revision == 1);
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Activate}));
	editor.ApplyProceduralPending();
	REQUIRE(published->Regions.size() == 2);
	const auto acceptedNear = published->Regions[0];
	const auto nearRevision = previewStore.Get<scene::EditableMesh>(nearMesh)->Revision;
	CHECK(nearRevision == 1);

	REQUIRE(editor.ProceduralPanel->Queue(VillageAt(10)));
	editor.ApplyProceduralPending();
	editor.ApplyProceduralPending();
	REQUIRE(editor.ProceduralPreview->Progress().size() == 1);
	CHECK(editor.ProceduralPreview->Progress().front().Status == script::ProceduralJobStatus::Ready);
	REQUIRE(editor.Commands->Undo());
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Activate}));
	editor.ApplyProceduralPending();
	CHECK(published->Regions[0].ArtifactBytes == acceptedNear.ArtifactBytes);
	CHECK(previewStore.Get<scene::EditableMesh>(nearMesh)->Revision == nearRevision);
	CHECK(editor.ProceduralPreview->ReservedBytes() == 0);
	procedural::Diagnostic diagnostic;
	script::ProceduralPublication checked;
	editor.Universe->Enter(world, [&](ecs::Store &store) {
		CHECK(editor.ProceduralPreview->CaptureAccepted(store, targets[0].Scope, checked, diagnostic));
	});

	REQUIRE(editor.ProceduralPanel->Queue(VillageAt(10)));
	editor.ApplyProceduralPending();
	editor.ApplyProceduralPending();
	REQUIRE(editor.ProceduralPreview->ReservedBytes() != 0);
	editor.ShowProcedural = false;
	editor.Universe->Enter(world, [&](ecs::Store &store) {
		REQUIRE(script::SetProceduralSource(store, VillageAt(11).Source, diagnostic));
	});
	editor.ApplyProceduralPending();
	CHECK_FALSE(editor.ProceduralPreview->Running());
	CHECK(editor.ProceduralPreview->ReservedBytes() == 0);
	CHECK(
		previewStore.Resource<script::ProceduralAuthority>()->Sources.front().Anchor.X ==
		10 * procedural::SUBCELLS_PER_CELL
	);
	CHECK(published->Regions[0].ArtifactBytes == acceptedNear.ArtifactBytes);
}

TEST_CASE(
	"Studio procedural cancellation feedback survives request retirement and idle redraw",
	"[studio][procedural][adapter][feedback]"
) {
	using namespace engine;
	ProceduralAdapterContext context;
	studio::Editor editor;
	InitializeAdapterEditor(editor, "procedural-cancel-feedback");
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Initialize}));
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Preview}));
	editor.ApplyProceduralPending();
	editor.ApplyProceduralPending();
	REQUIRE(editor.ProceduralPreview->Progress().size() == 2);
	REQUIRE(
		std::all_of(
			editor.ProceduralPreview->Progress().begin(),
			editor.ProceduralPreview->Progress().end(),
			[](const auto &row) { return row.Status == script::ProceduralJobStatus::Ready; }
		)
	);
	REQUIRE(editor.ProceduralPreview->ReservedBytes() > 0);
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Cancel}));
	editor.ApplyProceduralPending();
	CHECK_FALSE(editor.ProceduralPreview->Running());
	CHECK(editor.ProceduralPreview->ReservedBytes() == 0);
	CHECK(editor.ProceduralPreview->Progress().empty());
	CHECK(DrawProceduralEditor(editor).find("anceled") != std::string::npos);
	editor.ApplyProceduralPending();
	CHECK(DrawProceduralEditor(editor).find("anceled") != std::string::npos);
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Preview}));
	editor.ApplyProceduralPending();
	CHECK(editor.ProceduralPreview->Running());
	CHECK(DrawProceduralEditor(editor).find("anceled") == std::string::npos);
}

TEST_CASE(
	"Studio procedural source sync refusal remains visible after a successful idle pump",
	"[studio][procedural][adapter][feedback]"
) {
	using namespace engine;
	ProceduralAdapterContext context;
	studio::Editor editor;
	InitializeAdapterEditor(editor, "procedural-sync-feedback");
	REQUIRE(editor.ProceduralPanel->Queue({studio::ProceduralActionKind::Initialize}));
	editor.ApplyProceduralPending();
	const auto *previewAuthority =
		editor.ProceduralPreview->PreviewStore().Resource<script::ProceduralAuthority>();
	REQUIRE(previewAuthority);
	REQUIRE(previewAuthority->Regions.size() == 2);
	const auto oldOwner = previewAuthority->Owner;

	bool encoded = true;
	std::vector<uint8_t> originalBytes;
	procedural::Diagnostic diagnostic;
	editor.Universe->Enter(editor.ProceduralWorld, [&](ecs::Store &store) {
		REQUIRE(script::EncodeProceduralAuthority(store, originalBytes, diagnostic));
		store.RemoveResource<script::ProceduralAuthority>();
		std::vector<uint8_t> bytes;
		encoded = script::EncodeProceduralAuthority(store, bytes, diagnostic);
	});
	REQUIRE_FALSE(encoded);
	CHECK(diagnostic.Code == procedural::Status::MissingSource);
	CHECK(diagnostic.Field == "authority");

	editor.ApplyProceduralPending();
	CHECK(editor.ProceduralPanel->Status == diagnostic.Field);
	const auto firstDraw = DrawProceduralEditor(editor);
	CHECK(firstDraw.find(diagnostic.Field) != std::string::npos);
	editor.ApplyProceduralPending();
	const auto idleDraw = DrawProceduralEditor(editor);
	CHECK(editor.ProceduralPanel->Status == diagnostic.Field);
	CHECK(idleDraw.find(diagnostic.Field) != std::string::npos);
	previewAuthority = editor.ProceduralPreview->PreviewStore().Resource<script::ProceduralAuthority>();
	REQUIRE(previewAuthority);
	CHECK(previewAuthority->Owner == oldOwner);
	CHECK(previewAuthority->Regions.size() == 2);
	CHECK(editor.ProceduralPreview->Targets().size() == 2);
	bool restored = false;
	editor.Universe->Enter(editor.ProceduralWorld, [&](ecs::Store &store) {
		restored = script::DecodeProceduralAuthority(store, originalBytes, diagnostic);
	});
	REQUIRE(restored);
	editor.ApplyProceduralPending();
	CHECK(editor.ProceduralPanel->Status.empty());
	CHECK(DrawProceduralEditor(editor).find("authority") == std::string::npos);
}
