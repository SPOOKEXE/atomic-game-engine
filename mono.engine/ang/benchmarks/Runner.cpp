// Runner visit cost with and without hooks, and the Parallel crossover for a
// wave of cheap nodes. Fixtures are checked before timing.

#include <engine/ang/Compile.hpp>
#include <engine/ang/Runner.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <stdexcept>

TEST_SUITE_ID("engine.ang.bench.runner")

namespace {
	using namespace engine::ang;
	using engine::testing::Consume;

	struct Workers {
		Workers() {
			engine::parallel::Jobs::Start(0);
		}
		~Workers() {
			engine::parallel::Jobs::Stop();
		}
	};
	const Workers WORKERS;

	Registry MakeRegistry() {
		Registry registry = Registry::WithScalars();
		const auto square = [](EvalContext &context) {
			const double value = context.Input("in").AsNumber();
			context.SetOutput("out", Value(value * value));
			return NodeResult::Success();
		};
		registry.DefineNode(
			{.Id = "sync.square",
			 .Inputs = {{.Name = "in", .Type = "number", .Default = Value(3.0)}},
			 .Outputs = {{.Name = "out", .Type = "number"}},
			 .Evaluate = square}
		);
		registry.DefineNode(
			{.Id = "parallel.square",
			 .Inputs = {{.Name = "in", .Type = "number", .Default = Value(3.0)}},
			 .Outputs = {{.Name = "out", .Type = "number"}},
			 .Mode = ExecMode::Parallel,
			 .Evaluate = square}
		);
		return registry;
	}

	const Registry REGISTRY = MakeRegistry();

	// One wave of `count` independent nodes.
	Plan Wave(const char *def, size_t count) {
		Graph graph;
		for (size_t index = 0; index < count; ++index) {
			graph.Apply(edit::AddNode{{.Id = "n" + std::to_string(index), .Def = def}});
		}
		Plan plan;
		if (!Compile(graph, REGISTRY, {.Runs = Domain::Tick}, plan).Ok || plan.Waves.size() != 1) {
			throw std::runtime_error("ang benchmark fixture refused");
		}
		return plan;
	}

	struct Fixture {
		Runner Run;
		explicit Fixture(Plan plan, bool hooks = false) : Run(std::move(plan)) {
			if (hooks) {
				Hooks &set = Run.Callbacks();
				set.PreNode = [](const NodeInfo &info) { Consume(info.Index); };
				set.PostNode = [](const NodeInfo &info, NodeStatus) { Consume(info.Index); };
				set.PreCompute = [](const NodeInfo &info) { Consume(info.Index); };
				set.PostCompute = [](const NodeInfo &info, ComputeResult &) { Consume(info.Index); };
			}
			if (Run.Run().Ran != Run.CurrentPlan().Nodes.size()) {
				throw std::runtime_error("ang benchmark fixture did not run");
			}
		}
	};

	void Cold(Fixture &fixture) {
		fixture.Run.ClearCache();
		Consume(fixture.Run.Run().Ran);
	}

	void Warm(Fixture &fixture) {
		Consume(fixture.Run.Run().Cached);
	}

	Fixture CACHED_PLAIN(Wave("sync.square", 64));
	Fixture CACHED_HOOKED(Wave("sync.square", 64), true);
	Fixture COLD_PLAIN(Wave("sync.square", 64));
	Fixture COLD_HOOKED(Wave("sync.square", 64), true);
	Fixture SYNC_8(Wave("sync.square", 8));
	Fixture PARALLEL_8(Wave("parallel.square", 8));
	Fixture SYNC_64(Wave("sync.square", 64));
	Fixture PARALLEL_64(Wave("parallel.square", 64));
	Fixture SYNC_512(Wave("sync.square", 512));
	Fixture PARALLEL_512(Wave("parallel.square", 512));
}

BENCH("Run 64 cached, no hooks", 64) {
	Warm(CACHED_PLAIN);
}
BENCH("Run 64 cached, empty-body hooks", 64) {
	Warm(CACHED_HOOKED);
}
BENCH("Run 64 evaluated, no hooks", 64) {
	Cold(COLD_PLAIN);
}
BENCH("Run 64 evaluated, empty-body hooks", 64) {
	Cold(COLD_HOOKED);
}
BENCH("Run wave 8 sync", 8) {
	Cold(SYNC_8);
}
BENCH("Run wave 8 parallel", 8) {
	Cold(PARALLEL_8);
}
BENCH("Run wave 64 sync", 64) {
	Cold(SYNC_64);
}
BENCH("Run wave 64 parallel", 64) {
	Cold(PARALLEL_64);
}
BENCH("Run wave 512 sync", 512) {
	Cold(SYNC_512);
}
BENCH("Run wave 512 parallel", 512) {
	Cold(PARALLEL_512);
}
