#include "../src/TimelineRegions.hpp"

#include "../src/ImageGraphAnimationControl.hpp"
#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/ImageGraphRigid.hpp"

#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <imgui_internal.h>
#include <studio/PxcxSave.hpp>
TEST_SUITE_ID("studio.timeline_regions")
TEST_DEPENDS("studio.imagegraph")
namespace {
	using namespace engine::imagegraph;
	struct Host {
		Document Doc;
		studio::ImageGraphHistory History;
		studio::ImageGraphPlayback Playback;
		studio::TimelineRegions Regions;
		uint64_t Revision = 1;
		Diagnostic Error;
		Host() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {{"n", "value.number", "", {}, {{"value", 1.0}}}};
			Doc.Outputs = {{"v", "n", "number"}};
			Doc.Project.emplace();
			Doc.Timeline = TimelineSettings{20, 0, 19, "stop", 30, SourceAuthoringFrameBounds{}};
			studio::ApplyImageGraphTimeline(Doc, Playback);
			Regions.Synchronize(Doc, Revision, Playback);
		}
		bool
		Edit(std::optional<size_t> target, std::optional<AnimationRegion> next = {}, bool clear = false) {
			return Regions.Commit(
				Doc,
				Revision,
				Playback,
				target,
				next,
				clear,
				[&](const auto &edit) {
					bool accepted = studio::ApplyImageGraphDocumentEdit(Doc, History, edit);
					if (accepted) ++Revision;
					return accepted;
				},
				Error
			);
		}
	};
}
TEST_CASE(
	"Region endpoint priority and frozen playback retain exact signed fractional source observations",
	"[studio][regions]"
) {
	Host ui;
	ui.Doc.Project->AnimationRegions = {{"", {255, 255, 255, 255}, {2, .25, true}, {7, .75, false}}};
	ui.Regions.Selected = 0;
	ui.Regions.Bind(ui.Doc, ui.Playback);
	CHECK(studio::detail::SelectedRegionFirstFrame(ui.Playback) == -3.25);
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Playback) == 6.75);
	const auto held = ui.Playback;
	ui.Playback.SourceBounds->Start = {SourceFrameBoundPresence::Explicit, {4, .5, false}};
	ui.Playback.SourceBounds->End = {SourceFrameBoundPresence::Explicit, {12, 0, false}};
	CHECK(studio::detail::SelectedRegionFirstFrame(ui.Playback) == 3.5);
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Playback) == 11);
	CHECK(studio::detail::SelectedRegionFirstFrame(held) == -3.25);
	CHECK(studio::detail::AnimationPlayback(held).SelectionFrameStart == -2.25);
	AnimationControlInputs commands;
	commands.PlayFromStart = true;
	AnimationControlResult control;
	REQUIRE(
		BuildAnimationControl(commands, studio::detail::AnimationPlayback(held), control, ui.Error) ==
		Status::Ok
	);
	CHECK(control.Playback.RealFrame == -3.25);
	CHECK(control.Playback.CurrentFrame == -3);
	unsigned effects = 0;
	auto actual = held;
	REQUIRE(studio::detail::ApplyAnimationControl(actual, control, [&](auto) { ++effects; }, ui.Error));
	CHECK(studio::GetImageGraphFrame(actual) == FrameTime{3, 0, true});
	CHECK(effects == control.EffectCount);

	ui.Regions.Selected.reset();
	ui.Regions.Bind(ui.Doc, ui.Playback);
	ui.Playback.SourceBounds = SourceAuthoringFrameBounds{};
	CHECK(studio::detail::SelectedRegionFirstFrame(ui.Playback) == 0);
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Playback) == 19);
}
TEST_CASE(
	"Region edits preserve duplicate records through exact sorted permutation and one undo",
	"[studio][regions]"
) {
	Host ui;
	ui.Doc.Project->AnimationRegions = {
		{"same", {1, 2, 3, 255}, {2, 0, false}, {3, 0, false}},
		{"same", {4, 5, 6, 255}, {8, 0, false}, {9, 0, false}}
	};
	ui.Regions.Selected = 1;
	ui.Regions.Bind(ui.Doc, ui.Playback);
	const auto before = ui.Doc;
	auto change = ui.Doc.Project->AnimationRegions[1];
	change.Start = {1, 0, true};
	change.End = {3, 0, true};
	REQUIRE(ui.Edit(1, change));
	REQUIRE(ui.Regions.Selected);
	CHECK(*ui.Regions.Selected == 0);
	CHECK(ui.Doc.Project->AnimationRegions[0].Start == FrameTime{3, 0, true});
	CHECK(ui.Doc.Project->AnimationRegions[0].Color == Colour{4, 5, 6, 255});
	const auto after = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	++ui.Revision;
	ui.Regions.Synchronize(ui.Doc, ui.Revision, ui.Playback);
	CHECK_FALSE(ui.Regions.Selected);
	CHECK_FALSE(ui.Playback.SelectedRegion);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == after);
	Plan plan;
	REQUIRE(Compile(ui.Doc, plan, ui.Error) == Status::Ok);
	const auto encoded = Write(ui.Doc);
	Document decoded;
	REQUIRE(Read(encoded, decoded, ui.Error) == Status::Ok);
	CHECK(decoded == ui.Doc);
}
TEST_CASE(
	"Region settings history refusal preserves original definitions and selected endpoint facts",
	"[studio][regions]"
) {
	Host ui;
	ui.Doc.Project->AnimationRegions = {{"name", {1, 2, 3, 255}, {2, 0, false}, {8, 0, false}}};
	ui.Regions.Selected = 0;
	ui.Regions.Bind(ui.Doc, ui.Playback);
	REQUIRE(ui.Regions.BeginEdit(ui.Doc, ui.Error));
	const auto before = ui.Doc;
	const auto playback = ui.Playback;
	ui.History = studio::ImageGraphHistory{128, 1};
	auto change = *ui.Regions.Original;
	change.Label = "changed";
	CHECK_FALSE(ui.Edit(0, change));
	CHECK(ui.Doc == before);
	CHECK(ui.Playback.SelectedRegion == playback.SelectedRegion);
	REQUIRE(ui.Regions.Selected);
	CHECK(*ui.Regions.Selected == 0);
	REQUIRE(ui.Regions.Original);
	CHECK(*ui.Regions.Original == before.Project->AnimationRegions[0]);
	CHECK_FALSE(ui.History.CanUndo());
	CHECK_FALSE(ui.Regions.BeginEdit(ui.Doc, ui.Error, 1));
	CHECK(*ui.Regions.Original == before.Project->AnimationRegions[0]);
}
namespace {
	struct GuiHost : Host {
		ImGuiContext *Previous = ImGui::GetCurrentContext(), *Context = ImGui::CreateContext();
		ImVec2 Create, Settings, ClearSourceRange, Min, Max;
		unsigned Commits = 0;
		GuiHost() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~GuiHost() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({900, 500});
			ImGui::Begin("Regions", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			const auto origin = ImGui::GetCursorScreenPos(), padding = ImGui::GetStyle().FramePadding,
					   text = ImGui::CalcTextSize("Create region");
			Create = {origin.x + (text.x + padding.x * 2) * .5f, origin.y + (text.y + padding.y * 2) * .5f};
			const auto settingsText = ImGui::CalcTextSize("Region settings");
			Settings = {
				origin.x + text.x + padding.x * 2 + ImGui::GetStyle().ItemSpacing.x +
					(settingsText.x + padding.x * 2) * .5f,
				origin.y + (settingsText.y + padding.y * 2) * .5f
			};
			float clearX = origin.x;
			for (const auto *label : {"Create region", "Region settings", "Delete region", "Clear regions"})
				clearX += ImGui::CalcTextSize(label).x + padding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
			const auto clearText = ImGui::CalcTextSize("Clear source range");
			ClearSourceRange = {
				clearX + (clearText.x + padding.x * 2) * .5f, origin.y + (clearText.y + padding.y * 2) * .5f
			};
			Regions.Draw(Doc, Revision, Playback, 20, 0, Error, [&](const auto &edit) {
				bool accepted = studio::ApplyImageGraphDocumentEdit(Doc, History, edit);
				if (accepted) {
					++Revision;
					++Commits;
				}
				return accepted;
			});
			Min = ImGui::GetItemRectMin();
			Max = ImGui::GetItemRectMax();
			ImGui::End();
			ImGui::Render();
		}
		void Key(ImGuiKey key) {
			ImGui::GetIO().AddKeyEvent(key, true);
			Frame();
			ImGui::GetIO().AddKeyEvent(key, false);
			Frame();
		}
		void Move(ImVec2 p) {
			ImGui::GetIO().AddMousePosEvent(p.x, p.y);
			Frame();
		}
		void Down(ImVec2 p) {
			Move(p);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			Frame();
		}
		void Up() {
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			Frame();
		}
		void Click(ImVec2 p) {
			Down(p);
			Up();
		}
		ImVec2 Endpoint(bool end) const {
			const auto &r = Doc.Project->AnimationRegions[0];
			return {float(Min.x + 140 + FrameTimeToReal(end ? r.End : r.Start) * 20), Min.y + 12};
		}
	};
}
TEST_CASE(
	"Real region create selection toggle and endpoint drag publish one accepted history gesture",
	"[studio][regions]"
) {
	GuiHost ui;
	studio::SetImageGraphAuthorFrame(ui.Playback, {3, .25, false});
	ui.Frame();
	ui.Frame();
	const auto empty = ui.Doc;
	ui.Click(ui.Create);
	REQUIRE(ui.Doc.Project->AnimationRegions.size() == 1);
	CHECK(ui.Commits == 1);
	CHECK(ui.Doc.Project->AnimationRegions[0].Start == FrameTime{4, .25, false});
	CHECK(ui.Doc.Project->AnimationRegions[0].SourceRegionId.empty());
	const auto center = ImVec2{(ui.Endpoint(false).x + ui.Endpoint(true).x) * .5f, ui.Endpoint(false).y};
	ui.Click(center);
	REQUIRE(ui.Regions.Selected);
	REQUIRE(ui.Playback.SelectedRegion);
	CHECK(studio::detail::SelectedRegionFirstFrame(ui.Playback) == 3.25);
	ui.Click(center);
	CHECK_FALSE(ui.Regions.Selected);
	CHECK_FALSE(ui.Playback.SelectedRegion);
	ui.Click(center);
	REQUIRE(ui.Regions.Selected);
	const auto before = ui.Doc;
	const auto endpoint = ui.Endpoint(true);
	ui.Down(endpoint);
	REQUIRE(ui.Regions.Dragging);
	ui.Move({endpoint.x + 50, endpoint.y});
	CHECK(ui.Doc == before); // 2.5 rounds to even 2.
	ui.Up();
	CHECK_FALSE(ui.Regions.Dragging);
	CHECK(ui.Commits == 2);
	CHECK(ui.Doc.Project->AnimationRegions[0].End == FrameTime{22, 0, false});
	const auto after = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == empty);
	REQUIRE(ui.History.Redo(ui.Doc));
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == after);
}
TEST_CASE(
	"Real region endpoint release refusal preserves its source identity selection and redo branch",
	"[studio][regions]"
) {
	GuiHost ui;
	ui.Doc.Project->AnimationRegions = {
		{"duplicate", {1, 2, 3, 255}, {2, 0, false}, {8, 0, false}, "pxc:region:0"}
	};
	ui.Frame();
	ui.Frame();
	ui.Click({(ui.Endpoint(false).x + ui.Endpoint(true).x) * .5f, ui.Endpoint(false).y});
	REQUIRE(ui.Regions.Selected);
	const auto before = ui.Doc;
	const auto held = ui.Playback.SelectedRegion;
	auto edited = before;
	edited.Project->AnimationRegions[0].Color.Red = 9;
	ui.History = studio::ImageGraphHistory{128, std::max(Write(before).size(), Write(edited).size())};
	REQUIRE(ui.History.TryRecord(before, edited));
	ui.Doc = edited;
	REQUIRE(ui.History.Undo(ui.Doc));
	REQUIRE(ui.History.CanRedo());
	const auto endpoint = ui.Endpoint(true);
	ui.Down(endpoint);
	REQUIRE(ui.Regions.Dragging);
	ui.Move({endpoint.x + 40, endpoint.y});
	ui.Up();
	CHECK(ui.Doc == before);
	CHECK(ui.Playback.SelectedRegion == held);
	REQUIRE(ui.Regions.Selected);
	CHECK(*ui.Regions.Selected == 0);
	CHECK_FALSE(ui.Regions.Dragging);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.CanRedo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == edited);
}

TEST_CASE(
	"Region project observation uses frozen selected endpoints rather than a scoped node clock",
	"[studio][regions]"
) {
	Host ui;
	ui.Doc.Project->AnimationRegions = {
		{"region", {1, 2, 3, 255}, {2, .5, false}, {7, .75, false}, "pxc:region:0"}
	};
	ui.Regions.Selected = 0;
	ui.Regions.Bind(ui.Doc, ui.Playback);
	studio::SetImageGraphAuthorFrame(ui.Playback, {3, .25, false});
	const auto held = ui.Playback;
	EvaluationRequest request;
	SetFrameTime(request, {99, .5, false});
	engine::imagegraphphysics::RigidProvider provider;
	studio::detail::BindImageGraphRigid(request, provider, held, true, false);
	REQUIRE(request.SourceCacheProject);
	CHECK(request.SourceCacheProject->ProjectFrame == FrameTime{3, .25, false});
	CHECK(request.SourceCacheProject->ProjectLastFrame == 6.75);
	CHECK(request.SourceCacheProject->ProjectLoading);
	CHECK(GetFrameTime(request) == FrameTime{99, .5, false});
	ui.Playback.SelectedRegion.reset();
	ui.Playback.CurrentTick = 11;
	CHECK(request.SourceCacheProject->ProjectFrame == GetImageGraphFrame(held));
	CHECK(request.SourceCacheProject->ProjectLastFrame == 6.75);
}

TEST_CASE(
	"Real native region creation initializes source missing endpoints only for an absent timeline",
	"[studio][regions]"
) {
	GuiHost ui;
	ui.Doc.Timeline.reset();
	studio::ApplyImageGraphTimeline(ui.Doc, ui.Playback);
	studio::SetImageGraphAuthorFrame(ui.Playback, {3, .25, false});
	const auto before = ui.Doc;
	ui.Frame();
	ui.Frame();
	ui.Click(ui.Create);
	REQUIRE(ui.Doc.Timeline);
	REQUIRE(ui.Doc.Timeline->SourceBounds);
	CHECK(ui.Doc.Timeline->SourceBounds->Start.Presence == SourceFrameBoundPresence::Missing);
	ui.Click({ui.Min.x + 20, ui.Endpoint(false).y});
	REQUIRE(ui.Playback.SelectedRegion);
	CHECK(studio::detail::SelectedRegionFirstFrame(ui.Playback) == 3.25);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.Doc.Timeline);
}
TEST_CASE(
	"Real clear-source-range action opts a legacy window into selected bounds with one undo",
	"[studio][regions]"
) {
	GuiHost ui;
	ui.Doc.Timeline = TimelineSettings{20, 4, 12, "stop", 30, {}};
	ui.Doc.Project->AnimationRegions = {
		{"region", {1, 2, 3, 255}, {2, .5, false}, {8, .75, false}, "pxc:region:0"}
	};
	studio::ApplyImageGraphTimeline(ui.Doc, ui.Playback);
	ui.Frame();
	ui.Frame();
	ui.Click({ui.Min.x + 20, ui.Endpoint(false).y});
	REQUIRE(ui.Regions.Selected);
	CHECK(studio::detail::SelectedRegionFirstFrame(ui.Playback) == 4);
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Playback) == 12);
	const auto before = ui.Doc;
	const auto held = ui.Playback;
	ui.Click(ui.ClearSourceRange);
	CHECK(ui.Commits == 1);
	REQUIRE(ui.Doc.Timeline->SourceBounds);
	CHECK(studio::detail::SelectedRegionFirstFrame(ui.Playback) == 1.5);
	CHECK(studio::detail::SelectedRegionLastFrame(ui.Playback) == 7.75);
	CHECK(studio::detail::SelectedRegionFirstFrame(held) == 4);
	CHECK(studio::detail::SelectedRegionLastFrame(held) == 12);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	studio::ApplyImageGraphTimeline(ui.Doc, ui.Playback);
	++ui.Revision;
	ui.Regions.Synchronize(ui.Doc, ui.Revision, ui.Playback);
	CHECK_FALSE(ui.Playback.SelectedRegion);
	CHECK(studio::detail::SelectedRegionFirstFrame(ui.Playback) == 4);
	REQUIRE(ui.History.Redo(ui.Doc));
	REQUIRE(ui.Doc.Timeline->SourceBounds);
}
TEST_CASE(
	"Selected region playback uses the frozen interval without changing saved native range or key "
	"interpolation",
	"[studio][regions]"
) {
	Host ui;
	ui.Doc.Project->AnimationRegions = {
		{"region", {1, 2, 3, 255}, {2, .25, true}, {4, .75, false}, "pxc:region:0"}
	};
	ui.Regions.Selected = 0;
	ui.Regions.Bind(ui.Doc, ui.Playback);
	ui.Playback.Playing = true;
	studio::SetImageGraphAuthorFrame(ui.Playback, {2, .25, true});
	const auto before = ui.Doc;
	const auto savedStart = ui.Playback.StartTick, savedEnd = ui.Playback.EndTick;
	REQUIRE(studio::detail::AdvanceAnimationPlayback(ui.Playback, 1. / 30));
	CHECK(studio::GetImageGraphFrame(ui.Playback) == FrameTime{1, .25, true});
	CHECK(ui.Playback.StartTick == savedStart);
	CHECK(ui.Playback.EndTick == savedEnd);
	CHECK(ui.Doc == before);
	ui.Playback.Loop = false;
	studio::SetImageGraphAuthorFrame(ui.Playback, {3, .5, false});
	REQUIRE(studio::detail::AdvanceAnimationPlayback(ui.Playback, 1. / 30));
	CHECK(studio::GetImageGraphFrame(ui.Playback) == FrameTime{3, .75, false});
	CHECK_FALSE(ui.Playback.Playing);
	CHECK(ui.Doc == before);
	ui.Playback.Playing = true;
	ui.Playback.Loop = true;
	REQUIRE(studio::detail::AdvanceAnimationPlayback(ui.Playback, 1. / 30));
	CHECK(studio::GetImageGraphFrame(ui.Playback) == FrameTime{3, .25, true});
}

TEST_CASE(
	"Real region settings popup changes the source label once and preserves exact unedited clocks and origin",
	"[studio][regions]"
) {
	GuiHost ui;
	ui.Doc.Project->AnimationRegions = {
		{"old", {1, 2, 3, 255}, {2, .0000000000000001, false}, {8, .75, false}, "pxc:region:0"}
	};
	ui.Frame();
	ui.Frame();
	ui.Click({ui.Min.x + 20, ui.Endpoint(false).y});
	const auto before = ui.Doc;
	ui.Click(ui.Settings);
	REQUIRE(ui.Regions.Editing);
	auto *context = ImGui::GetCurrentContext();
	REQUIRE_FALSE(context->OpenPopupStack.empty());
	auto *popup = context->OpenPopupStack.back().Window;
	REQUIRE(popup);
	ui.Click(
		{popup->Pos.x + ImGui::GetStyle().WindowPadding.x + 20,
		 popup->Pos.y + ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() * .5f}
	);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
	ui.Frame();
	ui.Key(ImGuiKey_A);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
	ui.Frame();
	ImGui::GetIO().AddInputCharactersUTF8("renamed");
	ui.Frame();
	const auto applyId = popup->GetID("Apply region");
	for (size_t i = 0; i < 24 && context->NavId != applyId; ++i)
		ui.Key(ImGuiKey_Tab);
	REQUIRE(context->NavId == applyId);
	ui.Key(ImGuiKey_Enter);
	CHECK(ui.Commits == 1);
	CHECK_FALSE(ui.Regions.Editing);
	REQUIRE(ui.Doc.Project->AnimationRegions.size() == 1);
	auto expected = before;
	expected.Project->AnimationRegions[0].Label = "renamed";
	CHECK(ui.Doc == expected);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == expected);
}

TEST_CASE(
	"Region history saves moved opaque PXC records by original ownership and undo restores exact bytes",
	"[studio][regions][pxcx_save]"
) {
	engine::bake::PxcxArchive archive;
	archive.MetadataNumber = 121092;
	archive.MetadataText = "1.22.10.201";
	archive.GraphJson =
		R"({"nodes":[{"id":"n","type":"Node_Number_Simple","x":0,"y":0,"inputs":[{"r":{"d":1}}]}],"aRegion":[{"l":"same","c":197121,"fs":1,"fe":4,"future":17},{"l":"same","c":197121,"fs":5,"fe":8,"future":29},{"l":"same","c":197121,"fs":9,"fe":12,"future":41}]})";
	archive.GraphJson.push_back('\0');
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	engine::imagegraphio::PxcxImport imported;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, imported, failure));
	Host ui;
	ui.Doc = imported.Graph;
	REQUIRE(Migrate(ui.Doc, ui.Error) == Status::Ok);
	const auto before = ui.Doc;
	REQUIRE(ui.Doc.Project->AnimationRegions.size() == 3);
	ui.Regions.Selected = 1;
	ui.Regions.Bind(ui.Doc, ui.Playback);
	const auto keptOrigin = ui.Doc.Project->AnimationRegions[1].SourceRegionId;
	REQUIRE_FALSE(keptOrigin.empty());
	REQUIRE(ui.Edit(0));
	REQUIRE(ui.Regions.Selected == 0);
	auto moved = ui.Doc.Project->AnimationRegions[0];
	moved.Start = {14, .25, true};
	moved.End = {2, .5, true};
	REQUIRE(ui.Edit(0, moved));
	CHECK(ui.Doc.Project->AnimationRegions[0].SourceRegionId == keptOrigin);
	static std::atomic<uint64_t> serial{0};
	const auto directory =
		std::filesystem::temp_directory_path() /
		("atomic-region-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
		 "-" + std::to_string(serial.fetch_add(1)));
	REQUIRE(std::filesystem::create_directory(directory));
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{directory};
	const auto path = directory / "regions.pxc";
	REQUIRE(studio::SavePxcxProjection(path, imported.Source, ui.Doc, {}, ui.Error));
	const auto read = [&] {
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		REQUIRE(stream.is_open());
		const auto count = stream.tellg();
		REQUIRE(count > 0);
		std::vector<std::byte> result(static_cast<size_t>(count));
		stream.seekg(0);
		stream.read(reinterpret_cast<char *>(result.data()), count);
		REQUIRE(stream.gcount() == count);
		return result;
	};
	engine::bake::PxcxArchive saved;
	REQUIRE(engine::bake::ReadPxcx(read(), saved, failure));
	CHECK(saved.GraphJson.find(R"("future":17)") == std::string::npos);
	CHECK(saved.GraphJson.find(R"("future":29)") != std::string::npos);
	CHECK(saved.GraphJson.find(R"("future":41)") != std::string::npos);
	engine::imagegraphio::PxcxImport reloaded;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(saved, reloaded, failure));
	REQUIRE(reloaded.Graph.Project->AnimationRegions.size() == 2);
	CHECK(reloaded.Graph.Project->AnimationRegions[0].Start == moved.Start);
	CHECK(reloaded.Graph.Project->AnimationRegions[0].End == moved.End);
	REQUIRE(ui.History.Undo(ui.Doc));
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(studio::SavePxcxProjection(path, imported.Source, ui.Doc, {}, ui.Error));
	CHECK(read() == imported.Source.OriginalBytes);
}

TEST_CASE("Selected-region native ping-pong retains the pinned zero lower bounce", "[studio][regions]") {
	Host ui;
	ui.Playback.Playing = true;
	ui.Playback.PingPong = true;
	ui.Playback.Loop = false;
	SECTION("Nonzero start does not replace the source controller's lower zero") {
		ui.Playback.SelectedRegion = std::pair{FrameTime{6, .25, false}, FrameTime{9, .75, false}};
		studio::SetImageGraphAuthorFrame(ui.Playback, {8, .75, false});
		REQUIRE(studio::detail::AdvanceAnimationPlayback(ui.Playback, 1. / 30));
		CHECK(studio::GetImageGraphFrame(ui.Playback) == FrameTime{7, .75, false});
		CHECK(ui.Playback.Direction == -1);
		for (size_t i = 0; i < 7; ++i)
			REQUIRE(studio::detail::AdvanceAnimationPlayback(ui.Playback, 1. / 30));
		CHECK(studio::GetImageGraphFrame(ui.Playback) == FrameTime{0, .75, false});
		REQUIRE(studio::detail::AdvanceAnimationPlayback(ui.Playback, 1. / 30));
		CHECK(studio::GetImageGraphFrame(ui.Playback) == FrameTime{});
		CHECK(ui.Playback.Direction == 1);
	}
	SECTION("A fractional one-frame window still uses last minus one rather than an interval clamp") {
		ui.Playback.SelectedRegion = std::pair{FrameTime{5, .5, false}, FrameTime{5, .5, false}};
		studio::SetImageGraphAuthorFrame(ui.Playback, {4, .5, false});
		REQUIRE(studio::detail::AdvanceAnimationPlayback(ui.Playback, 1. / 30));
		CHECK(studio::GetImageGraphFrame(ui.Playback) == FrameTime{3, .5, false});
		CHECK(ui.Playback.Direction == -1);
		CHECK(ui.Playback.Playing);
	}
	SECTION("Negative endpoints preserve the source controller's explicit zero clamp") {
		ui.Playback.SelectedRegion = std::pair{FrameTime{2, .25, true}, FrameTime{0, .25, true}};
		studio::SetImageGraphAuthorFrame(ui.Playback, {3, .25, true});
		REQUIRE(studio::detail::AdvanceAnimationPlayback(ui.Playback, 1. / 30));
		CHECK(studio::GetImageGraphFrame(ui.Playback) == FrameTime{});
		CHECK(ui.Playback.Direction == 1);
		CHECK_FALSE(studio::detail::AdvanceAnimationPlayback(ui.Playback, 1. / 30));
		CHECK(studio::GetImageGraphFrame(ui.Playback) == FrameTime{});
		CHECK(ui.Playback.Direction == -1);
	}
}
TEST_CASE(
	"An externally revised region closes its real settings popup without publishing stale edits",
	"[studio][regions]"
) {
	GuiHost ui;
	ui.Doc.Project->AnimationRegions = {
		{"old", {1, 2, 3, 255}, {2, 0, false}, {8, 0, false}, "pxc:region:0"}
	};
	ui.Frame();
	ui.Frame();
	ui.Click({ui.Min.x + 20, ui.Endpoint(false).y});
	ui.Click(ui.Settings);
	REQUIRE(ui.Regions.Editing);
	REQUIRE_FALSE(ImGui::GetCurrentContext()->OpenPopupStack.empty());
	const auto before = ui.Doc;
	ui.Regions.Start = 200;
	ui.Regions.StartEdited = true;
	++ui.Revision;
	ui.Frame();
	CHECK_FALSE(ui.Regions.Editing);
	CHECK_FALSE(ui.Regions.Original);
	CHECK_FALSE(ui.Regions.Selected);
	CHECK_FALSE(ui.Playback.SelectedRegion);
	CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
	CHECK(ui.Commits == 0);
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.Undo(ui.Doc));
}
