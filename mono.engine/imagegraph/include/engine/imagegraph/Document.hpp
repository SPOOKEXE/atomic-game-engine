#pragma once

// Authored image graphs, their checked execution plan and bounded CPU results.
//
// Identifiers in this format are durable text. Plans and pixels are derived
// data and never become part of the saved document.

#include <engine/imagegraph/Surface.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
		// Three dimensional numeric value.
		Vector3,
		// Finite quaternion components used by source local transforms.
		Quaternion,
		// A closed integer choice whose meaning is declared by its node schema.
		Enum,
		// Owned runtime-only CPU mesh geometry and local transforms.
		Mesh,
		// Runtime-only planar audio samples with their source sample rate.
		AudioBit,
		// The remaining types are runtime-only typed sockets. They carry no authored document value, and a
		// link needs a source of the same type. Each mirrors one Pixel Composer junction type.
		// 2D deformable mesh.
		Mesh2D,
		// Numeric matrix with an authored document value.
		Matrix,
		// Particle system state.
		Particle,
		// Rigid body simulation objects.
		Rigid,
		// FLIP fluid domain state.
		FluidDomain,
		// Smoke simulation domain state.
		SmokeDomain,
		// Strand or hair simulation state.
		Strand,
		// Signed distance field.
		Sdf,
		// Bone hierarchy.
		Armature,
		// Positioned surfaces with transforms.
		Atlas,
		// Tile set with its source surface.
		Tileset,
		// Pixel builder bounding box.
		PixelBox,
		// 3D scene of objects, cameras and lights.
		Scene3D,
		// 3D material.
		Material3D,
		// 3D light.
		Light3D,
		// Raw byte buffer.
		Buffer,
		// Keyed structure.
		Struct,
		// Any junction type, resolved by the node that consumes it.
		Any,
		// Reference to another graph node.
		NodeRef,
		// Pixel Composer expression graph node.
		PcxNode,
		// Opaque host object reference.
		Object,
		// A surface whose size can change during evaluation.
		DynamicSurface,
	};

	// Stable text name of a value type, as written in documents and catalogues.
	std::string_view ValueTypeName(ValueType type);
	// Parses a name written by ValueTypeName.
	std::optional<ValueType> ParseValueTypeName(std::string_view name);
	// Whether the type carries an authored document value. Runtime-only sockets have no default. Matrix
	// is the one authored type after AudioBit.
	bool IsAuthoredValueType(ValueType type);

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

	// Bounded planar audio passed between nodes during one evaluation. Samples is the legacy mono shape;
	// Channels holds one equally sized array per channel and is mutually exclusive with Samples.
	struct AudioBit {
		std::vector<double> Samples;
		double SampleRate = 0.0;
		std::vector<std::vector<double>> Channels{};
		bool operator==(const AudioBit &) const = default;
	};

	// A numeric matrix, row-major, as node_matrix.gml Matrix stores it.
	struct MatrixValue {
		uint32_t Columns = 1;
		uint32_t Rows = 1;
		std::vector<double> Values{0.0};
		bool operator==(const MatrixValue &) const = default;
	};
	// Local source transform metadata. Wrappers are retained instead of decomposing a world matrix.
	struct MeshTransform3D {
		Vector3 Position{}, Anchor{};
		Quaternion Rotation{};
		Vector3 Scale{1, 1, 1};
		bool operator==(const MeshTransform3D &) const = default;
	};
	struct MeshVertex3D {
		Vector3 Position{}, Normal{};
		Vector2 UV{};
		Colour Tint{255, 255, 255, 255};
		bool operator==(const MeshVertex3D &) const = default;
	};
	struct MeshEdge3D {
		Vector3 From{}, To{};
		bool operator==(const MeshEdge3D &) const = default;
	};
	using MaterialSurface3D = Image;

	// Copies own distinct backing storage. Evaluators admit backing and retained buffers before copying.
	template <class T> class OwnedPayload3D {
		std::unique_ptr<T> Storage;

	  public:
		OwnedPayload3D() = default;
		OwnedPayload3D(const OwnedPayload3D &other)
			: Storage(other.Storage ? std::make_unique<T>(*other.Storage) : nullptr) {}
		OwnedPayload3D(OwnedPayload3D &&) noexcept = default;
		OwnedPayload3D &operator=(const OwnedPayload3D &other) {
			if (this == &other) return *this;
			OwnedPayload3D copy(other);
			Storage.swap(copy.Storage);
			return *this;
		}
		OwnedPayload3D &operator=(OwnedPayload3D &&) noexcept = default;
		explicit operator bool() const {
			return bool(Storage);
		}
		T &operator*() {
			return *Storage;
		}
		const T &operator*() const {
			return *Storage;
		}
		T *operator->() {
			return Storage.get();
		}
		const T *operator->() const {
			return Storage.get();
		}
		T &emplace() {
			auto next = std::make_unique<T>();
			Storage.swap(next);
			return *Storage;
		}
		bool operator==(const OwnedPayload3D &other) const {
			return Storage && other.Storage ? *Storage == *other.Storage
											: bool(Storage) == bool(other.Storage);
		}
	};
	struct MaterialData3D {
		std::optional<MaterialSurface3D> Surface, Normal, PropertiesMap;
		Vector2 TextureScale{1, 1}, TextureShift{};
		double TextureFilter = 0;
		double Diffuse = 1, Specular = 0, Shininess = 1, Reflectance = 0, NormalStrength = 1;
		bool Metal = false, MetallicMapped = false, RoughnessMapped = false;
		Vector2 MetallicRange{}, RoughnessRange{1, 1};
		bool operator==(const MaterialData3D &) const = default;
	};
	// Empty backing is the exact default descriptor and allocates nothing.
	struct MaterialValue3D {
		OwnedPayload3D<MaterialData3D> Data;
		const MaterialData3D &Get() const {
			static const MaterialData3D defaults;
			return Data ? *Data : defaults;
		}
		MaterialData3D &Edit() {
			return Data ? *Data : Data.emplace();
		}
		bool operator==(const MaterialValue3D &other) const {
			return Get() == other.Get();
		}
	};
	struct MeshPart3D {
		std::vector<MeshVertex3D> Vertices;
		uint32_t MaterialIndex = 0;
		bool operator==(const MeshPart3D &) const = default;
	};
	struct MeshData3D {
		std::vector<MeshPart3D> Parts;
		std::vector<MeshEdge3D> Edges;
		std::vector<MaterialValue3D> Materials;
		// Outermost wrapper first, base geometry last. Every present mesh has a local transform.
		std::vector<MeshTransform3D> LocalTransforms;
		bool operator==(const MeshData3D &) const = default;
	};
	// Absent Data retains the source no-mesh result without fabricating empty geometry.
	struct MeshValue3D {
		OwnedPayload3D<MeshData3D> Data;
		bool operator==(const MeshValue3D &) const = default;
	};
	// Array leaves shared by runtime carriers and bounded native authored arrays.
	using ElementValue = std::variant<
		bool,
		int64_t,
		double,
		std::string,
		Colour,
		Vector2,
		Gradient,
		Area,
		Curve,
		Vector3,
		Vector4,
		Quaternion,
		EnumValue,
		Path2D,
		MatrixValue,
		AudioBit,
		MeshValue3D,
		MaterialValue3D>;

	// Source arrays may mix leaves, nested arrays and owned surfaces. No pointer survives evaluation.
	struct SourceArrayItem {
		std::variant<ElementValue, std::vector<SourceArrayItem>, Image> Data;
		bool operator==(const SourceArrayItem &) const = default;
	};

	// Array elements retain their individual values and one declared element type.
	struct ArrayValue {
		ValueType ElementType = ValueType::Integer;
		std::vector<ElementValue> Elements;
		// Legacy homogeneous rows, exclusive with Elements and general Items.
		std::vector<std::vector<ElementValue>> Nested{};
		// General source shape, exclusive with Elements and Nested. Legacy consumers require normalization.
		std::vector<SourceArrayItem> Items{};
		bool operator==(const ArrayValue &) const = default;
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
		AudioBit,
		MatrixValue,
		MeshValue3D,
		MaterialValue3D>;

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
		// Source instance controls and animators resolve through this durable base node.
		std::string InstanceBase{};
		// Source input ports marked override_instance.
		std::vector<std::string> InstanceOverrides{};
		// Source controls declared animated, including animators with no keys.
		std::vector<std::string> SourceAnimatedInputs{};
		// Explicit source static modes preserve raw first-key reads without affecting native tracks.
		std::vector<std::string> SourceStaticInputs{};
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
		// Source boundary control node, empty for an ordinary native typed junction.
		std::string ControlNodeId{};
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
		// Source Color Depth choice: 0 Input, 1 Inherited, or concrete choices 2..8.
		int64_t ColorDepth = 1;
		// Group attributes resolve through the base before local inheritance.
		std::string InstanceBase{};
		// Source sampling attributes, with zero inheriting the base/parent/project chain.
		int64_t Interpolation = 0;
		int64_t Oversample = 0;
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

	// Sine controls for the legacy scalar offset or a source offset applied to each numeric component.
	struct KeyframeSineDriver {
		// Oscillations across the timeline's total frame count.
		double Frequency = 4.0;
		// Offset amplitude, shared by every source numeric component.
		double Amplitude = 1.0;
		// Phase in cycles.
		double Phase = 0.0;
		// Fade width as a fraction of the outgoing key interval.
		double Smooth = 0.0;
		bool operator==(const KeyframeSineDriver &) const = default;
	};

	// Source drivers retain their named controls rather than a serialized variant index.
	struct KeyframeLinearDriver {
		double Speed = 1;
		bool operator==(const KeyframeLinearDriver &) const = default;
	};
	struct KeyframeSnapDriver {
		double Size = 1;
		bool operator==(const KeyframeSnapDriver &) const = default;
	};
	struct KeyframeBounceDriver {
		int64_t Amount = 3;
		double Spacing = .5, Curve = 2;
		bool operator==(const KeyframeBounceDriver &) const = default;
	};
	struct KeyframeElasticDriver {
		int64_t Amount = 3;
		double Spacing = .5, Curve = 2;
		bool operator==(const KeyframeElasticDriver &) const = default;
	};
	struct KeyframeCurveDriver {
		imagegraph::Curve Data{
			{0, 1, 0, 0, 1, 0}, {{0, 0, 0, 0, 1.0 / 3, 1.0 / 3}, {-1.0 / 3, -1.0 / 3, 1, 1, 0, 0}}
		};
		bool operator==(const KeyframeCurveDriver &) const = default;
	};
	using KeyframeSourceDriver = std::variant<
		KeyframeLinearDriver,
		KeyframeSnapDriver,
		KeyframeBounceDriver,
		KeyframeElasticDriver,
		KeyframeCurveDriver,
		KeyframeSineDriver>;

	// Validates finite source controls and native bounded work, including the derived curve table.
	bool ValidKeyframeSourceDriver(const KeyframeSourceDriver &driver);

	// Converts one source control tuple: raw (0), or Euler degrees (1). Failure leaves result unchanged.
	bool ConvertSourceQuaternion(const Quaternion &tuple, int64_t mode, Quaternion &result);

	// Source key marker. The pinned source evaluates both kinds with ordinary key arithmetic.
	enum class KeyframeKind { Normal, Adder };

	// A value at one authored timeline position. Legacy step holds the left interval.
	struct Keyframe {
		// Target node identifier.
		std::string NodeId;
		// Target property identifier.
		std::string Port;
		// Whole magnitude of the authored timeline position.
		uint64_t Tick = 0;
		// Typed property value at this tick.
		Value Data;
		// Interpolation rule identifier.
		std::string Interpolation = "step";
		// Authored source-side handles, present only when Interpolation is "source".
		std::optional<KeyframeEase> Ease = std::nullopt;
		// Optional deterministic scalar offset applied while this key drives the track.
		std::optional<KeyframeSineDriver> SineDriver = std::nullopt;
		// Source driver is mutually exclusive with the legacy scalar sine driver.
		std::optional<KeyframeSourceDriver> SourceDriver = std::nullopt;
		// Fractional magnitude within Tick, bounded to [0, 1).
		double Subframe = 0;
		// The authored position is -(Tick + Subframe) when true; negative zero is invalid.
		bool NegativeFrame = false;
		// Durable source marker; Adder does not introduce additive arithmetic.
		KeyframeKind Kind = KeyframeKind::Normal;
		// Compares all authored keyframe fields.
		bool operator==(const Keyframe &) const = default;
	};

	// Logical copy footprint: fixed key storage, owned values/text and optional driver curve anchors.
	// Allocator bookkeeping is excluded. Invalid bounded payloads or byte overflow have no footprint.
	std::optional<uint64_t> KeyframePayloadBytes(const Keyframe &key);
	// Bounded admission for imported instance clones, before copying any owned payload.
	std::optional<uint64_t> NodeClonePayloadBytes(const Node &node);
	std::optional<uint64_t> ValueClonePayloadBytes(const Value &value);

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
		// Source quaternion controls store raw tuples (0) or Euler degrees (1). No implicit native mode.
		std::optional<int64_t> QuaternionMode = std::nullopt;
		bool operator==(const AnimationTrack &) const = default;
	};

	// Source preview grid controls used by coordinate gizmos.
	struct PreviewGridSettings {
		// Grid visibility; Control snaps to pixels when this is false.
		bool Show = false;
		// Snap to grid even without Control, independently of visibility.
		bool Snap = false;
		// Horizontal and vertical grid spacing in project pixels; zero disables that axis snapping.
		Vector2 Size{16, 16};
		// Compares all stored preview grid controls.
		bool operator==(const PreviewGridSettings &) const = default;
	};
	// Native text stores these axis names rather than enum ordinals.
	enum class PreviewRulerAxis {
		// A guide parallel to the horizontal axis constrains the vertical coordinate.
		Horizontal,
		// A guide parallel to the vertical axis constrains the horizontal coordinate.
		Vertical
	};
	// One ordered source preview guide, in project pixel coordinates.
	struct PreviewRulerGuide {
		// Horizontal guides constrain Y; vertical guides constrain X.
		PreviewRulerAxis Axis = PreviewRulerAxis::Horizontal;
		// Finite guide position; negative positions are permitted.
		double Position = 0;
		// Compares the guide axis and position.
		bool operator==(const PreviewRulerGuide &) const = default;
	};

	// Project attributes that source nodes inherit. A document without them uses these defaults, which are
	// Pixel Composer's fresh-project attributes.
	struct ProjectSettings {
		// Surface size used by Project-unit dimensions and nodes without an input surface.
		uint32_t SurfaceWidth = 32;
		uint32_t SurfaceHeight = 32;
		// Stored project attributes. A node's "Inherited" (0) attribute resolves to this value plus one.
		int64_t Interpolation = 0;
		int64_t Oversample = 3;
		// Project palette, used by palette inputs declared without their own default.
		std::vector<Colour> Palette{{255, 255, 255, 255}, {0, 0, 0, 255}};
		// Authored source grid controls inherited by coordinate gizmos.
		PreviewGridSettings PreviewGrid{};
		// Ordered source guides; snapping visits them in this stored order.
		std::vector<PreviewRulerGuide> PreviewRulers{};
		// Native durable visibility extension; source project files do not store this preference.
		bool ShowPreviewRulers = false;
		// Source project Color Depth index (0..6); default 1 selects RGBA8.
		int64_t ColorDepth = 1;
		bool operator==(const ProjectSettings &) const = default;
	};

	// Checks finite grid spacing, named guide axes, finite positions and the shared array count budget.
	bool ValidProjectPreviewSettings(const ProjectSettings &project);

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
		// Project attributes, written from format version 7.
		std::optional<ProjectSettings> Project;
		// Compares the complete authored document.
		bool operator==(const Document &) const = default;
	};

	// Retained candidate footprint, including container capacities and all owned authored metadata.
	std::optional<uint64_t> DocumentRetainedPayloadBytes(const Document &document);

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
		static constexpr size_t MaximumArrayDepth = 64;
		static constexpr size_t MaximumArrayBytes = 4 * 1024 * 1024;
		// Maximum authored colours in one posterize palette.
		static constexpr size_t MaximumPaletteEntries = 32;
		// Maximum colours in the project palette.
		static constexpr size_t MaximumProjectPaletteEntries = 256;
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
		// Maximum properties recorded on one node. The largest catalogue node, Particle, declares 94 inputs.
		static constexpr size_t MaximumPropertiesPerNode = 128;
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
		// Maximum samples across all channel planes in one recorded source frame.
		static constexpr size_t MaximumAudioSamplesPerFrame = 4096;
		// Whole source clips are bounded separately from captured tick frames.
		static constexpr size_t MaximumAudioClipSamples = 1'048'576;
		// Planar captures and analysis outputs permit at most eight equally sized channels.
		static constexpr size_t MaximumAudioChannels = 8;
		// Maximum decoded samples across all frames and channel planes in one capture document.
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

	// One deterministic planar source frame keyed by a durable capture ID and tick.
	struct AudioCaptureFrame {
		// 1 to 255 ASCII bytes from `[A-Za-z0-9_-]`, selected by an `image.audio_recording` node.
		std::string SourceId;
		// Exact timeline tick at which this sample window is available.
		uint64_t Tick = 0;
		// Ordered finite mono samples, mutually exclusive with Channels.
		std::vector<double> Samples;
		// Samples per second for timeline-indexed audio windows. Zero is legacy capture metadata.
		double SampleRate = 0.0;
		// Planar equal-length finite sample arrays. An empty value uses the legacy mono Samples field.
		std::vector<std::vector<double>> Channels{};
		// Compares the complete recorded payload and metadata.
		bool operator==(const AudioCaptureFrame &) const = default;
	};

	// A caller-owned whole clip, selected by its durable source name independently of timeline tick.
	struct AudioClipSource {
		std::string SourceId;
		AudioBit Data;
	};

	// A caller-owned image selected by a durable ID for one synchronous evaluation.
	struct RequestImageSource {
		std::string SourceId;
		Image Data;
	};

	class GroupReplayState;
	// Fixed evaluation inputs. Seed is reserved for deterministic random nodes.
	struct EvaluationRequest {
		uint64_t Tick = 0;
		uint64_t Seed = 0;
		// Fraction within Tick, bounded to [0, 1).
		double Subframe = 0.0;
		// Caller-owned recorded inputs, read during synchronous evaluation. A graph source selects an exact
		// ID and tick.
		std::span<const AudioCaptureFrame> AudioFrames{};
		// Caller image budget, applied before native output allocation. Hosts may lower the core limit.
		uint32_t MaximumImageDimension = Limits::MaximumDimension;
		// Whole clips supplied by a host adapter. The evaluator never opens source paths.
		std::span<const AudioClipSource> AudioClips{};
		// Signs the authoring timeline position only; captured audio still uses its exact unsigned Tick.
		bool NegativeFrame = false;
		// Native image.captured inputs. No pointer or borrowed span survives the evaluation.
		std::span<const RequestImageSource> ImageSources{};
		// Borrowed for this synchronous operation; the caller retains the immutable replay owner.
		const GroupReplayState *GroupReplay = nullptr;
		uint64_t GroupAuthoringRevision = 0;
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

	// An allocation-format dependency, distinct from a data input socket.
	struct GroupSurfaceDependency {
		size_t Consumer = 0;
		size_t Producer = 0;
		std::string Port;
		bool operator==(const GroupSurfaceDependency &) const = default;
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
		std::vector<GroupSurfaceDependency> GroupSurfaceDependencies;
		// Compares the compiled order and output mapping.
		bool operator==(const Plan &) const = default;
	};

	// One row-major RGBA8 result.

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

	// Source junction metadata is independent of the carried native Value variant.
	enum class SourceValueDisplay : uint8_t {
		Default,
		Range,
		Rotation,
		RotationRange,
		Slider,
		SliderRange,
		Padding,
		Vector,
		VectorRange,
		Area,
		EnumButton,
		EnumScroll,
		Seed,
		Palette,
		Curve
	};
	// Exact source declaration, including resources without a native Value carrier.
	enum class SourceSocketKind : uint8_t {
		Integer,
		Float,
		Boolean,
		Colour,
		Surface,
		FilePath,
		Curve,
		Text,
		Object,
		Node,
		Any,
		Path,
		Particle,
		Rigid,
		SmokeDomain,
		Struct,
		Strand,
		Mesh2D,
		Trigger,
		Mesh3D,
		Light3D,
		Camera3D,
		Scene3D,
		Material3D,
		PcxNode,
		Audio,
		FluidDomain,
		Sdf,
		Gradient
	};
	struct SourceSocketDomain {
		ValueType Type = ValueType::Any;
		std::optional<SourceValueDisplay> Display;
		// Absent when the catalogue does not establish the exact source declaration.
		std::optional<SourceSocketKind> Kind;
		bool operator==(const SourceSocketDomain &) const = default;
	};

	// One named, typed result selected from a value node's output sockets.
	struct EvaluatedValue {
		std::string Port;
		Value Data;
		std::optional<SourceSocketDomain> Domain;
		bool operator==(const EvaluatedValue &) const = default;
	};

	// One resolved node input image, owned by an evaluation snapshot.
	struct EvaluationInputImage {
		std::string Port;
		Image Data;
		std::optional<SourceSocketDomain> Domain;
	};
	struct EvaluationInputValue {
		std::string Port;
		Value Data;
		bool Linked = false;
		std::optional<SourceSocketDomain> Domain;
	};

	// A move-only, byte-accounted set of resolved node inputs. The owner keeps its
	// evaluation reservation alive while callers inspect the copied payloads.
	class EvaluationSnapshot {
	  public:
		EvaluationSnapshot();
		~EvaluationSnapshot();
		EvaluationSnapshot(EvaluationSnapshot &&) noexcept;
		EvaluationSnapshot &operator=(EvaluationSnapshot &&) noexcept;
		EvaluationSnapshot(const EvaluationSnapshot &) = delete;
		EvaluationSnapshot &operator=(const EvaluationSnapshot &) = delete;

		std::span<const EvaluationInputValue> Values() const noexcept;
		std::span<const EvaluationInputImage> Images() const noexcept;
		// Retained payload and storage metadata charged against the snapshot cap.
		uint64_t RetainedBytes() const noexcept;

	  private:
		struct Storage;
		std::unique_ptr<Storage> Data;
		friend Status EvaluateNodeInputs(
			const Document &,
			const Plan &,
			std::string_view,
			const EvaluationRequest &,
			EvaluationSnapshot &,
			Diagnostic &,
			uint64_t
		);
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

	// Prepares a bounded key copy for a property in the same native typed domain. Cross-domain source
	// transfers require a representable getter policy and are diagnosed. Failure preserves result.
	Status PrepareKeyframeCloneForProperty(
		const Document &document,
		const Keyframe &key,
		std::string_view nodeId,
		std::string_view property,
		Keyframe &result,
		Diagnostic &diagnostic
	);

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

	// Evaluates one node's dependency cone and captures its resolved inputs without
	// running the target node's executor. Failure preserves the previous snapshot.
	Status EvaluateNodeInputs(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		EvaluationSnapshot &snapshot,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes
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
