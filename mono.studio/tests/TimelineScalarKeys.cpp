#include "../src/TimelineScalarKeys.hpp"

#include "../src/ImageGraphDocumentEdit.hpp"
#include "../src/TimelineDopesheet.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui_internal.h>

TEST_SUITE_ID("studio.timeline_scalar_keys")
TEST_DEPENDS("studio.imagegraph")

namespace {
	using namespace engine::imagegraph;
	struct ScalarTimeline {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc;
		studio::ImageGraphHistory History;
		studio::TimelineKeyEditor Editor;
		Diagnostic Error;
		ImVec2 DeleteCenter{};
		ImGuiID DeleteId = 0;
		ImGuiID PublishedRowId = 0;
		ImGuiID SelectedRowId = 0;
		unsigned EditCalls = 0;
		bool Changed = false;
		uint64_t EditBudget = Limits::MaximumEvaluationBytes;
		unsigned SelectCalls = 0;

		ScalarTimeline() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.DisplaySize = {800, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Doc.FormatVersion = 10;
			Node owner{"owner", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}};
			Node alias{"alias", "pc.mirror_polar", {}, {}, {{"center", Vector2{.2, .3}}}};
			alias.InstanceBase = "owner";
			for (Node *node : {&owner, &alias})
				node->SourceAnimatedInputs = {"center"};
			SourceSeparatedVec2Animator ownerAxes;
			ownerAxes.Port = "center";
			ownerAxes.Initialized = true;
			ownerAxes.Axes[0].Keys = {{"owner", "center", 1, .25, "source", KeyframeEase{}}};
			ownerAxes.Axes[1].Keys = {
				{"owner", "center", 2, 2.0, "source", KeyframeEase{}},
				{"owner", "center", 4, 4.0, "source", KeyframeEase{}}
			};
			ownerAxes.Axes[0].Keys[0].SourceKeyId = "x-1";
			ownerAxes.Axes[1].Keys[0].SourceKeyId = "y-2";
			ownerAxes.Axes[1].Keys[1].SourceKeyId = "y-4";
			alias.SourceSeparatedVec2Animators.emplace().Inputs.push_back(ownerAxes);
			for (auto &axis : alias.SourceSeparatedVec2Animators->Inputs.front().Axes)
				for (auto &key : axis.Keys) {
					key.NodeId = "alias";
					key.SourceKeyId.clear();
				}
			owner.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(ownerAxes));
			Doc.Nodes = {std::move(owner), std::move(alias)};
			for (const char *node : {"owner", "alias"}) {
				auto key = Keyframe{node, "center", 0, Vector2{.2, .3}, "source", KeyframeEase{}};
				if (node == std::string_view("owner")) key.SourceKeyId = "combined-0";
				Doc.Keyframes.push_back(std::move(key));
				Doc.Tracks.push_back({node, "center", "hold", -1});
			}
			Doc.SourceAnimators.emplace();
			GroupSubtypeBinding binding{
				"alias", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "center"
			};
			binding.Axes = {
				GroupAxisStorage::Shared, "owner", "center", "owner", GroupSubtypeAnimator::Animated
			};
			Doc.SourceAnimators->Bindings.push_back(std::move(binding));
			Doc.Outputs = {{"out", "owner", "surface_out"}};
		}
		~ScalarTimeline() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({700, 500});
			ImGui::Begin("Scalar keys", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			if (ImGui::BeginTable("scalar-keys", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				for (const char *name :
					 {"Key", "Frame", "Interpolation", "Ease in", "Ease out", "Driver", "Kind", "Action"})
					ImGui::TableSetupColumn(name);
				ImGui::TableHeadersRow();
				const auto select = [&](const studio::ImageGraphKeyframeIdentity &) {
					++SelectCalls;
					SelectedRowId = ImGui::GetItemID();
				};
				const auto apply = [&](const auto &edit, uint64_t borrowedBytes) {
					++EditCalls;
					if (borrowedBytes >= EditBudget) {
						Error = {Status::LimitExceeded, {}, {}, "scalar table staging exceeds edit budget"};
						return false;
					}
					bool unchanged = false;
					const bool accepted = studio::ApplyImageGraphDocumentEdit(
						Doc,
						History,
						[&](Document &candidate) { return edit(candidate, EditBudget - borrowedBytes); },
						&unchanged
					);
					return accepted || unchanged;
				};
				Changed = studio::detail::DrawTimelineScalarKeys(Doc, Editor, Error, select, apply);
				const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
				DeleteCenter = {(minimum.x + maximum.x) * .5f, (minimum.y + maximum.y) * .5f};
				DeleteId = ImGui::GetItemID();
				ImGui::EndTable();
				ImGui::End();
				ImGui::Render();
			}
		}
		bool ClickDelete() {
			auto &io = Context->IO;
			io.AddMousePosEvent(DeleteCenter.x, DeleteCenter.y);
			Frame();
			io.AddMouseButtonEvent(0, true);
			Frame();
			bool published = Changed;
			if (Changed) PublishedRowId = DeleteId;
			io.AddMouseButtonEvent(0, false);
			Frame();
			if (Changed) PublishedRowId = DeleteId;
			return published || Changed;
		}
		void Key(ImGuiKey key) {
			auto &io = Context->IO;
			io.AddKeyEvent(key, true);
			Frame();
			io.AddKeyEvent(key, false);
			Frame();
		}
		bool FocusDelete() {
			for (unsigned step = 0; step < 128; ++step) {
				if (Context->NavWindow && Context->NavId == DeleteId) return true;
				Key(ImGuiKey_Tab);
			}
			return Context->NavWindow && Context->NavId == DeleteId;
		}
		void ClickLastRow() {
			auto &io = Context->IO;
			io.AddMousePosEvent(45, DeleteCenter.y);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Frame();
		}
		bool FocusControl(ImGuiID id) {
			for (unsigned step = 0; step < 192; ++step) {
				if (Context->NavWindow && Context->NavId == id) return true;
				Key(ImGuiKey_Tab);
			}
			return Context->NavWindow && Context->NavId == id;
		}
		bool FocusPopupChoice(const char *label) {
			for (unsigned step = 0; step < 8; ++step) {
				if (Context->OpenPopupStack.empty()) return false;
				auto *popup = Context->OpenPopupStack.back().Window;
				if (popup && Context->NavWindow == popup && Context->NavId == popup->GetID(label))
					return true;
				Key(ImGuiKey_DownArrow);
			}
			return false;
		}
		ImGuiID ControlId(const char *label) const {
			return ImHashStr(label, 0, SelectedRowId);
		}
		ImGuiID EaseOutTypeId() const {
			return ImHashStr("##type", 0, ImHashStr("ease-out", 0, SelectedRowId));
		}
		void ShiftTab() {
			auto &io = Context->IO;
			io.AddKeyEvent(ImGuiMod_Shift, true);
			io.AddKeyEvent(ImGuiKey_Tab, true);
			Frame();
			io.AddKeyEvent(ImGuiKey_Tab, false);
			Frame();
			io.AddKeyEvent(ImGuiMod_Shift, false);
			Frame();
		}
	};
}

TEST_CASE(
	"scalar table deletes a captured alias Y key in one history entry", "[studio][timeline_scalar_keys]"
) {
	ScalarTimeline ui;
	Diagnostic compileError;
	Plan plan;
	REQUIRE(Compile(ui.Doc, plan, compileError) == Status::Ok);
	const auto before = ui.Doc;
	ui.Frame();
	const auto deletedRowId = ui.DeleteId;
	REQUIRE(ui.ClickDelete());
	CHECK(ui.PublishedRowId == deletedRowId);
	CHECK(ui.Doc.Keyframes == before.Keyframes);
	const auto &ownerAxes = ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front();
	const auto &aliasAxes = ui.Doc.Nodes[1].SourceSeparatedVec2Animators->Inputs.front();
	CHECK(
		ownerAxes.Axes[0].Keys == before.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys
	);
	CHECK(ownerAxes.Axes[1].Keys.size() == 1);
	CHECK(GetFrameTime(ownerAxes.Axes[1].Keys.front()) == FrameTime{2, 0, false});
	CHECK(
		aliasAxes.Axes[0].Keys == before.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys
	);
	CHECK(aliasAxes.Axes[1].Keys.size() == 1);
	CHECK(GetFrameTime(aliasAxes.Axes[1].Keys.front()) == FrameTime{2, 0, false});
	const auto after = ui.Doc;
	REQUIRE(ui.History.CanUndo());
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == after);
}

TEST_CASE(
	"scalar table delete forwards budget and allows a refused click to retry",
	"[studio][timeline_scalar_keys]"
) {
	ScalarTimeline ui;
	ui.History = studio::ImageGraphHistory(128, 1);
	ui.EditBudget = 1;
	const auto before = ui.Doc;
	ui.Frame();
	CHECK_FALSE(ui.ClickDelete());
	CHECK(ui.Doc == before);
	CHECK(ui.Error.Code == Status::LimitExceeded);
	CHECK_FALSE(ui.History.CanUndo());
	ui.EditBudget = Limits::MaximumEvaluationBytes;
	ui.Frame();
	CHECK_FALSE(ui.ClickDelete());
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
	ui.History = studio::ImageGraphHistory{};
	ui.Frame();
	CHECK(ui.ClickDelete());
	CHECK_FALSE(ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.empty());
	CHECK(ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.size() == 1);
}

TEST_CASE(
	"mixed scalar dopesheet actions keep component channels and publish through history",
	"[studio][timeline_scalar_keys]"
) {
	ScalarTimeline ui;
	studio::TimelineDopesheet sheet;
	const auto &axes = ui.Doc.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes;
	ui.Editor.Selection = {
		studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[1]),
		studio::TimelineKeyEditor::Identity(axes[0].Keys[0], 0),
		studio::TimelineKeyEditor::Identity(axes[1].Keys[0], 1),
		studio::TimelineKeyEditor::Identity(axes[1].Keys[1], 1)
	};
	const auto before = ui.Doc;
	REQUIRE(sheet.BeginAction(ui.Doc, ui.Editor, studio::TimelineKeyAction::Reverse, ui.Error));
	REQUIRE(sheet.OriginalAxes == std::vector<int8_t>{-1, 0, 1, 1});
	CHECK(sheet.Destinations == std::vector<FrameTime>{{}, {1, 0, false}, {2, 0, false}, {}});
	const auto pinned = sheet.Originals;
	ui.Editor.Selection = {studio::TimelineKeyEditor::Identity(ui.Doc.Keyframes[0])};
	const auto edit = [&](Document &candidate, uint64_t available = Limits::MaximumEvaluationBytes) {
		return studio::WithImageGraphProjectedKeyPins(
			ui.Doc,
			candidate,
			sheet.Originals,
			available,
			[&](std::span<const Keyframe> pins, uint64_t remaining) {
				return sheet.PrepareCommit(candidate, ui.Editor, ui.Error, remaining, pins);
			},
			ui.Error,
			sheet.OriginalAxes
		);
	};
	auto refused = ui.Doc;
	CHECK_FALSE(edit(refused, 1));
	CHECK(refused == before);
	CHECK(sheet.Originals == pinned);
	CHECK(sheet.Transforming);
	REQUIRE(studio::ApplyImageGraphDocumentEdit(ui.Doc, ui.History, [&](Document &candidate) {
		return edit(candidate);
	}));
	sheet.PublishCommit(ui.Editor);
	CHECK(ui.Doc.Keyframes.size() == before.Keyframes.size());
	for (const auto &key : before.Keyframes)
		CHECK(std::find(ui.Doc.Keyframes.begin(), ui.Doc.Keyframes.end(), key) != ui.Doc.Keyframes.end());
	CHECK(
		ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys ==
		before.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys
	);
	CHECK(
		GetFrameTime(ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys[0]) ==
		FrameTime{}
	);
	CHECK(ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys[0].SourceKeyId == "y-4");
	REQUIRE(ui.Editor.Selection.size() == 4);
	CHECK(ui.Editor.Selection[1].Axis == 0);
	CHECK(ui.Editor.Selection[1].Time == FrameTime{1, 0, false});
	CHECK(ui.Editor.Selection[3].Axis == 1);
	CHECK(ui.Editor.Selection[3].Time == FrameTime{});
	const auto reversed = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == reversed);

	ui.Editor.Selection = {ui.Editor.Selection[3]};
	const auto selected = ui.Editor.Selection;
	REQUIRE(sheet.BeginDeletion(ui.Doc, ui.Editor, ui.Error));
	REQUIRE(sheet.OriginalAxes == std::vector<int8_t>{1});
	ui.History = studio::ImageGraphHistory(128, 1);
	CHECK_FALSE(studio::ApplyImageGraphDocumentEdit(ui.Doc, ui.History, [&](Document &candidate) {
		return edit(candidate);
	}));
	CHECK(ui.Doc == reversed);
	CHECK(ui.Editor.Selection == selected);
	CHECK(sheet.Deleting);
	sheet.OriginalAxes.push_back(0);
	auto invalid = ui.Doc;
	CHECK_FALSE(sheet.PrepareCommit(invalid, ui.Editor, ui.Error));
	CHECK(invalid == reversed);
	CHECK_FALSE(sheet.Prepared);
	sheet.PublishCommit(ui.Editor);
	CHECK(ui.Editor.Selection == selected);
	CHECK(sheet.Deleting);
	sheet.OriginalAxes.pop_back();
	ui.History = studio::ImageGraphHistory{};
	REQUIRE(studio::ApplyImageGraphDocumentEdit(ui.Doc, ui.History, [&](Document &candidate) {
		return edit(candidate);
	}));
	sheet.PublishCommit(ui.Editor);
	CHECK(ui.Editor.Selection.empty());
	CHECK(ui.Doc.Keyframes == reversed.Keyframes);
	for (const auto &node : ui.Doc.Nodes)
		CHECK(node.SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.size() == 1);
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == reversed);
}

TEST_CASE("scalar table deletes through aliases without local arrays", "[studio][timeline_scalar_keys]") {
	ScalarTimeline ui;
	ui.Doc.Nodes[1].SourceSeparatedVec2Animators = {};
	const auto before = ui.Doc;
	ui.Frame();
	const auto aliasRow = ui.DeleteId;
	REQUIRE(ui.ClickDelete());
	CHECK(ui.PublishedRowId == aliasRow);
	CHECK_FALSE(ui.Doc.Nodes[1].SourceSeparatedVec2Animators);
	CHECK(ui.Doc.Keyframes == before.Keyframes);
	const auto &axes = ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front();
	CHECK(axes.Axes[0].Keys == before.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys);
	REQUIRE(axes.Axes[1].Keys.size() == 1);
	CHECK(axes.Axes[1].Keys.front().SourceKeyId == "y-2");
	const auto deleted = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == deleted);
}

TEST_CASE(
	"scalar kind combo edits the last alias Y key through keyboard navigation and history",
	"[studio][timeline_scalar_keys]"
) {
	ScalarTimeline ui;
	Diagnostic compileError;
	Plan plan;
	REQUIRE(Compile(ui.Doc, plan, compileError) == Status::Ok);
	const auto before = ui.Doc;
	ui.Frame();
	const auto lastRowDelete = ui.DeleteId;
	REQUIRE(ui.FocusDelete());
	CHECK(ui.Context->NavId == lastRowDelete);
	ui.ShiftTab();
	REQUIRE(ui.Context->NavWindow);
	CHECK(ui.Context->NavId != lastRowDelete);
	ui.Key(ImGuiKey_Enter);
	ui.Key(ImGuiKey_DownArrow);
	ui.Key(ImGuiKey_Enter);
	REQUIRE(ui.Doc.Nodes[0].SourceSeparatedVec2Animators);
	REQUIRE(ui.Doc.Nodes[1].SourceSeparatedVec2Animators);
	auto expected = before;
	expected.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.back().Kind =
		KeyframeKind::Adder;
	expected.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.back().Kind =
		KeyframeKind::Adder;
	CHECK(ui.Doc == expected);
	CHECK(ui.Doc.Keyframes == before.Keyframes);
	CHECK(
		ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys ==
		before.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys
	);
	CHECK(
		ui.Doc.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys ==
		before.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys
	);
	CHECK(
		ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys[0] ==
		before.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys[0]
	);
	CHECK(
		ui.Doc.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys[0] ==
		before.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys[0]
	);
	REQUIRE(ui.History.CanUndo());
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == expected);
}

TEST_CASE(
	"scalar table edits alias interpolation and easing without crossing axes",
	"[studio][timeline_scalar_keys]"
) {
	ScalarTimeline ui;
	const auto before = ui.Doc;
	ui.Frame();
	ui.ClickLastRow();
	REQUIRE(ui.SelectedRowId);
	auto expected = before;
	SECTION("outgoing easing side") {
		REQUIRE(ui.FocusControl(ui.EaseOutTypeId()));
		ui.Key(ImGuiKey_Enter);
		REQUIRE(ui.FocusPopupChoice("bezier"));
		ui.Key(ImGuiKey_Enter);
		for (auto &node : expected.Nodes)
			node.SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.back().Ease->OutType = "bezier";
	}
	SECTION("interpolation") {
		REQUIRE(ui.FocusControl(ui.ControlId("##interpolation")));
		ui.Key(ImGuiKey_Enter);
		REQUIRE(ui.FocusPopupChoice("linear"));
		ui.Key(ImGuiKey_Enter);
		for (auto &node : expected.Nodes) {
			auto &key = node.SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.back();
			key.Interpolation = "linear";
			key.Ease.reset();
		}
	}
	INFO(ui.Error.Message);
	CHECK(ui.Doc == expected);
	REQUIRE(ui.History.CanUndo());
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == expected);
}

TEST_CASE(
	"scalar table chooses each source driver on an alias with no local array",
	"[studio][timeline_scalar_keys]"
) {
	for (size_t kind = 1; kind <= 7; ++kind) {
		ScalarTimeline ui;
		ui.Doc.Nodes[1].SourceSeparatedVec2Animators = {};
		const auto before = ui.Doc;
		ui.Frame();
		REQUIRE(ui.FocusDelete());
		ui.ShiftTab();
		ui.ShiftTab();
		ui.Key(ImGuiKey_Enter);
		REQUIRE(ui.Context->OpenPopupStack.size() == 1);
		ui.Key(ImGuiKey_Enter);
		REQUIRE(ui.Context->OpenPopupStack.size() == 2);
		for (size_t step = 0; step < kind; ++step)
			ui.Key(ImGuiKey_DownArrow);
		ui.Key(ImGuiKey_Enter);
		INFO(ui.Error.Message);
		CAPTURE(kind);
		const auto &key = ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[1].Keys.back();
		REQUIRE(key.SourceDriver);
		CHECK(key.SourceDriver->index() == kind - 1);
		CHECK(key.SourceKeyId == "y-4");
		CHECK_FALSE(ui.Doc.Nodes[1].SourceSeparatedVec2Animators);
		CHECK(ui.Doc.Keyframes == before.Keyframes);
		CHECK(
			ui.Doc.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0] ==
			before.Nodes[0].SourceSeparatedVec2Animators->Inputs.front().Axes[0]
		);
		Document reopened;
		Diagnostic error;
		REQUIRE(Read(Write(ui.Doc), reopened, error) == Status::Ok);
		CHECK(reopened == ui.Doc);
		REQUIRE(ui.History.Undo(ui.Doc));
		CHECK(ui.Doc == before);
	}
}

TEST_CASE("scalar metadata refusal retains document and history", "[studio][timeline_scalar_keys]") {
	ScalarTimeline ui;
	ui.History = studio::ImageGraphHistory(128, 1);
	const auto before = ui.Doc;
	ui.Frame();
	ui.ClickLastRow();
	REQUIRE(ui.SelectedRowId);
	REQUIRE(ui.FocusControl(ui.ControlId("##kind")));
	ui.Key(ImGuiKey_Enter);
	ui.Key(ImGuiKey_DownArrow);
	ui.Key(ImGuiKey_Enter);
	CHECK(ui.Doc == before);
	CHECK(ui.EditCalls > 0);
	CHECK_FALSE(ui.History.CanUndo());
}
