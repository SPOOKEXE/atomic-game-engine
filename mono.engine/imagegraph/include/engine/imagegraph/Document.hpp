#pragma once

// Bounded static 2D image composition. Hosts supply decoded source pixels.
// Pixels are top-row-first, straight-alpha RGBA8 with encoded sRGB channels.
// Normal source-over blends encoded channels. Bilinear sampling weights
// premultiplied encoded channels, then returns straight RGBA8.
// Durable node, kind, source and output identities are strings; plans are derived.
// @tier L9 · shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace engine::imagegraph {
	struct Limits {
		static constexpr size_t MaximumDocumentBytes = 1024 * 1024;
		static constexpr size_t MaximumNodes = 256;
		static constexpr size_t MaximumOutputs = 16;
		static constexpr uint32_t MaximumDimension = 4096;
		static constexpr size_t MaximumImageBytes = 64 * 1024 * 1024;
		static constexpr size_t MaximumRetainedBytes = 256 * 1024 * 1024;
		static constexpr uint64_t MaximumPixelWork = 64 * 1024 * 1024;
		static constexpr size_t MaximumJsonDepth = 16;
	};

	struct Image {
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<std::byte> Pixels;
		bool IsValid() const noexcept;
	};

	enum class Sampling : uint8_t { Nearest, Bilinear };
	struct Source {
		std::string Path;
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
	};
	struct Document {
		std::vector<Node> Nodes;
		std::vector<Output> Outputs;
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

	// The resolver owns decoding and must obey MaximumImageBytes before allocating.
	// A missing/invalid source returns false and sets its host diagnostic.
	using SourceResolver = std::function<bool(std::string_view, Image &, std::string &)>;

	// Stable authored kind and sampling spellings used by serialization and hosts.
	std::string_view Kind(const Operation &operation) noexcept;
	std::string_view SamplingName(Sampling sampling) noexcept;
	// Validates every authored node, including disconnected nodes, and rejects all cycles.
	// Refusal preserves the prior plan. Order is stable with respect to authored node order.
	bool Compile(const Document &document, Plan &out, Diagnostic &diagnostic);
	// Empty selection chooses the sole output; multiple outputs require an explicit name.
	// Only its dependency cone runs. Invalid plans, sources or budgets preserve out.
	bool Evaluate(
		const Document &document,
		const Plan &plan,
		std::string_view output,
		const SourceResolver &sources,
		Image &out,
		Diagnostic &diagnostic
	);
	// Version 1 JSON projects. Unknown fields/kinds/options are refused.
	// Read/Write preserve their destination on refusal; layout is retained verbatim.
	bool Read(std::string_view encoded, Document &out, Diagnostic &diagnostic);
	bool Write(const Document &document, std::string &out, Diagnostic &diagnostic);
}
