#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/ImageGraphKeyPinProjection.hpp"
#include "../src/ImageGraphSourceKeyEdit.hpp"
#include "../src/ImageGraphTimelineRead.hpp"
#include "../src/TimelineDopesheet.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("studio.timeline_scalar_dopesheet")
TEST_DEPENDS("studio.imagegraph")

namespace {
	using namespace engine::imagegraph;
	Document SharedScalarDocument() {
		Document document;
		document.FormatVersion = 10;
		Node owner{"owner", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}};
		Node alias{"alias", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}};
		alias.InstanceBase = "owner";
		for (Node *node : {&owner, &alias})
			node->SourceAnimatedInputs = {"center"};
		SourceSeparatedVec2Animator axes;
		axes.Port = "center";
		axes.Initialized = true;
		axes.Axes[0].Keys = {{"owner", "center", 1, .25, "source", KeyframeEase{}}};
		axes.Axes[0].Keys.front().SourceKeyId = "x-1";
		axes.Axes[0].Keys.front().SourceDriver = KeyframeLinearDriver{.25};
		axes.Axes[1].Keys = {
			{"owner", "center", 2, 2.0, "source", KeyframeEase{}},
			{"owner", "center", 4, 4.0, "source", KeyframeEase{}}
		};
		axes.Axes[1].Keys[0].SourceKeyId = "y-2";
		axes.Axes[1].Keys[1].SourceKeyId = "y-4";
		alias.SourceSeparatedVec2Animators.emplace().Inputs.push_back(axes);
		for (auto &axis : alias.SourceSeparatedVec2Animators->Inputs.front().Axes)
			for (auto &key : axis.Keys) {
				key.NodeId = "alias";
				key.SourceKeyId.clear();
			}
		owner.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
		document.Nodes = {std::move(owner), std::move(alias)};
		for (const char *node : {"alias", "owner"}) {
			auto key = Keyframe{node, "center", 0, Vector2{.2, .3}, "source", KeyframeEase{}};
			if (node == std::string_view("owner")) key.SourceKeyId = "combined-owner";
			document.Keyframes.push_back(std::move(key));
			document.Tracks.push_back({node, "center", "hold", -1});
		}
		document.SourceAnimators.emplace();
		GroupSubtypeBinding binding{
			"alias", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "center"
		};
		binding.Axes = {GroupAxisStorage::Shared, "owner", "center", "owner", GroupSubtypeAnimator::Animated};
		document.SourceAnimators->Bindings.push_back(std::move(binding));
		document.Outputs = {{"out", "owner", "surface_out"}};
		return document;
	}

	struct Sheet {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc = SharedScalarDocument();
		studio::ImageGraphHistory History;
		studio::TimelineKeyEditor Keys;
		studio::TimelineDopesheet View;
		Diagnostic Error;
		FrameTime Cursor{12, .25, false};
		uint64_t Revision = 0;
		bool UseSourceRead = false;
		studio::ImageGraphTimelineRead Read;
		studio::ImageGraphGroupHost Host;
		unsigned Changes = 0;
		unsigned PasteCalls = 0;
		ImVec2 CanvasMin, CanvasMax;

		Sheet() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Sheet() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		bool Apply() {
			if (UseSourceRead &&
				(!Read.Matches(Host, Revision) || View.OriginalObservationRevision != Read.DisplayRevision)) {
				Error = {Status::InvalidValue, {}, {}, "timeline source changed while editing"};
				return false;
			}
			const auto edit = [&](Document &staged, uint64_t available) {
				if (View.Copying) return View.PrepareCommit(staged, Keys, Error, available);
				return studio::WithImageGraphProjectedKeyPins(
					UseSourceRead ? *Read.Snapshot : Doc,
					staged,
					View.Originals,
					available,
					[&](std::span<const Keyframe> pins, uint64_t remaining) {
						return View.PrepareCommit(staged, Keys, Error, remaining, pins);
					},
					Error,
					View.OriginalAxes
				);
			};
			const bool accepted =
				UseSourceRead ? studio::ApplyImageGraphSourceKeyEdit(
									Doc,
									History,
									Host,
									Revision,
									{},
									edit,
									Error,
									Limits::MaximumEvaluationBytes - *Read.RetainedBytes()
								)
							  : studio::ApplyImageGraphDocumentEdit(Doc, History, [&](Document &staged) {
									return edit(staged, Limits::MaximumEvaluationBytes);
								});
			if (accepted) {
				View.PublishCommit(Keys);
				++Changes;
				++Revision;
			}
			return accepted;
		}
		void Frame() {
			if (UseSourceRead) {
				REQUIRE(studio::PrepareImageGraphTimelineRead(Doc, Host, Revision, {}, Read, Error));
				Keys.ObservationRevision = Read.DisplayRevision;
			}
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({900, 500});
			ImGui::Begin("Scalar Dopesheet", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			const auto paste = [&] {
				++PasteCalls;
				if (UseSourceRead && (!Read.Matches(Host, Revision) ||
									  Keys.OriginalObservationRevision != Read.DisplayRevision))
					return false;
				const auto edit = [&](Document &staged, uint64_t allowance) {
					return Keys.PrepareCommit(staged, Error, allowance);
				};
				const bool accepted =
					UseSourceRead ? studio::ApplyImageGraphSourceKeyEdit(
										Doc,
										History,
										Host,
										Revision,
										{},
										edit,
										Error,
										Limits::MaximumEvaluationBytes - *Read.RetainedBytes()
									)
								  : studio::ApplyImageGraphDocumentEdit(Doc, History, [&](Document &staged) {
										return edit(staged, Limits::MaximumEvaluationBytes);
									});
				if (accepted) {
					Keys.PublishCommit();
					++Changes;
					++Revision;
				}
				return accepted;
			};
			View.Draw(
				UseSourceRead ? *Read.Snapshot : Doc,
				UseSourceRead ? Read.DisplayRevision : Revision,
				Keys,
				Cursor,
				Error,
				[&] { return Apply(); },
				paste
			);
			CanvasMin = ImGui::GetItemRectMin();
			CanvasMax = ImGui::GetItemRectMax();
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
		void Click(size_t marker, bool shift = false) {
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, shift);
			Frame();
			Down(View.Markers.at(marker).Position);
			Up();
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
			Frame();
		}
		void Key(ImGuiKey key) {
			auto &io = ImGui::GetIO();
			io.AddKeyEvent(key, true);
			Frame();
			io.AddKeyEvent(key, false);
			Frame();
		}
		void Chord(ImGuiKey key) {
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
			Frame();
			Key(key);
			ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
			Frame();
		}
		void FocusTrack(size_t marker) {
			const auto position = View.Markers.at(marker).Position;
			Down({CanvasMin.x + 30, position.y});
			Up();
		}
		size_t Marker(std::string_view node, int8_t axis, FrameTime time) const {
			for (size_t index = 0; index < View.Markers.size(); ++index) {
				const auto &marker = View.Markers[index];
				if (marker.Row < View.Tracks.size() && View.Tracks[marker.Row].NodeId == node &&
					View.Tracks[marker.Row].Port == "center" && View.Tracks[marker.Row].Axis == axis &&
					marker.Time == time)
					return index;
			}
			return View.Markers.size();
		}
	};
}

TEST_CASE(
	"scalar Dopesheet tracks distinguish axes and move independently through history",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	Plan plan;
	REQUIRE(Compile(ui.Doc, plan, ui.Error) == Status::Ok);
	ui.Frame();
	ui.Frame();
	REQUIRE(ui.View.Markers.size() == 8);
	const auto x = ui.Marker("owner", 0, {1});
	const auto y = ui.Marker("owner", 1, {2});
	REQUIRE(x < ui.View.Markers.size());
	REQUIRE(y < ui.View.Markers.size());
	CHECK(ui.View.Tracks[ui.View.Markers[x].Row].Axis == 0);
	CHECK(ui.View.Tracks[ui.View.Markers[y].Row].Axis == 1);
	const auto xPoint = ui.View.Markers[x].Position, yPoint = ui.View.Markers[y].Position;
	ui.Down({xPoint.x - 10, xPoint.y - 10});
	ui.Mouse({yPoint.x + 10, yPoint.y + 10});
	ui.Up();
	REQUIRE(ui.Keys.Selection.size() == 2);
	CHECK(ui.Keys.Selection[0].Axis == 0);
	CHECK(ui.Keys.Selection[1].Axis == 1);
	ui.Keys.Selection.clear();
	ui.Click(x);
	ui.Click(y, true);
	REQUIRE(ui.Keys.Selection.size() == 2);
	CHECK(ui.Keys.Selection[0].Axis == 0);
	CHECK(ui.Keys.Selection[1].Axis == 1);
	const auto before = ui.Doc;
	const auto point = ui.View.Markers[x].Position;
	ui.Down(point);
	REQUIRE(ui.View.Dragging);
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	CHECK(ui.Doc == before);
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	const auto &ownerAxes = ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front();
	const auto &aliasAxes = ui.Doc.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	CHECK(GetFrameTime(ownerAxes.Axes[0].Keys.front()) == FrameTime{5, 0, false});
	CHECK(std::any_of(ownerAxes.Axes[1].Keys.begin(), ownerAxes.Axes[1].Keys.end(), [](const auto &key) {
		return key.SourceKeyId == "y-2" && GetFrameTime(key) == FrameTime{6, 0, false};
	}));
	CHECK(
		ownerAxes.Axes[1].Keys.front() ==
		before.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.back()
	);
	CHECK(GetFrameTime(aliasAxes.Axes[0].Keys.front()) == FrameTime{5, 0, false});
	CHECK(std::any_of(aliasAxes.Axes[1].Keys.begin(), aliasAxes.Axes[1].Keys.end(), [](const auto &key) {
		return GetFrameTime(key) == FrameTime{6, 0, false};
	}));
	CHECK(ui.Doc.Keyframes == before.Keyframes);
	const auto moved = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == moved);
}

TEST_CASE(
	"scalar Dopesheet scale and Ctrl+D operate on selected axis channels",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet scaled;
	scaled.Frame();
	scaled.Frame();
	const auto x = scaled.Marker("owner", 0, {1});
	const auto y = scaled.Marker("owner", 1, {2});
	scaled.Click(x);
	scaled.Click(y, true);
	const auto beforeScale = scaled.Doc;
	auto &scaleIo = ImGui::GetIO();
	scaleIo.AddKeyEvent(ImGuiMod_Ctrl, true);
	scaleIo.AddKeyEvent(ImGuiMod_Alt, true);
	scaled.Frame();
	const auto scalePoint = scaled.View.Markers[x].Position;
	scaled.Down(scalePoint);
	REQUIRE(scaled.View.Scaling);
	scaled.Mouse({scalePoint.x + 2 * float(scaled.View.PixelsPerFrame), scalePoint.y});
	scaled.Up();
	INFO(scaled.Error.Message);
	REQUIRE(scaled.Changes == 1);
	const auto &scaledAxes = scaled.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front();
	CHECK(GetFrameTime(scaledAxes.Axes[0].Keys.front()) == FrameTime{3, 0, false});
	CHECK(GetFrameTime(scaledAxes.Axes[1].Keys.front()) == FrameTime{2, 0, false});
	REQUIRE(scaled.History.Undo(scaled.Doc));
	CHECK(scaled.Doc == beforeScale);
	scaleIo.AddKeyEvent(ImGuiMod_Ctrl, false);
	scaleIo.AddKeyEvent(ImGuiMod_Alt, false);
	scaled.Frame();

	Sheet copied;
	copied.Frame();
	copied.Frame();
	const auto copyX = copied.Marker("owner", 0, {1});
	const auto copyY = copied.Marker("owner", 1, {2});
	copied.Click(copyX);
	copied.Click(copyY, true);
	const auto beforeCopy = copied.Doc;
	const auto mouse = copied.View.Markers[copyX].Position;
	copied.Mouse(mouse);
	copied.Chord(ImGuiKey_D);
	REQUIRE(copied.View.Dragging);
	REQUIRE(copied.View.Copying);
	copied.Mouse({mouse.x + 5 * float(copied.View.PixelsPerFrame), mouse.y});
	copied.Down({mouse.x + 5 * float(copied.View.PixelsPerFrame), mouse.y});
	copied.Up();
	INFO(copied.Error.Message);
	REQUIRE(copied.Changes == 1);
	const auto &copyAxes = copied.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front();
	REQUIRE(copyAxes.Axes[0].Keys.size() == 2);
	REQUIRE(copyAxes.Axes[1].Keys.size() == 3);
	const auto xClone =
		std::find_if(copyAxes.Axes[0].Keys.begin(), copyAxes.Axes[0].Keys.end(), [](const auto &key) {
			return GetFrameTime(key) == FrameTime{6, 0, false};
		});
	const auto yClone =
		std::find_if(copyAxes.Axes[1].Keys.begin(), copyAxes.Axes[1].Keys.end(), [](const auto &key) {
			return GetFrameTime(key) == FrameTime{7, 0, false};
		});
	REQUIRE(xClone != copyAxes.Axes[0].Keys.end());
	REQUIRE(yClone != copyAxes.Axes[1].Keys.end());
	CHECK(xClone->SourceKeyId.empty());
	CHECK_FALSE(xClone->SourceDriver);
	CHECK(yClone->SourceKeyId.empty());
	const auto afterCopy = copied.Doc;
	REQUIRE(copied.History.Undo(copied.Doc));
	CHECK(copied.Doc == beforeCopy);
	REQUIRE(copied.History.Redo(copied.Doc));
	CHECK(copied.Doc == afterCopy);
}

TEST_CASE("scalar Dopesheet Alt copy resets source IDs and drivers", "[studio][timeline_scalar_dopesheet]") {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	const auto x = ui.Marker("owner", 0, {1});
	REQUIRE(x < ui.View.Markers.size());
	const auto before = ui.Doc;
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Alt, true);
	ui.Frame();
	const auto point = ui.View.Markers[x].Position;
	ui.Down(point);
	REQUIRE(ui.View.Copying);
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	const auto &keys = ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys;
	REQUIRE(keys.size() == 2);
	CHECK(keys[0].SourceKeyId == "x-1");
	CHECK(
		keys[0].SourceDriver ==
		before.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.front().SourceDriver
	);
	CHECK(GetFrameTime(keys[1]) == FrameTime{5, 0, false});
	CHECK(keys[1].SourceKeyId.empty());
	CHECK_FALSE(keys[1].SourceDriver);
	CHECK_FALSE(
		ui.Doc.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.back().SourceDriver
	);
	io.AddKeyEvent(ImGuiMod_Alt, false);
	ui.Frame();
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
}

TEST_CASE(
	"scalar Dopesheet Delete refuses stale captures and retries after history refusal",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet stale;
	stale.Frame();
	stale.Frame();
	const auto y = stale.Marker("alias", 1, {4});
	REQUIRE(y < stale.View.Markers.size());
	stale.Click(y);
	const auto selected = stale.Keys.Selection;
	REQUIRE(stale.View.BeginDeletion(stale.Doc, stale.Keys, stale.Error));
	const auto originals = stale.View.Originals;
	stale.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys[1].Data = 9.0;
	CHECK_FALSE(stale.Apply());
	CHECK(stale.Error.Code == Status::InvalidValue);
	CHECK(stale.View.Deleting);
	CHECK(stale.Keys.Selection == selected);
	CHECK(stale.View.Originals == originals);
	CHECK_FALSE(stale.History.CanUndo());
	stale.View.Cancel();

	Sheet ui;
	ui.History = studio::ImageGraphHistory(128, 1);
	ui.Frame();
	ui.Frame();
	const auto target = ui.Marker("alias", 1, {4});
	REQUIRE(target < ui.View.Markers.size());
	ui.Click(target);
	const auto before = ui.Doc;
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(ui.CanvasMin.x + 160, ui.View.Markers[target].Position.y);
	ui.Frame();
	ui.Key(ImGuiKey_Delete);
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	ui.History = studio::ImageGraphHistory{};
	ui.Key(ImGuiKey_Delete);
	CHECK(ui.Changes == 1);
	CHECK(ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.size() == 1);
	const auto deleted = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == deleted);
}

TEST_CASE(
	"scalar Dopesheet refuses a focused component paste without changing the clipboard",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	const auto x = ui.Marker("owner", 0, {1});
	const auto combined = ui.Marker("owner", -1, {});
	REQUIRE(x < ui.View.Markers.size());
	REQUIRE(combined < ui.View.Markers.size());
	ui.Click(combined);
	const auto before = ui.Doc;
	ui.Mouse(ui.View.Markers[combined].Position);
	ui.Chord(ImGuiKey_C);
	REQUIRE(ui.Keys.Clipboard.size() == 1);
	REQUIRE(ui.Keys.ClipboardAxes == std::vector<int8_t>{-1});
	const auto clipboard = ui.Keys.Clipboard;
	const auto clipboardAxes = ui.Keys.ClipboardAxes;
	ui.FocusTrack(x);
	REQUIRE(ui.View.FocusedTrack);
	CHECK(ui.View.FocusedTrack->Axis == 0);
	ui.Mouse({ui.CanvasMin.x + 160, ui.View.Markers[x].Position.y});
	ui.Chord(ImGuiKey_V);
	CHECK(ui.Doc == before);
	CHECK(ui.Keys.Clipboard == clipboard);
	CHECK(ui.Keys.ClipboardAxes == clipboardAxes);
	CHECK(ui.Keys.Selection.size() == 1);
	CHECK_FALSE(ui.Keys.Active);
	CHECK(ui.PasteCalls == 1);
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
	CHECK(ui.Error.Code == Status::TypeMismatch);
}

TEST_CASE(
	"scalar graph collision replaces only its own axis at a shared clock",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	for (auto &node : ui.Doc.Nodes)
		node.SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.front().Tick = 4;
	ui.Frame();
	ui.Frame();
	const auto y = ui.Marker("alias", 1, {2});
	REQUIRE(y < ui.View.Markers.size());
	const auto before = ui.Doc;
	const auto point = ui.View.Markers[y].Position;
	ui.Down(point);
	ui.Mouse({point.x + 2 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	CHECK(ui.Doc.Keyframes == before.Keyframes);
	for (size_t index = 0; index < ui.Doc.Nodes.size(); ++index) {
		const auto &axes = ui.Doc.Nodes[index].SourceSeparatedVec2Animators->Inputs.front().Axes;
		CHECK(axes[0].Keys == before.Nodes[index].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys);
		REQUIRE(axes[1].Keys.size() == 1);
		CHECK(GetFrameTime(axes[1].Keys.front()) == FrameTime{4, 0, false});
		CHECK(std::get<double>(axes[1].Keys.front().Data) == 2.0);
	}
	CHECK(
		ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.front().SourceKeyId == "y-2"
	);
	REQUIRE(ui.Keys.Selection.size() == 1);
	CHECK(ui.Keys.Selection[0].Axis == 1);
	CHECK(ui.Keys.Selection[0].Time == FrameTime{4, 0, false});
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
}

TEST_CASE(
	"scalar graph rebuild refuses invalid clocks without replacing the retained display",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	REQUIRE(ui.View.Rebuild(ui.Doc, 0, ui.Keys, ui.Error));
	std::vector<studio::ImageGraphKeyframeIdentity> identities;
	for (const auto &marker : ui.View.Markers)
		identities.push_back(ui.View.MarkerIdentity(marker));
	ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.front().Subframe = 2;
	CHECK_FALSE(ui.View.Rebuild(ui.Doc, 1, ui.Keys, ui.Error));
	CHECK(ui.View.Revision == 0);
	REQUIRE(ui.View.Markers.size() == identities.size());
	for (size_t index = 0; index < identities.size(); ++index)
		CHECK(ui.View.MarkerIdentity(ui.View.Markers[index]) == identities[index]);
	CHECK_FALSE(ui.History.CanUndo());
}

TEST_CASE(
	"scalar graph selects all channels and deletes duplicate physical alias selections once",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	ui.Mouse(ui.View.Markers.front().Position);
	ui.Chord(ImGuiKey_A);
	REQUIRE(ui.Keys.Selection.size() == 8);
	ui.Chord(ImGuiKey_C);
	REQUIRE(ui.Keys.Clipboard.size() == 8);
	CHECK(ui.Keys.ClipboardAxes == std::vector<int8_t>{-1, -1, 0, 1, 1, 0, 1, 1});
	const auto before = ui.Doc;
	ui.Key(ImGuiKey_Delete);
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	CHECK(ui.Keys.Selection.empty());
	CHECK(ui.Doc.Keyframes.empty());
	for (const auto &node : ui.Doc.Nodes)
		for (const auto &axis : node.SourceSeparatedVec2Animators->Inputs.front().Axes)
			CHECK(axis.Keys.empty());
	const auto cleared = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == cleared);
}

TEST_CASE(
	"scalar graph double click selects between-key endpoints on one alias axis",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	ui.Frame();
	ui.Frame();
	const auto left = ui.Marker("alias", 1, {2});
	const auto right = ui.Marker("alias", 1, {4});
	REQUIRE(left < ui.View.Markers.size());
	REQUIRE(right < ui.View.Markers.size());
	const auto a = ui.View.Markers[left].Position, b = ui.View.Markers[right].Position;
	const ImVec2 between{(a.x + b.x) * .5f, a.y};
	const auto before = ui.Doc;
	ui.Down(between);
	ui.Up();
	ui.Down(between);
	ui.Up();
	REQUIRE(ui.Keys.Selection.size() == 2);
	CHECK(ui.Keys.Selection[0] == studio::ImageGraphKeyframeIdentity{"alias", "center", {2}, 1});
	CHECK(ui.Keys.Selection[1] == studio::ImageGraphKeyframeIdentity{"alias", "center", {4}, 1});
	ui.Chord(ImGuiKey_C);
	REQUIRE(ui.Keys.Clipboard.size() == 2);
	CHECK(ui.Keys.ClipboardAxes == std::vector<int8_t>{1, 1});
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
}

TEST_CASE(
	"scalar alias graph gestures borrow missing projections and refresh after history",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	ui.Doc.Nodes[1].SourceSeparatedVec2Animators = {};
	const auto before = ui.Doc;
	ui.Frame();
	ui.Frame();
	REQUIRE(ui.View.Markers.size() == 8);
	const auto x = ui.Marker("alias", 0, {1});
	REQUIRE(x < ui.View.Markers.size());
	ui.Click(x);
	ui.Chord(ImGuiKey_C);
	REQUIRE(ui.Keys.Clipboard.size() == 1);
	CHECK(ui.Keys.Clipboard.front().NodeId == "alias");
	CHECK(ui.Keys.Clipboard.front().SourceKeyId.empty());
	CHECK(ui.Keys.ClipboardAxes == std::vector<int8_t>{0});
	const auto point = ui.View.Markers[x].Position;
	ui.Down(point);
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	CHECK_FALSE(ui.Doc.Nodes[1].SourceSeparatedVec2Animators);
	CHECK(ui.Doc.Keyframes == before.Keyframes);
	CHECK(
		GetFrameTime(ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.front()) ==
		FrameTime{5, 0, false}
	);
	CHECK(ui.Marker("alias", 0, {1}) == ui.View.Markers.size());
	CHECK(ui.Marker("alias", 0, {5}) < ui.View.Markers.size());
	const auto moved = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	++ui.Revision;
	ui.Frame();
	CHECK(ui.Doc == before);
	CHECK(ui.Marker("alias", 0, {1}) < ui.View.Markers.size());
	CHECK(ui.Marker("alias", 0, {5}) == ui.View.Markers.size());
	REQUIRE(ui.History.Redo(ui.Doc));
	++ui.Revision;
	ui.Frame();
	CHECK(ui.Doc == moved);
	const auto target = ui.Marker("alias", 0, {5});
	REQUIRE(target < ui.View.Markers.size());
	ui.Click(target);
	ui.Key(ImGuiKey_Delete);
	REQUIRE(ui.Changes == 2);
	CHECK_FALSE(ui.Doc.Nodes[1].SourceSeparatedVec2Animators);
	CHECK(ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.empty());
	CHECK(ui.Marker("alias", 0, {5}) == ui.View.Markers.size());
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == moved);
}

TEST_CASE(
	"initial scalar alias mouse gestures use host reads and survive history reset",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	ui.UseSourceRead = true;
	ui.Revision = 1;
	ui.Doc.SourceAnimators = {};
	ui.Doc.Nodes[1].SourceSeparatedVec2Animators = {};
	const auto authored = ui.Doc;
	ui.Frame();
	ui.Frame();
	CHECK(ui.Doc == authored);
	REQUIRE(ui.Read.Snapshot);
	const auto before = *ui.Read.Snapshot;
	const auto x = ui.Marker("alias", 0, {1});
	REQUIRE(x < ui.View.Markers.size());
	ui.Click(x);
	ui.Chord(ImGuiKey_C);
	REQUIRE(ui.Keys.Clipboard.size() == 1);
	CHECK(ui.Keys.Clipboard.front().NodeId == "alias");
	CHECK(ui.Keys.Clipboard.front().SourceKeyId.empty());
	CHECK(ui.Keys.ClipboardAxes == std::vector<int8_t>{0});
	const auto point = ui.View.Markers[x].Position;
	ui.Down(point);
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	ui.Up();
	INFO(ui.Error.Message);
	REQUIRE(ui.Changes == 1);
	CHECK(ui.Marker("alias", 0, {5}) < ui.View.Markers.size());
	CHECK(ui.Doc.Keyframes == before.Keyframes);
	const auto moved = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	++ui.Revision;
	ui.Host.Clear();
	ui.Frame();
	CHECK(ui.Doc == before);
	CHECK(ui.Marker("alias", 0, {1}) < ui.View.Markers.size());
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	++ui.Revision;
	ui.Host.Clear();
	ui.Frame();
	CHECK(ui.Doc == moved);
	const auto y = ui.Marker("alias", 1, {4});
	REQUIRE(y < ui.View.Markers.size());
	ui.Click(y);
	ui.Key(ImGuiKey_Delete);
	REQUIRE(ui.Changes == 2);
	CHECK(ui.Marker("alias", 1, {4}) == ui.View.Markers.size());
	CHECK(ui.Marker("alias", 0, {5}) < ui.View.Markers.size());
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == moved);
}

TEST_CASE(
	"scalar alias drag refuses same-revision replay changes before history",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	ui.UseSourceRead = true;
	ui.Revision = 1;
	ui.Doc.SourceAnimators = {};
	ui.Doc.Nodes[1].SourceSeparatedVec2Animators = {};
	ui.Frame();
	ui.Frame();
	const auto x = ui.Marker("alias", 0, {1});
	REQUIRE(x < ui.View.Markers.size());
	const auto point = ui.View.Markers[x].Position;
	ui.Down(point);
	REQUIRE(ui.View.Dragging);
	const auto pins = ui.View.Originals;
	const auto stamp = ui.View.OriginalObservationRevision;
	const auto before = ui.Doc;
	GroupReplayState replacement;
	REQUIRE(RebindGroupReplay(ui.Doc, ui.Host.Replay, ui.Revision, replacement, ui.Error) == Status::Ok);
	ui.Host.Replay = std::move(replacement);
	ui.Mouse({point.x + 4 * float(ui.View.PixelsPerFrame), point.y});
	CHECK(ui.Read.DisplayRevision != stamp);
	CHECK_FALSE(ui.Apply());
	CHECK(ui.Error.Code == Status::InvalidValue);
	CHECK(ui.Doc == before);
	CHECK(ui.View.Originals == pins);
	CHECK(ui.Changes == 0);
	CHECK_FALSE(ui.History.CanUndo());
	ui.View.Cancel();
	ui.Up();
}

TEST_CASE(
	"scalar alias clipboard pastes to the focused component through host history",
	"[studio][timeline_scalar_dopesheet]"
) {
	Sheet ui;
	ui.UseSourceRead = true;
	ui.Revision = 1;
	ui.Doc.SourceAnimators = {};
	ui.Doc.Nodes[1].SourceSeparatedVec2Animators = {};
	ui.Frame();
	ui.Frame();
	const auto x = ui.Marker("alias", 0, {1});
	const auto y = ui.Marker("alias", 1, {4});
	REQUIRE(x < ui.View.Markers.size());
	REQUIRE(y < ui.View.Markers.size());
	ui.Click(x);
	ui.Chord(ImGuiKey_C);
	const auto clipboard = ui.Keys.Clipboard;
	const auto clipboardAxes = ui.Keys.ClipboardAxes;
	REQUIRE(clipboard.size() == 1);
	const auto before = *ui.Read.Snapshot;
	ui.FocusTrack(y);
	REQUIRE(ui.View.FocusedTrack);
	CHECK(ui.View.FocusedTrack->Axis == 1);
	ui.Cursor = {4};
	ui.Mouse({ui.CanvasMin.x + 160, ui.View.Markers[y].Position.y});
	ui.Chord(ImGuiKey_V);
	INFO(ui.Error.Message);
	REQUIRE(ui.PasteCalls == 1);
	REQUIRE(ui.Changes == 1);
	CHECK(ui.Keys.Clipboard == clipboard);
	CHECK(ui.Keys.ClipboardAxes == clipboardAxes);
	CHECK(ui.Doc.Keyframes == before.Keyframes);
	const auto &axes = ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes;
	CHECK(axes[0].Keys == before.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys);
	REQUIRE(axes[1].Keys.size() == 2);
	const auto &pasted = axes[1].Keys.back();
	CHECK(GetFrameTime(pasted) == FrameTime{4});
	CHECK(pasted.Data == clipboard.front().Data);
	CHECK(pasted.SourceKeyId.empty());
	CHECK_FALSE(pasted.SourceDriver);
	CHECK_FALSE(pasted.SineDriver);
	CHECK(ui.Keys.Selection == std::vector<studio::ImageGraphKeyframeIdentity>{{"alias", "center", {4}, 1}});
	const auto after = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == after);
}
