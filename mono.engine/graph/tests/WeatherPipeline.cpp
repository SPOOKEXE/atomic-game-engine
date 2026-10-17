#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.graph.weatherpipeline")
TEST_DEPENDS("engine.graph.rendergraph")
TEST_DEPENDS("engine.graph.pipelinecatalogue")

using engine::core::Name;
using engine::graph::Build;
using engine::graph::CompiledGraph;
using engine::graph::DefaultPbrDocument;
using engine::graph::DefaultPbrTierBDocument;
using engine::graph::GraphStatus;
using engine::graph::Node;
using engine::graph::NodeCatalogue;
using engine::graph::NodeId;
using engine::graph::NodeScope;
using engine::graph::PipelineDocumentStatus;
using engine::graph::RenderGraph;
using engine::graph::ResourceDesc;
using engine::graph::ResourceFormat;
using engine::graph::ResourceId;
using engine::graph::ResourceKind;
using engine::graph::ResourceLifetime;

namespace {
	const Node *FindNode(const RenderGraph &graph, std::string_view name) {
		for (uint32_t value = 1; value <= graph.Count(); value++) {
			const Node *node = graph.Find(NodeId{value});
			if (node != nullptr && node->Name == Name(name)) return node;
		}
		return nullptr;
	}

	const ResourceDesc *FindResourceDesc(const RenderGraph &graph, std::string_view name) {
		for (uint32_t value = 1; value <= graph.ResourceCount(); value++) {
			const ResourceDesc *resource = graph.FindResourceDesc(ResourceId{value});
			if (resource != nullptr && resource->Name == Name(name)) return resource;
		}
		return nullptr;
	}

	bool ContainsNode(const RenderGraph &graph, std::string_view name) {
		return FindNode(graph, name) != nullptr;
	}
}

TEST_CASE("weather pages feed world work and each view's integration", "[graph][weather]") {
	engine::graph::RegisterRenderNodeKinds();
	RenderGraph graph;
	Name offender;
	REQUIRE(Build(DefaultPbrDocument(), graph, offender) == PipelineDocumentStatus::Ok);

	CompiledGraph compiled;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
	const auto sharedPosition = [&compiled, &graph](std::string_view name) {
		for (size_t index = 0; index < compiled.Shared.size(); index++)
			if (graph.Find(compiled.Shared[index])->Name == Name(name)) return index;
		return compiled.Shared.size();
	};
	CHECK(sharedPosition("weather-residency") < sharedPosition("weather-field"));
	CHECK(sharedPosition("weather-field") < sharedPosition("weather-summary"));
	CHECK(std::any_of(compiled.PerView.begin(), compiled.PerView.end(), [&graph](NodeId id) {
		return graph.Find(id)->Name == Name("weather-integrate");
	}));

	const ResourceDesc *pages = FindResourceDesc(graph, "weather-pages");
	const ResourceDesc *previousPages = FindResourceDesc(graph, "weather-previous-pages");
	REQUIRE(pages != nullptr);
	REQUIRE(previousPages != nullptr);
	CHECK(pages->Kind == ResourceKind::Buffer);
	CHECK(pages->Format == ResourceFormat::R8);
	CHECK(pages->Width == 512);
	CHECK(pages->Height == 1);
	CHECK(pages->BufferStride == 16);
	CHECK(pages->Lifetime == ResourceLifetime::History);
	CHECK(previousPages->Kind == ResourceKind::Buffer);
	CHECK(previousPages->Format == ResourceFormat::R8);
	CHECK(previousPages->Width == 512);
	CHECK(previousPages->Height == 1);
	CHECK(previousPages->BufferStride == 16);
	CHECK(previousPages->Lifetime == ResourceLifetime::History);

	const Node *residency = FindNode(graph, "weather-residency");
	const Node *field = FindNode(graph, "weather-field");
	const Node *summary = FindNode(graph, "weather-summary");
	const Node *integrate = FindNode(graph, "weather-integrate");
	REQUIRE(residency != nullptr);
	REQUIRE(field != nullptr);
	REQUIRE(summary != nullptr);
	REQUIRE(integrate != nullptr);
	CHECK(residency->Scope == NodeScope::World);
	CHECK(field->Scope == NodeScope::World);
	CHECK(summary->Scope == NodeScope::World);
	CHECK(integrate->Scope == NodeScope::View);
	CHECK((residency->WritePorts == std::vector<Name>{Name("pages"), Name("previous-pages")}));
	CHECK(field->ReadPorts == std::vector<Name>{Name("pages")});
	CHECK((summary->ReadPorts == std::vector<Name>{Name("field"), Name("pages")}));
	CHECK(
		(integrate->ReadPorts == std::vector<Name>{
									 Name("colour"),
									 Name("depth"),
									 Name("field"),
									 Name("summary"),
									 Name("pages"),
									 Name("previous-pages")
								 })
	);
	CHECK(graph.FindResourceDesc(residency->Writes.front())->Name == Name("weather-pages"));
	CHECK(graph.FindResourceDesc(residency->Writes[1])->Name == Name("weather-previous-pages"));
	CHECK(graph.FindResourceDesc(field->Reads.front())->Name == Name("weather-pages"));
	CHECK(graph.FindResourceDesc(field->Writes.front())->Name == Name("weather-field"));
	CHECK(graph.FindResourceDesc(summary->Reads[0])->Name == Name("weather-field"));
	CHECK(graph.FindResourceDesc(summary->Reads[1])->Name == Name("weather-pages"));
	CHECK(graph.FindResourceDesc(integrate->Reads[4])->Name == Name("weather-pages"));
	CHECK(graph.FindResourceDesc(integrate->Reads[5])->Name == Name("weather-previous-pages"));
	CHECK(graph.FindResourceDesc(integrate->Writes.front())->Name == Name("weather-clouds"));

	const auto *residencySpec = NodeCatalogue::Find(Name("weather-residency"));
	REQUIRE(residencySpec != nullptr);
	CHECK(residencySpec->BuiltInBackend);
	CHECK(residencySpec->Queue == engine::graph::ExecutionQueue::Graphics);
	CHECK((residencySpec->Outputs[0].Name == Name("pages")));
	CHECK((residencySpec->Outputs[1].Name == Name("previous-pages")));
}

TEST_CASE("Tier B removes weather residency with the weather pipeline", "[graph][weather]") {
	RenderGraph graph;
	Name offender;
	REQUIRE(Build(DefaultPbrTierBDocument(), graph, offender) == PipelineDocumentStatus::Ok);
	CHECK(FindResourceDesc(graph, "weather-pages") == nullptr);
	CHECK(FindResourceDesc(graph, "weather-previous-pages") == nullptr);
	CHECK(FindResourceDesc(graph, "weather-field") == nullptr);
	CHECK(FindResourceDesc(graph, "weather-summary") == nullptr);
	CHECK(FindResourceDesc(graph, "weather-lit") == nullptr);
	CHECK_FALSE(ContainsNode(graph, "weather-residency"));
	CHECK_FALSE(ContainsNode(graph, "weather-field"));
	CHECK_FALSE(ContainsNode(graph, "weather-summary"));
	CHECK_FALSE(ContainsNode(graph, "weather-integrate"));

	const Node *fog = FindNode(graph, "fog");
	REQUIRE(fog != nullptr);
	REQUIRE(fog->Reads.size() >= 1);
	CHECK(graph.FindResourceDesc(fog->Reads.front())->Name == Name("sky-lit"));
}
