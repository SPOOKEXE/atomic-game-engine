#pragma once

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>
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
	// The canvas remains a view over a native image document and its named controls.
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
		std::vector<engine::imagegraph::Parameter> Parameters;
		std::vector<engine::imagegraph::Binding> Bindings;
	};
	struct ImageComposerState {
		nodegraph::Graph Graph;
		nodegraph::Canvas Canvas;
		std::vector<engine::imagegraph::Output> Outputs;
		std::string SelectedOutput = "image";
		std::vector<engine::imagegraph::Parameter> Parameters;
		std::vector<engine::imagegraph::Binding> Bindings;
		std::filesystem::path SourceRoot;
		std::unordered_map<std::string, engine::imagegraph::Image> Sources;
		size_t SourceBytes = 0;
		uint64_t SourceVersion = 0;
		std::unordered_map<std::string, uint64_t> SourceVersions;
		struct SourceUpload {
			std::string Key;
			engine::core::Name Texture;
			uint64_t Version = 0;
		};
		std::vector<SourceUpload> UploadedSources;
		uint64_t SourceRevision = 1;
		uint64_t AttemptRevision = 0;
		std::string Attempt;
		engine::imagegraph::Image Preview;
		engine::assets::TextureData PreviewTexture;
		void *PreviewHandle = nullptr;
		uint32_t PreviewWidth = 0;
		uint32_t PreviewHeight = 0;
		uint64_t PreviewRevision = 0;
		bool GpuPreview = false;
		bool Initialized = false;
		std::string Error;
		std::string EvaluationError;
		std::string Notice;
		std::array<char, 1024> ProjectPath{};
		std::array<char, 128> OutputName{"image"};
		std::array<char, 128> AssetName{"image.atex"};
		std::array<char, 128> LiveAssetName{"image.aimagegraph"};
		std::array<char, 128> InputName{"input"};
		std::string BindingProperty;
		std::string PublishedGraph;
		std::string PublishedKey;
		std::string TargetProperty;
		ImageComposerSnapshot Last;
		std::vector<ImageComposerSnapshot> Past;
		std::vector<ImageComposerSnapshot> Future;
		// Stable graph/output names avoid growing the intern table on every edit.
		engine::core::Name PreviewOwner;
		std::array<engine::core::Name, 2> PreviewNames;
		size_t PreviewSlot = 0;
	};
	struct ImageComposerPreview {
		void *Handle = nullptr;
		uint32_t Width = 0;
		uint32_t Height = 0;
	};
	struct ImageComposerHost {
		engine::imagegraph::SourceResolver Sources;
		engine::imagegraph::TypedSourceResolver TypedSources;
		// GPU hosts accept a validated resolved graph and exact cached sources without CPU readback.
		// Its selected preview output uses linear storage so ImGui preserves encoded byte channels.
		std::function<bool(
			const engine::imagegraph::Document &,
			std::string_view,
			const engine::imagegraph::TypedSourceResolver &,
			ImageComposerPreview &,
			engine::imagegraph::Diagnostic &
		)>
			Gpu;
		std::function<
			bool(const engine::imagegraph::Document &, std::string_view, std::string &, std::string &)>
			LivePublish;
		std::vector<std::string> ImageSlots;
		std::function<void(std::string_view, std::string_view, std::string_view)> Apply;
		std::function<bool(const engine::assets::TextureData &, void *&, std::string &)> Upload;
		std::function<void(std::span<const std::byte>, const std::string &)> Register;
		std::function<void()> Publish;
		std::filesystem::path BakedRoot;
	};

	bool SaveImageComposerDocument(
		const ImageComposerState &, engine::imagegraph::Document &, engine::imagegraph::Diagnostic &
	);
	bool BindImageComposerInput(
		ImageComposerState &,
		nodegraph::NodeId,
		std::string_view property,
		std::string_view input,
		engine::imagegraph::Diagnostic &
	);
	bool CanApplyLiveImageComposer(const ImageComposerState &);
	bool PublishLiveImageComposer(ImageComposerState &, const ImageComposerHost &, std::string_view name);
	struct ImageComposerWrite {
		engine::ecs::Entity Instance{};
		engine::game::PropertyValue Before{};
		engine::game::PropertyValue After{};
	};
	// Creates one persistent controller and atomically assigns its URI to eligible image slots.
	bool ApplyImageComposerSelection(
		engine::ecs::Store &,
		std::span<const engine::ecs::Entity>,
		engine::core::Name property,
		engine::core::Name graph,
		engine::core::Name output,
		engine::core::Name instanceKey,
		bool authoritative,
		engine::ecs::Entity &created,
		std::vector<ImageComposerWrite> &writes,
		std::string &failure
	);
	bool EditImageGraphInstanceInput(
		engine::ecs::Store &,
		engine::ecs::Entity,
		const engine::imagegraph::InputOverride &,
		bool reset,
		bool authoritative,
		const engine::imagegraph::Document *,
		engine::game::PropertyValue &before,
		engine::game::PropertyValue &after,
		std::string &failure
	);
	// Primitive named controls used by Composer defaults and scene instance overrides.
	bool DrawImageComposerInput(const char *label, engine::imagegraph::InputValue &value);
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
