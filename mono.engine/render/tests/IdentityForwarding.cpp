#include "IdentityForwarding.hpp"

#include <engine/graph/PipelineProfile.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.render.identityforwarding")
TEST_DEPENDS("engine.graph.pipelineprofile")

namespace {
	using namespace engine;
	using render::detail::BuildIdentityContracts;
	using render::detail::IdentityCompatible;

	graph::ResourceDesc Colour(const char *name) {
		return {.Name = core::Name(name), .Format = graph::ResourceFormat::RGBA16F};
	}
}

TEST_CASE("identity forwarding preserves the authored image contract", "[render][identity]") {
	const auto source = Colour("source");
	auto target = Colour("target");
	CHECK(IdentityCompatible(source, target));
	SECTION("history owns a separately published image") {
		target.Lifetime = graph::ResourceLifetime::History;
	}
	SECTION("external output cannot acquire a transient binding") {
		target.External = true;
	}
	SECTION("history input cannot become a transient output") {
		auto history = source;
		history.Lifetime = graph::ResourceLifetime::History;
		CHECK_FALSE(IdentityCompatible(history, target));
		return;
	}
	SECTION("scaling requires publication") {
		target.Divisor = 2;
	}
	SECTION("different absolute extent requires publication") {
		target.Width = 64;
	}
	SECTION("format conversion requires publication") {
		target.Format = graph::ResourceFormat::RGBA8;
	}
	SECTION("access cannot widen through forwarding") {
		target.Access = graph::ResourceAccess::ReadWrite;
	}
	SECTION("multisample storage is not an identity colour binding") {
		target.Samples = 2;
	}
	SECTION("array storage is not an identity colour binding") {
		target.Layers = 2;
	}
	SECTION("mip views require publication") {
		target.FirstMip = 1;
	}
	SECTION("multiple mips require publication") {
		target.MipCount = 2;
	}
	SECTION("owner scope cannot change") {
		target.Owner = core::Name("another-world");
	}
	CHECK_FALSE(IdentityCompatible(source, target));
}

TEST_CASE(
	"identity contracts keep branches alive without extending across a later source write",
	"[render][identity]"
) {
	graph::RenderGraph graph;
	const auto source = graph.AddResource(Colour("source"));
	const auto middle = graph.AddResource(Colour("middle"));
	const auto output = graph.AddResource(Colour("output"));
	const auto later = graph.AddResource(Colour("later"));
	const auto producer =
		graph.AddNode({.Name = core::Name("producer"), .Kind = core::Name("raster"), .Writes = {source}});
	const auto first = graph.AddNode(
		{.Name = core::Name("first"), .Kind = core::Name("dof"), .Reads = {source}, .Writes = {middle}}
	);
	const auto second = graph.AddNode(
		{.Name = core::Name("second"), .Kind = core::Name("god-rays"), .Reads = {middle}, .Writes = {output}}
	);
	const auto branch = graph.AddNode(
		{.Name = core::Name("branch"),
		 .Kind = core::Name("raster"),
		 .Reads = {source, output},
		 .Writes = {later}}
	);
	const std::array ordered{producer, first, second, branch};
	const auto contracts = BuildIdentityContracts(graph, ordered);
	REQUIRE(contracts.size() == 2);
	CHECK(contracts[0].Source == source);
	CHECK(contracts[1].Source == middle);
	graph::CompiledGraph compiled;
	compiled.PerView.assign(ordered.begin(), ordered.end());
	const std::array pinned{source, middle};
	const auto aliases = graph::BuildResourceAliases(graph, compiled, pinned);
	CHECK(aliases.AllocationOf(source) != aliases.AllocationOf(later));
	CHECK(aliases.AllocationOf(middle) != aliases.AllocationOf(later));

	const auto rewrite =
		graph.AddNode({.Name = core::Name("rewrite"), .Kind = core::Name("raster"), .Writes = {source}});
	const std::array rewritten{producer, first, second, branch, rewrite};
	const auto refused = BuildIdentityContracts(graph, rewritten);
	REQUIRE(refused.size() == 1);
	CHECK(refused[0].Source == middle);
}

TEST_CASE("identity forwarding remains within the producing view", "[render][identity]") {
	for (const auto scope : {graph::NodeScope::World, graph::NodeScope::Frame}) {
		graph::RenderGraph graph;
		const auto source = graph.AddResource(Colour("source"));
		const auto output = graph.AddResource(Colour("output"));
		const auto producer = graph.AddNode(
			{.Name = core::Name("producer"), .Kind = core::Name("raster"), .Writes = {source}, .Scope = scope}
		);
		const auto effect = graph.AddNode(
			{.Name = core::Name("effect"), .Kind = core::Name("dof"), .Reads = {source}, .Writes = {output}}
		);
		CHECK(BuildIdentityContracts(graph, std::array{producer, effect}).empty());
	}
}

TEST_CASE("lens identity binds its colour ports regardless of port declaration order", "[render][identity]") {
	graph::RenderGraph graph;
	const auto colour = graph.AddResource(Colour("colour"));
	const auto depth = graph.AddResource(Colour("depth"));
	const auto output = graph.AddResource(Colour("output"));
	const auto scratch = graph.AddResource(Colour("scratch"));
	const auto producer = graph.AddNode(
		{.Name = core::Name("producer"), .Kind = core::Name("raster"), .Writes = {colour, depth}}
	);
	const auto lenses = graph.AddNode(
		{.Name = core::Name("lenses"),
		 .Kind = core::Name("shader-lenses"),
		 .Reads = {depth, colour},
		 .ReadPorts = {core::Name("depth"), core::Name("colour")},
		 .Writes = {scratch, output},
		 .WritePorts = {core::Name("scratch"), core::Name("colour")}}
	);
	const auto contracts = BuildIdentityContracts(graph, std::array{producer, lenses});
	REQUIRE(contracts.size() == 1);
	CHECK(contracts[0].Source == colour);
	CHECK(contracts[0].Target == output);
}

TEST_CASE("weather bypass preserves sampled colour across its storage output", "[render][identity]") {
	graph::RegisterRenderNodeKinds();
	graph::RenderGraph graph;
	const auto sky = graph.AddResource(Colour("sky"));
	auto storage = Colour("weather");
	storage.Kind = graph::ResourceKind::Storage;
	const auto weather = graph.AddResource(storage);
	const auto scratch = graph.AddResource(Colour("scratch"));
	const auto volume = graph.AddResource(Colour("volume"));
	const auto producer =
		graph.AddNode({.Name = core::Name("producer"), .Kind = core::Name("raster"), .Writes = {sky}});
	const auto reconstruct = graph.AddNode(
		{.Name = core::Name("weather"),
		 .Kind = core::Name("weather-reconstruct"),
		 .Reads = {sky},
		 .ReadPorts = {core::Name("colour")},
		 .Writes = {scratch, weather},
		 .WritePorts = {core::Name("resolved"), core::Name("colour")}}
	);
	const auto fog = graph.AddNode(
		{.Name = core::Name("fog"),
		 .Kind = core::Name("fog"),
		 .Reads = {weather},
		 .ReadPorts = {core::Name("colour")},
		 .Writes = {volume}}
	);
	const std::array ordered{producer, reconstruct, fog};
	const auto contracts = BuildIdentityContracts(graph, ordered);
	REQUIRE(contracts.size() == 2);
	CHECK(contracts[0].Source == sky);
	CHECK(contracts[0].Target == weather);
	CHECK(contracts[1].Source == weather);
	CHECK(contracts[1].Target == volume);
	CHECK_FALSE(IdentityCompatible(Colour("sky"), storage));

	SECTION("storage bindings cannot acquire a colour-only texture") {
		struct RestoreKind {
			graph::NodeKindSpec Original;
			~RestoreKind() {
				(void)graph::RegisterNodeKind(std::move(Original));
			}
		} restore{*graph::NodeCatalogue::Find(core::Name("dispatch"))};
		auto consumer = restore.Original;
		consumer.Inputs[0].Kind = graph::ResourceKind::Storage;
		REQUIRE(graph::RegisterNodeKind(std::move(consumer)));
		const auto reader = graph.AddNode(
			{.Name = core::Name("storage-reader"),
			 .Kind = core::Name("dispatch"),
			 .Reads = {weather},
			 .ReadPorts = {core::Name("source")}}
		);
		CHECK(BuildIdentityContracts(graph, std::array{producer, reconstruct, fog, reader}).empty());
	}
	SECTION("unknown consumers require materialized usage") {
		const auto reader = graph.AddNode(
			{.Name = core::Name("unknown-reader"),
			 .Kind = core::Name("identity-unknown-reader"),
			 .Reads = {weather},
			 .ReadPorts = {core::Name("image")}}
		);
		CHECK(BuildIdentityContracts(graph, std::array{producer, reconstruct, fog, reader}).empty());
	}
	SECTION("colour captures do not need storage usage") {
		const auto reader = graph.AddNode(
			{.Name = core::Name("colour-reader"),
			 .Kind = core::Name("identity-unknown-colour-reader"),
			 .Reads = {sky},
			 .ReadPorts = {core::Name("image")}}
		);
		CHECK(BuildIdentityContracts(graph, std::array{producer, reconstruct, fog, reader}).size() == 2);
	}
}

TEST_CASE("fixed identity publication preserves later logical alias readers", "[render][identity]") {
	graph::RenderGraph graph;
	const auto earlier = graph.AddResource(Colour("lit"));
	const auto target = graph.AddResource(Colour("volume-lit"));
	const auto unrelated = graph.AddResource(Colour("unrelated"));
	const auto before =
		graph.AddNode({.Name = core::Name("before"), .Kind = core::Name("capture"), .Reads = {earlier}});
	const auto fog =
		graph.AddNode({.Name = core::Name("fog"), .Kind = core::Name("fog"), .Writes = {target}});
	const auto reader =
		graph.AddNode({.Name = core::Name("reader"), .Kind = core::Name("capture"), .Reads = {earlier}});
	const auto aliasesOutput = [&](graph::ResourceId read) { return read == earlier || read == target; };
	const auto observed = [&](std::span<const graph::NodeId> ordered, graph::NodeId publication) {
		return render::detail::IdentityAliasReadAfter(graph, ordered, publication, target, aliasesOutput);
	};
	CHECK_FALSE(observed(std::array{before, fog}, fog));
	CHECK(observed(std::array{fog, reader}, fog));
	CHECK(observed(std::array{reader}, {}));
	const auto namedOutputReader = graph.AddNode(
		{.Name = core::Name("named-output-reader"),
		 .Kind = core::Name("capture"),
		 .Reads = {target, unrelated}}
	);
	CHECK_FALSE(observed(std::array{fog, namedOutputReader}, fog));
	REQUIRE(graph.SetEnabled(reader, false));
	CHECK_FALSE(observed(std::array{fog, reader}, fog));
}

TEST_CASE("installed common port roles govern identity reader materialization", "[render][identity]") {
	graph::RegisterRenderNodeKinds();
	struct Restore {
		graph::NodeKindSpec Original;
		~Restore() {
			(void)graph::RegisterNodeKind(std::move(Original));
		}
	} restore{*graph::NodeCatalogue::Find(core::Name("dispatch"))};
	auto spec = restore.Original;
	spec.Inputs.front().Kind = graph::ResourceKind::Texture;
	REQUIRE(graph::RegisterNodeKind(spec));
	graph::RenderGraph graph;
	const auto image = graph.AddResource(Colour("image"));
	graph::Node consumer{
		.Name = core::Name("consumer"),
		.Kind = spec.Kind,
		.Reads = {image},
		.ReadPorts = {spec.Inputs.front().Name}
	};
	graph.AddNode(consumer);
	CHECK(render::detail::IdentityReadersSampleOnly(graph, image));
	SECTION("a replacement changes the installed common role") {
		spec.Inputs.front().Kind = graph::ResourceKind::Storage;
		REQUIRE(graph::RegisterNodeKind(spec));
		CHECK_FALSE(render::detail::IdentityReadersSampleOnly(graph, image));
	}
	SECTION("a refused replacement preserves accepted alias eligibility") {
		spec.Inputs.push_back(spec.Inputs.front());
		CHECK_FALSE(graph::RegisterNodeKind(spec));
		CHECK(render::detail::IdentityReadersSampleOnly(graph, image));
	}
	SECTION("missing named bindings retain their materialization fallback") {
		graph::RenderGraph positional;
		const auto resource = positional.AddResource(Colour("positional"));
		consumer.Reads = {resource};
		consumer.ReadPorts.clear();
		positional.AddNode(consumer);
		CHECK_FALSE(render::detail::IdentityReadersSampleOnly(positional, resource));
	}
	SECTION("unregistered kinds retain their materialization fallback") {
		graph::RenderGraph unknown;
		const auto resource = unknown.AddResource(Colour("unknown"));
		consumer.Kind = core::Name("unregistered-identity-reader");
		consumer.Reads = {resource};
		unknown.AddNode(consumer);
		CHECK_FALSE(render::detail::IdentityReadersSampleOnly(unknown, resource));
	}
}
