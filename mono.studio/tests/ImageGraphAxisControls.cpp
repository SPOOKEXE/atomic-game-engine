#include "../src/ImageGraphAxisControls.hpp"

#include "../src/ImageGraphGroupHost.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <imgui.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

TEST_SUITE_ID("studio.imagegraph.axis_controls")

namespace {
	using namespace engine::imagegraph;

	SourceSeparatedVec2Animator AxisState(std::string port, bool separated) {
		SourceSeparatedVec2Animator axes;
		axes.Port = std::move(port);
		axes.Separated = separated;
		axes.Axes[0].Keys = {
			{"mirror", "center", 0, .1, "source", KeyframeEase{}},
			{"mirror", "center", 10, .9, "source", KeyframeEase{}}
		};
		axes.Axes[1].Keys = {
			{"mirror", "center", 0, .2, "source", KeyframeEase{}},
			{"mirror", "center", 10, .8, "source", KeyframeEase{}}
		};
		return axes;
	}

	Document MirrorDocument(bool separated = false) {
		Document document;
		document.FormatVersion = 9;
		Node mirror{"mirror", "pc.mirror_polar", "", {}, {{"center", Vector2{.9, .9}}}};
		mirror.SourceAnimatedInputs = {"center"};
		mirror.SourceSeparatedVec2Animators.emplace().Inputs.push_back(AxisState("center", separated));
		document.Nodes = {
			{"source",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
			std::move(mirror)
		};
		document.Links = {{"source", "image", "mirror", "surface_in"}};
		document.Outputs = {{"out", "mirror", "surface_out"}};
		document.Keyframes = {
			{"mirror", "center", 0, Vector2{.1, .2}, "source", KeyframeEase{}},
			{"mirror", "center", 10, Vector2{.9, .8}, "source", KeyframeEase{}}
		};
		document.Tracks = {{"mirror", "center", "hold"}};
		document.Timeline = TimelineSettings{11};
		return document;
	}

	struct AxisButtonFrame {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		std::optional<bool> Result;
		ImVec2 Center{};

		AxisButtonFrame() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {800, 400};
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~AxisButtonFrame() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}

		void Draw(const Node &node, std::string_view port, const GroupReplayState &replay) {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({360, 120});
			ImGui::Begin(
				"Axis control",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
			);
			Result = studio::detail::DrawImageGraphAxisControl(node, port, replay);
			if (engine::imagegraph::SupportsSourceAxisTransition(node, port)) {
				const auto minimum = ImGui::GetItemRectMin();
				const auto maximum = ImGui::GetItemRectMax();
				Center = {(minimum.x + maximum.x) * .5f, (minimum.y + maximum.y) * .5f};
			}
			ImGui::End();
			ImGui::Render();
		}

		std::optional<bool> Click(const Node &node, std::string_view port, const GroupReplayState &replay) {
			ImGui::SetCurrentContext(Context);
			Draw(node, port, replay);
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(Center.x, Center.y);
			Draw(node, port, replay);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			Draw(node, port, replay);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Draw(node, port, replay);
			return Result;
		}
	};

	bool Prepare(
		studio::ImageGraphGroupHost &host, const Document &document, uint64_t revision, Diagnostic &error
	) {
		Plan plan;
		const auto status = Compile(document, plan, error);
		if (status != Status::Ok) return false;
		EvaluationRequest request;
		return host.Prepare(document, plan, revision, request, error);
	}
}

TEST_CASE(
	"Axis control button follows an alias local flag and ignores unsupported ports",
	"[studio][imagegraph][axis_controls]"
) {
	auto document = MirrorDocument(true);
	Node copy = document.Nodes[1];
	copy.Id = "copy";
	copy.InstanceBase = "mirror";
	copy.InstanceOverrides = {"center"};
	copy.SourceStaticInputs = {"center"};
	copy.SourceAnimatedInputs.clear();
	auto &copyAxes = copy.SourceSeparatedVec2Animators->Inputs.front();
	copyAxes.Separated = false;
	copyAxes.Initialized = false;
	copyAxes.Axes = {};
	document.Nodes.push_back(std::move(copy));
	studio::ImageGraphGroupHost host;
	Diagnostic error;
	INFO(error.Message);
	REQUIRE(Prepare(host, document, 1, error));
	const auto *binding = host.Replay.Binding("copy", "center");
	REQUIRE(binding);
	CHECK(binding->Getter == GroupSubtypeAnimator::Static);
	CHECK(binding->Writer == GroupSubtypeAnimator::Animated);
	AxisButtonFrame ui;
	const auto &alias = document.Nodes.back();
	CHECK_FALSE(engine::imagegraph::SupportsSourceAxisTransition(alias, "surface_in"));
	ui.Draw(alias, "center", host.Replay);
	CHECK(ui.Click(alias, "center", host.Replay) == true);
	CHECK_FALSE(ui.Click(alias, "surface_in", host.Replay).has_value());
}

TEST_CASE(
	"Axis small button toggles local storage and history restores both transitions",
	"[studio][imagegraph][axis_controls][history]"
) {
	auto document = MirrorDocument();
	const auto original = document;
	studio::ImageGraphGroupHost host;
	Diagnostic error;
	REQUIRE(Prepare(host, document, 1, error));
	studio::ImageGraphHistory history;
	AxisButtonFrame ui;
	EvaluationRequest request;
	request.Tick = 0;
	request.GroupReplay = &host.Replay;
	request.GroupAuthoringRevision = 1;
	const auto &mirror = document.Nodes[1];
	REQUIRE(ui.Click(mirror, "center", host.Replay) == true);
	const SourceAxisTransition separate{"mirror", "center", true, true};
	const bool separatedAccepted = host.ToggleAxes(document, history, 1, separate, request, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(separatedAccepted);
	const auto separated = document;
	CHECK(document.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Separated);
	CHECK(document.Keyframes == original.Keyframes);
	CHECK(document.Nodes[1].Values == original.Nodes[1].Values);
	CHECK(document.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes[0].Keys.size() == 2);
	REQUIRE(history.Undo(document));
	CHECK(document == original);
	REQUIRE(history.Redo(document));
	CHECK(document == separated);

	const auto currentRevision = host.Revision;
	request.GroupReplay = &host.Replay;
	request.GroupAuthoringRevision = currentRevision;
	const auto &separatedMirror = document.Nodes[1];
	REQUIRE(ui.Click(separatedMirror, "center", host.Replay) == false);
	const SourceAxisTransition combine{"mirror", "center", false, true};
	const bool combinedAccepted =
		host.ToggleAxes(document, history, currentRevision, combine, request, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(combinedAccepted);
	const auto combined = document;
	CHECK_FALSE(document.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Separated);
	CHECK(document.Nodes[1].Values == original.Nodes[1].Values);
	CHECK(
		document.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes ==
		separated.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Axes
	);
	REQUIRE(history.Undo(document));
	CHECK(document == separated);
	REQUIRE(history.Redo(document));
	CHECK(document == combined);
}

TEST_CASE(
	"Axis edit history cap refuses the button action without publishing authored or replay changes",
	"[studio][imagegraph][axis_controls][history]"
) {
	auto document = MirrorDocument();
	const auto original = document;
	studio::ImageGraphGroupHost host;
	Diagnostic error;
	REQUIRE(Prepare(host, document, 1, error));
	const auto retained = host.Replay.RetainedBytes();
	studio::ImageGraphHistory history(128, 1);
	AxisButtonFrame ui;
	const auto &mirror = document.Nodes[1];
	REQUIRE(ui.Click(mirror, "center", host.Replay) == true);
	EvaluationRequest request;
	request.GroupReplay = &host.Replay;
	request.GroupAuthoringRevision = 1;
	CHECK_FALSE(host.ToggleAxes(
		document, history, 1, SourceAxisTransition{"mirror", "center", true, true}, request, error
	));
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(document == original);
	CHECK_FALSE(history.CanUndo());
	CHECK(host.Revision == 1);
	CHECK(host.Replay.RetainedBytes() == retained);
}
