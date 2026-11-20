#pragma once

// The asset pipeline editor's canvas vocabulary, and the two conversions between
// a canvas graph and a `bake::Document`.
//
// The document stays the saved form: a world's `bake::PipelineSet` holds edit
// lists, and the canvas is rebuilt from one when a pipeline is opened. Node
// positions are not part of the format, so a loaded pipeline is laid out by
// chain (one column per step from its input).

#include <engine/bake/Graph.hpp>
#include <engine/bakegraph/Document.hpp>

#include <filesystem>
#include <nodegraph/Graph.hpp>
#include <string>
#include <vector>

namespace studio {
	// Category every asset pipeline node type is registered under. The canvas
	// add menu filters on it so render and composer nodes stay out.
	inline constexpr const char *ASSET_PIPELINE_CATEGORY = "Asset Pipeline";

	// Registers one canvas node type per `bake::NodeKind`, with ports taken from
	// `bake::NodeSchemas`. Safe to call every frame; the second call does nothing.
	void RegisterAssetPipelineNodeTypes();

	// Rebuilds `graph` from a document. Clears it first.
	//
	// @return `false` with `problem` set when the document names a node position
	//         it does not hold, or a kind the canvas has no type for.
	bool LoadAssetPipelineGraph(
		const engine::bake::Document &document, nodegraph::Graph &graph, std::string &problem
	);

	// Writes `graph` as a document: every node operation, then every wire.
	//
	// @return `false` with `problem` set when the canvas holds a node that is not
	//         an asset pipeline node (a compressed group, for example).
	bool SaveAssetPipelineGraph(
		const nodegraph::Graph &graph, engine::bake::Document &document, std::string &problem
	);

	// Runs a pipeline: each source name is a file under `rawRoot`, and every
	// `Write` lands as a file under `bakedRoot`.
	//
	// @param baked   Filled with what was written, for the caller to register.
	// @param status  Why it failed, or a one-line summary on success.
	// @return `false` when the document would not build or run, or a file
	//         could not be written.
	bool BakeAssetPipeline(
		const engine::bake::Document &document,
		const std::filesystem::path &rawRoot,
		const std::filesystem::path &bakedRoot,
		std::vector<engine::bake::BakedAsset> &baked,
		std::string &status
	);
}
