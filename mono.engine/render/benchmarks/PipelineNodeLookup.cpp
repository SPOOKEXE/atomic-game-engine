#include "../src/PipelineNodeLookup.hpp"

#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

TEST_SUITE_ID("engine.render.bench.pipeline-node-lookup")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {
	using namespace engine;
	using namespace engine::graph;
	using engine::render::detail::EnabledNodeIndex;
	using engine::render::detail::FindEnabledNodeByScan;
	using engine::render::detail::FindFirstNodeByScan;
	using engine::testing::Consume;

	const std::array kinds{
		core::Name("surface-capture"),
		core::Name("mirror-capture"),
		core::Name("portal-capture"),
		core::Name("overlay"),
		core::Name("transparent"),
		core::Name("transparent-layer"),
		core::Name("shadow"),
		core::Name("interface"),
		core::Name("spatial-overlay"),
		core::Name("gbuffer"),
		core::Name("depth-peel"),
		core::Name("depth-linearise"),
		core::Name("depth-validity"),
		core::Name("ssao"),
		core::Name("deferred-lighting"),
		core::Name("fog"),
		core::Name("tonemap"),
		core::Name("camera-motion"),
		core::Name("world"),
		core::Name("cull-frustum"),
	};
	const std::array extentKinds{
		core::Name("depth-linearise"), core::Name("ssao"), core::Name("deferred-lighting")
	};

	const RenderGraph &DefaultGraph() {
		static const RenderGraph graph = [] {
			RenderGraph built;
			core::Name offender;
			if (Build(DefaultPbrDocument(), built, offender) != PipelineDocumentStatus::Ok) {
				throw std::runtime_error("default PBR document did not build");
			}
			return built;
		}();
		return graph;
	}

	const RenderGraph &LargeGraph() {
		static const RenderGraph graph = [] {
			RenderGraph built;
			for (uint32_t index = 0; index < 128; index++) {
				const std::string name = "synthetic-node-" + std::to_string(index);
				const uint32_t queryStart = 128 - static_cast<uint32_t>(kinds.size());
				const core::Name kind = index >= queryStart
											? kinds[index - queryStart]
											: core::Name(("synthetic-kind-" + std::to_string(index)).c_str());
				built.AddNode(Node{.Name = core::Name(name), .Kind = kind});
			}
			return built;
		}();
		return graph;
	}

	uint64_t ScanQueries(const RenderGraph &graph) {
		uint64_t found = 0;
		for (const core::Name kind : kinds) {
			const graph::Node *node = FindEnabledNodeByScan(graph, kind);
			found += node == nullptr ? 0 : node->Name.Id();
		}
		return found;
	}

	uint64_t ScanExtentQueries(const RenderGraph &graph) {
		uint64_t found = 0;
		for (const core::Name kind : extentKinds) {
			const graph::Node *node = FindFirstNodeByScan(graph, kind);
			found += node == nullptr ? 0 : node->Name.Id();
		}
		return found;
	}

	uint64_t IndexedQueries(const RenderGraph &graph, const EnabledNodeIndex &index) {
		uint64_t found = 0;
		for (const core::Name kind : kinds) {
			const graph::Node *node = index.Enabled(graph, kind);
			found += node == nullptr ? 0 : node->Name.Id();
		}
		return found;
	}

	uint64_t IndexedExtentQueries(const RenderGraph &graph, const EnabledNodeIndex &index) {
		uint64_t found = 0;
		for (const core::Name kind : extentKinds) {
			const graph::Node *node = index.First(graph, kind);
			found += node == nullptr ? 0 : node->Name.Id();
		}
		return found;
	}
}

BENCH("default PBR · repeated graph scans · 20 queried kinds", 1000) {
	const RenderGraph &graph = DefaultGraph();
	for (size_t iteration = 0; iteration < 1000; iteration++) {
		Consume(ScanQueries(graph));
	}
}

BENCH("default PBR · build index and query · 20 kinds", 1000) {
	const RenderGraph &graph = DefaultGraph();
	for (size_t iteration = 0; iteration < 1000; iteration++) {
		EnabledNodeIndex index;
		index.Rebuild(graph);
		Consume(IndexedQueries(graph, index));
	}
}

BENCH("default PBR · installed index queries · 20 kinds", 1000) {
	const RenderGraph &graph = DefaultGraph();
	EnabledNodeIndex index;
	index.Rebuild(graph);
	for (size_t iteration = 0; iteration < 1000; iteration++) {
		Consume(IndexedQueries(graph, index));
	}
}

BENCH("default PBR · repeated extent scans · 3 kinds", 1000) {
	const RenderGraph &graph = DefaultGraph();
	for (size_t iteration = 0; iteration < 1000; iteration++) {
		Consume(ScanExtentQueries(graph));
	}
}

BENCH("default PBR · installed extent index · 3 kinds", 1000) {
	const RenderGraph &graph = DefaultGraph();
	EnabledNodeIndex index;
	index.Rebuild(graph);
	for (size_t iteration = 0; iteration < 1000; iteration++) {
		Consume(IndexedExtentQueries(graph, index));
	}
}

BENCH("128-node graph · repeated graph scans · 20 queried kinds", 1000) {
	const RenderGraph &graph = LargeGraph();
	for (size_t iteration = 0; iteration < 1000; iteration++) {
		Consume(ScanQueries(graph));
	}
}

BENCH("128-node graph · installed index queries · 20 kinds", 1000) {
	const RenderGraph &graph = LargeGraph();
	EnabledNodeIndex index;
	index.Rebuild(graph);
	for (size_t iteration = 0; iteration < 1000; iteration++) {
		Consume(IndexedQueries(graph, index));
	}
}
