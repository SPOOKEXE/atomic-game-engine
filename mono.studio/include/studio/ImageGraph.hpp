#pragma once

// The Studio adapter between durable imagegraph documents and nodegraph's
// session-local canvas model.

#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/scene/ImageGraphBinding.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <nodegraph/Graph.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace studio {

	inline constexpr uint32_t IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION = 128;
	// Maximum input bytes for one cached image at RGBA32 float precision.
	inline constexpr size_t IMAGE_COMPOSER_PREVIEW_SURFACE_BYTES_PER_PIXEL = 16;
	inline constexpr size_t IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES =
		static_cast<size_t>(IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION) *
		IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION * IMAGE_COMPOSER_PREVIEW_SURFACE_BYTES_PER_PIXEL;
	// Maximum bytes for the RGBA8 texture uploaded for display.
	inline constexpr size_t IMAGE_COMPOSER_PREVIEW_DISPLAY_MAXIMUM_BYTES =
		static_cast<size_t>(IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION) *
		IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION * 4;
	inline constexpr size_t IMAGE_GRAPH_FILE_MAXIMUM_BYTES = 8 * 1024 * 1024;
	inline constexpr size_t IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES = 8;
	// Retained image capacity is bounded by eight maximum typed surfaces.
	inline constexpr size_t IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES =
		IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES * IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES;
	// One typed candidate may overlap the retained cache during replacement.
	inline constexpr size_t IMAGE_COMPOSER_PREVIEW_CACHE_PEAK_BYTES =
		(IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES + 1) * IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES;

	// Editor playback advances a bounded fixed-tick interval and can seek within a
	// frame for preview evaluation.
	struct ImageGraphPlayback {
		bool Playing = false;
		bool Loop = true;
		bool PingPong = false;
		uint64_t CurrentTick = 0;
		double Subframe = 0.0;
		uint64_t StartTick = 0;
		uint64_t EndTick = 240;
		uint64_t TotalFrames = 241;
		double FramesPerSecond = 30.0;
		double Accumulator = 0.0;
		int8_t Direction = 1;
		bool NegativeFrame = false;
		bool Rendering = false;
		bool Simulating = false;
		bool FrameProgress = false;
		double RealFrame = 0.0;
		double LastTime = 0.0;
		double RealTime = 0.0;
	};

	engine::imagegraph::FrameTime GetImageGraphFrame(const ImageGraphPlayback &playback);
	// Seeks an authoring clock, independently of the bounded playback range.
	bool SetImageGraphAuthorFrame(ImageGraphPlayback &playback, engine::imagegraph::FrameTime frame);
	bool SeekImageGraphAuthorFrame(ImageGraphPlayback &playback, double frame, bool control, bool alt);

	// Loads durable timeline fields into the editor playback cursor.
	void ApplyImageGraphTimeline(const engine::imagegraph::Document &document, ImageGraphPlayback &playback);

	// Sets an unrounded frame position inside the current playback range.
	// @return Whether the frame cursor changed.
	bool SetImageGraphPlaybackFrame(ImageGraphPlayback &playback, double frame);

	// Advances fixed-tick playback with capped wall-time catch-up and reports cursor changes.
	bool AdvanceImageGraphPlayback(ImageGraphPlayback &playback, double elapsedSeconds);

	// A small LRU of successful, bounded preview images keyed by authored revision,
	// selected output, signed fractional frame and rigid playback observations. Failed evaluations
	// never replace a good frame. Observation bits are rigid Playing=1, rigid FrameProgress=2 and
	// source-cache Playing=4.
	class ImageGraphPreviewCache {
	  public:
		const engine::imagegraph::Image *Find(
			uint64_t revision,
			size_t outputIndex,
			uint64_t tick,
			double subframe = 0.0,
			bool negativeFrame = false,
			uint8_t rigidObservation = 0
		);
		bool Store(
			uint64_t revision,
			size_t outputIndex,
			uint64_t tick,
			const engine::imagegraph::Image &image,
			double subframe = 0.0,
			bool negativeFrame = false,
			uint8_t rigidObservation = 0
		);
		// Invalidates one derived output across retained frames, preserving unrelated previews.
		void InvalidateOutput(size_t outputIndex);
		void Clear();
		size_t HeldBytes() const;
		// Includes retained entry metadata and pixel capacities.
		uint64_t RetainedBytes() const noexcept;

	  private:
		struct Entry {
			uint64_t Revision = 0;
			size_t OutputIndex = 0;
			uint64_t Tick = 0;
			double Subframe = 0.0;
			bool NegativeFrame = false;
			uint8_t RigidObservation = 0;
			uint64_t LastUsed = 0;
			engine::imagegraph::Image Image;
		};
		std::vector<Entry> Entries;
		uint64_t UseSerial = 0;
	};

	// Recorded capture loading shares the successful-input cache invalidation used by WAV sources.
	bool LoadImageGraphAudioCapture(
		std::vector<engine::imagegraph::AudioCaptureFrame> &frames,
		ImageGraphPreviewCache &cache,
		const std::filesystem::path &filePath,
		engine::imagegraph::Diagnostic &diagnostic
	);
	void ClearImageGraphAudioCapture(
		std::vector<engine::imagegraph::AudioCaptureFrame> &frames, ImageGraphPreviewCache &cache
	);

	// Explicit host inputs remain outside the authored document and its undo history.
	// Successful replacement clears cached images; failure preserves both clips and cache.
	// Stages a replacement while the old source remains resident; committing is the host's decision.
	bool ReadImageGraphWavSource(
		std::span<const engine::imagegraph::AudioClipSource> sources,
		std::string_view sourceId,
		const std::filesystem::path &filePath,
		engine::imagegraph::AudioClipSource &loaded,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t byteBudget = engine::imagegraph::Limits::MaximumEvaluationBytes
	);

	bool LoadImageGraphWavSource(
		std::vector<engine::imagegraph::AudioClipSource> &sources,
		ImageGraphPreviewCache &cache,
		std::string_view sourceId,
		const std::filesystem::path &filePath,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t byteBudget = engine::imagegraph::Limits::MaximumEvaluationBytes
	);
	bool RemoveImageGraphWavSource(
		std::vector<engine::imagegraph::AudioClipSource> &sources,
		ImageGraphPreviewCache &cache,
		std::string_view sourceId
	);

	// Keeps durable document names separate from nodegraph's numeric handles.
	struct ImageGraphCanvasIds {
		std::unordered_map<std::string, nodegraph::NodeId> ToCanvas;
		std::unordered_map<nodegraph::NodeId, std::string> ToDocument;
		std::unordered_map<std::string, nodegraph::GroupId> GroupsToCanvas;
		std::unordered_map<nodegraph::GroupId, std::string> GroupsToDocument;
		std::unordered_map<std::string, engine::imagegraph::Vector2> OriginalPositions;
		std::unordered_set<std::string> EmptyGroups;
		std::unordered_set<std::string> IssuedNodeIds;
		std::unordered_set<std::string> IssuedGroupIds;
		std::vector<engine::imagegraph::Link> UnmappedLinks;
		uint64_t NextNodeId = 1;
		uint64_t NextGroupId = 1;
	};

	// Registers current imagegraph schemas and their typed canvas ports.
	void RegisterImageGraphNodeTypes();

	// Returns the same authored default for palette creation and missing inspector values.
	// @param nodeType Durable node type identifier.
	// @param property Durable property identifier.
	// @return An explicit default when the schema property has an editable value.
	std::optional<engine::imagegraph::Value>
	ImageGraphPropertyDefault(std::string_view nodeType, std::string_view property);

	// Materializes source defaults that clone project values when a node is created.
	std::optional<engine::imagegraph::Value> ImageGraphPropertyDefault(
		const engine::imagegraph::Document &document, std::string_view nodeType, std::string_view property
	);

	// Rebuilds the canvas with an explicit durable-text to local-id map.
	// @param document Authored graph to load without changing its durable ids.
	// @param graph    Canvas model to replace.
	// @param ids      Durable and canvas id mapping for the loaded graph.
	// @param error    Receives a reason when the document exceeds canvas limits.
	bool LoadImageGraphCanvas(
		const engine::imagegraph::Document &document,
		nodegraph::Graph &graph,
		ImageGraphCanvasIds &ids,
		std::string &error
	);

	// Copies canvas layout, links and groups back while retaining values, outputs
	// and keyframes from the authored document.
	// @param graph    Edited canvas model.
	// @param basis    Source document whose non-canvas fields are retained.
	// @param ids      Durable and canvas id mapping, updated for new graph items.
	// @param document Receives the reconstructed authored document.
	// @param error    Receives a reason when the document exceeds authored limits.
	bool SaveImageGraphCanvas(
		const nodegraph::Graph &graph,
		const engine::imagegraph::Document &basis,
		ImageGraphCanvasIds &ids,
		engine::imagegraph::Document &document,
		std::string &error
	);

	// Refuses CPU preview inputs that exceed the Studio texture budget.
	// @param document Authored graph to inspect.
	// @param diagnostic Receives the first Solid dimension outside the budget.
	// @return Whether every Solid node fits the bounded RGBA8 preview texture.
	bool CheckImageComposerPreviewBudget(
		const engine::imagegraph::Document &document, engine::imagegraph::Diagnostic &diagnostic
	);

	using ImageGraphPreviewValue = std::variant<
		engine::imagegraph::Image,
		engine::imagegraph::EvaluatedValue,
		engine::imagegraph::ImageArray>;

	// Evaluates a selected image, image sequence or typed value with one explicit fixed request.
	engine::imagegraph::Status EvaluateImageGraphPreview(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		std::string_view outputId,
		const engine::imagegraph::EvaluationRequest &request,
		ImageGraphPreviewValue &preview,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);

	// Checks finite numeric samples and bounds before a flat or per-channel array enters the UI.
	bool CheckImageGraphArrayPreview(
		const engine::imagegraph::ArrayValue &array, engine::imagegraph::Diagnostic &diagnostic
	);

	// Writes a schema-typed inspector value into one durable node property.
	// @param document Authored graph to update.
	// @param nodeId   Durable target node identifier.
	// @param property Durable schema property identifier.
	// @param value    Value to author.
	// @param error    Receives a diagnostic when the edit cannot be applied.
	bool SetImageGraphValue(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		std::string_view property,
		engine::imagegraph::Value value,
		engine::imagegraph::Diagnostic &error
	);

	// Applies a complete validated project override without changing the graph on refusal.
	bool SetImageGraphProjectSettings(
		engine::imagegraph::Document &document,
		const engine::imagegraph::ProjectSettings &settings,
		engine::imagegraph::Diagnostic &error
	);

	// Restores fresh-project inheritance without downgrading the document format.
	void RemoveImageGraphProjectSettings(engine::imagegraph::Document &document);

	// Adds or replaces one instance input on a schema that exposes dynamic inputs.
	bool SetImageGraphDynamicInput(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		engine::imagegraph::DynamicInput input,
		engine::imagegraph::Diagnostic &error
	);

	// Resizes complete pinned source input groups and removes all authored state for deleted sockets.
	bool SetSourceImageGraphDynamicGroupCount(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		size_t groups,
		engine::imagegraph::Diagnostic &error
	);

	// Removes a dynamic input and its incoming links.
	bool RemoveImageGraphDynamicInput(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		std::string_view inputId,
		engine::imagegraph::Diagnostic &error
	);

	// Authors Array Split's complete output count and prunes removed routes and selectors atomically.
	bool SetImageGraphSplitOutputCount(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		size_t count,
		engine::imagegraph::Diagnostic &error,
		bool authorMinimum = true
	);

	// Adds the source constructor projection of a PXC boundary, including its control and routes.
	bool AddSourceImageGraphGroupPort(
		engine::imagegraph::Document &document,
		std::string_view groupId,
		std::string_view controlId,
		engine::imagegraph::PortDirection direction,
		engine::imagegraph::Diagnostic &error
	);

	// Adds one nested canvas group with a validated optional parent.
	bool AddImageGraphGroup(
		engine::imagegraph::Document &document,
		std::string groupId,
		std::string name,
		std::string_view parentId,
		engine::imagegraph::Diagnostic &error
	);

	// Adds a typed group socket and its durable junction record.
	bool AddImageGraphGroupPort(
		engine::imagegraph::Document &document,
		std::string_view groupId,
		engine::imagegraph::GroupPort port,
		engine::imagegraph::Junction junction,
		engine::imagegraph::Diagnostic &error
	);

	// Adds one typed route between node or junction endpoints.
	bool AddImageGraphRoute(
		engine::imagegraph::Document &document,
		engine::imagegraph::Link route,
		engine::imagegraph::Diagnostic &error
	);

	// Removes a route by declaration index.
	bool RemoveImageGraphRoute(
		engine::imagegraph::Document &document, size_t index, engine::imagegraph::Diagnostic &error
	);

	// Adds or replaces the current authored property value at one timeline tick.
	// @param document    Authored graph to update.
	// @param nodeId      Durable target node identifier.
	// @param property    Durable schema property identifier.
	// @param tick        Fixed authored timeline tick.
	// @param interpolation Interpolation rule stored with the key: step, linear, cubic or source.
	// @param error       Receives a diagnostic when the key cannot be authored.
	bool SetImageGraphKeyframe(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		std::string_view property,
		uint64_t tick,
		std::string_view interpolation,
		engine::imagegraph::Diagnostic &error,
		double subframe = 0,
		bool negativeFrame = false
	);

	// Removes one key at the given tick.
	// @param document Authored graph to update.
	// @param nodeId   Durable target node identifier.
	// @param property Durable schema property identifier.
	// @param tick     Fixed authored timeline tick.
	// @param error    Receives a diagnostic when the target property is invalid.
	// @return Whether a matching key was removed.
	bool RemoveImageGraphKeyframe(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		std::string_view property,
		uint64_t tick,
		engine::imagegraph::Diagnostic &error,
		double subframe = 0,
		bool negativeFrame = false
	);

	// Authors the retained source marker without changing interpolation or arithmetic.
	bool SetImageGraphKeyframeKind(
		engine::imagegraph::Document &document,
		size_t index,
		engine::imagegraph::KeyframeKind kind,
		engine::imagegraph::Diagnostic &error
	);

	struct ImageGraphKeyframeIdentity {
		std::string NodeId, Port;
		engine::imagegraph::FrameTime Time;
		bool operator==(const ImageGraphKeyframeIdentity &) const = default;
	};

	// Captures bounded key payloads by durable property and exact authored time.
	bool CaptureImageGraphKeyframes(
		const engine::imagegraph::Document &document,
		std::span<const ImageGraphKeyframeIdentity> selection,
		std::vector<engine::imagegraph::Keyframe> &result,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);

	// Moves pinned originals, or pastes driverless clones, with source nonnegative collision policy.
	// A move refuses changed originals; the clipboard is an independent bounded snapshot.
	bool TransferImageGraphKeyframes(
		engine::imagegraph::Document &document,
		std::span<const engine::imagegraph::Keyframe> originals,
		const engine::imagegraph::FrameTime &oldAnchor,
		const engine::imagegraph::FrameTime &newAnchor,
		bool copy,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);

	// Applies exact per-key destination clocks with the same collision and pinned-original policy.
	// Budget covers logical key payloads, separately from non-key document data and host history.
	bool RetimeImageGraphKeyframes(
		engine::imagegraph::Document &document,
		std::span<const engine::imagegraph::Keyframe> originals,
		std::span<const engine::imagegraph::FrameTime> destinations,
		bool copy,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);

	// Targeted source paste maps one property directly, or multiple properties by source display name.
	bool PasteImageGraphKeyframesToProperty(
		engine::imagegraph::Document &document,
		std::span<const engine::imagegraph::Keyframe> clipboard,
		const engine::imagegraph::FrameTime &cursor,
		std::string_view nodeId,
		std::string_view property,
		engine::imagegraph::Diagnostic &error,
		uint64_t availableBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	);

	// Changes one saved interpolation rule.
	// @param document Authored graph to update.
	// @param index    Keyframe declaration index.
	// @param rule     Supported rule: step, linear, cubic or source.
	// @param error    Receives a diagnostic when the index or rule is invalid.
	bool SetImageGraphKeyframeInterpolation(
		engine::imagegraph::Document &document,
		size_t index,
		std::string_view rule,
		engine::imagegraph::Diagnostic &error
	);

	// Authors source-side ease handles for one key and keeps its track's interpolation compatible.
	// @param document Authored graph to update.
	// @param index    Keyframe declaration index.
	// @param ease     Incoming and outgoing side types and handles.
	// @param error    Receives a diagnostic when the index or ease data is invalid.
	bool SetImageGraphKeyframeEase(
		engine::imagegraph::Document &document,
		size_t index,
		engine::imagegraph::KeyframeEase ease,
		engine::imagegraph::Diagnostic &error
	);

	// Source controls are checked by the core validator before a transactional document edit.
	bool SetImageGraphKeyframeSourceDriver(
		engine::imagegraph::Document &document,
		size_t index,
		const std::optional<engine::imagegraph::KeyframeSourceDriver> &driver,
		engine::imagegraph::Diagnostic &error
	);
	// Authors an explicit source quaternion interpretation without converting the stored tuples.
	bool SetImageGraphTrackQuaternionMode(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		std::string_view property,
		std::optional<int64_t> mode,
		engine::imagegraph::Diagnostic &error
	);

	// Adds or removes a bounded scalar sine driver on one keyframe.
	// @param document Authored graph to update.
	// @param index    Keyframe declaration index.
	// @param driver   Sine parameters, or no value to remove the driver.
	// @param error    Receives a diagnostic when the index or parameters are invalid.
	bool SetImageGraphKeyframeSineDriver(
		engine::imagegraph::Document &document,
		size_t index,
		std::optional<engine::imagegraph::KeyframeSineDriver> driver,
		engine::imagegraph::Diagnostic &error
	);

	// Stores a checked v4 timeline range and playback end mode.
	bool SetImageGraphTimeline(
		engine::imagegraph::Document &document,
		engine::imagegraph::TimelineSettings timeline,
		engine::imagegraph::Diagnostic &error
	);

	// Removes optional timeline metadata unless a wrap track still needs its frame count.
	bool
	RemoveImageGraphTimeline(engine::imagegraph::Document &document, engine::imagegraph::Diagnostic &error);

	// Adds or replaces the end and loop-tail policy for one keyed property.
	bool SetImageGraphAnimationTrack(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		std::string_view property,
		std::string end,
		int64_t loopRange,
		engine::imagegraph::Diagnostic &error
	);

	// Removes an authored end and loop-tail override for one keyed property.
	bool RemoveImageGraphAnimationTrack(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		std::string_view property,
		engine::imagegraph::Diagnostic &error
	);

	// Selects an existing preview output's source by durable node and port ids.
	// @param document Authored graph to update.
	// @param outputId Durable output selector.
	// @param nodeId   Durable source node identifier.
	// @param port     Durable image, scalar or array output port identifier.
	// @param error    Receives a diagnostic when the binding is invalid.
	bool SetImageGraphOutput(
		engine::imagegraph::Document &document,
		std::string_view outputId,
		std::string_view nodeId,
		std::string_view port,
		engine::imagegraph::Diagnostic &error
	);

	std::optional<engine::imagegraph::FrameTime>
	PreviousImageGraphKey(const engine::imagegraph::Document &document, engine::imagegraph::FrameTime frame);
	std::optional<engine::imagegraph::FrameTime>
	NextImageGraphKey(const engine::imagegraph::Document &document, engine::imagegraph::FrameTime frame);
	std::optional<uint64_t>
	PreviousImageGraphKey(const engine::imagegraph::Document &document, uint64_t tick);
	std::optional<uint64_t> NextImageGraphKey(const engine::imagegraph::Document &document, uint64_t tick);

	// One durable output row shown by the asset and sink panel shell.
	struct ImageComposerSinkRow {
		std::string OutputId;
		std::string NodeId;
		std::string Port;
		bool Selected = false;
		bool TargetExists = false;
		bool operator==(const ImageComposerSinkRow &) const = default;
	};

	// Describes authored output bindings for the preview sink panel.
	// @param document      Authored graph to inspect.
	// @param selectedOutput Output selected by the current Studio session.
	// @return Rows in their saved declaration order.
	std::vector<ImageComposerSinkRow>
	DescribeImageComposerSinks(const engine::imagegraph::Document &document, std::string_view selectedOutput);

	// Whether a native graph name is one safe ASCII filename segment.
	// @param name User-visible graph name without a suffix.
	// @return True for 1..255 ASCII letters, digits, underscores or hyphens.
	bool IsSafeImageGraphName(std::string_view name);

	// Resolves the native graph name through the shared imagegraphs directory.
	// @param assetsDirectory Program asset root.
	// @param name            One safe graph name without an extension.
	// @return The document path, or an empty path for an unsafe name.
	std::filesystem::path
	ImageGraphDocumentPath(const std::filesystem::path &assetsDirectory, std::string_view name);

	// Reads and parses one bounded native graph document without partial output.
	// @param assetsDirectory Program asset root.
	// @param name            One safe graph name without an extension.
	// @param document        Receives the parsed graph only on success.
	// @param error           Receives a readable refusal reason.
	// @return Whether the file is within the native document cap and parses.
	bool ReadImageGraphDocument(
		const std::filesystem::path &assetsDirectory,
		std::string_view name,
		engine::imagegraph::Document &document,
		std::string &error
	);

	// The durable selectors and evaluation controls entered for one scene sink.
	struct ImageGraphBindingDraft {
		std::string Graph;
		std::string Output;
		std::string Texture;
		uint64_t Seed = 0;
		uint64_t FixedTick = 0;
		engine::scene::ImageGraphTickPolicy TickPolicy = engine::scene::ImageGraphTickPolicy::Fixed;
		engine::scene::ImageGraphColorSpace ColorSpace = engine::scene::ImageGraphColorSpace::Display;
	};

	// Validates a form against one loaded native document and builds its ECS row.
	// @param draft     User-entered selectors and evaluation controls.
	// @param document  Parsed native image graph document.
	// @param binding   Receives the validated component.
	// @param error     Receives a readable refusal reason.
	// @return Whether the selectors refer to a safe graph and an existing output.
	bool BuildImageGraphBinding(
		const ImageGraphBindingDraft &draft,
		const engine::imagegraph::Document &document,
		engine::scene::ImageGraphBinding &binding,
		std::string &error
	);

	// A canvas-only view of a parsed PXCX project. The archive remains the
	// authoritative copy of fields this projection does not understand.
	struct PxcxImageGraphProjection {
		engine::imagegraph::Document Graph;
		std::vector<engine::imagegraph::Diagnostic> Diagnostics;
	};

	// Derives visible opaque nodes and positional links without rewriting archive data.
	// @param archive Parsed source project.
	// @param projection Receives the canvas projection and unsupported-node diagnostics.
	// @param error Receives a reason when the canvas cannot represent the structural facts.
	// @return Whether the archive's structural facts fit the imagegraph canvas limits.
	bool ProjectPxcxImageGraph(
		const engine::bake::PxcxArchive &archive, PxcxImageGraphProjection &projection, std::string &error
	);

	// Registers generic grey sockets for the foreign positional indices in one archive.
	// @param archive Parsed source project whose links define visible socket indices.
	void RegisterPxcxCanvasNodeTypes(const engine::bake::PxcxArchive &archive);

	std::string PxcxOpaqueNodeType(std::string_view foreignType);
	std::string PxcxInputPortId(uint32_t inputIndex);
	std::string PxcxOutputPortId(uint32_t outputIndex);

	// A whole-document undo log bounded by both transition count and serialized
	// bytes. The text grammar is canonical, so every edit path restores the same
	// authored fields.
	class ImageGraphHistory {
	  public:
		// @param capacity Maximum undo and redo transitions retained together.
		// @param byteCapacity Maximum serialized bytes retained across both stacks.
		explicit ImageGraphHistory(size_t capacity = 128, size_t byteCapacity = 16 * 1024 * 1024);
		// Records the state before one changed document operation.
		// @param before Document before the edit.
		// @param after  Document after the edit.
		void Record(const engine::imagegraph::Document &before, const engine::imagegraph::Document &after);
		// Refuses an edit that cannot retain both undo and redo, preserving existing history.
		bool TryRecord(const engine::imagegraph::Document &before, const engine::imagegraph::Document &after);
		// Replaces the current document with its previous state when budget allows.
		// @param document Document to replace.
		// @return Whether a previous state was restored.
		bool Undo(engine::imagegraph::Document &document);
		// Replaces the current document with its next state when budget allows.
		// @param document Document to replace.
		// @return Whether a next state was restored.
		bool Redo(engine::imagegraph::Document &document);
		void Clear();
		bool CanUndo() const;
		bool CanRedo() const;

	  private:
		size_t Capacity = 128;
		size_t ByteCapacity = 16 * 1024 * 1024;
		size_t RetainedBytes = 0;
		std::vector<std::string> UndoText;
		std::vector<std::string> RedoText;
	};

}
