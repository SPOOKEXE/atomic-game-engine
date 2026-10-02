#include "../../mono.engine/imagegraphphysics/tests/MixedRigidSimulationFixture.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/scripthost/ComposerLua.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("studio.imagegraph_checkpoint_mixed52")
TEST_DEPENDS("engine.imagegraphphysics.rigid_graph")
using namespace engine::imagegraph;
namespace {
	Document CheckpointFixture() {
		auto document = engine::imagegraphphysics::testing::MixedRigidSimulationFixture();
		document.Nodes.push_back({"random", "pc.random", "", {}, {{"seed", int64_t{12345}}}});
		document.Nodes.push_back(
			{"counter", "pc.counter", "", {}, {{"async", true}, {"start", 1.}, {"speed", 2.}}}
		);
		document.Nodes.push_back({"cached", "pc.interlaced", "", {}, {{"size", 1.}, {"delay", int64_t{1}}}});
		Node controls{"controls", "pc.struct", "", {}, {}};
		for (size_t i = 0; i < 3; ++i) {
			controls.DynamicInputs.push_back(
				{"key_" + std::to_string(i), ValueType::Text, Value{std::to_string(i)}}
			);
			controls.DynamicInputs.push_back({"value_" + std::to_string(i), ValueType::Any, {}});
		}
		document.Nodes.push_back(controls);
		document.Links.insert(
			document.Links.end(),
			{{"render", "surface_out", "cached", "surface_in"},
			 {"random", "result", "controls", "value_0"},
			 {"counter", "value", "controls", "value_1"},
			 {"cached", "surface_out", "controls", "value_2"}}
		);
		return document;
	}
} // namespace
TEST_CASE(
	"current-frame checkpoint rebuilds genuine rigid-driven FLIP from "
	"all five recorded journals",
	"[studio][checkpoint52]"
) {
	auto document = CheckpointFixture();
	Plan plan;
	Diagnostic diagnostic;
	auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CapturedFeedbackHost host;
	engine::imagegraphphysics::RigidProvider provider;
	EvaluationRequest request;
	request.RigidProvider = &provider;
	request.RigidFrameProgress = true;
	auto run = [&](uint64_t budget = Limits::MaximumEvaluationBytes) {
		auto ok = host.Prepare(document, plan, 19, 1, request, diagnostic, budget, "fluid_state", "controls");
		INFO(diagnostic.Message);
		return ok;
	};
	StatefulOutputEvaluationResult prefix;
	for (uint64_t tick = 0; tick < 12; ++tick) {
		request.Tick = tick;
		REQUIRE(run());
	}
	prefix.Simulation = *request.SimulationReplay;
	prefix.Surfaces = *request.SurfaceReplay;
	prefix.Random = *request.RandomReplay;
	prefix.Data = *request.DataReplay;
	prefix.Rigid = *request.RigidReplay;
	REQUIRE(!prefix.Simulation.Entries.empty());
	REQUIRE(!prefix.Surfaces.Entries.empty());
	REQUIRE(!prefix.Random.Entries.empty());
	REQUIRE(!prefix.Data.Entries.empty());
	REQUIRE(!prefix.Rigid.Owners.empty());
	auto expected = [&](bool playing, bool progress = true) {
		auto clock = request;
		clock.Tick = 12;
		clock.RigidPlaying = playing;
		clock.RigidFrameProgress = progress;
		clock.SimulationAuthoringRevision = clock.RigidAuthoringRevision = 19;
		clock.SimulationReplay = &prefix.Simulation;
		clock.SurfaceReplay = &prefix.Surfaces;
		clock.RandomReplay = &prefix.Random;
		clock.DataReplay = &prefix.Data;
		clock.RigidReplay = &prefix.Rigid;
		StatefulInputEvaluationResult result;
		const std::array<std::string, 1> outputs{"fluid_state"};
		auto code = EvaluateStatefulNodeInputs(
			document, plan, "controls", clock, result, diagnostic, Limits::MaximumEvaluationBytes, outputs
		);
		INFO(diagnostic.Message);
		REQUIRE(code == Status::Ok);
		return result;
	};
	const auto paused = expected(false), played = expected(true), stopped = expected(true, false);
	REQUIRE(paused.Simulation != played.Simulation);
	request.Tick = 12;
	for (const auto &flags :
		 {std::pair{false, true},
		  std::pair{true, true},
		  std::pair{true, false},
		  std::pair{true, false},
		  std::pair{false, true},
		  std::pair{true, true}}) {
		request.RigidPlaying = flags.first;
		request.RigidFrameProgress = flags.second;
		REQUIRE(run());
		const auto &result = !flags.second ? stopped : flags.first ? played : paused;
		REQUIRE(*request.SimulationReplay == result.Simulation);
		REQUIRE(*request.SurfaceReplay == result.Surfaces);
		REQUIRE(*request.RandomReplay == result.Random);
		REQUIRE(*request.DataReplay == result.Data);
		REQUIRE(*request.RigidReplay == result.Rigid);
		REQUIRE(
			std::get<EvaluatedValue>(host.Value("fluid_state")->Output) ==
			std::get<EvaluatedValue>(result.Outputs[0].Output)
		);
	}
	const auto held = *request.SimulationReplay;
	const auto heldSurfaces = *request.SurfaceReplay;
	const auto heldRandom = *request.RandomReplay;
	const auto heldData = *request.DataReplay;
	const auto heldRigid = *request.RigidReplay;
	const auto heldOutput = std::get<EvaluatedValue>(host.Value("fluid_state")->Output);
	request.RigidPlaying = false;
	REQUIRE(!run(1));
	REQUIRE(*request.SimulationReplay == held);
	REQUIRE(*request.SurfaceReplay == heldSurfaces);
	REQUIRE(*request.RandomReplay == heldRandom);
	REQUIRE(*request.DataReplay == heldData);
	REQUIRE(*request.RigidReplay == heldRigid);
	REQUIRE(std::get<EvaluatedValue>(host.Value("fluid_state")->Output) == heldOutput);
	REQUIRE(run());
	REQUIRE(*request.SimulationReplay == paused.Simulation);
	request.RigidPlaying = true;
	request.MaximumImageDimension = 1;
	const auto oldSimulation = *request.SimulationReplay;
	const auto oldRigid = *request.RigidReplay;
	const auto oldData = *request.DataReplay;
	const auto oldRandom = *request.RandomReplay;
	const auto oldSurfaces = *request.SurfaceReplay;
	const auto oldOutput = std::get<EvaluatedValue>(host.Value("fluid_state")->Output);
	REQUIRE(!run());
	REQUIRE(diagnostic.Code == Status::LimitExceeded);
	REQUIRE(*request.SimulationReplay == oldSimulation);
	REQUIRE(*request.RigidReplay == oldRigid);
	REQUIRE(*request.DataReplay == oldData);
	REQUIRE(*request.RandomReplay == oldRandom);
	REQUIRE(*request.SurfaceReplay == oldSurfaces);
	REQUIRE(std::get<EvaluatedValue>(host.Value("fluid_state")->Output) == oldOutput);
	request.MaximumImageDimension = Limits::MaximumDimension;
	REQUIRE(run());
	REQUIRE(*request.SimulationReplay == played.Simulation);
	REQUIRE(*request.RigidReplay == played.Rigid);
}
TEST_CASE(
	"current-frame checkpoint refuses Lua session side effects before "
	"provider execution",
	"[studio][checkpoint52]"
) {
	Diagnostic diagnostic;
	engine::imagegraphphysics::RigidProvider provider;
	// Actual Lua VM session remains owned by the provider. A refresh must refuse
	// before reexecuting its side effects; a normal next frame still runs once.
	auto luaDocument = CheckpointFixture();
	luaDocument.Nodes.push_back(
		{"lua",
		 "pc.lua_compute",
		 "",
		 {},
		 {{"lua_code", std::string{"counter=(counter or 0)+1 print(counter) return counter"}},
		  {"function_name", std::string{"checkpointFixture"}},
		  {"execute_on_frame", true}}}
	);
	std::erase_if(luaDocument.Links, [](const auto &link) {
		return link.ToNode == "controls" && link.ToPort == "value_0";
	});
	luaDocument.Links.push_back({"lua", "return_value", "controls", "value_0"});
	Plan luaPlan;
	REQUIRE(Compile(luaDocument, luaPlan, diagnostic) == Status::Ok);
	auto lua = engine::script::MakeComposerLuaHost();
	CapturedFeedbackHost luaHost;
	auto luaRequest = EvaluationRequest{};
	luaRequest.RigidProvider = &provider;
	luaRequest.RigidFrameProgress = true;
	luaRequest.HostProvider = lua.get();
	REQUIRE(luaHost.Prepare(
		luaDocument,
		luaPlan,
		19,
		1,
		luaRequest,
		diagnostic,
		Limits::MaximumEvaluationBytes,
		"fluid_state",
		"controls"
	));
	REQUIRE(lua->TakeMessages().size() == 1);
	const auto luaRigid = *luaRequest.RigidReplay;
	const auto luaSimulation = *luaRequest.SimulationReplay;
	luaRequest.RigidPlaying = true;
	REQUIRE(!luaHost.Prepare(
		luaDocument,
		luaPlan,
		19,
		1,
		luaRequest,
		diagnostic,
		Limits::MaximumEvaluationBytes,
		"fluid_state",
		"controls"
	));
	REQUIRE(diagnostic.Code == Status::UnsupportedExecution);
	REQUIRE(lua->TakeMessages().empty());
	REQUIRE(*luaRequest.RigidReplay == luaRigid);
	REQUIRE(*luaRequest.SimulationReplay == luaSimulation);
	luaRequest.Tick = 1;
	REQUIRE(luaHost.Prepare(
		luaDocument,
		luaPlan,
		19,
		1,
		luaRequest,
		diagnostic,
		Limits::MaximumEvaluationBytes,
		"fluid_state",
		"controls"
	));
	auto messages = lua->TakeMessages();
	REQUIRE(messages.size() == 1);
	REQUIRE(messages[0].Text == "2");
}
