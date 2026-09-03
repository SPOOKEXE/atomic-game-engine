// The per-view backend dispatch table used by the render graph.
//
// A view builds this table before it records a command. The default catalogue
// is represented by the same built-in kinds the node families bind. It does
// not run a graph or create a device because those costs belong to execute
// graph.

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/render/GraphRunner.hpp>
#include <engine/testing/Bench.hpp>

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
