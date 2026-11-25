// The Asset Pipeline canvas converts to and from `bake::Document` without losing
// a parameter or a wire, so a pipeline opened and saved unchanged is the same text.

#include "AssetPipelineGraph.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_SUITE_ID("studio.asset-pipeline")

namespace {
	using engine::bake::Document;
	using engine::bake::NodeKind;
	using engine::bake::OperationKind;

	Document Sample() {
		Document document;
		document.Record({.Kind = OperationKind::AddSource, .Text = "crate.obj"});
		document.Record({.Kind = OperationKind::AddNode, .Node = NodeKind::Import});
		document.Record({.Kind = OperationKind::AddFit, .Number = 2.5f});
		document.Record({.Kind = OperationKind::AddDecimate, .Number = 0.25f});
		document.Record({.Kind = OperationKind::AddWrite, .Text = "crate"});
		document.Record({.Kind = OperationKind::AddSource, .Text = "sheet.png"});
		document.Record({.Kind = OperationKind::AddNode, .Node = NodeKind::Import});
		document.Record({.Kind = OperationKind::AddFlipbook, .Number = 12.0f, .Side = 4, .Frames = 14});
		document.Record({.Kind = OperationKind::AddResize, .Width = 512, .Height = 256});
		document.Record({.Kind = OperationKind::AddWrite, .Text = "sheet"});
		for (uint32_t from : {1u, 2u, 3u, 4u, 6u, 7u, 8u, 9u})
			document.Record({.Kind = OperationKind::Connect, .From = from, .To = from + 1});
		return document;
	}
}

TEST_CASE("an asset pipeline opened and saved unchanged writes the same text", "[studio]") {
	const Document original = Sample();
	engine::ang::Graph graph;
	std::string problem;
	REQUIRE(studio::LoadAssetPipelineGraph(original, graph, problem));
	CHECK(graph.Nodes().size() == 10);
	CHECK(graph.Links().size() == 8);

	Document saved;
	REQUIRE(studio::SaveAssetPipelineGraph(graph, saved, problem));
	CHECK(engine::bake::Write(saved) == engine::bake::Write(original));
}

TEST_CASE("a loaded asset pipeline lays each chain out left to right", "[studio]") {
	engine::ang::Graph graph;
	std::string problem;
	REQUIRE(studio::LoadAssetPipelineGraph(Sample(), graph, problem));
	const auto &nodes = graph.Nodes();
	// Each chain's steps advance one column; the two chains take separate rows.
	CHECK(nodes[0].Position.X < nodes[1].Position.X);
	CHECK(nodes[1].Position.X < nodes[4].Position.X);
	CHECK(nodes[0].Position.Y != nodes[5].Position.Y);
	CHECK(nodes[0].Position.X == nodes[5].Position.X);
}

TEST_CASE("an asset pipeline wire to a missing node is refused on load", "[studio]") {
	Document document;
	document.Record({.Kind = OperationKind::AddSource, .Text = "a.png"});
	document.Record({.Kind = OperationKind::Connect, .From = 1, .To = 2});
	engine::ang::Graph graph;
	std::string problem;
	CHECK_FALSE(studio::LoadAssetPipelineGraph(document, graph, problem));
	CHECK_FALSE(problem.empty());
}

TEST_CASE("an asset pipeline wire between mismatched kinds is dropped and reported", "[studio]") {
	// A texture step fed a mesh: the canvas port types refuse it.
	Document document;
	document.Record({.Kind = OperationKind::AddBuiltin, .Text = "cube"});
	document.Record({.Kind = OperationKind::AddNode, .Node = NodeKind::Opaque});
	document.Record({.Kind = OperationKind::Connect, .From = 1, .To = 2});
	engine::ang::Graph graph;
	std::string problem;
	CHECK_FALSE(studio::LoadAssetPipelineGraph(document, graph, problem));
	CHECK(graph.Nodes().size() == 2);
	CHECK(graph.Links().empty());
}

TEST_CASE("an asset pipeline bakes a raw file from disk into the baked folder", "[studio]") {
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "studio-asset-pipeline-bake";
	fs::remove_all(root);
	fs::create_directories(root / "raw");
	{
		std::ofstream obj(root / "raw" / "tri.obj");
		obj << "v 0 0 0\nv 4 0 0\nv 0 4 0\nf 1 2 3\n";
	}

	// Built on the canvas and saved, the way the panel's Bake button does it.
	engine::ang::Graph graph;
	std::string problem;
	Document chain;
	chain.Record({.Kind = OperationKind::AddSource, .Text = "tri.obj"});
	chain.Record({.Kind = OperationKind::AddNode, .Node = NodeKind::Import});
	chain.Record({.Kind = OperationKind::AddFit, .Number = 1.0f});
	chain.Record({.Kind = OperationKind::AddWrite, .Text = "tri"});
	for (uint32_t from : {1u, 2u, 3u})
		chain.Record({.Kind = OperationKind::Connect, .From = from, .To = from + 1});
	REQUIRE(studio::LoadAssetPipelineGraph(chain, graph, problem));
	Document saved;
	REQUIRE(studio::SaveAssetPipelineGraph(graph, saved, problem));

	std::vector<engine::bake::BakedAsset> baked;
	std::string status;
	REQUIRE(studio::BakeAssetPipeline(saved, root / "raw", root / "baked", baked, status));
	REQUIRE(baked.size() == 1);
	CHECK(baked[0].Kind == engine::assets::AssetKind::Mesh);
	CHECK(fs::file_size(root / "baked" / baked[0].Name) == baked[0].Bytes.size());

	// A missing raw file is refused by name rather than baking nothing.
	Document missing;
	missing.Record({.Kind = OperationKind::AddSource, .Text = "absent.obj"});
	CHECK_FALSE(studio::BakeAssetPipeline(missing, root / "raw", root / "baked", baked, status));
	CHECK(status.find("absent.obj") != std::string::npos);
	fs::remove_all(root);
}
