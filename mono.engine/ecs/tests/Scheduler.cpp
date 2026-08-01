#include <engine/core/FrameGraph.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.scheduler")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.core.framegraph")

using Catch::Approx;
using engine::core::FrameGraph;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;

namespace {
	struct Health {
		int Value = 0;
	};
}

TEST_CASE("phases run in order regardless of registration order", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	std::vector<std::string> order;

	// Registered back to front on purpose.
	scheduler.Add("d", Phase::PreRender, [&](Store &) { order.emplace_back("d"); });
	scheduler.Add("c", Phase::PostSimulation, [&](Store &) { order.emplace_back("c"); });
	scheduler.Add("b", Phase::Simulation, [&](Store &) { order.emplace_back("b"); });
	scheduler.Add("a", Phase::PreSimulation, [&](Store &) { order.emplace_back("a"); });

	scheduler.Tick(store, 0.016f);

	REQUIRE(order == std::vector<std::string> { "a", "b", "c", "d" });
}

TEST_CASE("the delta reaches every system through the world's clock",
	"[scheduler]") {
	Store store("test");
	Scheduler scheduler;

	float seen = 0.0f;
	scheduler.Add("read-delta", Phase::Simulation,
		[&](Store &tickStore) { seen = tickStore.Time().Delta; });

	scheduler.Tick(store, 0.25f);
	REQUIRE(seen == Approx(0.25f));
}

TEST_CASE("RunPhases runs a range and leaves the clock alone", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;

	std::vector<std::string> order;
	scheduler.Add("sim", Phase::Simulation, [&](Store &) { order.emplace_back("sim"); });
	scheduler.Add("draw", Phase::PreRender, [&](Store &) { order.emplace_back("draw"); });

	// What a client does: the simulation phases without the render ones, and
	// no time passing except where the caller says it does.
	scheduler.ClearTimings();
	scheduler.RunPhases(store, Phase::PreSimulation, Phase::PostSimulation);

	REQUIRE(order == std::vector<std::string> { "sim" });
	REQUIRE(store.Time().Tick == 0);

	scheduler.RunPhases(store, Phase::PreRender, Phase::PreRender);
	REQUIRE(order == std::vector<std::string> { "sim", "draw" });
	REQUIRE(store.Time().Tick == 0);
}

TEST_CASE("timings accumulate across the calls that make up one frame",
	"[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	scheduler.Add("stepped", Phase::Simulation, [](Store &) {});

	// Three ticks owed by one frame is one row showing what the system cost
	// that frame, not the last of three identical rows.
	scheduler.ClearTimings();
	for (int tick = 0; tick < 3; tick++) {
		scheduler.RunPhases(store, Phase::PreSimulation, Phase::PostSimulation);
	}

	REQUIRE(scheduler.Timings().size() == 1);
	REQUIRE(scheduler.Timings()[0].Name == "stepped");
}

TEST_CASE("a system mutates the store it is handed", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;

	const Entity entity = store.Create();
	store.Set<Health>(entity, Health { 100 });

	scheduler.Add("decay", Phase::Simulation, [](Store &tickStore) {
		tickStore.Each<Health>([](Entity, Health &health) { health.Value -= 10; });
	});

	scheduler.Tick(store, 0.016f);
	scheduler.Tick(store, 0.016f);

	REQUIRE(store.Get<Health>(entity)->Value == 80);
}

TEST_CASE("every system is timed", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;

	scheduler.Add("first", Phase::Simulation, [](Store &) {});
	scheduler.Add("second", Phase::PreRender, [](Store &) {});

	scheduler.Tick(store, 0.016f);

	const auto &timings = scheduler.Timings();
	REQUIRE(timings.size() == 2);
	REQUIRE(timings[0].Name == "first");
	REQUIRE(timings[0].RunPhase == Phase::Simulation);
	REQUIRE(timings[1].Name == "second");
	REQUIRE(timings[1].RunPhase == Phase::PreRender);
}

TEST_CASE("timings are this run's, not every run's", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	scheduler.Add("only", Phase::Simulation, [](Store &) {});

	scheduler.Tick(store, 0.016f);
	scheduler.Tick(store, 0.016f);

	REQUIRE(scheduler.Timings().size() == 1);
}

TEST_CASE("systems appear in the frame graph without their own instrumentation",
	"[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	scheduler.Add("uninstrumented", Phase::Simulation, [](Store &) {});

	// Disabling clears the published frame, so the assertions have to run
	// while collection is still on. The guard is what turns it back off.
	struct Collecting {
		Collecting() {
			FrameGraph::SetEnabled(true);
		}
		~Collecting() {
			FrameGraph::SetEnabled(false);
		}
	} collecting;

	FrameGraph::BeginFrame();
	scheduler.Tick(store, 0.016f);
	FrameGraph::EndFrame();

	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 2);
	REQUIRE(spans[0].Name == "Scheduler::RunPhases");
	REQUIRE(spans[1].Name == "uninstrumented");
	REQUIRE(spans[1].Depth == 1);
}
