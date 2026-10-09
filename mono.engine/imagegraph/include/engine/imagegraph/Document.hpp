#pragma once

// Bounded 2D image composition and pure named-input snapshots. Hosts supply pixels.
// Pixels are top-row-first, straight-alpha RGBA8. Colour sources use encoded sRGB
// channel values; data sources preserve numeric channel values. Operations blend
// stored channels and bilinear sampling weights premultiplied values. Output Space
// declares storage intent without transforming those values.
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
		static constexpr size_t MaximumRetainedBytes = 256 * 1024 * 1024;
		// Largest total pixel work allowed for one selected output.
		static constexpr uint64_t MaximumPixelWork = 64 * 1024 * 1024;
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
	// Owned top-row-first RGBA8 pixels produced by one bounded evaluation.
	struct Image {
		// Pixel width.
		uint32_t Width = 0;
		// Pixel height.
		uint32_t Height = 0;
		// Straight-alpha RGBA8 bytes in row-major order.
		std::vector<std::byte> Pixels;
		// Declared interpretation of the RGB bytes.
		OutputSpace Space = OutputSpace::SRGB;
		// Checks the dimensions, exact byte extent and declared colour space.
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
	// Composites the second input over the first with a uniform opacity.
	struct Blend {
		// Opacity applied to the foreground input, from zero to one.
		double Opacity = 1;
	};
	// The seven operations understood by the document compiler and evaluator.
	using Operation = std::variant<Source, Solid, Resize, Crop, Transform, Flip, Blend>;

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
		// Exact RGBA8 storage size.
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
		// Total retained RGBA8 storage across reachable nodes.
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
	// Version 1 static and version 2 named-input JSON projects. Unknown fields/kinds/options are refused.
	// Read/Write preserve their destination on refusal; layout is retained verbatim.
	bool Read(std::string_view encoded, Document &out, Diagnostic &diagnostic);
	// Writes a canonical document encoding; refusal preserves out.
	bool Write(const Document &document, std::string &out, Diagnostic &diagnostic);
}
