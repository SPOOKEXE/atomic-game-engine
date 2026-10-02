#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>

TEST_SUITE_ID("engine.imagegraph.stateful_outputs")
using namespace engine::imagegraph;
namespace {
	class CountHost final : public HostNodeProvider {
	  public:
		size_t Calls = 0;
		bool Capture(const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &) override {
			++Calls;
			output.Authored = invocation.Authored;
			output.Tick = invocation.Request.Tick;
			output.Subframe = invocation.Request.Subframe;
			output.NegativeFrame = invocation.Request.NegativeFrame;
			output.Inputs.assign(invocation.Inputs.begin(), invocation.Inputs.end());
			output.Outputs = {{"content", std::string{"17"}}, {"path", std::string{"27"}}};
			return true;
		}
	};
	const StatefulNamedOutput &Find(const StatefulOutputEvaluationResult &result, std::string_view id) {
		for (const auto &output : result.Outputs)
			if (output.Id == id) return output;
		throw std::runtime_error("missing output");
	}
	Document StatefulGraph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"input", "image.captured", "", {}, {{"source_id", std::string{"input"}}}},
			{"interlace", "pc.interlaced", "", {}, {}},
			{"grid", "pc.verlet_sim_mesh_grid", "", {}, {{"subdivision", Vector2{1, 1}}}},
			{"step", "image.verlet_simple", "", {}, {{"substep", int64_t{1}}, {"gravity", Vector2{0, 1}}}}
		};
		document.Links = {{"input", "image", "interlace", "surface_in"}, {"grid", "mesh", "step", "mesh"}};
		document.Outputs = {{"surface", "interlace", "surface_out"}, {"mesh", "step", "mesh"}};
		return document;
	}
}
TEST_CASE(
	"Output union executes a shared host producer once and captures distinct ports",
	"[imagegraph][stateful_outputs]"
) {
	Document document;
	document.FormatVersion = 9;
	Node host{"host", "pc.text_file_read", "", {}, {}};
	document.Nodes = {host, {"derived", "pc.string", "", {}, {}}};
	document.Links = {{"host", "content", "derived", "text"}};
	document.Outputs = {
		{"raw", "host", "content"}, {"other", "host", "path"}, {"downstream", "derived", "text"}
	};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	CountHost provider;
	EvaluationRequest request;
	request.HostProvider = &provider;
	StatefulOutputEvaluationResult result;
	const std::array<std::string, 3> outputs{"raw", "other", "downstream"};
	const auto status = EvaluateStatefulOutputs(document, plan, outputs, request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(provider.Calls == 1);
	CHECK(std::get<std::string>(std::get<EvaluatedValue>(Find(result, "raw").Output).Data) == "17");
	CHECK(std::get<std::string>(std::get<EvaluatedValue>(Find(result, "other").Output).Data) == "27");
	CHECK(std::get<std::string>(std::get<EvaluatedValue>(Find(result, "downstream").Output).Data) == "17");
}
TEST_CASE("Output union publishes both replay owners atomically", "[imagegraph][stateful_outputs]") {
	auto document = StatefulGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	image.Width = image.Height = 2;
	image.Pixels.assign(16, 64);
	image.Hash = SurfaceHash(image);
	const std::array<RequestImageSource, 1> sources{{{"input", image}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	request.SimulationAuthoringRevision = 1;
	StatefulOutputEvaluationResult result;
	const std::array<std::string, 2> outputs{"surface", "mesh"};
	const auto status = EvaluateStatefulOutputs(document, plan, outputs, request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Simulation.Entries.size() == 1);
	REQUIRE(result.Surfaces.Entries.size() == 1);
	const auto simulation = result.Simulation;
	const auto surfaces = result.Surfaces;
	const auto hash = std::get<Image>(Find(result, "surface").Output).Hash;
	request.Tick = 1;
	request.SimulationReplay = &result.Simulation;
	request.SurfaceReplay = &result.Surfaces;
	const std::array<std::string, 2> missing{"surface", "missing"}, duplicate{"surface", "surface"};
	CHECK(EvaluateStatefulOutputs(document, plan, missing, request, result, diagnostic) != Status::Ok);
	CHECK(
		EvaluateStatefulOutputs(document, plan, duplicate, request, result, diagnostic) == Status::DuplicateId
	);
	CHECK(
		EvaluateStatefulOutputs(document, plan, outputs, request, result, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(result.Simulation == simulation);
	CHECK(result.Surfaces == surfaces);
	CHECK(std::get<Image>(Find(result, "surface").Output).Hash == hash);
}

TEST_CASE("input snapshot shares a host producer with selected outputs", "[imagegraph][stateful_outputs]") {
	Document document;
	document.FormatVersion = 9;
	Node host{"host", "pc.text_file_read", "", {}, {}};
	document.Nodes = {host, {"renderer", "pc.string", "", {}, {}}};
	document.Links = {{"host", "path", "renderer", "text"}};
	document.Outputs = {{"raw", "host", "content"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	CountHost provider;
	EvaluationRequest request;
	request.HostProvider = &provider;
	StatefulInputEvaluationResult result;
	const std::array<std::string, 1> outputs{"raw"};
	const auto captured = EvaluateStatefulNodeInputs(
		document, plan, "renderer", request, result, diagnostic, Limits::MaximumEvaluationBytes, outputs
	);
	INFO(diagnostic.Message);
	REQUIRE(captured == Status::Ok);
	CHECK(provider.Calls == 1);
	REQUIRE(result.Outputs.size() == 1);
	CHECK(std::get<std::string>(std::get<EvaluatedValue>(result.Outputs[0].Output).Data) == "17");
	const auto inputs = result.Inputs.Values();
	const auto fov =
		std::find_if(inputs.begin(), inputs.end(), [](const auto &input) { return input.Port == "text"; });
	REQUIRE(fov != inputs.end());
	CHECK(std::get<std::string>(fov->Data) == "27");
	CHECK(result.Inputs.RetainedBytes() > 0);
	const std::array<std::string, 1> absent{"absent"};
	CHECK(
		EvaluateStatefulNodeInputs(
			document, plan, "renderer", request, result, diagnostic, Limits::MaximumEvaluationBytes, absent
		) == Status::InvalidOutput
	);
	CHECK(provider.Calls == 1);
	CHECK(std::get<std::string>(result.Inputs.Values()[std::distance(inputs.begin(), fov)].Data) == "27");
}

TEST_CASE(
	"renderer inputs named output and disconnected cache action share one atomic closure",
	"[imagegraph][stateful_outputs][feedback]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
		{"renderer", "image.transform_3d", "", {}, {}},
		{"grid", "pc.verlet_sim_mesh_grid", "", {}, {{"subdivision", Vector2{1, 1}}}},
		{"cache", "pc.verlet_sim_mesh_cache", "", {}, {}}
	};
	document.Links = {{"solid", "image", "renderer", "surface"}, {"grid", "mesh", "cache", "mesh"}};
	document.Outputs = {{"still", "solid", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	const std::array<std::string, 1> outputs{"still"};
	const std::array<std::string_view, 1> action{"cache"};
	EvaluationRequest request;
	request.SimulationCacheCaptures = action;
	request.Subframe = .5;
	StatefulInputEvaluationResult result;
	const auto captured = EvaluateStatefulNodeInputs(
		document, plan, "renderer", request, result, diagnostic, Limits::MaximumEvaluationBytes, outputs
	);
	INFO(diagnostic.Message);
	REQUIRE(captured == Status::Ok);
	REQUIRE(result.Inputs.Images().size() == 1);
	REQUIRE(result.Outputs.size() == 1);
	CHECK(result.Inputs.Images()[0].Data == std::get<Image>(result.Outputs[0].Output));
	const auto cache = std::find_if(
		result.Simulation.Entries.begin(), result.Simulation.Entries.end(), [](const auto &entry) {
			return entry.NodeId == "cache";
		}
	);
	REQUIRE(cache != result.Simulation.Entries.end());
	REQUIRE(cache->Cache);
	CHECK(cache->Cache->size() == 4);
	const auto previous = result.Simulation;
	const std::array<std::string_view, 2> duplicate{"cache", "cache"};
	request.SimulationCacheCaptures = duplicate;
	CHECK(
		EvaluateStatefulNodeInputs(
			document, plan, "renderer", request, result, diagnostic, Limits::MaximumEvaluationBytes, outputs
		) == Status::DuplicateId
	);
	CHECK(result.Simulation == previous);
	CHECK(result.Inputs.Images()[0].Data == std::get<Image>(result.Outputs[0].Output));

	// The host uses the same input-only route without requiring a renderer output or feedback binding.
	CapturedFeedbackHost host;
	request.SimulationCacheCaptures = action;
	const auto prepared = host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(prepared);
	REQUIRE(host.Active());
	REQUIRE(host.Snapshot().Images().size() == 1);
	REQUIRE(request.SimulationReplay);
	const auto hostCache = std::find_if(
		request.SimulationReplay->Entries.begin(),
		request.SimulationReplay->Entries.end(),
		[](const auto &entry) { return entry.NodeId == "cache"; }
	);
	REQUIRE(hostCache != request.SimulationReplay->Entries.end());
	REQUIRE(hostCache->Cache);
	CHECK(hostCache->Cache->size() == 4);
	const auto retainedPositions = *hostCache->Cache;
	request = {};
	request.Tick = 2;
	REQUIRE(host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "still"));
	REQUIRE(request.SimulationReplay);
	const auto retainedCache = [&](const EvaluationRequest &sample) {
		return std::find_if(
			sample.SimulationReplay->Entries.begin(),
			sample.SimulationReplay->Entries.end(),
			[](const auto &entry) { return entry.NodeId == "cache"; }
		);
	};
	REQUIRE(retainedCache(request) != request.SimulationReplay->Entries.end());
	CHECK(*retainedCache(request)->Cache == retainedPositions);
	request = {};
	REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "renderer", request, diagnostic));
	REQUIRE(request.SimulationReplay);
	REQUIRE(retainedCache(request) != request.SimulationReplay->Entries.end());
	CHECK(*retainedCache(request)->Cache == retainedPositions);
}
