#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/TimelineDopesheet.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.timeline_source_alias_gestures")
TEST_DEPENDS("studio.imagegraph")
namespace {
	using namespace engine::imagegraph;
	struct AliasSheet {
		ImGuiContext *PreviousContext = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc;
		studio::ImageGraphHistory History;
		studio::TimelineKeyEditor Keys;
		studio::TimelineDopesheet View;
		Diagnostic Error;
		uint64_t Revision = 0;
		unsigned Changes = 0, Attempts = 0;
		AliasSheet() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Doc.FormatVersion = 10;
			Doc.Nodes = {
				{"owner", "pc.invert", {}, {}, {{"mix", .9}}}, {"alias", "pc.invert", {}, {}, {{"mix", .25}}}
			};
			Doc.Nodes[1].InstanceBase = "owner";
			for (auto &node : Doc.Nodes)
				node.SourceAnimatedInputs = {"mix"};
			Keyframe physical{"owner", "native:animator:0", 1, .25, "source", KeyframeEase{}};
			physical.SourceKeyId = "retained-key";
			physical.SourceDriver = KeyframeLinearDriver{.125};
			auto alias = physical;
			alias.NodeId = "alias";
			alias.Port = "mix";
			alias.SourceKeyId.clear();
			Doc.Keyframes = {{"owner", "mix", 0, .9, "source", KeyframeEase{}}, alias};
			Doc.Tracks = {{"owner", "mix", "hold", -1}, {"alias", "mix", "hold", -1}};
			Doc.Outputs = {{"result", "owner", "surface_out"}};
			Doc.SourceAnimators.emplace();
			GroupSubtypeBinding binding{
				"alias", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "mix"
			};
			binding.AnimatorPort = physical.Port;
			Doc.SourceAnimators->Bindings = {binding};
			DetachedSourceAnimator metadata;
			metadata.Id = physical.Port;
			metadata.OwnerId = "owner";
			metadata.OriginalPort = "mix";
			metadata.Writer = GroupSubtypeAnimator::Animated;
			metadata.Type = ValueType::Scalar;
			metadata.Track = AnimationTrack{"owner", physical.Port, "hold", -1};
			Doc.SourceAnimators->Detached = {metadata};
			Doc.SourceAnimators->DetachedValues = {{"owner", std::nullopt, {physical}, physical.Port}};
		}
		~AliasSheet() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(PreviousContext);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({900, 500});
			ImGui::Begin("Dopesheet", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			View.Draw(Doc, Revision, Keys, {}, Error, [&] {
				++Attempts;
				const bool accepted = studio::ApplyImageGraphDocumentEdit(Doc, History, [&](Document &doc) {
					return View.PrepareCommit(doc, Keys, Error);
				});
				if (accepted) {
					View.PublishCommit(Keys);
					++Changes;
					++Revision;
				}
				return accepted;
			});
			ImGui::End();
			ImGui::Render();
		}
		void Mouse(ImVec2 point) {
			ImGui::GetIO().AddMousePosEvent(point.x, point.y);
			Frame();
		}
		void Down(ImVec2 point) {
			Mouse(point);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			Frame();
		}
		void Up() {
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			Frame();
			Frame();
		}
	};
}

TEST_CASE(
	"Dopesheet mouse drag moves retained alias writer in one undo", "[studio][timeline][source_aliases]"
) {
	AliasSheet ui;
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	const auto point = ui.View.Markers.at(1).Position;
	ui.Down(point);
	REQUIRE(ui.View.Dragging);
	ui.Mouse({point.x + 3 * float(ui.View.PixelsPerFrame), point.y});
	CHECK(ui.Doc == before);
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	const auto &physical = ui.Doc.SourceAnimators->DetachedValues[0].Keys[0];
	CHECK(physical.Tick == 4);
	CHECK(physical.SourceKeyId == "retained-key");
	CHECK(physical.SourceDriver == before.SourceAnimators->DetachedValues[0].Keys[0].SourceDriver);
	const auto owner = std::find_if(ui.Doc.Keyframes.begin(), ui.Doc.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "owner" && key.Port == "mix" && key.Tick == 0;
	});
	REQUIRE(owner != ui.Doc.Keyframes.end());
	CHECK(*owner == before.Keyframes[0]);
	CHECK(ui.Doc.SourceAnimators->Bindings == before.SourceAnimators->Bindings);
	const auto accepted = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == accepted);
}

TEST_CASE("Refused alias mouse capture keeps prior selection", "[studio][timeline][source_aliases]") {
	AliasSheet ui;
	ui.Frame();
	ui.Frame();
	ui.Keys.Selection = {studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[0])};
	const auto selected = ui.Keys.Selection;
	const auto before = ui.Doc;
	bool exhaustedBudget = false;
	SECTION("The captured alias key is deleted") {
		// A stale display must not replace selection after its logical key was deleted.
		ui.Doc.SourceAnimators->DetachedValues[0].Keys.clear();
		ui.Doc.Keyframes.erase(ui.Doc.Keyframes.begin() + 1);
	}
	SECTION("The host exhausts the gesture payload budget") {
		ui.Keys.BorrowedBytes = Limits::MaximumEvaluationBytes;
		exhaustedBudget = true;
	}
	ui.Down(ui.View.Markers.at(1).Position);
	CHECK_FALSE(ui.View.Dragging);
	CHECK(ui.Keys.Selection == selected);
	if (exhaustedBudget) {
		CHECK(ui.Error.Code == Status::LimitExceeded);
		CHECK(ui.Error.NodeId == "alias");
		CHECK(ui.Error.Port == "mix");
	}
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
	ui.Up();
	CHECK(ui.Keys.Selection == selected);
	CHECK(ui.Doc.Nodes == before.Nodes);
}

TEST_CASE(
	"Alias drag rejects changed physical writer without an undo entry", "[studio][timeline][source_aliases]"
) {
	AliasSheet ui;
	ui.Frame();
	ui.Frame();
	const auto point = ui.View.Markers.at(1).Position;
	ui.Down(point);
	REQUIRE(ui.View.Dragging);
	ui.Doc.SourceAnimators->DetachedValues[0].Keys[0].SourceDriver = KeyframeLinearDriver{.5};
	const auto changed = ui.Doc;
	ui.Mouse({point.x + 2 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	CHECK(ui.Doc == changed);
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
	CHECK_FALSE(ui.View.Dragging);
}

TEST_CASE("Dopesheet Delete clears retained alias writer in one undo", "[studio][timeline][source_aliases]") {
	AliasSheet ui;
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	ui.Keys.Selection = {studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[1])};
	ui.Mouse(ui.View.Markers.at(1).Position);
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiKey_Delete, true);
	ui.Frame();
	io.AddKeyEvent(ImGuiKey_Delete, false);
	ui.Frame();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	CHECK(ui.Doc.SourceAnimators->DetachedValues[0].Keys.empty());
	CHECK(ui.Doc.Keyframes.size() == 1);
	CHECK(ui.Keys.Selection.empty());
	CHECK(ui.Doc.SourceAnimators->Detached == before.SourceAnimators->Detached);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
}

TEST_CASE(
	"Alias gesture rejects a lingering projection whose physical key was deleted",
	"[studio][timeline][source_aliases]"
) {
	AliasSheet ui;
	ui.Frame();
	ui.Frame();
	const auto point = ui.View.Markers.at(1).Position;
	ui.Doc.SourceAnimators->DetachedValues[0].Keys.clear();
	const auto changed = ui.Doc;
	ui.Down(point);
	REQUIRE(ui.View.Dragging);
	ui.Mouse({point.x + 2 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	CHECK(ui.Attempts == 1);
	CHECK(ui.Changes == 0);
	CHECK(ui.Doc == changed);
	CHECK_FALSE(ui.History.CanUndo());
	CHECK_FALSE(ui.View.Dragging);
	CHECK(ui.Error.Code == Status::InvalidValue);
}
