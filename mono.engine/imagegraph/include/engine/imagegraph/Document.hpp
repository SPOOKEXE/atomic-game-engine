#pragma once

// Bounded 2D image composition and pure named-input snapshots. Hosts supply pixels.
// Pixels are top-row-first and straight-alpha. Colour sources use encoded sRGB
// channel values; data sources preserve numeric channel values. Operations work on
// stored channels in [0, 1] and quantize each intermediate to 8 bits, or 16 bits
// with wide precision. Bilinear sampling weights premultiplied values. Output Space
// declares storage intent without transforming those values; output Format selects
// RGBA8, R16F or RGBA16F bytes.
// Durable node, kind, source and output identities are strings; plans are derived.
// @tier L9 · shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace engine::imagegraph {
	// Hard bounds applied while reading, compiling and evaluating a graph.
	struct Limits {
		// Largest accepted serialized document.
		static constexpr size_t MaximumDocumentBytes = 1024 * 1024;
		// Largest number of authored nodes.
		static constexpr size_t MaximumNodes = 256;
		// Largest number of named outputs.
		static constexpr size_t MaximumOutputs = 16;
		// Largest number of named parameters.
		static constexpr size_t MaximumParameters = 64;
		// Largest number of node-property bindings.
		static constexpr size_t MaximumBindings = 256;
		// Largest width or height of an evaluated image.
		static constexpr uint32_t MaximumDimension = 4096;
		// Largest single evaluated image in bytes.
		static constexpr size_t MaximumImageBytes = 64 * 1024 * 1024;
		// Largest combined retained pixel storage in one evaluation.
		// Wide intermediates count eight bytes per pixel against this budget.
		static constexpr size_t MaximumRetainedBytes = size_t(1024) * 1024 * 1024;
		// Largest total pixel work allowed for one selected output.
		static constexpr uint64_t MaximumPixelWork = uint64_t(1024) * 1024 * 1024;
		// Largest node input count (channel merge takes four).
		static constexpr size_t MaximumInputs = 4;
		// Largest number of colour ramp stops.
		static constexpr size_t MaximumRampStops = 16;
		// Largest gaussian blur radius in pixels.
		static constexpr double MaximumBlurRadius = 64;
		// Largest mesh attribute dilation in pixels.
		static constexpr uint32_t MaximumPadding = 16;
		// Deepest nested JSON value accepted by the document reader.
		static constexpr size_t MaximumJsonDepth = 16;
	};

	// Defines whether a source's RGB bytes are encoded colour or numeric data.
	enum class SourceInterpretation : uint8_t {
		// Apply colour sampling according to the source texture's declared encoding.
		Colour,
		// Preserve source channels as data without colour interpretation.
		Data
	};
	// Labels the output channel space without converting the stored byte values.
	enum class OutputSpace : uint8_t {
		// Stored RGB bytes represent sRGB values.
		SRGB,
		// Stored RGB bytes represent linear values.
		Linear
	};
	// Bits stored per channel for every intermediate of one evaluation.
	enum class Precision : uint8_t {
		// Eight-bit channels, the v1 and v2 numeric contract.
		Byte,
		// Sixteen-bit channels for data graphs whose smooth falloffs band at eight bits.
		Wide
	};
	// Byte layout of an evaluated output image.
	enum class OutputFormat : uint8_t {
		// Four unsigned bytes per pixel.
		RGBA8,
		// The red channel as one little-endian IEEE binary16 value.
		R16F,
		// Four little-endian IEEE binary16 values.
		RGBA16F
	};
	// Bytes one pixel of a format occupies.
	size_t BytesPerPixel(OutputFormat format) noexcept;
	// Owned top-row-first pixels produced by one bounded evaluation, or a source.
	struct Image {
		// Pixel width.
		uint32_t Width = 0;
		// Pixel height.
		uint32_t Height = 0;
		// Straight-alpha bytes in row-major order, laid out as Format says.
		std::vector<std::byte> Pixels;
		// Declared interpretation of the RGB bytes.
		OutputSpace Space = OutputSpace::SRGB;
		// Byte layout of Pixels. Sources resolved for evaluation must be RGBA8.
		OutputFormat Format = OutputFormat::RGBA8;
		// Checks the dimensions, exact byte extent, format and declared colour space.
		bool IsValid() const noexcept;
	};

	// Filter used when an operation samples between source pixels.
	enum class Sampling : uint8_t {
		// Select the closest source pixel.
		Nearest,
		// Interpolate neighboring samples using premultiplied values.
		Bilinear
	};
	// Reads an image supplied by the host's source resolver.
	struct Source {
		// Reference string passed to the host's source resolver.
		std::string Path;
		// Colour or data interpretation applied to the resolved pixels.
		SourceInterpretation Interpretation = SourceInterpretation::Colour;
	};
	// Creates a constant-color image at the requested extent.
	struct Solid {
		// Output pixel width.
		uint32_t Width = 1;
		// Output pixel height.
		uint32_t Height = 1;
		// Constant straight-alpha RGBA8 value.
		std::array<uint8_t, 4> Colour{255, 255, 255, 255};
	};
	// Resamples an input image to a new extent.
	struct Resize {
		// Output pixel width.
		uint32_t Width = 1;
		// Output pixel height.
		uint32_t Height = 1;
		// Filter used to sample the input.
		Sampling Filter = Sampling::Nearest;
	};
	// Copies a rectangular region from the input, filling out-of-bounds pixels transparently.
	struct Crop {
		// Left coordinate in the input image.
		int32_t X = 0;
		// Top coordinate in the input image.
		int32_t Y = 0;
		// Output pixel width.
		uint32_t Width = 1;
		// Output pixel height.
		uint32_t Height = 1;
	};
	// Applies an inverse-mapped scale, rotation and translation into a fixed output extent.
	struct Transform {
		// Output pixel width.
		uint32_t Width = 1;
		// Output pixel height.
		uint32_t Height = 1;
		// Horizontal translation in output pixels.
		double TranslateX = 0;
		// Vertical translation in output pixels.
		double TranslateY = 0;
		// Horizontal scale; zero is invalid.
		double ScaleX = 1;
		// Vertical scale; zero is invalid.
		double ScaleY = 1;
		// Rotation in degrees around the pivot.
		double Degrees = 0;
		// X coordinate about which rotation and scaling are applied.
		double PivotX = 0;
		// Y coordinate about which rotation and scaling are applied.
		double PivotY = 0;
		// Filter used for inverse-mapped samples.
		Sampling Filter = Sampling::Nearest;
	};
	// Mirrors an input image on either axis.
	struct Flip {
		// Reflect pixels across the vertical axis.
		bool Horizontal = false;
		// Reflect pixels across the horizontal axis.
		bool Vertical = false;
	};
	// Separable blend functions applied before alpha compositing.
	enum class BlendMode : uint8_t {
		Normal,
		Multiply,
		Screen,
		Overlay,
		Add,
		Subtract,
		SoftLight,
		Difference
	};
	// Composites the second input over the first. An optional third input is a mask
	// whose red channel scales the opacity per pixel.
	struct Blend {
		// Opacity applied to the foreground input, from zero to one.
		double Opacity = 1;
		// Function combining the two colours where both are present.
		BlendMode Mode = BlendMode::Normal;
	};

	// Generators fill their own extent. An optional Vector input replaces the pixel's
	// normalized position with that input's red and green channels, sampled at the
	// same normalized position, so a baked UV field can drive the pattern.
	enum class NoiseType : uint8_t {
		// Interpolated lattice values.
		Value,
		// Interpolated lattice gradients.
		Gradient
	};
	// Fractal lattice noise in [0, 1], alpha one.
	struct Noise {
		// Output pixel width.
		uint32_t Width = 256;
		// Output pixel height.
		uint32_t Height = 256;
		// Lattice kind.
		NoiseType Type = NoiseType::Value;
		// Lattice cells across one unit of position.
		double Scale = 4;
		// Number of summed octaves.
		uint32_t Octaves = 4;
		// Frequency multiplier per octave.
		double Lacunarity = 2;
		// Amplitude multiplier per octave.
		double Gain = 0.5;
		// Selects an independent lattice.
		uint32_t Seed = 0;
	};
	// Value a voronoi node writes.
	enum class VoronoiFeature : uint8_t {
		// Distance to the nearest feature point.
		F1,
		// Distance to the second nearest feature point.
		F2,
		// F2 minus F1.
		F2MinusF1,
		// Distance to the nearest cell edge.
		Edge,
		// A random value per cell.
		Cell
	};
	// Cellular noise; distances are in cell units clamped to [0, 1].
	struct Voronoi {
		// Output pixel width.
		uint32_t Width = 256;
		// Output pixel height.
		uint32_t Height = 256;
		// Cells across one unit of position.
		double Scale = 8;
		// Feature point jitter from a regular grid (zero) to fully random (one).
		double Randomness = 1;
		// Value written per pixel.
		VoronoiFeature Feature = VoronoiFeature::F1;
		// Selects an independent point set.
		uint32_t Seed = 0;
	};
	// Gradient shape.
	enum class GradientType : uint8_t {
		// Projection onto the start-to-end segment.
		Linear,
		// Distance from start, divided by the start-to-end length.
		Radial
	};
	// A clamped [0, 1] ramp in normalized position space.
	struct Gradient {
		// Output pixel width.
		uint32_t Width = 256;
		// Output pixel height.
		uint32_t Height = 256;
		// Shape of the ramp.
		GradientType Type = GradientType::Linear;
		// Normalized start position.
		double StartX = 0;
		// Normalized start position.
		double StartY = 0;
		// Normalized end position.
		double EndX = 1;
		// Normalized end position.
		double EndY = 0;
	};
	// Wave layout.
	enum class WaveType : uint8_t {
		// Parallel bands along Degrees.
		Bands,
		// Concentric rings around the centre.
		Rings
	};
	// Shape of one wave period.
	enum class WaveProfile : uint8_t { Sine, Saw, Triangle };
	// Periodic bands or rings with optional noise distortion, in [0, 1].
	struct Wave {
		// Output pixel width.
		uint32_t Width = 256;
		// Output pixel height.
		uint32_t Height = 256;
		// Layout of the wave.
		WaveType Type = WaveType::Bands;
		// Shape of one period.
		WaveProfile Profile = WaveProfile::Sine;
		// Periods across one unit of position.
		double Scale = 4;
		// Band direction in degrees.
		double Degrees = 0;
		// Offset in periods.
		double Phase = 0;
		// Noise displacement in periods.
		double Distortion = 0;
		// Noise cells across one unit of position.
		double DetailScale = 2;
		// Noise octaves.
		uint32_t Detail = 2;
		// Selects an independent distortion lattice.
		uint32_t Seed = 0;
	};
	// Channel read as a scalar factor.
	enum class Channel : uint8_t { Red, Green, Blue, Alpha, Luminance };
	// How a ramp moves between stops.
	enum class RampInterpolation : uint8_t {
		// Straight line between neighbouring stops.
		Linear,
		// The nearest stop at or below the factor.
		Constant,
		// Smoothstep between neighbouring stops.
		Smooth
	};
	// One ramp stop.
	struct RampStop {
		// Factor in [0, 1]; stops are ascending.
		double Position = 0;
		// Straight-alpha RGBA8 colour.
		std::array<uint8_t, 4> Colour{0, 0, 0, 255};
		// Compares position and colour.
		bool operator==(const RampStop &) const = default;
	};
	// Maps a scalar factor through ascending colour stops.
	struct Ramp {
		// Channel of the input read as the factor.
		Channel Factor = Channel::Luminance;
		// Interpolation between stops.
		RampInterpolation Interpolation = RampInterpolation::Linear;
		// One to MaximumRampStops ascending stops.
		std::vector<RampStop> Stops{{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}};
	};
	// Channel bit set: red 1, green 2, blue 4, alpha 8. Serialized as text such as `rgb`.
	using ChannelSet = uint8_t;
	// Maps a range to another range with clamp and gamma on the selected channels.
	struct Levels {
		// Input value mapped to OutputMinimum.
		double InputMinimum = 0;
		// Input value mapped to OutputMaximum.
		double InputMaximum = 1;
		// Lowest output value.
		double OutputMinimum = 0;
		// Highest output value.
		double OutputMaximum = 1;
		// Exponent applied as t^(1/Gamma); must be positive.
		double Gamma = 1;
		// Clamp the normalized input to [0, 1] before gamma.
		bool Clamp = true;
		// Channels changed; others pass through.
		ChannelSet Channels = 7;
	};
	// Per-channel arithmetic.
	enum class MathOperation : uint8_t { Add, Subtract, Multiply, Divide, Minimum, Maximum, Power };
	// A op B per selected channel, where B is the second input or the constant Value.
	// Division by zero writes zero; Power raises max(A, 0). Results clamp to [0, 1].
	struct Math {
		// Arithmetic applied.
		MathOperation Operation = MathOperation::Multiply;
		// Constant B per channel when no second input is connected.
		std::array<double, 4> Value{1, 1, 1, 1};
		// Channels changed; others pass through from A.
		ChannelSet Channels = 15;
	};
	// Source of one merged channel.
	struct ChannelPick {
		// Input index, or -1 for constant zero and -2 for constant one.
		int8_t Input = 0;
		// Channel of that input, 0 red to 3 alpha.
		uint8_t Channel = 0;
		// Compares input and channel.
		bool operator==(const ChannelPick &) const = default;
	};
	// Splits and merges channels: each output channel picks any input channel or a constant.
	struct Channels {
		// Red, green, blue and alpha sources. Serialized as `0.r`, `1.g`, `zero` or `one`.
		std::array<ChannelPick, 4> Picks{{{0, 0}, {0, 1}, {0, 2}, {0, 3}}};
	};
	// One minus the value on the selected channels.
	struct Invert {
		// Channels inverted; others pass through.
		ChannelSet Channels = 7;
	};
	// Separable gaussian blur of premultiplied colour with clamped edges.
	struct Blur {
		// Kernel radius in pixels; sigma is Radius / 3. Zero copies.
		double Radius = 2;
	};
	// Converts a height channel to a tangent-space normal encoded as 0.5 + 0.5n.
	struct NormalMap {
		// Height gradient multiplier.
		double Strength = 1;
		// Channel read as height.
		Channel Height = Channel::Red;
		// Negate green for DirectX-style maps.
		bool FlipY = false;
	};
	// Offsets each pixel by the second input's red and green channels.
	struct Warp {
		// Pixels moved by a full-scale vector; 0.5 is no offset.
		double Strength = 8;
		// Filter used to sample the displaced position.
		Sampling Filter = Sampling::Bilinear;
	};
	// Rasterizes one float4 attribute channel of a host-supplied mesh into a UV tile.
	// Uncovered pixels are transparent; Padding dilates covered edges outwards.
	struct MeshAttribute {
		// Output pixel width.
		uint32_t Width = 1024;
		// Output pixel height.
		uint32_t Height = 1024;
		// Host mesh reference, such as `editable-mesh://42`.
		std::string Mesh = "editable-mesh://1";
		// Attribute channel, zero to three.
		uint32_t Attribute = 0;
		// One to three of `xyzw` written to red, green and blue; one component fills all three.
		std::string Components = "x";
		// UDIM tile column; UV u in [TileU, TileU + 1) maps to the tile.
		int32_t TileU = 0;
		// UDIM tile row.
		int32_t TileV = 0;
		// Attribute value mapped to zero.
		double Minimum = 0;
		// Attribute value mapped to one.
		double Maximum = 1;
		// Dilation in pixels.
		uint32_t Padding = 2;
	};
	// Addressing for coordinates outside [0, 1] in image.lookup.
	enum class LookupWrap : uint8_t {
		// Hold the nearest edge pixel.
		Clamp,
		// Tile the image.
		Repeat,
		// Write transparent pixels outside the image.
		Transparent
	};
	// Samples the first input at coordinates read from the second input's red and green.
	// The output takes the Vector input's extent. Red 0..1 spans the image left to right and
	// green 0..1 top to bottom, after Scale and Offset. Pixels whose Vector alpha is zero
	// stay transparent, so a mesh attribute's uncovered texels stay uncovered.
	struct Lookup {
		// Filter used between image pixels.
		Sampling Filter = Sampling::Bilinear;
		// Addressing outside the image.
		LookupWrap Wrap = LookupWrap::Clamp;
		// Coordinate multiplier, applied before Offset.
		double ScaleX = 1;
		double ScaleY = 1;
		// Coordinate offset, in image widths and heights.
		double OffsetX = 0;
		double OffsetY = 0;
	};
	// Every operation understood by the document compiler and evaluator, in kind order.
	using Operation = std::variant<
		Source,
		Solid,
		Resize,
		Crop,
		Transform,
		Flip,
		Blend,
		Noise,
		Voronoi,
		Gradient,
		Wave,
		Ramp,
		Levels,
		Math,
		Channels,
		Invert,
		Blur,
		NormalMap,
		Warp,
		MeshAttribute,
		Lookup>;

	// One authored operation and its links to upstream nodes.
	struct Node {
		// Durable document-local node identifier.
		std::string Id;
		// Operation and its parameters.
		Operation Value = Source{};
		// Upstream node identifiers, in operation input order.
		std::vector<std::string> Inputs;
		// Editor position, not used during evaluation.
		std::array<double, 2> Position{};
	};
	// A named image result selected for evaluation or runtime publication.
	struct Output {
		// Durable output name.
		std::string Name;
		// Identifier of the node that produces this result.
		std::string Node;
		// Declared output interpretation; does not convert pixel bytes.
		OutputSpace Space = OutputSpace::SRGB;
		// Byte layout of the evaluated output.
		OutputFormat Format = OutputFormat::RGBA8;
	};
	// Values supported by authored parameters and resolved input overrides.
	using InputValue = std::variant<double, bool, std::array<uint8_t, 4>, std::string>;
	// A named external value with its authored fallback.
	struct Parameter {
		// Durable parameter name.
		std::string Name;
		// Value used when no runtime override is supplied.
		InputValue Default;
	};
	// Connects a named parameter to one supported node property.
	struct Binding {
		// Identifier of the node receiving the value.
		std::string Node;
		// Stable operation property key, such as `path` or `opacity`.
		std::string Property;
		// Name of the parameter supplying this property.
		std::string Input;
	};
	// One per-instance value that replaces a parameter's authored default.
	struct InputOverride {
		// Name of the declared parameter to replace.
		std::string Name;
		// Replacement value, which must match the parameter's type.
		InputValue Value;
	};
	// Authored graph nodes, outputs and optional named input declarations.
	struct Document {
		// Operations in authored order.
		std::vector<Node> Nodes;
		// Named results available for selection.
		std::vector<Output> Outputs;
		// Defaults exposed to bindings and runtime overrides.
		std::vector<Parameter> Parameters{};
		// Connections from parameters to node properties.
		std::vector<Binding> Bindings{};
		// Intermediate channel depth for every node.
		Precision Storage = Precision::Byte;
	};
	// Refusal details returned by parsing, compiling, planning or evaluation.
	struct Diagnostic {
		// Node identifier when a failure is associated with one.
		std::string Node;
		// Human-readable reason for refusing the request.
		std::string Message;
	};
	// Stable topological order and resolved edge indices for a document.
	struct Plan {
		// Node indices in evaluation order.
		std::vector<size_t> Order;
		// For each node, indices of its input nodes.
		std::vector<std::vector<size_t>> Inputs;
		// Node index for each authored output.
		std::vector<size_t> Outputs;
		// Compares the compiled node and edge mappings.
		bool operator==(const Plan &) const = default;
	};

	// Known dimensions for a source node during output planning.
	struct SourceExtent {
		// Identifier of the source node.
		std::string Node;
		// Resolved source pixel width.
		uint32_t Width = 0;
		// Resolved source pixel height.
		uint32_t Height = 0;
	};
	// Planned storage requirements for one node image.
	struct ImageExtent {
		// Planned image width in pixels.
		uint32_t Width = 0;
		// Planned image height in pixels.
		uint32_t Height = 0;
		// Exact intermediate storage size: four bytes per pixel, or eight when wide.
		size_t Bytes = 0;
		// Compares the planned dimensions and byte count.
		bool operator==(const ImageExtent &) const = default;
	};
	// Bounded execution data for one selected output and its reachable nodes.
	struct ExecutionPlan {
		// Reachable node indices in evaluation order.
		std::vector<size_t> Order;
		// Planned dimensions for each document node; omitted nodes have zero extents.
		std::vector<ImageExtent> Extents;
		// Document node index that produces the selected output.
		size_t Target = 0;
		// Index in Document::Outputs of the requested output.
		size_t Output = 0;
		// Total retained intermediate storage across reachable nodes.
		size_t RetainedBytes = 0;
		// Total pixel work for the selected dependency cone.
		uint64_t PixelWork = 0;
		// Compares the selected execution data and budgets.
		bool operator==(const ExecutionPlan &) const = default;
	};
	// The resolver owns decoding and must obey MaximumImageBytes before allocating.
	// A missing/invalid source returns false and sets its host diagnostic.
	using SourceResolver = std::function<bool(std::string_view, Image &, std::string &)>;

	// Receives the interpretation needed to normalize colour versus numeric textures.
	using TypedSourceResolver = std::function<bool(const Source &, Image &, std::string &)>;
	// Triangles and one attribute channel copied from a host mesh for rasterization.
	struct MeshAttributeData {
		// Per-vertex texture coordinates.
		std::vector<std::array<float, 2>> UVs;
		// Per-vertex float4 values of the selected attribute channel.
		std::vector<std::array<float, 4>> Values;
		// Triangle list; every index is below UVs.size() and Values.size().
		std::vector<uint32_t> Indices;
	};
	// Copies a mesh attribute node's triangle data. A missing mesh or channel returns
	// false with a host diagnostic.
	using MeshResolver = std::function<bool(const MeshAttribute &, MeshAttributeData &, std::string &)>;
	// Host callbacks for evaluation. A graph without mesh attribute nodes needs no Meshes.
	struct Resolvers {
		// Decodes image sources.
		TypedSourceResolver Sources;
		// Copies mesh attributes.
		MeshResolver Meshes;
	};
	// Rasterizes one attribute into straight RGBA float pixels in [0, 1], alpha one where
	// covered, Width * Height * 4 values. GPU hosts upload this as a data source.
	// Refusal preserves out.
	bool RasteriseMeshAttribute(
		const MeshAttribute &, const MeshAttributeData &, std::vector<float> &out, Diagnostic &
	);
	// How a host can edit or bind one authored property.
	enum class PropertyKind : uint8_t {
		// Integer pixel extent.
		Dimension,
		// Non-negative integer.
		Count,
		// Signed integer.
		Offset,
		// Finite real number.
		Number,
		// Boolean.
		Flag,
		// One of Choices.
		Choice,
		// RGBA8 colour.
		Colour,
		// Free text such as a source path.
		Text,
		// Ramp stops, as text `0:000000ff 1:ffffffff`.
		Stops,
		// Four numbers, as text `1 1 1 1`.
		Vector,
		// Channel set, as text such as `rgb`.
		ChannelSet,
		// Four channel picks, as text `0.r 0.g 0.b 0.a`.
		Picks
	};
	// One authored property of a node kind.
	struct PropertySpec {
		// Stable JSON and binding key.
		std::string Key;
		// Editing and binding shape.
		PropertyKind Kind = PropertyKind::Number;
		// Inclusive numeric bounds where they apply.
		double Minimum = 0;
		// Inclusive numeric bounds where they apply.
		double Maximum = 0;
		// Spellings of a Choice.
		std::vector<std::string> Choices;
		// Whether a parameter binding may replace it.
		bool Bindable = true;
	};
	// Properties of a kind in serialization order, as a live document carries them;
	// empty for an unknown kind.
	std::vector<PropertySpec> DescribeKind(std::string_view kind);
	// Default-constructed operation for a kind; false for an unknown kind.
	bool DefaultOperation(std::string_view kind, Operation &out);
	// Reads one property as an input value: numbers as double, flags as bool, colours as
	// RGBA8 and everything else as its text spelling. False for an unknown key.
	bool GetProperty(const Operation &, std::string_view key, InputValue &out);
	// Writes one property from the shape GetProperty returns. Ranges are checked by
	// Compile; a wrong type, non-integral integer or unparsable text returns false and
	// preserves the operation.
	bool SetProperty(Operation &, std::string_view key, const InputValue &value);
	// Smallest and largest connected input counts for a kind.
	bool InputRange(std::string_view kind, size_t &minimum, size_t &maximum);
	// Stable hash of an operation's kind and every property, for host caches. Never saved.
	uint64_t Fingerprint(const Operation &) noexcept;

	// Resolves defaults and named overrides to a validated snapshot with no bindings.
	// Invalid names, duplicate overrides, incompatible types or values preserve out.
	bool ResolveInputs(const Document &, std::span<const InputOverride>, Document &out, Diagnostic &);
	// Property uses the same stable control keys as project JSON, including path/filter.
	// Returns the InputValue variant index, or -1 for a property this node does not expose.
	int PropertyType(const Operation &, std::string_view property) noexcept;
	// Selected dependency cone, dimensions and budgets shared by CPU and GPU hosts.
	// Extents are indexed by document node; omitted nodes have zero extents.
	// Reachable sources require exact extents. Refusal preserves out.
	bool Prepare(
		const Document &,
		const Plan &,
		std::string_view output,
		std::span<const SourceExtent>,
		ExecutionPlan &out,
		Diagnostic &
	);
	// Evaluates one selected output with source and mesh resolvers. The same document,
	// sources and seeds always produce the same bytes. Refusal preserves out.
	bool EvaluateWith(
		const Document &, const Plan &, std::string_view output, const Resolvers &, Image &out, Diagnostic &
	);
	// Evaluates one selected output while passing source interpretation to its resolver.
	// Refusal preserves out.
	bool EvaluateTyped(
		const Document &,
		const Plan &,
		std::string_view output,
		const TypedSourceResolver &,
		Image &out,
		Diagnostic &
	);
	// Stable authored kind and sampling spellings used by serialization and hosts.
	std::string_view Kind(const Operation &operation) noexcept;
	// Returns the stable serialized name for a sampling filter.
	std::string_view SamplingName(Sampling sampling) noexcept;
	// Validates every authored node, including disconnected nodes, and rejects all cycles.
	// Refusal preserves the prior plan. Order is stable with respect to authored node order.
	bool Compile(const Document &document, Plan &out, Diagnostic &diagnostic);
	// Empty selection chooses the sole output; multiple outputs require an explicit name.
	// Only its dependency cone runs. Invalid plans, sources or budgets preserve out.
	// This legacy resolver accepts colour sources; data sources require EvaluateTyped.
	bool Evaluate(
		const Document &document,
		const Plan &plan,
		std::string_view output,
		const SourceResolver &sources,
		Image &out,
		Diagnostic &diagnostic
	);
	// Version 1 static, version 2 named-input and version 3 precision JSON projects.
	// Version 3 adds root `precision` and output `format`. Unknown fields/kinds/options are refused.
	// Read/Write preserve their destination on refusal; layout is retained verbatim.
	bool Read(std::string_view encoded, Document &out, Diagnostic &diagnostic);
	// Writes a canonical document encoding; refusal preserves out.
	bool Write(const Document &document, std::string &out, Diagnostic &diagnostic);
}
