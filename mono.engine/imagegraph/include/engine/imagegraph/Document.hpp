#pragma once

// Authored image graphs, their checked execution plan and bounded CPU results.
//
// Identifiers in this format are durable text. Plans and pixels are derived
// data and never become part of the saved document.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace engine::imagegraph {
	// The value kinds supported by authored properties and graph ports.
	enum class ValueType : uint8_t {
		// Boolean property value.
		Boolean,
		// Signed integer property value.
		Integer,
		// Finite double precision property value.
		Scalar,
		// UTF-8 authored text.
		Text,
		// Straight-alpha RGBA8 value.
		Colour,
		// Two dimensional numeric value.
		Vector2,
		// Evaluated image payload.
		Image,
		// A bounded homogeneous array of authored scalar values.
		Array,
		Gradient,
		Area,
		Curve,
		Vector4,
		Path2D,
		// Three dimensional authored value. Render adapters consume it, never the CPU evaluator.
		Vector3,
		// Authored unit quaternion for render-owned 3D transforms.
		Quaternion,
		// A closed integer choice whose meaning is declared by its node schema.
		Enum,
		// Opaque render-owned mesh output. It has no CPU payload.
		Mesh,
		// Runtime-only mono samples with their source sample rate.
		AudioBit,
	};

	// An 8-bit straight-alpha colour, matching the CPU output pixel format.
	struct Colour {
		// Red component.
		uint8_t Red = 0;
		// Green component.
		uint8_t Green = 0;
		// Blue component.
		uint8_t Blue = 0;
		// Alpha component.
		uint8_t Alpha = 255;
		// Compares every channel.
		bool operator==(const Colour &) const = default;
	};

	// A two-dimensional authored vector.
	struct Vector2 {
		// Horizontal coordinate.
		double X = 0.0;
		// Vertical coordinate.
		double Y = 0.0;
		// Compares both coordinates.
		bool operator==(const Vector2 &) const = default;
	};

	struct Vector4 {
		double X = 0, Y = 0, Z = 0, W = 0;
		bool operator==(const Vector4 &) const = default;
	};

	struct Vector3 {
		double X = 0, Y = 0, Z = 0;
		bool operator==(const Vector3 &) const = default;
	};

	struct Quaternion {
		double X = 0, Y = 0, Z = 0, W = 1;
		bool operator==(const Quaternion &) const = default;
	};

	struct EnumValue {
		int64_t Value = 0;
		bool operator==(const EnumValue &) const = default;
	};

	struct GradientKey {
		double Time = 0;
		Colour Color{};
		bool operator==(const GradientKey &) const = default;
	};

	struct Gradient {
		uint8_t Mode = 0;
		std::vector<GradientKey> Keys;
		bool operator==(const Gradient &) const = default;
	};

	struct Area {
		double CenterX = 0.5, CenterY = 0.5, HalfWidth = 0.5, HalfHeight = 0.5;
		uint8_t Shape = 0, Mode = 0;
		bool operator==(const Area &) const = default;
	};

	// The source curve stores six header numbers followed by six numbers per anchor.
	struct Curve {
		std::array<double, 6> Header{};
		std::vector<std::array<double, 6>> Anchors;
		bool operator==(const Curve &) const = default;
	};

	struct PathAnchor {
		std::array<double, 6> Controls{};
		int64_t Index = 0;
		bool operator==(const PathAnchor &) const = default;
	};

	struct PathWeight {
		double Position = 0, Weight = 1;
		bool operator==(const PathWeight &) const = default;
	};

	struct Path2D {
		bool Loop = false;
		std::vector<PathAnchor> Anchors;
		std::vector<PathWeight> Weights;
		bool operator==(const Path2D &) const = default;
	};

	// A typed authored value. Image data is supplied by node execution.
	using ElementValue = std::variant<bool, int64_t, double, std::string, Colour, Vector2>;

	// Array elements retain their individual values and one declared element type.
	struct ArrayValue {
		ValueType ElementType = ValueType::Integer;
		std::vector<ElementValue> Elements;
		bool operator==(const ArrayValue &) const = default;
	};

	// A bounded mono source passed between audio nodes during one evaluation.
	struct AudioBit {
		std::vector<double> Samples;
		double SampleRate = 0.0;
		bool operator==(const AudioBit &) const = default;
	};

	using Value = std::variant<
		bool,
		int64_t,
		double,
		std::string,
		Colour,
		Vector2,
		ArrayValue,
		Gradient,
		Area,
		Curve,
		Vector4,
		Path2D,
		Vector3,
		Quaternion,
		EnumValue,
		AudioBit>;

	// The side of a typed graph socket.
	enum class PortDirection : uint8_t {
		// Data flows into the node.
		Input,
		// Data flows out of the node.
		Output,
	};

	// One stable socket declaration in a node schema.
	struct PortSchema {
		// Durable port identifier.
		std::string_view Id;
		// Value type carried by the socket.
		ValueType Type;
		// Whether the socket receives or produces data.
		PortDirection Direction;
	};

	// One stable authored property declaration in a node schema.
	struct PropertySchema {
		// Durable property identifier.
		std::string_view Id;
		// Required value type.
		ValueType Type;
	};

	// Read-only registration data used by validation and editor adapters.
	struct NodeSchema {
		// Durable node type identifier.
		std::string_view Type;
		// Typed input and output sockets.
		std::span<const PortSchema> Ports;
		// Typed authored property fields.
		std::span<const PropertySchema> Properties;
		// Whether instances may declare additional typed input sockets.
		bool DynamicInputs = false;
	};

	// A value attached to a stable property name on one node.
	struct AuthoredValue {
		// Property identifier declared by the node schema.
		std::string Port;
		// Typed property value.
		Value Data;
		// Compares the property name and value.
		bool operator==(const AuthoredValue &) const = default;
	};

	// One instance-specific input socket, in editor display order.
	struct DynamicInput {
		std::string Id;
		ValueType Type = ValueType::Image;
		std::optional<Value> Default;
		bool operator==(const DynamicInput &) const = default;
	};

	// One authored node instance. Id remains stable when nodes are reordered.
	struct Node {
		// Durable instance identifier.
		std::string Id;
		// Stable node type identifier from the registry.
		std::string Type;
		// Group identifier, or empty for the root canvas.
		std::string GroupId;
		// Canvas position, in authored coordinates.
		Vector2 Position;
		// Authored property values.
		std::vector<AuthoredValue> Values;
		// Instance-specific sockets used by nodes with a variable input count.
		std::vector<DynamicInput> DynamicInputs{};
		// Compares all authored node fields.
		bool operator==(const Node &) const = default;
	};

	// A typed connection from one node output to another node input.
	struct Link {
		// Source node identifier.
		std::string FromNode;
		// Source output port identifier.
		std::string FromPort;
		// Destination node identifier.
		std::string ToNode;
		// Destination input port identifier.
		std::string ToPort;
		// Compares both endpoints.
		bool operator==(const Link &) const = default;
	};

	// A named subgraph socket routed through one typed junction.
	struct GroupPort {
		std::string Id;
		std::string JunctionId;
		PortDirection Direction = PortDirection::Input;
		bool operator==(const GroupPort &) const = default;
	};

	// A named canvas group. Node membership is stored on each node.
	struct Group {
		// Durable group identifier.
		std::string Id;
		// User-visible group name.
		std::string Name;
		// Parent group, or empty for a group on the root canvas.
		std::string ParentId{};
		// Public sockets for a nested graph, in inspector order.
		std::vector<GroupPort> Ports{};
		// Compares the identifier and name.
		bool operator==(const Group &) const = default;
	};

	// Typed wire junction. Links use its Id with the port name "value".
	struct Junction {
		std::string Id;
		std::string GroupId;
		ValueType Type = ValueType::Image;
		std::optional<Value> Default;
		bool operator==(const Junction &) const = default;
	};

	// A durable image or image-array output that a host can select by Id.
	struct Output {
		// Durable output selector.
		std::string Id;
		// Source node identifier.
		std::string NodeId;
		// Source output port identifier.
		std::string Port;
		// Compares the output binding.
		bool operator==(const Output &) const = default;
	};

	// Source-style side easing is separate from legacy left-interval interpolation.
	struct KeyframeEase {
		std::string InType = "linear";
		std::string OutType = "linear";
		Vector2 In{0, 1};
		Vector2 Out{0, 0};
		bool operator==(const KeyframeEase &) const = default;
	};

	// A deterministic sine offset added to one scalar key's interpolated value.
	struct KeyframeSineDriver {
		// Oscillations across the timeline's total frame count.
		double Frequency = 4.0;
		// Scalar offset amplitude.
		double Amplitude = 1.0;
		// Phase in cycles.
		double Phase = 0.0;
		// Fade width as a fraction of the outgoing key interval.
		double Smooth = 0.0;
		bool operator==(const KeyframeSineDriver &) const = default;
	};

	// A value at one fixed timeline tick. Legacy step holds the left interval.
	struct Keyframe {
		// Target node identifier.
		std::string NodeId;
		// Target property identifier.
		std::string Port;
		// Fixed authored timeline tick.
		uint64_t Tick = 0;
		// Typed property value at this tick.
		Value Data;
		// Interpolation rule identifier.
		std::string Interpolation = "step";
		// Authored source-side handles, present only when Interpolation is "source".
		std::optional<KeyframeEase> Ease = std::nullopt;
		// Optional deterministic scalar offset applied while this key drives the track.
		std::optional<KeyframeSineDriver> SineDriver = std::nullopt;
		// Compares all authored keyframe fields.
		bool operator==(const Keyframe &) const = default;
	};

	struct TimelineSettings {
		uint64_t Frames = 1;
		uint64_t First = 0;
		uint64_t Last = 0;
		std::string Playback = "loop";
		// Authored playback rate. Evaluation remains independent of wall time.
		double FramesPerSecond = 30.0;
		bool operator==(const TimelineSettings &) const = default;
	};

	struct AnimationTrack {
		std::string NodeId;
		std::string Port;
		std::string End = "hold";
		int64_t LoopRange = -1;
		bool operator==(const AnimationTrack &) const = default;
	};

	// The versioned, lossless authored graph document.
	struct Document {
		// Text format version.
		uint32_t FormatVersion = 1;
		// Nodes in canvas declaration order.
		std::vector<Node> Nodes;
		// Authored directed links.
		std::vector<Link> Links;
		// Named canvas groups.
		std::vector<Group> Groups;
		// Typed routes, including routes through nested group interfaces.
		std::vector<Junction> Junctions;
		// Host-selectable image outputs.
		std::vector<Output> Outputs;
		// Timeline values, retained even when evaluation does not use animation.
		std::vector<Keyframe> Keyframes;
		// Explicit playback range; absent in documents migrated from earlier formats.
		std::optional<TimelineSettings> Timeline;
		// Per-property keyframe end and loop-tail policies.
		std::vector<AnimationTrack> Tracks;
		// Compares the complete authored document.
		bool operator==(const Document &) const = default;
	};

	// Bounds graph work and every CPU image allocation.
	struct Limits {
		// Maximum authored node count.
		static constexpr size_t MaximumNodes = 4096;
		// Maximum authored link count.
		static constexpr size_t MaximumLinks = 8192;
		// Maximum group count.
		static constexpr size_t MaximumGroups = 1024;
		static constexpr size_t MaximumGroupPorts = 64;
		static constexpr size_t MaximumJunctions = 8192;
		static constexpr size_t MaximumDynamicInputsPerNode = 64;
		static constexpr size_t MaximumArrayElements = 4096;
		static constexpr size_t MaximumArrayBytes = 4 * 1024 * 1024;
		// Maximum authored colours in one posterize palette.
		static constexpr size_t MaximumPaletteEntries = 32;
		static constexpr size_t MaximumGradientKeys = 128;
		static constexpr size_t MaximumCurveAnchors = 256;
		static constexpr size_t MaximumPathAnchors = 1024;
		static constexpr size_t MaximumPathWeights = 1024;
		static constexpr size_t MaximumDocumentBytes = 64 * 1024 * 1024;
		// Maximum output count.
		static constexpr size_t MaximumOutputs = 64;
		// Maximum keyframe count.
		static constexpr size_t MaximumKeyframes = 65536;
		static constexpr size_t MaximumTracks = 65536;
		// Maximum properties recorded on one node.
		static constexpr size_t MaximumPropertiesPerNode = 64;
		// Maximum bytes in one authored text field.
		static constexpr size_t MaximumTextBytes = 1024 * 1024;
		// Maximum width or height of one image.
		static constexpr uint32_t MaximumDimension = 4096;
		// Maximum bytes allocated for one RGBA8 output.
		static constexpr uint64_t MaximumOutputBytes = 64 * 1024 * 1024;
		// Maximum simultaneously retained intermediate CPU image bytes.
		static constexpr uint64_t MaximumEvaluationBytes = 128 * 1024 * 1024;
		// Maximum decoded recorded-audio source frames attached to one evaluation.
		static constexpr size_t MaximumAudioCaptureFrames = 4096;
		// Maximum mono samples in one recorded source frame.
		static constexpr size_t MaximumAudioSamplesPerFrame = 4096;
		// Maximum decoded mono samples retained by one capture document.
		static constexpr size_t MaximumAudioCaptureSamples = 262144;
		// Maximum encoded bytes in one recorded-audio capture document.
		static constexpr size_t MaximumAudioCaptureDocumentBytes = 8 * 1024 * 1024;
		// Maximum fixed timeline tick accepted by a document or evaluation.
		static constexpr uint64_t MaximumTick = 10'000'000;
		// Maximum frames in one requested inclusive tick range.
		static constexpr size_t MaximumRangeFrames = 4096;
	};

	// Inclusive fixed-tick range. A step need not land exactly on Last.
	struct TickRange {
		// First evaluated tick.
		uint64_t First = 0;
		// Last allowed evaluated tick.
		uint64_t Last = 0;
		// Tick spacing between outputs.
		uint64_t Step = 1;
	};

	// One deterministic mono source frame keyed by a durable capture ID and tick.
	struct AudioCaptureFrame {
		// 1 to 255 ASCII bytes from `[A-Za-z0-9_-]`, selected by an `image.audio_recording` node.
		std::string SourceId;
		// Exact timeline tick at which this sample window is available.
		uint64_t Tick = 0;
		// Ordered finite mono samples for this source frame.
		std::vector<double> Samples;
		// Samples per second for timeline-indexed audio windows. Zero is legacy capture metadata.
		double SampleRate = 0.0;
		// Compares the source ID, tick, ordered samples and sample rate.
		bool operator==(const AudioCaptureFrame &) const = default;
	};

	// Fixed evaluation inputs. Seed is reserved for deterministic random nodes.
	struct EvaluationRequest {
		uint64_t Tick = 0;
		uint64_t Seed = 0;
		// Fraction within Tick, bounded to [0, 1).
		double Subframe = 0.0;
		// Caller-owned recorded inputs, read during synchronous evaluation. A graph source selects an exact
		// ID and tick.
		std::span<const AudioCaptureFrame> AudioFrames{};
	};

	// Why parsing, compilation or evaluation failed.
	enum class Status : uint8_t {
		// Operation succeeded.
		Ok,
		// Text record could not be parsed.
		Malformed,
		// Document uses an unsupported format version.
		UnsupportedVersion,
		// A declared count or resource exceeds its limit.
		LimitExceeded,
		// A stable identifier is empty or repeated.
		DuplicateId,
		// Node type is absent from the registry.
		UnknownNode,
		// Property or socket identifier is absent from the node schema.
		UnknownPort,
		// A value or link has incompatible types.
		TypeMismatch,
		// An authored field violates its node schema.
		InvalidValue,
		// A node refers to an undeclared group.
		InvalidGroup,
		// An output does not name a declared output of the required type.
		InvalidOutput,
		// A link is duplicated or feeds an already connected input.
		DuplicateLink,
		// Directed links form a cycle.
		Cycle,
		// A valid node has no implementation in the CPU evaluator.
		UnsupportedExecution,
	};

	// A deterministic error location in authored text.
	struct Diagnostic {
		// Machine-readable failure code.
		Status Code = Status::Ok;
		// Durable node identifier, when one applies.
		std::string NodeId;
		// Durable property or port identifier, when one applies.
		std::string Port;
		// Short human-readable explanation.
		std::string Message;
		// Compares the error code and location.
		bool operator==(const Diagnostic &) const = default;
	};

	// A typed input value resolved from a junction route with no upstream link.
	struct ResolvedInput {
		std::string NodeId;
		std::string Port;
		Value Data;
		bool operator==(const ResolvedInput &) const = default;
	};

	// A stable order and selected output index produced by Compile.
	struct Plan {
		// Node indices in deterministic execution order.
		std::vector<size_t> NodeOrder;
		// Source node index for each document output, in output declaration order.
		std::vector<size_t> OutputNodes;
		// Junction routes flattened to direct node dependencies.
		std::vector<Link> EffectiveLinks;
		// Defaults passed through junctions to node inputs.
		std::vector<ResolvedInput> ResolvedInputs;
		// Compares the compiled order and output mapping.
		bool operator==(const Plan &) const = default;
	};

	// One row-major RGBA8 result.
	struct Image {
		// Pixel width.
		uint32_t Width = 0;
		// Pixel height.
		uint32_t Height = 0;
		// Straight-alpha RGBA8 bytes, row-major.
		std::vector<uint8_t> Pixels;
		// FNV-1a hash of the pixel bytes.
		uint64_t Hash = 0;
	};

	// A bounded ordered set of complete images, never an implicit atlas.
	template <class T> struct ArrayItem {
		// Children preserve nested arrays; image leaves index ImageArray::Images.
		std::variant<T, std::vector<ArrayItem<T>>> Data;
		bool operator==(const ArrayItem &) const = default;
	};
	using ImageArrayItem = ArrayItem<size_t>;

	struct ImageArray {
		std::vector<Image> Images;
		std::vector<ImageArrayItem> Items;
	};

	// One named, typed result selected from a value node's output sockets.
	struct EvaluatedValue {
		std::string Port;
		Value Data;
		bool operator==(const EvaluatedValue &) const = default;
	};

	// Serializes all authored fields with a stable, versioned text grammar.
	std::string Write(const Document &document);

	// Returns a static node schema, or null for an unregistered type.
	const NodeSchema *FindSchema(std::string_view type);

	// Parses a document without discarding incomplete or unknown authored nodes.
	Status Read(const std::string &text, Document &document, Diagnostic &diagnostic);

	// Upgrades a valid legacy v1 document to the v2 authored grammar.
	Status Migrate(Document &document, Diagnostic &diagnostic);

	// Checks limits, identities, schemas, links, outputs and cycles.
	Status Compile(const Document &document, Plan &plan, Diagnostic &diagnostic);

	// Evaluates a selected supported output into bounded, deterministic RGBA8 pixels.
	Status Evaluate(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		Image &image,
		Diagnostic &diagnostic
	);

	// Checks an inclusive fixed-tick range before a host streams its frames.
	Status ValidateTickRange(const TickRange &range, size_t &frameCount, Diagnostic &diagnostic);

	// Resolves the selected output's authored keyframes at one fixed tick.
	Status Evaluate(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		Image &image,
		Diagnostic &diagnostic
	);

	// Evaluates every image of a selected array output without dropping elements.
	Status EvaluateArray(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		ImageArray &images,
		Diagnostic &diagnostic
	);

	// Evaluates a scalar, boolean, text, colour, vector or authored array output.
	Status EvaluateValue(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		EvaluatedValue &value,
		Diagnostic &diagnostic
	);

	// Resolves authored values for one node reachable from the selected output at
	// a fixed timeline position. This performs no image execution.
	Status ResolveNodeValues(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const std::string &nodeId,
		const EvaluationRequest &request,
		std::vector<AuthoredValue> &values,
		Diagnostic &diagnostic
	);

	// Legacy tick-only entry point, equivalent to a request with seed zero.
	Status Evaluate(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		uint64_t tick,
		Image &image,
		Diagnostic &diagnostic
	);
}
