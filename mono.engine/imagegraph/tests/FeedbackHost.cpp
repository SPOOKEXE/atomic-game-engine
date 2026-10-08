#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
TEST_SUITE_ID("engine.imagegraph.feedback_host")
using namespace engine::imagegraph;

namespace {
	Document CommonBoolean(bool grouped) {
		Document document;
		document.FormatVersion = 11;
		document.Nodes = {{"first", "pc.boolean", grouped ? "frame" : "", {}, {{"value", true}}}};
		document.Outputs = {{"first-value", "first", "boolean"}};
		if (grouped) document.Groups = {{"frame", "Frame"}};
		SourceCommonOwnerRecord owner;
		owner.SourceOwnerId = "first";
		owner.SourceType = "Node_Boolean";
		owner.NativeOwnerId = "first";
		owner.UpdateAnimatorOwnerId = "first";
		owner.UpdateAnimatorPort = "native:animator:1";
		document.SourceCommonOwners = {owner};
		document.SourceAnimators.emplace();
		DetachedSourceAnimator animator;
		animator.OwnerId = "first";
		animator.Id = "native:animator:1";
		animator.OriginalPort = "pxcx.update_in_trigger";
		animator.Type = ValueType::Boolean;
		document.SourceAnimators->Detached = {animator};
		GroupSubtypeOverlay payload;
		payload.NodeId = "first";
		payload.Port = animator.Id;
		payload.Fixed = false;
		document.SourceAnimators->DetachedValues = {payload};
		return document;
	}
}

TEST_CASE(
	"native common steps survive normal feedback preparation and failed replacement",
	"[imagegraph][feedback][source_common]"
) {
	const auto exercise = [](bool grouped) {
		Document document = CommonBoolean(grouped);
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		CapturedFeedbackHost host;
		EvaluationRequest request;
		request.SourceSafeMode = false;
		REQUIRE(
			host.InitializeSourceCommonRuntime(
				document, plan, request, SourceNodeInitialState::Loaded, diagnostic, 1, 1
			) == Status::Ok
		);
		const SourceCommonRuntimeObservations observations{};
		REQUIRE(
			host.StepSourceCommonRuntime(document, plan, request, observations, diagnostic, 1, 1) ==
			Status::Ok
		);
		EvaluatedValue held;
		REQUIRE(
			host.ReadSourceCommonGetter(
				document, plan, "first", SourceCommonSelector::Update, request, held, diagnostic, 1, 1
			) == Status::Ok
		);
		CHECK(std::get<bool>(held.Data) == false);

		request = {};
		request.SourceSafeMode = false;
		REQUIRE(host.Prepare(
			document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "first-value"
		));
		CHECK(host.Value("first-value") != nullptr);
		REQUIRE(request.GroupRender != nullptr);
		CHECK(request.SourceCommon == &request.GroupRender->Common);
		CHECK(request.SourceCommonAnimators == &request.GroupRender->CommonAnimators);
		CHECK(request.SimulationReplay != nullptr);
		CHECK(request.SurfaceReplay != nullptr);
		CHECK(request.RandomReplay != nullptr);
		CHECK(request.DataReplay != nullptr);
		CHECK(request.RigidReplay != nullptr);
		const auto prepared = host.PreparedGroups(1, 1, {});
		REQUIRE(prepared != nullptr);
		REQUIRE(prepared->Common.Owners.size() == 1);
		CHECK(prepared->Common.Owners.front().OwnerId == "first");
		REQUIRE(
			host.ReadSourceCommonGetter(
				document, plan, "first", SourceCommonSelector::Update, request, held, diagnostic, 1, 1
			) == Status::Ok
		);
		CHECK(std::get<bool>(held.Data) == false);

		const auto before = *prepared;
		CHECK(
			host.StepSourceCommonRuntime(document, plan, request, observations, diagnostic, 1, 1, 1) ==
			Status::LimitExceeded
		);
		const auto after = host.PreparedGroups(1, 1, {});
		REQUIRE(after != nullptr);
		CHECK(after->Common == before.Common);
		CHECK(after->CommonAnimators == before.CommonAnimators);
		CHECK(after->SourceCommonWrites == before.SourceCommonWrites);
	};

	SECTION("ungrouped graph") {
		exercise(false);
	}
	SECTION("grouped graph") {
		exercise(true);
	}
}

TEST_CASE(
	"native common steps retain the preceding feedback image without exposing stale named outputs",
	"[imagegraph][feedback][source_common]"
) {
	Document document = CommonBoolean(false);
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = document.Project->SurfaceHeight = 1;
	document.Nodes.push_back(
		{"prior", "image.captured", {}, {}, {{"source_id", std::string{"feedback:out"}}}}
	);
	document.Nodes.push_back({"invert", "image.invert", {}, {}, {{"include_alpha", false}}});
	document.Links = {{"prior", "image", "invert", "image"}};
	document.Outputs = {{"out", "invert", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.SourceSafeMode = false;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	REQUIRE(host.Output("out") != nullptr);
	REQUIRE(host.Output("out")->Pixels.front() == 255);
	REQUIRE(
		host.InitializeSourceCommonRuntime(
			document, plan, request, SourceNodeInitialState::Loaded, diagnostic, 1, 1
		) == Status::Ok
	);
	request = {};
	request.Tick = 1;
	request.SourceSafeMode = false;
	REQUIRE(host.StepSourceCommonRuntime(document, plan, request, {}, diagnostic, 1, 1) == Status::Ok);
	CHECK(host.Value("out") == nullptr);
	CHECK_FALSE(host.PreparedFrame(1, 1).has_value());
	CHECK(host.PreparedData(1, 1) == nullptr);
	EvaluatedValue held;
	REQUIRE(
		host.ReadSourceCommonGetter(
			document, plan, "first", SourceCommonSelector::Update, request, held, diagnostic, 1, 1
		) == Status::Ok
	);
	const auto common = host.PreparedGroups(1, 1, {1, 0, false});
	REQUIRE(common != nullptr);
	const auto before = common->Common;
	CHECK_FALSE(host.Prepare(document, plan, 1, 1, request, diagnostic, 1));
	CHECK(host.Value("out") == nullptr);
	REQUIRE(host.PreparedGroups(1, 1, {1, 0, false}) != nullptr);
	CHECK(host.PreparedGroups(1, 1, {1, 0, false})->Common == before);
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	REQUIRE(host.Output("out") != nullptr);
	CHECK(host.Output("out")->Pixels.front() == 0);
	REQUIRE(request.ImageSources.size() == 1);
	CHECK(request.ImageSources.front().Data.Pixels.front() == 255);
	const FrameTime publishedFrame{1, 0, false};
	CHECK(host.PreparedFrame(1, 1) == std::optional<FrameTime>{publishedFrame});
	// Repeating the same frame after a common pulse still consumes tick zero's image, not tick one's.
	request = {};
	request.Tick = 1;
	request.SourceSafeMode = false;
	REQUIRE(host.StepSourceCommonRuntime(document, plan, request, {}, diagnostic, 1, 1) == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	REQUIRE(host.Output("out") != nullptr);
	CHECK(host.Output("out")->Pixels.front() == 0);
}

TEST_CASE(
	"empty common owner initialization retires membership while preserving ordinary host outputs",
	"[imagegraph][feedback][source_common]"
) {
	auto document = CommonBoolean(true);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.SourceSafeMode = false;
	REQUIRE(
		host.InitializeSourceCommonRuntime(
			document, plan, request, SourceNodeInitialState::Loaded, diagnostic, 1, 1
		) == Status::Ok
	);
	REQUIRE(host.StepSourceCommonRuntime(document, plan, request, {}, diagnostic, 1, 1) == Status::Ok);
	REQUIRE(
		host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "first-value")
	);
	const auto value = host.Value("first-value");
	REQUIRE(value != nullptr);
	const auto savedOutput = std::get<EvaluatedValue>(value->Output);
	const auto prepared = host.PreparedGroups(1, 1, {});
	REQUIRE(prepared != nullptr);
	const auto outputs = prepared->Outputs;
	const auto readiness = prepared->Nodes;
	const auto simulation = *request.SimulationReplay;
	const auto data = *request.DataReplay;
	document.SourceCommonOwners.clear();
	document.SourceAnimators = {};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	// Owner retirement still validates the supplied plan and frame before replacing any history.
	const auto common = prepared->Common;
	const auto animators = prepared->CommonAnimators;
	const auto bindings = prepared->SourceCommonBindings;
	const auto inputs = prepared->SourceCommonInputs;
	const auto writes = prepared->SourceCommonWrites;
	const auto retained = host.RetainedBytes();
	const auto unchanged = [&] {
		CHECK(prepared->Common == common);
		CHECK(prepared->CommonAnimators == animators);
		CHECK(prepared->SourceCommonBindings == bindings);
		CHECK(prepared->SourceCommonInputs == inputs);
		CHECK(prepared->SourceCommonWrites == writes);
		CHECK(prepared->Outputs == outputs);
		CHECK(prepared->Nodes == readiness);
		REQUIRE(host.Value("first-value") != nullptr);
		CHECK(std::get<EvaluatedValue>(host.Value("first-value")->Output) == savedOutput);
		CHECK(host.RetainedBytes() == retained);
	};
	auto invalidPlan = plan;
	invalidPlan.NodeOrder.clear();
	CHECK(
		host.InitializeSourceCommonRuntime(
			document, invalidPlan, request, SourceNodeInitialState::Loaded, diagnostic, 2, 1
		) == Status::InvalidOutput
	);
	unchanged();
	auto invalidClock = request;
	invalidClock.Subframe = std::numeric_limits<double>::quiet_NaN();
	CHECK(
		host.ReconcileSourceCommonRuntime(document, plan, invalidClock, {}, diagnostic, 2, 1) ==
		Status::InvalidValue
	);
	unchanged();
	auto invalidDocument = document;
	invalidDocument.Nodes.front().Type = "absent.type";
	CHECK(
		host.InitializeSourceCommonRuntime(
			invalidDocument, plan, request, SourceNodeInitialState::Loaded, diagnostic, 2, 1
		) == Status::UnknownNode
	);
	unchanged();
	CHECK(
		host.InitializeSourceCommonRuntime(
			document, plan, request, SourceNodeInitialState::Loaded, diagnostic, 2, 1, 1
		) == Status::LimitExceeded
	);
	CHECK(prepared->Common.Owners.size() == 1);
	REQUIRE(
		host.InitializeSourceCommonRuntime(
			document, plan, request, SourceNodeInitialState::Loaded, diagnostic, 2, 1
		) == Status::Ok
	);
	CHECK(prepared->Common.Owners.empty());
	CHECK(prepared->CommonAnimators.Detached.empty());
	CHECK(prepared->CommonAnimators.DetachedValues.empty());
	CHECK(prepared->SourceCommonBindings.empty());
	CHECK(prepared->SourceCommonInputs.empty());
	CHECK(prepared->SourceCommonWrites.empty());
	CHECK(prepared->Outputs == outputs);
	CHECK(prepared->Nodes == readiness);
	REQUIRE(host.Value("first-value") != nullptr);
	CHECK(std::get<EvaluatedValue>(host.Value("first-value")->Output) == savedOutput);
	CHECK(*request.SimulationReplay == simulation);
	CHECK(*request.DataReplay == data);
	EvaluatedValue unavailable;
	CHECK(
		host.ReadSourceCommonGetter(
			document, plan, "first", SourceCommonSelector::Update, request, unavailable, diagnostic, 2, 1
		) == Status::InvalidValue
	);
}

TEST_CASE(
	"Feedback host persists authored output binding and preserves generations on failed steps",
	"[imagegraph][feedback]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:out"}}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
	};
	document.Links = {{"prior", "image", "invert", "image"}};
	document.Outputs = {{"out", "invert", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	REQUIRE(host.Output("out") != nullptr);
	CHECK(host.Output("out")->Pixels[0] == 255);
	REQUIRE(request.ImageSources.size() == 1);
	CHECK(request.ImageSources[0].Data.Pixels[0] == 0);
	request = {};
	request.Tick = 1;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 0);
	REQUIRE(request.ImageSources.size() == 1);
	CHECK(request.ImageSources[0].Data.Pixels[0] == 255);
	request = {};
	request.Tick = 1;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 0);
	request.Subframe = .5;
	CHECK_FALSE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 0);
	request = {};
	request.Tick = 3;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 0);
	request = {};
	REQUIRE(host.Prepare(document, plan, 1, 2, request, diagnostic));
	CHECK(host.Output("out")->Pixels[0] == 255);
	request = {};
	request.Tick = 1;
	CHECK_FALSE(host.Prepare(document, plan, 1, 2, request, diagnostic, 1));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(host.Output("out")->Pixels[0] == 255);
	host.Clear();
	CHECK_FALSE(host.Active());
}

TEST_CASE("Persistent feedback streams beyond seek work bounds", "[imagegraph][feedback]") {
	Document document;
	document.FormatVersion = 9;
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = 1;
	document.Project->SurfaceHeight = 1;
	document.Nodes = {
		{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:out"}}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
	};
	document.Links = {{"prior", "image", "invert", "image"}};
	document.Outputs = {{"out", "invert", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	for (uint64_t tick = 0; tick <= 4096; tick++) {
		EvaluationRequest request;
		request.Tick = tick;
		REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic));
	}
	REQUIRE(host.Output("out") != nullptr);
	CHECK(host.Output("out")->Pixels[0] == 255);
}

TEST_CASE(
	"shared host carries original interlace inputs through playback edit and reset",
	"[imagegraph][feedback][stateful_replay]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{};
	document.Timeline->Frames = 4;
	document.Nodes = {
		{"input", "image.captured", "", {}, {{"source_id", std::string{"input"}}}},
		{"interlace", "pc.interlaced", "", {}, {{"loop", true}}}
	};
	document.Links = {{"input", "image", "interlace", "surface_in"}};
	document.Outputs = {{"out", "interlace", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	CapturedFeedbackHost host;
	Image image;
	image.Width = image.Height = 2;
	image.Pixels.resize(16);
	std::vector<RequestImageSource> sources{{"input", image}};
	for (uint64_t tick = 0; tick < 4; ++tick) {
		for (size_t pixel = 0; pixel < 4; ++pixel) {
			sources[0].Data.Pixels[pixel * 4] = static_cast<uint8_t>(40 + tick * 30);
			sources[0].Data.Pixels[pixel * 4 + 3] = 255;
		}
		sources[0].Data.Hash = SurfaceHash(sources[0].Data);
		EvaluationRequest request;
		request.Tick = tick;
		request.ImageSources = sources;
		const auto prepared =
			host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out");
		INFO(diagnostic.Message);
		REQUIRE(prepared);
		REQUIRE(host.Output("out"));
		CHECK(host.Output("out")->Pixels[0] == 40 + tick * 30);
		CHECK(host.Output("out")->Pixels[8] == (tick ? 40 + (tick - 1) * 30 : 0));
	}
	EvaluationRequest request;
	request.ImageSources = sources;
	REQUIRE(host.Prepare(document, plan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	CHECK(host.Output("out")->Pixels[8] == 130);
	const auto previous = *host.Output("out");
	request = {};
	request.Tick = 1;
	request.ImageSources = sources;
	CHECK_FALSE(host.Prepare(document, plan, 2, 1, request, diagnostic, 1, "out"));
	CHECK(*host.Output("out") == previous);
	host.Clear();
	request = {};
	request.ImageSources = sources;
	REQUIRE(host.Prepare(document, plan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	CHECK(host.Output("out")->Pixels[8] == 0);
}

TEST_CASE(
	"shared host captures renderer inputs and feedback outputs in one frame transaction",
	"[imagegraph][feedback]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = document.Project->SurfaceHeight = 1;
	document.Nodes = {
		{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:out"}}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}},
		{"renderer", "image.transform_3d", "", {}, {}},
		{"grid", "pc.verlet_sim_mesh_grid", "", {}, {{"subdivision", Vector2{1, 1}}}},
		{"cache", "pc.verlet_sim_mesh_cache", "", {}, {}}
	};
	document.Links = {{"prior", "image", "invert", "image"}, {"invert", "image", "renderer", "surface"}};
	document.Links.push_back({"grid", "mesh", "cache", "mesh"});
	document.Outputs = {{"out", "invert", "image"}, {"rendered", "renderer", "rendered"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic));
	REQUIRE(host.Snapshot().Images().size() == 1);
	REQUIRE(host.Output("out"));
	CHECK(host.Snapshot().Images()[0].Data == *host.Output("out"));
	CHECK(host.Snapshot().Images()[0].Data.Pixels[0] == 255);
	request = {};
	request.Tick = 1;
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic));
	CHECK(host.Snapshot().Images()[0].Data.Pixels[0] == 0);
	request = {};
	request.Tick = 1;
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic));
	CHECK(host.Snapshot().Images()[0].Data.Pixels[0] == 0);
	const std::array<std::string_view, 1> capture{"cache"};
	request.SimulationCacheCaptures = capture;
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic));
	REQUIRE(host.Output("out"));
	CHECK(host.Output("out")->Pixels[0] == 0);
	CHECK(host.Snapshot().Images()[0].Data.Pixels[0] == 0);
	request.SimulationCacheCaptures = {};
	const auto previous = host.Snapshot().Images()[0].Data;
	request.Tick = 2;
	CHECK_FALSE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic, 1));
	CHECK(host.Snapshot().Images()[0].Data == previous);
	CHECK(*host.Output("out") == previous);
}

TEST_CASE(
	"differential host preserves sampled history through edits signed rewinds and repeated frames",
	"[imagegraph][feedback][source_data]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"d", "pc.differential", "", {}, {{"value", 10.0}}}};
	document.Outputs = {{"out", "d", "result"}};
	Plan plan;
	Diagnostic diagnostic;
	CapturedFeedbackHost host;
	uint64_t revision = 0;
	const auto sample =
		[&](double value, FrameTime frame, uint64_t maximumBytes = Limits::MaximumEvaluationBytes) {
			document.Nodes[0].Values[0].Data = value;
			const auto compiled = Compile(document, plan, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(compiled == Status::Ok);
			EvaluationRequest request;
			REQUIRE(SetFrameTime(request, frame));
			return host.Prepare(document, plan, ++revision, 1, request, diagnostic, maximumBytes, "out");
		};
	const auto result = [&]() {
		const auto *output = host.Value("out");
		REQUIRE(output != nullptr);
		return std::get<double>(std::get<EvaluatedValue>(output->Output).Data);
	};
	REQUIRE(sample(10, {2}));
	CHECK(result() == 5);
	REQUIRE(sample(20, {4}));
	CHECK(result() == 5);
	REQUIRE(sample(14, {2, 0, true}));
	CHECK(result() == 1);
	REQUIRE(sample(22, {2}));
	CHECK(result() == 2);
	REQUIRE(sample(30, {2}));
	CHECK(result() == 0);
	REQUIRE(sample(40, {3}));
	CHECK(result() == 10);
	CHECK_FALSE(sample(50, {4}, 1));
	CHECK(result() == 10);
	REQUIRE(sample(50, {4}));
	CHECK(result() == 10);
	REQUIRE(sample(60, {5000}));
	CHECK(result() == Catch::Approx(10.0 / 4996));
}

TEST_CASE(
	"constructor-only Verlet host retains geometry and ledger clocks without fixed solver steps",
	"[imagegraph][feedback]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"grid", "pc.verlet_sim_mesh_grid", "", {}, {{"subdivision", Vector2{1, 1}}}},
		{"static", "value.number", "", {}, {{"value", 2.}}}
	};
	document.Outputs = {{"mesh", "grid", "mesh"}, {"number", "static", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::array<std::string, 1> meshOutput{"mesh"}, staticOutput{"number"};
	const auto cone = AnalyzeStatefulTemporalCone(document, plan, meshOutput);
	REQUIRE(cone.Valid);
	CHECK(cone.Simulation);
	CHECK_FALSE(cone.FixedSimulationSteps);
	CHECK_FALSE(AnalyzeStatefulTemporalCone(document, plan, staticOutput).Simulation);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(document, plan, 7, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "mesh"));
	REQUIRE(request.SimulationReplay);
	REQUIRE(request.SimulationReplay->Entries.size() == 1);
	const auto original = request.SimulationReplay->Entries.front();
	REQUIRE(original.State.Initialized);
	CHECK(original.State.Tick == 0);
	CHECK(original.State.AuthoringRevision == 7);
	request.Subframe = .5;
	REQUIRE(host.Prepare(document, plan, 7, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "mesh"));
	CHECK(request.SimulationReplay->Entries.front() == original);
	request.Tick = 1;
	request.Subframe = 0;
	REQUIRE(host.Prepare(document, plan, 7, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "mesh"));
	const auto next = request.SimulationReplay->Entries.front();
	CHECK(next.State.Tick == 1);
	CHECK(next.State.Mesh == original.State.Mesh);
	CHECK(next.Topology == original.Topology);
	REQUIRE(host.Value("mesh"));
	const auto retained = *host.Value("mesh");
	const auto replay = *request.SimulationReplay;
	request.Tick = 4097;
	CHECK_FALSE(
		host.Prepare(document, plan, 7, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "mesh")
	);
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(host.Value("mesh")->Id == retained.Id);
	CHECK(std::get<EvaluatedValue>(host.Value("mesh")->Output) == std::get<EvaluatedValue>(retained.Output));
	CHECK(*request.SimulationReplay == replay);
}

TEST_CASE(
	"Published feedback clock is generation scoped and survives failed preparation",
	"[imagegraph][feedback][prepared_frame]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = document.Project->SurfaceHeight = 1;
	document.Nodes = {
		{"prior", "image.captured", "", {}, {{"source_id", std::string("feedback:out")}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
	};
	document.Links = {{"prior", "image", "invert", "image"}};
	document.Outputs = {{"out", "invert", "image"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	CapturedFeedbackHost host;
	CHECK_FALSE(host.PreparedFrame(3, 4));
	EvaluationRequest request;
	REQUIRE(host.Prepare(document, plan, 3, 4, request, error));
	REQUIRE(host.PreparedFrame(3, 4));
	CHECK(*host.PreparedFrame(3, 4) == FrameTime{});
	CHECK_FALSE(host.PreparedFrame(2, 4));
	CHECK_FALSE(host.PreparedFrame(3, 5));
	const auto original = *host.Output("out");
	request = {};
	request.Tick = 1;
	CHECK_FALSE(host.Prepare(document, plan, 3, 4, request, error, 1));
	CHECK(host.PreparedFrame(3, 4) == std::optional(FrameTime{}));
	CHECK(*host.Output("out") == original);
	for (const auto invalid : {FrameTime{1, .5}, FrameTime{1, 0, true}}) {
		request = {};
		REQUIRE(SetFrameTime(request, invalid));
		CHECK_FALSE(host.Prepare(document, plan, 3, 4, request, error));
		CHECK(host.PreparedFrame(3, 4) == std::optional(FrameTime{}));
		CHECK(*host.Output("out") == original);
	}
	request = {};
	request.Tick = 1;
	REQUIRE(host.Prepare(document, plan, 3, 4, request, error));
	CHECK(host.PreparedFrame(3, 4) == std::optional(FrameTime{1}));
	host.RestartCycle();
	CHECK_FALSE(host.PreparedFrame(3, 4));
	request = {};
	REQUIRE(host.Prepare(document, plan, 3, 4, request, error));
	CHECK(host.PreparedFrame(3, 4) == std::optional(FrameTime{}));
	CHECK(*host.Output("out") == original);
	host.Clear();
	CHECK_FALSE(host.PreparedFrame(3, 4));
}
TEST_CASE(
	"Published direct-data clock keeps exact signed fractional observations",
	"[imagegraph][feedback][prepared_frame]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"d", "pc.differential", "", {}, {{"value", 10.0}}}};
	document.Outputs = {{"out", "d", "result"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	const FrameTime frame{2, .25, true};
	REQUIRE(SetFrameTime(request, frame));
	REQUIRE(host.Prepare(document, plan, 1, 2, request, error, Limits::MaximumEvaluationBytes, "out"));
	CHECK(host.PreparedFrame(1, 2) == std::optional(frame));
	CHECK_FALSE(host.PreparedFrame(2, 2));
	CHECK_FALSE(host.PreparedFrame(1, 3));
	request = {};
	REQUIRE(SetFrameTime(request, {3, .5}));
	CHECK_FALSE(host.Prepare(document, plan, 1, 2, request, error, 1, "out"));
	CHECK(host.PreparedFrame(1, 2) == std::optional(frame));
}

TEST_CASE(
	"Host loaded cache refresh preserves frame rows and owner state across append",
	"[imagegraph][feedback][cache_group][load]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = 2;
	document.Project->SurfaceHeight = 1;
	document.Timeline = TimelineSettings{6, 0, 5, "loop", 24};
	document.Nodes = {
		{"input",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{10, 20, 30, 255}}}},
		{"cache", "pc.cache", "", {}, {{"animated", false}}}
	};
	document.Nodes[1].SourceProperties = {
		{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}
	};
	document.Links = {{"input", "image", "cache", "surface_in"}};
	document.Outputs = {{"out", "cache", "cache_surface"}};
	CapturedFeedbackHost host;
	Diagnostic error;
	const std::array<std::string_view, 1> initial{"cache"};
	REQUIRE(host.RefreshLoadedSourceCacheGroups(document, initial, error));
	Plan plan;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	request.SourceCachePlayback =
		SourceCachePlaybackObservation{true, SourceCacheSampling::ObservedFrame, true};
	request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 0.0, false, false};
	REQUIRE(host.Prepare(document, plan, 1, 1, request, error, Limits::MaximumEvaluationBytes, "out"));
	const auto prior = *host.PreparedData(1, 1);
	const auto pixels = *host.Output("out");
	document.Nodes.push_back(
		{"loaded",
		 "pc.cache_array",
		 "",
		 {},
		 {{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}}
	);
	document.Nodes.back().SourceProperties = {
		{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}, std::string{"new-number"}}}},
		{"serialize", false}
	};
	document.Nodes.push_back({"new-number", "pc.number_simple", "", {}, {{"value", 99.0}}});
	const std::array<std::string_view, 1> loaded{"loaded"};
	CHECK_FALSE(host.RefreshLoadedSourceCacheGroups(document, loaded, error, 1));
	CHECK(*host.PreparedData(1, 1) == prior);
	CHECK(*host.Output("out") == pixels);
	int admissions = 0;
	for (const bool throws : {false, true}) {
		CHECK_FALSE(host.RefreshLoadedSourceCacheGroups(
			document, loaded, error, [&](uint64_t remainingBytes) -> bool {
				++admissions;
				CHECK(remainingBytes > 0);
				CHECK(
					remainingBytes < Limits::MaximumEvaluationBytes - host.RetainedBytes() -
										 *DocumentRetainedPayloadBytes(document)
				);
				CHECK(*host.PreparedData(1, 1) == prior);
				if (throws) throw std::bad_alloc{};
				return false;
			}
		));
		CHECK(error.Code == Status::LimitExceeded);
		CHECK(*host.PreparedData(1, 1) == prior);
		CHECK(*host.Output("out") == pixels);
	}
	CHECK(admissions == 2);
	REQUIRE(host.RefreshLoadedSourceCacheGroups(document, {}, error, [&](uint64_t remainingBytes) {
		++admissions;
		CHECK(
			remainingBytes ==
			Limits::MaximumEvaluationBytes - host.RetainedBytes() - *DocumentRetainedPayloadBytes(document)
		);
		return true;
	}));
	CHECK(*host.PreparedData(1, 1) == prior);
	CHECK(*host.Output("out") == pixels);
	REQUIRE(host.RefreshLoadedSourceCacheGroups(document, loaded, error, [&](uint64_t) {
		++admissions;
		CHECK(*host.PreparedData(1, 1) == prior);
		return true;
	}));
	CHECK(admissions == 4);
	REQUIRE(host.PreparedData(1, 1));
	const auto refreshed = *host.PreparedData(1, 1);
	CHECK(refreshed.Entries == prior.Entries);
	CHECK(refreshed.CacheGroups.Owners[0] == prior.CacheGroups.Owners[0]);
	const auto member = std::find_if(
		refreshed.CacheGroups.Nodes.begin(), refreshed.CacheGroups.Nodes.end(), [](const auto &node) {
			return node.NodeId == "input";
		}
	);
	REQUIRE(member != refreshed.CacheGroups.Nodes.end());
	CHECK_FALSE(member->RenderActive);
	CHECK(member->OwnerId == "loaded");
	CHECK(*host.Output("out") == pixels);
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	request.SourceCacheProject->ProjectLastFrame = 5.0;
	REQUIRE(host.Prepare(document, plan, 2, 1, request, error, Limits::MaximumEvaluationBytes, "out"));
	CHECK(*host.Output("out") == pixels);
	CHECK(host.PreparedData(2, 1)->CacheGroups.Owners.size() == 2);
	request.SourceCachePlayback->Playing = false;
	REQUIRE(host.Prepare(document, plan, 2, 1, request, error, Limits::MaximumEvaluationBytes, "out"));
	const auto after = std::find_if(
		host.PreparedData(2, 1)->CacheGroups.Nodes.begin(),
		host.PreparedData(2, 1)->CacheGroups.Nodes.end(),
		[](const auto &node) { return node.NodeId == "input"; }
	);
	REQUIRE(after != host.PreparedData(2, 1)->CacheGroups.Nodes.end());
	CHECK_FALSE(after->RenderActive);
	CHECK(after->OwnerId == "loaded");
}

TEST_CASE(
	"Host replay start keeps loaded callback ownership instead of document order",
	"[imagegraph][feedback][cache_group][load]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = 2;
	document.Project->SurfaceHeight = 1;
	document.Timeline = TimelineSettings{6, 0, 5, "loop", 24};
	document.Nodes = {
		{"input",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{10, 20, 30, 255}}}},
		{"cache-a", "pc.cache", "", {}, {{"animated", false}}},
		{"cache-b", "pc.cache", "", {}, {{"animated", false}}}
	};
	for (size_t i = 1; i < document.Nodes.size(); ++i)
		document.Nodes[i].SourceProperties = {
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}, {"serialize", false}
		};
	document.Links = {{"input", "image", "cache-a", "surface_in"}};
	document.Outputs = {{"out", "cache-a", "cache_surface"}};
	CapturedFeedbackHost host;
	Diagnostic error;
	const std::array<std::string_view, 2> owners{"cache-b", "cache-a"};
	REQUIRE(host.RefreshLoadedSourceCacheGroups(document, owners, error));
	Plan plan;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	request.SourceCachePlayback =
		SourceCachePlaybackObservation{true, SourceCacheSampling::NativePlayedPrefix, true};
	request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5.0, false, false};
	REQUIRE(host.Prepare(document, plan, 1, 1, request, error, Limits::MaximumEvaluationBytes, "out"));
	const auto checkOwnership = [&](uint64_t revision) {
		const auto *data = host.PreparedData(revision, 1);
		REQUIRE(data);
		REQUIRE(data->CacheGroups.Owners.size() >= 2);
		CHECK(data->CacheGroups.Owners[0].NodeId == "cache-b");
		CHECK(data->CacheGroups.Owners[1].NodeId == "cache-a");
		const auto input = std::find_if(
			data->CacheGroups.Nodes.begin(), data->CacheGroups.Nodes.end(), [](const auto &node) {
				return node.NodeId == "input";
			}
		);
		REQUIRE(input != data->CacheGroups.Nodes.end());
		CHECK(input->OwnerId == "cache-a");
	};
	checkOwnership(1);
	const auto pixels = *host.Output("out");
	document.Nodes.push_back({"cache-c", "pc.cache", "", {}, {{"animated", false}}});
	document.Nodes.back().SourceProperties = document.Nodes[1].SourceProperties;
	const std::array<std::string_view, 2> appended{"cache-c", "cache-a"};
	REQUIRE(host.RefreshLoadedSourceCacheGroups(document, appended, error));
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 2, 1, request, error, Limits::MaximumEvaluationBytes, "out"));
	checkOwnership(2);
	CHECK(*host.Output("out") == pixels);
	request.SourceCachePlayback->Playing = false;
	request.SourceCachePlayback->Sampling = SourceCacheSampling::ObservedFrame;
	REQUIRE(host.Prepare(document, plan, 2, 1, request, error, Limits::MaximumEvaluationBytes, "out"));
	checkOwnership(2);
}

TEST_CASE(
	"Grouped host journals remain admitted while publishing a nongrouped feedback revision",
	"[imagegraph][feedback][group_render][budget]"
) {
	Document grouped;
	grouped.FormatVersion = 10;
	grouped.Groups = {{"group", "Group"}};
	grouped.Nodes = {
		{"number", "pc.number_simple", "group", {}, {{"value", 7.0}}},
		{"surface",
		 "image.solid",
		 "group",
		 {},
		 {{"width", int64_t{256}}, {"height", int64_t{256}}, {"colour", Colour{12, 34, 56, 255}}}}
	};
	grouped.Outputs = {{"result", "number", "number"}};
	Plan groupedPlan;
	Diagnostic diagnostic;
	REQUIRE(Compile(grouped, groupedPlan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(
		grouped, groupedPlan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "result"
	));
	const auto *heldGroups = host.PreparedGroups(1, 1, {});
	REQUIRE(heldGroups);
	const auto groupBytes = RetainedGroupRenderSessionBytes(*heldGroups);
	REQUIRE(groupBytes > 256 * 256 * 4);
	const auto retained = host.RetainedBytes();
	const auto heldOutput = *host.Value("result");
	const auto nodes = heldGroups->Outputs.Nodes.size();
	Document feedback;
	feedback.FormatVersion = 9;
	feedback.Project = ProjectSettings{};
	feedback.Project->SurfaceWidth = feedback.Project->SurfaceHeight = 1;
	feedback.Nodes = {
		{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:out"}}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
	};
	feedback.Links = {{"prior", "image", "invert", "image"}};
	feedback.Outputs = {{"out", "invert", "image"}};
	Plan feedbackPlan;
	REQUIRE(Compile(feedback, feedbackPlan, diagnostic) == Status::Ok);
	request = {};
	CHECK_FALSE(host.Prepare(feedback, feedbackPlan, 2, 1, request, diagnostic, groupBytes / 2, "out"));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(host.RetainedBytes() == retained);
	REQUIRE(host.PreparedGroups(1, 1, {}));
	CHECK(host.PreparedGroups(1, 1, {})->Outputs.Nodes.size() == nodes);
	REQUIRE(host.Value("result"));
	CHECK(
		std::get<EvaluatedValue>(host.Value("result")->Output) == std::get<EvaluatedValue>(heldOutput.Output)
	);
	CHECK_FALSE(host.PreparedFrame(2, 1));
	request = {};
	REQUIRE(
		host.Prepare(feedback, feedbackPlan, 2, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out")
	);
	REQUIRE(host.Output("out"));
	CHECK(host.Output("out")->Pixels[0] == 255);
	CHECK_FALSE(host.PreparedGroups(1, 1, {}));
	CHECK(host.PreparedGroups(2, 1, {}) == nullptr);
	CHECK(host.RetainedBytes() < retained);
}
