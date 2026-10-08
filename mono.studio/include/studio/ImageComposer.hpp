#pragma once

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/imagegraph/Document.hpp>

#include <array>
#include <filesystem>
#include <functional>
#include <nodegraph/Editor.hpp>
#include <nodegraph/Graph.hpp>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace studio {
	// The canvas remains a view over a native static image document.
	void RegisterImageComposerNodeTypes();
	bool LoadImageComposerGraph(
		const engine::imagegraph::Document &document,
		nodegraph::Graph &out,
		engine::imagegraph::Diagnostic &diagnostic
	);
	bool SaveImageComposerGraph(
		const nodegraph::Graph &graph,
		std::span<const engine::imagegraph::Output> outputs,
		engine::imagegraph::Document &out,
		engine::imagegraph::Diagnostic &diagnostic
	);

	struct ImageComposerSnapshot {
		nodegraph::Graph Graph;
		std::vector<engine::imagegraph::Output> Outputs;
		std::string SelectedOutput;
		std::filesystem::path SourceRoot;
	};
	struct ImageComposerState {
		nodegraph::Graph Graph;
		nodegraph::Canvas Canvas;
		std::vector<engine::imagegraph::Output> Outputs;
		std::string SelectedOutput = "image";
		std::filesystem::path SourceRoot;
		std::unordered_map<std::string, engine::imagegraph::Image> Sources;
		size_t SourceBytes = 0;
		uint64_t SourceRevision = 1;
		uint64_t AttemptRevision = 0;
		std::string Attempt;
		engine::imagegraph::Image Preview;
		engine::assets::TextureData PreviewTexture;
		void *PreviewHandle = nullptr;
		bool Initialized = false;
		std::string Error;
		std::string EvaluationError;
		std::string Notice;
		std::array<char, 1024> ProjectPath{};
		std::array<char, 128> OutputName{"image"};
		std::array<char, 128> AssetName{"image.atex"};
		ImageComposerSnapshot Last;
		std::vector<ImageComposerSnapshot> Past;
		std::vector<ImageComposerSnapshot> Future;
		// Two stable preview names avoid growing the intern table on every edit.
		engine::core::Name PreviewOwner;
		std::array<engine::core::Name, 2> PreviewNames;
		size_t PreviewSlot = 0;
	};
	struct ImageComposerHost {
		engine::imagegraph::SourceResolver Sources;
		std::function<bool(const engine::assets::TextureData &, void *&, std::string &)> Upload;
		std::function<void(std::span<const std::byte>, const std::string &)> Register;
		std::function<void()> Publish;
		std::filesystem::path BakedRoot;
	};

	void InitialiseImageComposer(ImageComposerState &state);
	void CommitImageComposer(ImageComposerState &state);
	bool SetImageComposerOutput(ImageComposerState &state, std::string_view name, nodegraph::NodeId node);
	bool UndoImageComposer(ImageComposerState &state);
	bool RedoImageComposer(ImageComposerState &state);
	void ReloadImageComposerSources(ImageComposerState &state);
	// A refused edit keeps the last good preview; export still refuses stale pixels.
	bool RefreshImageComposer(ImageComposerState &state, const ImageComposerHost &host);
	bool OpenImageComposerProject(ImageComposerState &state, const std::filesystem::path &path);
	bool SaveImageComposerProject(ImageComposerState &state, const std::filesystem::path &path);
	bool ExportImageComposer(ImageComposerState &state, const ImageComposerHost &host, std::string_view name);
	void DrawImageComposer(ImageComposerState &state, bool &open, const ImageComposerHost &host);
}
