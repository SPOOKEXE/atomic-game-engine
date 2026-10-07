#include "../src/ImageGraphAxisControls.hpp"

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <imgui.h>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.image_composer.axis_observations")
TEST_DEPENDS("studio.imagegraph.axis_controls")

namespace {
	using namespace engine::imagegraph;
	using namespace studio::detail;
	Document SourceDocument() {
		Document document;
		document.FormatVersion = 10;
		Node mirror{"mirror", "pc.mirror_polar", {}, {}, {{"center", Vector2{.9, .9}}}};
		mirror.SourceAnimatedInputs = {"center"};
		mirror.SourceInputExpressions = {{"center", "value + self.center", true}};
		SourceSeparatedVec2Animator axes;
		axes.Port = "center";
		axes.Separated = true;
		// Native linear scalar keys admit fractional interpolation without guessing source key-map coercion.
		axes.Axes[0].Keys = {
			{"mirror", "center", 0, .1, "linear", std::nullopt},
			{"mirror", "center", 0, .9, "linear", std::nullopt}
		};
		axes.Axes[1].Keys = {
			{"mirror", "center", 0, .2, "linear", std::nullopt},
			{"mirror", "center", 0, .8, "linear", std::nullopt}
		};
		REQUIRE(SetFrameTime(axes.Axes[0].Keys[0], {2, .25, true}));
		REQUIRE(SetFrameTime(axes.Axes[0].Keys[1], {3, .5, false}));
		REQUIRE(SetFrameTime(axes.Axes[1].Keys[0], {1, .75, true}));
		REQUIRE(SetFrameTime(axes.Axes[1].Keys[1], {3, .5, false}));
		mirror.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
		document.Nodes = {
			{"source",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
			std::move(mirror)
		};
		document.Links = {{"source", "image", "mirror", "surface_in"}};
		document.Outputs = {{"out", "mirror", "surface_out"}};
		document.Keyframes = {{"mirror", "center", 0, Vector2{.9, .9}, "source", KeyframeEase{}}};
		document.Tracks = {{"mirror", "center", "hold"}};
		return document;
	}
	struct SourceProcessing {
		Document DocumentState = SourceDocument();
		studio::ImageGraphGroupHost Host;
		studio::ImageGraphHistory History;
		ImageGraphAxisObservation Observation;
		ImageGraphAxisObservationIdentity Current{"mirror", "mirror", 1, 7, {1, 0, false}};
		EvaluationRequest Request;
		Diagnostic Error;
		SourceProcessing() {
			Plan plan;
			REQUIRE(Compile(DocumentState, plan, Error) == Status::Ok);
			REQUIRE(SetFrameTime(Request, Current.Frame));
			REQUIRE(Host.Prepare(DocumentState, plan, Current.AuthoringRevision, Request, Error));
			Request.GroupReplay = &Host.Replay;
			Request.GroupAuthoringRevision = Current.AuthoringRevision;
			Current.GroupRevision = Host.Replay.ObservationRevision();
			// Normal processing precedes the action. No evaluation occurs in the button handler.
			ImageGraphAxisProcessingObserver observer{Current};
			Request.SourceInputObserver = &observer;
			Image processed;
			const auto status = Evaluate(DocumentState, plan, "out", Request, processed, Error);
			Request.SourceInputObserver = nullptr;
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			REQUIRE(observer.Candidate.Identity.has_value());
			Observation = std::move(observer.Candidate);
		}
		bool Combine() {
			return ApplyImageGraphAxisControlWithObservation(
				Host,
				DocumentState,
				History,
				Observation,
				Current,
				{"mirror", "center", false, true},
				Request,
				Error
			);
		}
	};
	struct Button {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		ImVec2 Center{};
		unsigned Changes = 0;
		Button() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {800, 400};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Button() {
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		void Frame(SourceProcessing &processing) {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({360, 120});
			ImGui::Begin(
				"Axis observations",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
			);
			const auto action = DrawImageGraphAxisControl(
				processing.DocumentState.Nodes[1], "center", processing.Host.Replay
			);
			const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
			Center = {(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2};
			if (action) {
				REQUIRE_FALSE(*action);
				if (processing.Combine()) ++Changes;
			}
			ImGui::End();
			ImGui::Render();
		}
		void Click(SourceProcessing &processing) {
			Frame(processing);
			ImGui::GetIO().AddMousePosEvent(Center.x, Center.y);
			Frame(processing);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			Frame(processing);
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			Frame(processing);
		}
	};
}

TEST_CASE(
	"Combine button retains the processed self map while sampling signed fractional getter clocks",
	"[studio][axis_observations]"
) {
	SourceProcessing processing;
	const auto before = processing.DocumentState;
	const auto retainedMap = processing.Observation.Inputs;
	const auto center = std::find_if(retainedMap.begin(), retainedMap.end(), [](const auto &input) {
		return input.Port == "center";
	});
	REQUIRE(center != retainedMap.end());
	const auto observed = std::get<Vector2>(center->Data);
	CHECK(observed.X == Catch::Approx(2 * (.1 + .8 * 3.25 / 5.75)));
	CHECK(observed.Y == Catch::Approx(2 * (.2 + .6 * 2.75 / 5.25)));
	Button button;
	button.Click(processing);
	INFO(processing.Error.Message);
	REQUIRE(button.Changes == 1);
	REQUIRE(processing.DocumentState.Keyframes.size() == 3);
	const std::array clocks{FrameTime{2, .25, true}, FrameTime{1, .75, true}, FrameTime{3, .5, false}};
	const std::array raw{Vector2{.1, .2}, Vector2{.1 + .8 * .5 / 5.75, .2}, Vector2{.9, .8}};
	for (size_t index = 0; index < clocks.size(); ++index) {
		const auto &key = processing.DocumentState.Keyframes[index];
		CHECK(GetFrameTime(key) == clocks[index]);
		const auto &tuple = std::get<ArrayValue>(key.Data);
		REQUIRE(tuple.Items.size() == 2);
		const Vector2 value{
			std::get<double>(std::get<ElementValue>(tuple.Items[0].Data)),
			std::get<double>(std::get<ElementValue>(tuple.Items[1].Data))
		};
		CHECK(value.X == Catch::Approx(raw[index].X + observed.X));
		CHECK(value.Y == Catch::Approx(raw[index].Y + observed.Y));
	}
	CHECK(processing.Observation.Inputs == retainedMap);
	const auto accepted = processing.DocumentState;
	REQUIRE(processing.History.Undo(processing.DocumentState));
	CHECK(processing.DocumentState == before);
	CHECK_FALSE(processing.History.CanUndo());
	REQUIRE(processing.History.Redo(processing.DocumentState));
	CHECK(processing.DocumentState == accepted);
}

TEST_CASE(
	"Missing stale or misowned processed axis maps refuse the button before changing history",
	"[studio][axis_observations]"
) {
	SourceProcessing processing;
	const auto before = processing.DocumentState;
	const auto replayBytes = processing.Host.Replay.RetainedBytes();
	SECTION("Missing observation") {
		processing.Observation = {};
	}
	SECTION("Changed authoring revision") {
		++processing.Current.AuthoringRevision;
	}
	SECTION("Changed retained group processing generation") {
		++processing.Current.GroupRevision;
	}
	SECTION("Changed external input generation") {
		++processing.Current.InputRevision;
	}
	SECTION("Changed signed fractional playback time") {
		processing.Current.Frame = {1, .25, true};
	}
	SECTION("Different expression owner") {
		processing.Current.OwnerId = "source";
	}
	SECTION("Different selected node") {
		processing.Current.NodeId = "source";
	}
	Button button;
	button.Click(processing);
	CHECK(button.Changes == 0);
	CHECK(processing.Error.Code == Status::InvalidValue);
	CHECK(processing.DocumentState == before);
	CHECK(processing.Host.Revision == 1);
	CHECK(processing.Host.Replay.RetainedBytes() == replayBytes);
	CHECK_FALSE(processing.History.CanUndo());
}

TEST_CASE(
	"Retained source axis maps admit replacement overlap and refuse duplicate or oversized payloads "
	"atomically",
	"[studio][axis_observations]"
) {
	SourceProcessing processing;
	const auto beforeIdentity = processing.Observation.Identity;
	const auto beforeInputs = processing.Observation.Inputs;
	const auto held = processing.Observation.RetainedBytes();
	REQUIRE(held);
	const std::array duplicate{AuthoredValue{"center", Vector2{}}, AuthoredValue{"center", Vector2{1, 2}}};
	CHECK_FALSE(RetainImageGraphAxisObservation(
		processing.Current, duplicate, processing.Observation, processing.Error
	));
	CHECK(processing.Error.Code == Status::InvalidValue);
	const std::array replacement{AuthoredValue{"center", Vector2{1, 2}}};
	CHECK_FALSE(RetainImageGraphAxisObservation(
		processing.Current, replacement, processing.Observation, processing.Error, *held
	));
	CHECK(processing.Error.Code == Status::LimitExceeded);
	CHECK(processing.Observation.Identity == beforeIdentity);
	CHECK(processing.Observation.Inputs == beforeInputs);
	ImageGraphAxisObservation knownEmpty;
	REQUIRE(RetainImageGraphAxisObservation(processing.Current, {}, knownEmpty, processing.Error));
	SourceAxisTransition transition{"mirror", "center", false};
	REQUIRE(BindImageGraphAxisObservation(knownEmpty, processing.Current, transition, processing.Error));
	REQUIRE(transition.ObservedInputs);
	CHECK(transition.ObservedInputs->empty());
}

TEST_CASE("selected axis owner sets retain mixed inherited contexts and publish only complete previews") {
	Document original;
	original.FormatVersion = 10;
	Node base{
		"base",
		"pc.gradient_points_n",
		{},
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"blend_mode", EnumValue{0}},
		 {"attribute_color_depth", EnumValue{3}}}
	};
	for (size_t index = 0; index < 2; ++index) {
		const auto suffix = std::to_string(index);
		base.DynamicInputs.push_back(
			{"point_i_" + suffix, ValueType::Vector2, Vector2{double(index), double(index)}}
		);
		base.DynamicInputs.push_back({"point_i_unit_" + suffix, ValueType::Enum, EnumValue{0}});
		base.DynamicInputs.push_back({"color_i_" + suffix, ValueType::Colour, Colour{255, 255, 255, 255}});
		base.DynamicInputs.push_back({"influence_i_" + suffix, ValueType::Scalar, 6.0});
	}
	Node copy = base;
	copy.Id = "copy";
	copy.InstanceBase = "base";
	copy.InstanceOverrides = {"point_i_1"};
	original.Nodes = {std::move(base), std::move(copy)};
	original.Junctions = {{"point", {}, ValueType::Vector2, Value{Vector2{.3, .4}}}};
	original.Links = {{"point", "value", "copy", "point_i_1"}};
	original.Outputs = {{"base-out", "base", "surface_out"}, {"copy-out", "copy", "surface_out"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(original, plan, error) == Status::Ok);
	studio::ImageGraphGroupHost host;
	EvaluationRequest request;
	const FrameTime frame{2, .25, true};
	REQUIRE(SetFrameTime(request, frame));
	REQUIRE(host.Prepare(original, plan, 1, request, error));
	// A real physical input move retains the original writer but selects the local
	// expression context for the surviving linked input. Other axes remain inherited.
	auto document = original;
	// Unit controls are attributes with no physical source index; the alias move
	// leaves them declared and moves only physical point/color/influence sockets.
	const std::array<std::string_view, 3> fields{"point_i_", "color_i_", "influence_i_"};
	std::vector<SourceInputMove> moves;
	std::vector<std::array<std::string, 2>> names;
	names.reserve(fields.size());
	for (const auto field : fields)
		names.push_back({std::string(field) + "0", std::string(field) + "1"});
	for (const auto &name : names) {
		std::erase_if(document.Nodes[1].DynamicInputs, [&](const auto &input) {
			return input.Id == name[0];
		});
		for (auto &input : document.Nodes[1].DynamicInputs)
			if (input.Id == name[1]) input.Id = name[0];
		moves.push_back({"copy", name[0], {}});
		moves.push_back({"copy", name[1], name[0]});
	}
	document.Nodes[1].InstanceOverrides = {"point_i_0"};
	document.Links[0].ToPort = "point_i_0";
	GroupReplayState moved;
	const auto moveStatus =
		RebindGroupReplayWithInputMoves(original, document, moves, host.Replay, 2, moved, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(moveStatus == Status::Ok);
	host.Replay = std::move(moved);
	host.Revision = 2;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(host.Replay.Binding("copy", "point_i_0"));
	REQUIRE(host.Replay.Binding("copy", "point_i_0")->AnimatorPort == "point_i_1");
	REQUIRE(SourceInputExpressionOwner(document, plan, "copy", "point_i_0", &host.Replay) == "copy");
	REQUIRE(SourceInputExpressionOwner(document, plan, "copy", "dimension", &host.Replay) == "base");
	request.GroupReplay = &host.Replay;
	request.GroupAuthoringRevision = 2;
	ImageGraphAxisProcessingObservers candidate, retained;
	REQUIRE(PrepareImageGraphAxisProcessingObservers(
		document, plan, host.Replay, "copy", 2, 7, frame, candidate, error
	));
	REQUIRE(candidate.Owners.size() == 2);
	REQUIRE(candidate.ObservesNode("base"));
	REQUIRE(candidate.ObservesNode("copy"));
	REQUIRE_FALSE(candidate.ObservesNode("unrelated"));
	request.SourceInputObserver = &candidate;
	Image output;
	INFO(error.Message);
	REQUIRE(Evaluate(document, plan, "copy-out", request, output, error) == Status::Ok);
	REQUIRE(candidate.Find({"copy", "copy", 2, 7, frame, host.Replay.ObservationRevision()}));
	REQUIRE_FALSE(candidate.Find({"copy", "base", 2, 7, frame, host.Replay.ObservationRevision()}));
	REQUIRE(Evaluate(document, plan, "base-out", request, output, error) == Status::Ok);
	request.SourceInputObserver = nullptr;
	PublishImageGraphAxisProcessingObservers(retained, std::move(candidate), true, "copy", 2, 7, frame);
	REQUIRE(retained.Find({"copy", "base", 2, 7, frame, host.Replay.ObservationRevision()}));
	REQUIRE(retained.Find({"copy", "copy", 2, 7, frame, host.Replay.ObservationRevision()}));
	REQUIRE_FALSE(retained.Find({"copy", "base", 2, 8, frame, host.Replay.ObservationRevision()}));
	SECTION("optional receipt cap preserves normal preview and invalidates the exact old owner map") {
		ImageGraphAxisProcessingObservers limited;
		REQUIRE(PrepareImageGraphAxisProcessingObservers(
			document, plan, host.Replay, "copy", 2, 7, frame, limited, error
		));
		limited.MaximumReceiptBytes = 1;
		request.SourceInputObserver = &limited;
		REQUIRE(Evaluate(document, plan, "copy-out", request, output, error) == Status::Ok);
		request.SourceInputObserver = nullptr;
		REQUIRE(error.Code == Status::Ok);
		PublishImageGraphAxisProcessingObservers(retained, std::move(limited), true, "copy", 2, 7, frame);
		const ImageGraphAxisObservationIdentity identity{
			"copy", "copy", 2, 7, frame, host.Replay.ObservationRevision()
		};
		REQUIRE_FALSE(retained.Find(identity));
		REQUIRE(retained.FailureFor(identity) == Status::LimitExceeded);
	}

	SECTION("later failure invalidates prior receipts") {
		ImageGraphAxisProcessingObservers failed;
		REQUIRE(PrepareImageGraphAxisProcessingObservers(
			document, plan, host.Replay, "copy", 2, 7, frame, failed, error
		));
		PublishImageGraphAxisProcessingObservers(retained, std::move(failed), false, "copy", 2, 7, frame);
		REQUIRE(retained.Owners.empty());
	}
	SECTION("same-context cached preview preserves prior processing maps") {
		ImageGraphAxisProcessingObservers cached;
		REQUIRE(PrepareImageGraphAxisProcessingObservers(
			document, plan, host.Replay, "copy", 2, 7, frame, cached, error
		));
		PublishImageGraphAxisProcessingObservers(retained, std::move(cached), true, "copy", 2, 7, frame);
		REQUIRE(retained.Find({"copy", "base", 2, 7, frame, host.Replay.ObservationRevision()}));
		REQUIRE(retained.Find({"copy", "copy", 2, 7, frame, host.Replay.ObservationRevision()}));
	}
}
