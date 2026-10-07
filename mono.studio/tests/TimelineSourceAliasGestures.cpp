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
		void SharedFractionalKeys() {
			auto &physical = Doc.SourceAnimators->DetachedValues[0].Keys;
			SetFrameTime(physical[0], {1, .25, false});
			auto middle = physical[0];
			SetFrameTime(middle, {3, .5, false});
			middle.SourceKeyId = "middle-key";
			middle.SourceDriver.reset();
			middle.Data = .5;
			auto last = middle;
			SetFrameTime(last, {5, .75, false});
			last.SourceKeyId = "last-key";
			last.Data = .75;
			physical.push_back(middle);
			physical.push_back(last);
			auto sibling = Doc.Nodes[1];
			sibling.Id = "sibling";
			Doc.Nodes.push_back(sibling);
			auto binding = Doc.SourceAnimators->Bindings[0];
			binding.NodeId = "sibling";
			Doc.SourceAnimators->Bindings.push_back(binding);
			Doc.Tracks.push_back({"sibling", "mix", "hold", -1});
			Doc.Keyframes.resize(1);
			for (const auto &node : {"alias", "sibling"})
				for (auto key : physical) {
					key.NodeId = node;
					key.Port = "mix";
					key.SourceKeyId.clear();
					Doc.Keyframes.push_back(std::move(key));
				}
		}
		ImVec2 Marker(std::string_view node, FrameTime time) const {
			const auto marker =
				std::find_if(View.Markers.begin(), View.Markers.end(), [&](const auto &entry) {
					return View.Tracks[entry.Row].NodeId == node && entry.Time == time;
				});
			REQUIRE(marker != View.Markers.end());
			return marker->Position;
		}
		void CheckRoundTrip(const Document &before) {
			const auto accepted = Doc;
			REQUIRE(History.Undo(Doc));
			CHECK(Doc == before);
			CHECK_FALSE(History.CanUndo());
			REQUIRE(History.Redo(Doc));
			CHECK(Doc == accepted);
			CHECK_FALSE(History.CanRedo());
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

TEST_CASE(
	"Alt-copy through one visible alias fans out one physical copy", "[studio][timeline][source_aliases]"
) {
	AliasSheet ui;
	ui.SharedFractionalKeys();
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	ui.Keys.Selection = {
		studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[1]),
		studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[4])
	};
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Alt, true);
	ui.Frame();
	const auto point = ui.Marker("alias", {1, .25, false});
	ui.Down(point);
	REQUIRE(ui.View.Copying);
	REQUIRE(ui.View.Originals.size() == 1);
	ui.Mouse({point.x + 2.75f * float(ui.View.PixelsPerFrame), point.y});
	CHECK(ui.Doc == before);
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	const auto &physical = ui.Doc.SourceAnimators->DetachedValues[0].Keys;
	REQUIRE(physical.size() == 4);
	const auto copy =
		std::find_if(physical.begin(), physical.end(), [](const auto &key) { return key.Tick == 4; });
	REQUIRE(copy != physical.end());
	CHECK(copy->SourceKeyId.empty());
	CHECK_FALSE(copy->SourceDriver);
	CHECK(std::count_if(ui.Doc.Keyframes.begin(), ui.Doc.Keyframes.end(), [](const auto &key) {
			  return (key.NodeId == "alias" || key.NodeId == "sibling") && key.Tick == 4;
		  }) == 2);
	REQUIRE(ui.Keys.Selection.size() == 1);
	CHECK(ui.Keys.Selection[0].NodeId == "alias");
	CHECK(ui.Keys.Selection[0].Time == FrameTime{4, 0, false});
	ui.CheckRoundTrip(before);
}

TEST_CASE(
	"Ctrl Alt scale deduplicates shared aliases at fractional destinations",
	"[studio][timeline][source_aliases]"
) {
	AliasSheet ui;
	ui.SharedFractionalKeys();
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	for (const auto &key : ui.Doc.Keyframes)
		if (key.NodeId != "owner") ui.Keys.Selection.push_back(studio::TimelineKeyEditor::Identity(key));
	const auto selected = ui.Keys.Selection;
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiMod_Alt, true);
	ui.Frame();
	const auto point = ui.Marker("alias", {5, .75, false});
	ui.Down(point);
	REQUIRE(ui.View.Scaling);
	REQUIRE(ui.View.Originals.size() == 6);
	ui.Mouse({point.x + 2.25f * float(ui.View.PixelsPerFrame), point.y});
	CHECK(ui.Doc == before);
	SECTION("Release commits once") {
		ui.Up();
		INFO(ui.Error.Message);
		REQUIRE(ui.Changes == 1);
		const auto &physical = ui.Doc.SourceAnimators->DetachedValues[0].Keys;
		REQUIRE(physical.size() == 3);
		CHECK(GetFrameTime(physical[0]) == FrameTime{1, .25, false});
		CHECK(GetFrameTime(physical[1]) == FrameTime{4, .625, false});
		CHECK(GetFrameTime(physical[2]) == FrameTime{8, 0, false});
		CHECK(physical[1].SourceKeyId == "middle-key");
		CHECK(ui.Keys.Selection.size() == 6);
		CHECK(std::count_if(ui.Keys.Selection.begin(), ui.Keys.Selection.end(), [](const auto &id) {
				  return id.Time == FrameTime{4, .625, false};
			  }) == 2);
		ui.CheckRoundTrip(before);
	}
	SECTION("Escape cancels before release") {
		io.AddKeyEvent(ImGuiKey_Escape, true);
		ui.Frame();
		io.AddKeyEvent(ImGuiKey_Escape, false);
		ui.Up();
		CHECK(ui.Doc == before);
		CHECK(ui.Keys.Selection == selected);
		CHECK(ui.Attempts == 0);
		CHECK_FALSE(ui.History.CanUndo());
		CHECK_FALSE(ui.View.Dragging);
	}
}

TEST_CASE(
	"Alt-copy collision replaces one shared physical key in one undo", "[studio][timeline][source_aliases]"
) {
	AliasSheet ui;
	ui.SharedFractionalKeys();
	for (auto &key : ui.Doc.Keyframes)
		if (GetFrameTime(key) == FrameTime{3, .5, false}) SetFrameTime(key, {4, 0, false});
	SetFrameTime(ui.Doc.SourceAnimators->DetachedValues[0].Keys[1], {4, 0, false});
	ui.Frame();
	ui.Frame();
	const auto before = ui.Doc;
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Alt, true);
	ui.Frame();
	const auto point = ui.Marker("sibling", {1, .25, false});
	ui.Down(point);
	REQUIRE(ui.View.Copying);
	ui.Mouse({point.x + 2.75f * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	const auto &physical = ui.Doc.SourceAnimators->DetachedValues[0].Keys;
	REQUIRE(physical.size() == 3);
	const auto copy = std::find_if(physical.begin(), physical.end(), [](const auto &key) {
		return GetFrameTime(key) == FrameTime{4, 0, false};
	});
	REQUIRE(copy != physical.end());
	CHECK(copy->Data == before.SourceAnimators->DetachedValues[0].Keys[0].Data);
	CHECK(copy->SourceKeyId.empty());
	CHECK_FALSE(copy->SourceDriver);
	CHECK(std::none_of(physical.begin(), physical.end(), [](const auto &key) {
		return key.SourceKeyId == "middle-key";
	}));
	CHECK(std::count_if(ui.Doc.Keyframes.begin(), ui.Doc.Keyframes.end(), [](const auto &key) {
			  return (key.NodeId == "alias" || key.NodeId == "sibling") &&
					 GetFrameTime(key) == FrameTime{4, 0, false};
		  }) == 2);
	REQUIRE(ui.Keys.Selection.size() == 1);
	CHECK(ui.Keys.Selection[0].NodeId == "sibling");
	ui.CheckRoundTrip(before);
}
