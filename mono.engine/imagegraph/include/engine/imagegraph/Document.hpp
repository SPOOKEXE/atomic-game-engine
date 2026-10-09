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
	struct Limits {
		static constexpr size_t MaximumDocumentBytes = 1024 * 1024;
		static constexpr size_t MaximumNodes = 256;
		static constexpr size_t MaximumOutputs = 16;
		static constexpr size_t MaximumParameters = 64;
		static constexpr size_t MaximumBindings = 256;
		static constexpr uint32_t MaximumDimension = 4096;
		static constexpr size_t MaximumImageBytes = 64 * 1024 * 1024;
		static constexpr size_t MaximumRetainedBytes = 256 * 1024 * 1024;
		static constexpr uint64_t MaximumPixelWork = 64 * 1024 * 1024;
		static constexpr size_t MaximumJsonDepth = 16;
	};

	enum class SourceInterpretation : uint8_t { Colour, Data };
	enum class OutputSpace : uint8_t { SRGB, Linear };
	struct Image {
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<std::byte> Pixels;
		OutputSpace Space = OutputSpace::SRGB;
		bool IsValid() const noexcept;
	};

	enum class Sampling : uint8_t { Nearest, Bilinear };
	struct Source {
		std::string Path;
		SourceInterpretation Interpretation = SourceInterpretation::Colour;
	};
	struct Solid {
		uint32_t Width = 1;
		uint32_t Height = 1;
		std::array<uint8_t, 4> Colour{255, 255, 255, 255};
	};
	struct Resize {
		uint32_t Width = 1;
		uint32_t Height = 1;
		Sampling Filter = Sampling::Nearest;
	};
	struct Crop {
		int32_t X = 0;
		int32_t Y = 0;
		uint32_t Width = 1;
		uint32_t Height = 1;
	};
	struct Transform {
		uint32_t Width = 1;
		uint32_t Height = 1;
		double TranslateX = 0;
		double TranslateY = 0;
		double ScaleX = 1;
		double ScaleY = 1;
		double Degrees = 0;
		double PivotX = 0;
		double PivotY = 0;
		Sampling Filter = Sampling::Nearest;
	};
	struct Flip {
		bool Horizontal = false;
		bool Vertical = false;
	};
	struct Blend {
		double Opacity = 1;
	};
	using Operation = std::variant<Source, Solid, Resize, Crop, Transform, Flip, Blend>;

	struct Node {
		std::string Id;
		Operation Value = Source{};
		std::vector<std::string> Inputs;
		std::array<double, 2> Position{};
	};
	struct Output {
		std::string Name;
		std::string Node;
		OutputSpace Space = OutputSpace::SRGB;
	};
	using InputValue = std::variant<double, bool, std::array<uint8_t, 4>, std::string>;
	struct Parameter {
		std::string Name;
		InputValue Default;
	};
	struct Binding {
		std::string Node;
		std::string Property;
		std::string Input;
	};
	struct InputOverride {
		std::string Name;
		InputValue Value;
	};
	struct Document {
		std::vector<Node> Nodes;
		std::vector<Output> Outputs;
		std::vector<Parameter> Parameters{};
		std::vector<Binding> Bindings{};
	};
	struct Diagnostic {
		std::string Node;
		std::string Message;
	};
	struct Plan {
		std::vector<size_t> Order;
		std::vector<std::vector<size_t>> Inputs;
		std::vector<size_t> Outputs;
		bool operator==(const Plan &) const = default;
	};

	struct SourceExtent {
		std::string Node;
		uint32_t Width = 0;
		uint32_t Height = 0;
	};
	struct ImageExtent {
		uint32_t Width = 0;
		uint32_t Height = 0;
		size_t Bytes = 0;
		bool operator==(const ImageExtent &) const = default;
	};
	struct ExecutionPlan {
		std::vector<size_t> Order;
		std::vector<ImageExtent> Extents;
		size_t Target = 0;
		size_t Output = 0;
		size_t RetainedBytes = 0;
		uint64_t PixelWork = 0;
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
	bool Write(const Document &document, std::string &out, Diagnostic &diagnostic);
}
