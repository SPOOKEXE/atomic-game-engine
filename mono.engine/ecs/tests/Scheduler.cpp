#include <engine/core/FrameGraph.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
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
using engine::ecs::SystemOrder;
using engine::ecs::SystemScheduleStatus;

namespace scheduler_test {
	struct Health {
		int Value = 0;
	};
}

using namespace scheduler_test;

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

	REQUIRE(order == std::vector<std::string>{"a", "b", "c", "d"});
}

TEST_CASE("the complete engine tick exposes its seven fixed phases", "[scheduler]") {
	CHECK(engine::ecs::GetPhaseName(Phase::Input) == "input");
	CHECK(engine::ecs::GetPhaseName(Phase::Simulation) == "simulation");
	CHECK(engine::ecs::GetPhaseName(Phase::Physics) == "physics");
	CHECK(engine::ecs::GetPhaseName(Phase::Animation) == "animation");
	CHECK(engine::ecs::GetPhaseName(Phase::Replication) == "replication");
	CHECK(engine::ecs::GetPhaseName(Phase::RenderPreparation) == "render preparation");
	CHECK(engine::ecs::GetPhaseName(Phase::Render) == "render");
}

TEST_CASE("a system body hot reload keeps its schedule position", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	std::vector<std::string> order;
	scheduler.Add("first", Phase::Simulation, [&](Store &) { order.emplace_back("old"); });
	scheduler.Add(
		"second",
		Phase::Simulation,
		[&](Store &) { order.emplace_back("second"); },
		SystemOrder{{}, {"first"}}
	);

	REQUIRE(scheduler.SystemRevision("first") == 1);
	REQUIRE(scheduler.Replace("first", 2, [&](Store &) { order.emplace_back("new"); }));
	REQUIRE_FALSE(scheduler.Replace("first", 2, [](Store &) {}));
	REQUIRE(scheduler.SystemRevision("first") == 2);
	scheduler.Tick(store, 0.016f);
	CHECK(order == std::vector<std::string>{"new", "second"});
}

TEST_CASE("before and after edges order systems within one phase", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	std::vector<std::string> order;

	// Registered against the requested order so insertion cannot satisfy it.
	scheduler.Add("finish", Phase::PreRender, [&](Store &) { order.emplace_back("finish"); });
	scheduler.Add(
		"consume",
		Phase::PreRender,
		[&](Store &) { order.emplace_back("consume"); },
		SystemOrder{{"finish"}, {"derive"}}
	);
	scheduler.Add("derive", Phase::PreRender, [&](Store &) { order.emplace_back("derive"); });

	scheduler.Tick(store, 0.016f);
	REQUIRE(order == std::vector<std::string>{"derive", "consume", "finish"});
}

TEST_CASE("invalid system dependencies have actionable validation results", "[scheduler]") {
	SECTION("unknown system") {
		Scheduler scheduler;
		scheduler.Add("consumer", Phase::Simulation, [](Store &) {}, SystemOrder{{}, {"missing"}});
		const auto issue = scheduler.Validate();
		CHECK(issue.Status == SystemScheduleStatus::UnknownDependency);
		CHECK(issue.System == "consumer");
		CHECK(issue.Dependency == "missing");
	}

	SECTION("cross phase") {
		Scheduler scheduler;
		scheduler.Add("input", Phase::Input, [](Store &) {});
		scheduler.Add("simulation", Phase::Simulation, [](Store &) {}, SystemOrder{{}, {"input"}});
		CHECK(scheduler.Validate().Status == SystemScheduleStatus::CrossPhaseDependency);
	}

	SECTION("cycle") {
		Scheduler scheduler;
		scheduler.Add("a", Phase::Simulation, [](Store &) {}, SystemOrder{{}, {"b"}});
		scheduler.Add("b", Phase::Simulation, [](Store &) {}, SystemOrder{{}, {"a"}});
		CHECK(scheduler.Validate().Status == SystemScheduleStatus::Cycle);
	}

	SECTION("duplicate name") {
		Scheduler scheduler;
		scheduler.Add("same", Phase::Simulation, [](Store &) {});
		scheduler.Add("same", Phase::Simulation, [](Store &) {});
		CHECK(scheduler.Validate().Status == SystemScheduleStatus::DuplicateName);
	}
}

TEST_CASE("a shared installer can detect an existing phase-local system", "[scheduler]") {
	Scheduler scheduler;
	CHECK_FALSE(scheduler.HasSystem("shared", Phase::Input));
	scheduler.Add("shared", Phase::Input, [](Store &) {});
	CHECK(scheduler.HasSystem("shared", Phase::Input));
	CHECK_FALSE(scheduler.HasSystem("shared", Phase::Simulation));
}

TEST_CASE("optional edges order modular systems only when both are installed", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	std::vector<std::string> order;

	scheduler.Add("host", Phase::Simulation, [&](Store &) { order.emplace_back("host"); });
	scheduler.Add(
		"module",
		Phase::Simulation,
		[&](Store &) { order.emplace_back("module"); },
		SystemOrder{{}, {}, {"host", "not-installed"}, {}}
	);

	REQUIRE(scheduler.Validate().Status == SystemScheduleStatus::Ready);
	scheduler.Tick(store, 0.016f);
	CHECK(order == std::vector<std::string>{"module", "host"});
}

TEST_CASE("independent read-only systems share a parallel wave", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	std::atomic<int> arrived = 0;
	std::atomic<int> overlapped = 0;
	std::atomic<int> sawStore = 0;

	const auto body = [&](const Store &world) {
		if (world.Name() == "test") {
			sawStore.fetch_add(1, std::memory_order_relaxed);
		}
		arrived.fetch_add(1, std::memory_order_release);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (arrived.load(std::memory_order_acquire) < 2 && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::yield();
		}
		if (arrived.load(std::memory_order_acquire) == 2) {
			overlapped.fetch_add(1, std::memory_order_relaxed);
		}
	};

	const bool startedPool = engine::parallel::Jobs::WorkerCount() == 0;
	if (startedPool) {
		engine::parallel::Jobs::Start(1);
	}
	scheduler.AddParallel("read-a", Phase::Simulation, body);
	scheduler.AddParallel("read-b", Phase::Simulation, body);
	scheduler.Tick(store, 0.016f);
	if (startedPool) {
		engine::parallel::Jobs::Stop();
	}

	CHECK(overlapped.load(std::memory_order_relaxed) == 2);
	CHECK(sawStore.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("parallel system timings are reported on the frame owner", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	scheduler.AddParallel("read-a", Phase::Simulation, [](const Store &) {});
	scheduler.AddParallel("read-b", Phase::Simulation, [](const Store &) {});

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
	const auto first =
		std::find_if(spans.begin(), spans.end(), [](const auto &span) { return span.Name == "read-a"; });
	const auto second =
		std::find_if(spans.begin(), spans.end(), [](const auto &span) { return span.Name == "read-b"; });
	REQUIRE(first != spans.end());
	REQUIRE(second != spans.end());
	CHECK(first->Reported);
	CHECK(second->Reported);
}

// Phase order is fixed and does not need a named edge.
TEST_CASE("a phase boundary outranks registration order", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;
	std::vector<std::string> order;

	scheduler.Add("drawn-first", Phase::PreRender, [&](Store &) { order.emplace_back("drawn-first"); });
	scheduler.Add("simulated-second", Phase::Simulation, [&](Store &) {
		order.emplace_back("simulated-second");
	});

	scheduler.Tick(store, 0.016f);

	REQUIRE(order == std::vector<std::string>{"simulated-second", "drawn-first"});
}

TEST_CASE("the delta reaches every system through the world's clock", "[scheduler]") {
	Store store("test");
	Scheduler scheduler;

	float seen = 0.0f;
	scheduler.Add("read-delta", Phase::Simulation, [&](Store &tickStore) { seen = tickStore.Time().Delta; });

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

	REQUIRE(order == std::vector<std::string>{"sim"});
	REQUIRE(store.Time().Tick == 0);

	scheduler.RunPhases(store, Phase::PreRender, Phase::PreRender);
	REQUIRE(order == std::vector<std::string>{"sim", "draw"});
	REQUIRE(store.Time().Tick == 0);
}

TEST_CASE("timings accumulate across the calls that make up one frame", "[scheduler]") {
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
	store.Set<Health>(entity, Health{100});

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

TEST_CASE("systems appear in the frame graph without their own instrumentation", "[scheduler]") {
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

	// The scheduler, a span per phase, and the system inside its own. Four
	// phases run even when only one holds a system: an empty phase costing
	// nothing is worth seeing, because a phase that suddenly costs something is
	// how a bottleneck announces itself.
	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 9);

	REQUIRE(spans[0].Name == "ecs.systems");
	REQUIRE(spans[0].Depth == 0);
	REQUIRE(spans[0].Category == engine::core::ProfileCategory::ECS);

	// Phases in declaration order, each a child of the scheduler.
	REQUIRE(spans[1].Name == "input");
	REQUIRE(spans[2].Name == "simulation");
	REQUIRE(spans[4].Name == "physics");
	REQUIRE(spans[5].Name == "animation");
	REQUIRE(spans[6].Name == "replication");
	REQUIRE(spans[7].Name == "render preparation");
	REQUIRE(spans[8].Name == "render");
	for (size_t index : {1u, 2u, 4u, 5u, 6u, 7u, 8u}) {
		REQUIRE(spans[index].Depth == 1);
		REQUIRE(spans[index].Category == engine::core::ProfileCategory::ECS);
	}

	// The system sits under its phase rather than under the scheduler, which is
	// what makes "which phase" answerable before "which system".
	REQUIRE(spans[3].Name == "uninstrumented");
	REQUIRE(spans[3].Depth == 2);
	REQUIRE(spans[3].Category == engine::core::ProfileCategory::ECS);
}

TEST_CASE("every system's time is ECS time", "[scheduler]") {
	// Each engine and game system runs through the scheduler, so a slow system
	// is ECS time and the category bar has to say so. Before this the same work
	// was filed under `Simulation` alongside the driver's own machinery, and
	// the two could not be told apart.
	Store store("test");
	Scheduler scheduler;
	scheduler.Add("busy", Phase::Simulation, [](Store &) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	});

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

	REQUIRE(FrameGraph::CategoryMilliseconds(engine::core::ProfileCategory::ECS) >= 1.0f);
	REQUIRE(FrameGraph::CategoryMilliseconds(engine::core::ProfileCategory::Simulation) == 0.0f);
}
