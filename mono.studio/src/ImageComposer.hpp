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
	// Registers the image and source nodes used by the composer canvas.
	void RegisterImageComposerNodeTypes();
	// Builds an editor canvas from a validated image graph document.
	bool LoadImageComposerGraph(
		const engine::imagegraph::Document &document,
		nodegraph::Graph &out,
		engine::imagegraph::Diagnostic &diagnostic
	);
	// Compiles an editor canvas and its outputs into a validated graph document.
	bool SaveImageComposerGraph(
		const nodegraph::Graph &graph,
		std::span<const engine::imagegraph::Output> outputs,
		engine::imagegraph::Document &out,
		engine::imagegraph::Diagnostic &diagnostic
	);

	// Undoable snapshot of the authored graph and its document controls.
	struct ImageComposerSnapshot {
		// Canvas graph at the time of the snapshot.
		nodegraph::Graph Graph;
		// Named image outputs authored in the document.
		std::vector<engine::imagegraph::Output> Outputs;
		// Output selected by the composer.
		std::string SelectedOutput;
		// Base directory used to resolve source paths.
		std::filesystem::path SourceRoot;
		// Named parameters and their defaults.
		std::vector<engine::imagegraph::Parameter> Parameters;
		// Links from graph nodes to named parameters.
		std::vector<engine::imagegraph::Binding> Bindings;
	};
	// Mutable authoring, source, preview, and history state for the composer.
	struct ImageComposerState {
		// Editable node graph.
		nodegraph::Graph Graph;
		// Selection and view state for the canvas.
		nodegraph::Canvas Canvas;
		// Outputs available for selection and publishing.
		std::vector<engine::imagegraph::Output> Outputs;
		// Currently selected output name.
		std::string SelectedOutput = "image";
		// Named document parameters and their defaults.
		std::vector<engine::imagegraph::Parameter> Parameters;
		// Parameter bindings attached to graph nodes.
		std::vector<engine::imagegraph::Binding> Bindings;
		// Root used to resolve relative source paths.
		std::filesystem::path SourceRoot;
		// Decoded source images currently available to evaluation.
		std::unordered_map<std::string, engine::imagegraph::Image> Sources;
		// Total decoded source bytes currently held.
		size_t SourceBytes = 0;
		// Version of the current source set.
		uint64_t SourceVersion = 0;
		// Most recently observed version for each source key.
		std::unordered_map<std::string, uint64_t> SourceVersions;
		// GPU texture upload tracked for one source version.
		struct SourceUpload {
			// Stable key identifying the source image.
			std::string Key;
			// Registered texture name used by graph evaluation.
			engine::core::Name Texture;
			// Source version represented by the uploaded texture.
			uint64_t Version = 0;
		};
		// Uploaded sources retained for the current graph preview.
		std::vector<SourceUpload> UploadedSources;
		// World supplying local editable image sources, when active.
		engine::core::Name LocalSourceWorld;
		// Version of the local editable image source set.
		uint64_t LocalSourceVersion = 0;
		// Revision of source content used to validate live publication.
		uint64_t SourceRevision = 1;
		// Source revision attached to the last evaluation attempt.
		uint64_t AttemptRevision = 0;
		// Key describing the graph and inputs of the last attempt.
		std::string Attempt;
		// CPU-evaluated preview image.
		engine::imagegraph::Image Preview;
		// Encoded texture prepared for display or export.
		engine::assets::TextureData PreviewTexture;
		// Host texture handle used to display the preview.
		void *PreviewHandle = nullptr;
		// Width of the displayed preview texture.
		uint32_t PreviewWidth = 0;
		// Height of the displayed preview texture.
		uint32_t PreviewHeight = 0;
		// Graph revision represented by the current preview.
		uint64_t PreviewRevision = 0;
		// True when the displayed preview came from the GPU evaluator.
		bool GpuPreview = false;
		// True after the default graph and controls have been initialized.
		bool Initialized = false;
		// Most recent authoring or export failure.
		std::string Error;
		// Most recent graph evaluation failure.
		std::string EvaluationError;
		// Non-failure status shown by the composer.
		std::string Notice;
		// Path buffer for the currently open project.
		std::array<char, 1024> ProjectPath{};
		// Editable name for the selected output.
		std::array<char, 128> OutputName{"image"};
		// Editable name for a baked texture asset.
		std::array<char, 128> AssetName{"image.atex"};
		// Editable name for a live graph asset.
		std::array<char, 128> LiveAssetName{"image.aimagegraph"};
		// Editable name for a graph input parameter.
		std::array<char, 128> InputName{"input"};
		// Property selected for parameter binding.
		std::string BindingProperty;
		// Name of the graph most recently published live.
		std::string PublishedGraph;
		// Fingerprint of the graph and sources used by the last live publication.
		std::string PublishedKey;
		// Image property selected for applying the live graph.
		std::string TargetProperty;
		// Most recent undo snapshot.
		ImageComposerSnapshot Last;
		// Snapshots available for undo.
		std::vector<ImageComposerSnapshot> Past;
		// Snapshots available for redo.
		std::vector<ImageComposerSnapshot> Future;
		// Stable graph/output names avoid growing the intern table on every edit.
		engine::core::Name PreviewOwner;
		// Alternating names used to retain two preview textures.
		std::array<engine::core::Name, 2> PreviewNames;
		// Preview texture currently selected for display.
		size_t PreviewSlot = 0;
	};
	// Borrowed host texture and dimensions for the latest rendered preview.
	struct ImageComposerPreview {
		// Host-owned texture handle consumed by the preview UI.
		void *Handle = nullptr;
		// Width of the host texture in pixels.
		uint32_t Width = 0;
		// Height of the host texture in pixels.
		uint32_t Height = 0;
	};
	// Callbacks and asset locations supplied by the Studio host.
	struct ImageComposerHost {
		// Resolves source paths to decoded images.
		engine::imagegraph::SourceResolver Sources;
		// Resolves typed graph sources, including non-file inputs.
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
		// Publishes the current graph document under a live asset name.
		std::function<
			bool(const engine::imagegraph::Document &, std::string_view, std::string &, std::string &)>
			LivePublish;
		// Image properties available as live graph targets.
		std::vector<std::string> ImageSlots;
		// Applies a graph reference to one image property on selected instances.
		std::function<void(std::string_view, std::string_view, std::string_view)> Apply;
		// Uploads texture data and returns its host handle.
		std::function<bool(const engine::assets::TextureData &, void *&, std::string &)> Upload;
		// Registers baked texture bytes under the supplied asset name.
		std::function<void(std::span<const std::byte>, const std::string &)> Register;
		// Refreshes the host after published assets change.
		std::function<void()> Publish;
		// Directory receiving baked texture assets.
		std::filesystem::path BakedRoot;
	};

	// Copies the current authoring state into an image graph document.
	bool SaveImageComposerDocument(
		const ImageComposerState &, engine::imagegraph::Document &, engine::imagegraph::Diagnostic &
	);
	// Binds a node property to a named document input.
	bool BindImageComposerInput(
		ImageComposerState &,
		nodegraph::NodeId,
		std::string_view property,
		std::string_view input,
		engine::imagegraph::Diagnostic &
	);
	// Reports whether the last live publication still matches the current edits.
	bool CanApplyLiveImageComposer(const ImageComposerState &);
	// Publishes the current graph and records its live asset identity.
	bool PublishLiveImageComposer(ImageComposerState &, const ImageComposerHost &, std::string_view name);
	// Property change recorded while applying a graph to selected instances.
	struct ImageComposerWrite {
		// Instance whose image property was changed.
		engine::ecs::Entity Instance{};
		// Property value before the assignment.
		engine::game::PropertyValue Before{};
		// Property value written by the assignment.
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
	// Applies or resets one typed input override on a live graph instance.
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
	// Draws the editor for one supported input value and reports whether it changed.
	bool DrawImageComposerInput(const char *label, engine::imagegraph::InputValue &value);
	// Creates the default graph and initializes authoring history.
	void InitialiseImageComposer(ImageComposerState &state);
	// Records the current authoring state as the latest undo point.
	void CommitImageComposer(ImageComposerState &state);
	// Adds or updates an output and maps it to a canvas node.
	bool SetImageComposerOutput(ImageComposerState &state, std::string_view name, nodegraph::NodeId node);
	// Restores the previous committed authoring state when available.
	bool UndoImageComposer(ImageComposerState &state);
	// Reapplies the next authoring state when available.
	bool RedoImageComposer(ImageComposerState &state);
	// Clears cached sources so the next evaluation loads them again.
	void ReloadImageComposerSources(ImageComposerState &state);
	// A refused edit keeps the last good preview; export still refuses stale pixels.
	// Evaluates the current graph and updates the preview using the available host path.
	bool RefreshImageComposer(ImageComposerState &state, const ImageComposerHost &host);
	// Opens and decodes an image graph project from disk.
	bool OpenImageComposerProject(ImageComposerState &state, const std::filesystem::path &path);
	// Writes the current image graph project to disk.
	bool SaveImageComposerProject(ImageComposerState &state, const std::filesystem::path &path);
	// Bakes the selected output to a texture asset and registers it with the host.
	bool ExportImageComposer(ImageComposerState &state, const ImageComposerHost &host, std::string_view name);
	// Draws the image graph editor and updates its open state.
	void DrawImageComposer(ImageComposerState &state, bool &open, const ImageComposerHost &host);
}
