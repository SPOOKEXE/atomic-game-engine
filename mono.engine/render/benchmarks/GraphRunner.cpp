// Compare binding and whole GraphRunner calls against the mutable NodeTable.
// Each whole-run call executes four views. Scheduling and GPU work are excluded.

#include "ViewRecording.hpp"

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/render/GraphRunner.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.render.bench.graphrunner")

using engine::core::Name;
using engine::graph::RunContext;
using engine::render::NodeHandler;
using engine::render::NodeTable;
using engine::testing::Consume;

namespace {
	const std::vector<Name> &BuiltInKinds() {
		static const std::vector<Name> kinds = [] {
			engine::graph::RegisterRenderNodeKinds();
			std::vector<Name> names;
			for (const engine::graph::NodeKindSpec &spec : engine::graph::NodeCatalogue::All()) {
				if (spec.BuiltInBackend) {
					names.push_back(spec.Kind);
				}
			}
			return names;
		}();
		return kinds;
	}
}

BENCH("NodeTable · build built-in handlers", 1'000) {
	const std::vector<Name> &kinds = BuiltInKinds();
	const NodeHandler handler = [](const RunContext &) { return true; };
	for (size_t iteration = 0; iteration < 1'000; iteration++) {
		NodeTable table;
		for (const Name kind : kinds) {
			table.Set(kind, handler);
		}
		Consume(table.Count());
	}
}

BENCH("NodeTable · find every built-in handler", 1'000) {
	const std::vector<Name> &kinds = BuiltInKinds();
	const NodeHandler handler = [](const RunContext &) { return true; };
	NodeTable table;
	for (const Name kind : kinds) {
		table.Set(kind, handler);
	}
	for (size_t iteration = 0; iteration < 1'000; iteration++) {
		for (const Name kind : kinds) {
			Consume(table.Find(kind));
		}
	}
}

BENCH("NodeDispatch · bind current recording", 1'000) {
	using namespace engine::render;
	struct Recordings {
		Renderer Owner;
		std::unique_ptr<ViewRecording::Impl> State = std::make_unique<ViewRecording::Impl>();
		FrameResult FirstResult;
		FrameResult SecondResult;
		ViewRecording First{Owner, *State, FirstResult};
		ViewRecording Second{Owner, *State, SecondResult};
	};
	static Recordings recordings;
	for (size_t iteration = 0; iteration < 1'000; iteration++) {
		ViewRecording &current = iteration % 2 == 0 ? recordings.First : recordings.Second;
		Consume(current.Dispatch());
	}
}

namespace {
	constexpr size_t DISPATCH_FRAMES = 1'000;
	constexpr size_t DISPATCH_VIEWS = 4;
	constexpr std::array CPU_KINDS{
		"world", "camera", "entities", "cull-frustum", "cull-distance", "filter-tag", "order-draw"
	};

	struct DispatchWorkload {
		engine::render::Renderer Owner;
		std::unique_ptr<engine::render::ViewRecording::Impl> State =
			std::make_unique<engine::render::ViewRecording::Impl>();
		engine::render::InstalledPipeline Pipeline;
		std::array<engine::render::FrameResult, DISPATCH_VIEWS> Results;
		std::array<std::unique_ptr<engine::render::ViewRecording>, DISPATCH_VIEWS> Recordings;
		std::array<NodeTable, DISPATCH_VIEWS> Tables;
		std::array<engine::render::NodeDispatch, DISPATCH_VIEWS> Dispatches;
		std::vector<RunContext> Contexts;
		std::vector<Name> ExpectedNodes;
		bool Cpu = false;

		explicit DispatchWorkload(bool cpu) : Cpu(cpu) {
			State->ProfileTier = engine::render::ProfilingTier::Off;
			if (cpu) {
				for (const char *kind : CPU_KINDS) {
					const Name name(kind);
					Contexts.push_back(
						{.Node = {},
						 .Name = name,
						 .Kind = name,
						 .View = 0,
						 .World = 0,
						 .Reads = {},
						 .Writes = {}}
					);
					ExpectedNodes.push_back(name);
				}
			} else {
				// Output handlers always execute and require device state. Every other
				// family is inactive with an undamaged scene and no retained families.
				const std::array outputs{
					Name("spatial-overlay"),
					Name("eye-image"),
					Name("interface"),
					Name("overlay"),
					Name("present"),
					Name("viewer"),
					Name("shadow-capture"),
					Name("capture"),
					Name("output-image")
				};
				for (const Name kind : BuiltInKinds()) {
					if (std::find(outputs.begin(), outputs.end(), kind) == outputs.end())
						Contexts.push_back(
							{.Node = {},
							 .Name = kind,
							 .Kind = kind,
							 .View = 0,
							 .World = 0,
							 .Reads = {},
							 .Writes = {}}
						);
				}
			}
			for (size_t view = 0; view < DISPATCH_VIEWS; ++view) {
				Results[view].Nodes.reserve(CPU_KINDS.size());
				Recordings[view] =
					std::make_unique<engine::render::ViewRecording>(Owner, *State, Results[view]);
				Recordings[view]->Pipeline = &Pipeline;
				Recordings[view]->Request.Damage.Scene = cpu;
				Tables[view] = BuildTable(view);
				Dispatches[view] = Recordings[view]->Dispatch();
			}
		}

		NodeTable BuildTable(size_t view) {
			NodeTable table;
			for (const Name kind : BuiltInKinds())
				table.Set(kind, [](const RunContext &) { return true; });
			if (Cpu) {
				for (const char *kind : CPU_KINDS)
					table.Set(Name(kind), [recording = Recordings[view].get()](const RunContext &context) {
						return recording->FinishCpuNode(context);
					});
			}
			return table;
		}
	};

	// The prebuilt pairs isolate runner lookup and execution, including Has+Run.
	// The binding pairs add table copying for idle views or catalogue rebuilding
	// plus seven real CPU handlers for dirty views. Other table handlers are no-ops,
	// so this is a controlled catalogue baseline, not a full legacy renderer frame.
	// Each reported call is one four-view frame, not one individual node invocation.
	void RunDispatchWorkload(bool cpu, bool contextual, bool bind) {
		static DispatchWorkload idle(false);
		static DispatchWorkload active(true);
		DispatchWorkload &work = cpu ? active : idle;
		size_t submitted = 0;
		for (size_t frame = 0; frame < DISPATCH_FRAMES; ++frame) {
			for (size_t view = 0; view < DISPATCH_VIEWS; ++view) {
				work.Results[view].Nodes.clear();
				std::optional<NodeTable> rebound;
				if (bind && !contextual) {
					if (cpu)
						rebound.emplace(work.BuildTable(view));
					else
						rebound.emplace(work.Tables[view]);
				}
				const auto dispatch =
					bind && contextual ? work.Recordings[view]->Dispatch() : work.Dispatches[view];
				engine::render::GraphRunner runner =
					contextual ? engine::render::GraphRunner(dispatch, engine::render::ProfilingTier::Off)
							   : engine::render::GraphRunner(
									 bind ? *rebound : work.Tables[view], engine::render::ProfilingTier::Off
								 );
				for (auto context : work.Contexts) {
					context.View = view;
					context.World = view / 2;
					if (!runner.Run(context)) throw std::runtime_error("dispatch benchmark handler refused");
				}
				submitted += runner.Submitted();
				Consume(work.Results[view].Nodes);
			}
		}
		if (submitted != DISPATCH_FRAMES * DISPATCH_VIEWS * work.Contexts.size())
			throw std::runtime_error("dispatch benchmark submitted count mismatch");
		for (const auto &result : work.Results)
			if (result.Nodes != work.ExpectedNodes)
				throw std::runtime_error("dispatch benchmark independent output mismatch");
		Consume(submitted);
	}
}

BENCH("NodeTable · execute 4 views inactive families", 1'000) {
	RunDispatchWorkload(false, false, false);
}

BENCH("NodeDispatch · execute 4 views inactive families", 1'000) {
	RunDispatchWorkload(false, true, false);
}

BENCH("NodeTable · copy catalogue and execute 4 views inactive families", 1'000) {
	RunDispatchWorkload(false, false, true);
}

BENCH("NodeDispatch · bind and execute 4 views inactive families", 1'000) {
	RunDispatchWorkload(false, true, true);
}

BENCH("NodeTable · execute 4 views 7 CPU stages", 1'000) {
	RunDispatchWorkload(true, false, false);
}

BENCH("NodeDispatch · execute 4 views 7 CPU stages", 1'000) {
	RunDispatchWorkload(true, true, false);
}

BENCH("NodeTable · rebuild catalogue and execute 4 views 7 CPU stages", 1'000) {
	RunDispatchWorkload(true, false, true);
}

BENCH("NodeDispatch · bind and execute 4 views 7 CPU stages", 1'000) {
	RunDispatchWorkload(true, true, true);
}
