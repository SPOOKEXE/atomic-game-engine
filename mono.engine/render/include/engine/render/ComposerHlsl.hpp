#pragma once

// CPU authoring contract for the pinned pc.hlsl paired surface program.
// Cooking is explicit and does not install a pipeline or invoke a render host.
// @tier L12 client
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace engine::assets {
	struct ShaderData;
}
namespace engine::render::hlsl {
	// Explicit authoring dependencies are borrowed only during assembly.
	struct Library {
		std::string_view Name;
		std::string_view Source;
	};
	enum class ArgumentKind : uint8_t { Float, Int, Vec2, Vec3, Vec4, Mat3, Mat4, Sampler2D, Color };
	// Source dynamic argument kinds use the pinned enum ordering.
	struct Argument {
		std::string Name;
		ArgumentKind Kind = ArgumentKind::Float;
		bool operator==(const Argument &) const = default;
	};
	// Text and declarations are borrowed only during assembly and cooking.
	struct Definition {
		std::string_view Vertex; // Source reads this socket but compiles its fixed vertex program.
		std::string_view Main;
		std::string_view Global;
		std::string_view Libraries;
		std::span<const Argument> Arguments;
	};
	// Owns assembled text and bounded missing-library warnings.
	struct Source {
		std::string Vertex;
		std::string Fragment;
		std::vector<std::string> MissingLibraries;
	};
	// Offsets and matrix layout are checked again against the accepted fragment.
	struct Member {
		std::string Name;
		ArgumentKind Kind = ArgumentKind::Float;
		uint32_t Offset = 0, Bytes = 0, MatrixStride = 0;
		bool RowMajor = false;
		bool operator==(const Member &) const = default;
	};
	// Owned executable words; the containing Program identifies the stage.
	struct Stage {
		std::vector<uint32_t> SpirV;
		bool operator==(const Stage &) const = default;
	};
	// Fixed vertex and authored fragment pair, with admitted reflected packing.
	struct Program {
		Stage Vertex, Fragment;
		std::vector<Member> Members;
		uint32_t UniformBytes = 0, SamplerCount = 0;
		std::vector<Argument> Arguments;
		bool operator==(const Program &) const = default;
	};
	// Source texture identity and the slot the selected backend actually reads.
	struct SamplerBinding {
		std::string SourceName;
		uint32_t SourceSlot = 0, BackendSlot = 0;
	};
	// Numbers are scalar/vector components or column-major matrix elements.
	// Strings and numbers are borrowed only for the duration of Pack.
	struct Value {
		std::string_view Name;
		std::span<const double> Numbers;
	};
	// All failures preserve the caller's previous output.
	// Uses the source fixed vertex program, library assembly order and generated Data block.
	std::optional<std::string> Assemble(
		const Definition &, std::span<const Library>, Source &, uint64_t maximumBytes = 128ull * 1024 * 1024
	);
	// Local HLSL compiler options leave the existing GLSL compiler configuration untouched.
	std::optional<std::string>
	Cook(const Definition &, const Source &, Program &, uint64_t maximumBytes = 128ull * 1024 * 1024);
	// Validates stage interfaces, descriptor families, authored argument kinds and stored metadata.
	std::optional<std::string> Admit(const Program &);
	// MSL compacts active resources; SPIR-V preserves declared descriptor slots.
	std::optional<std::string>
	SamplerBindings(const Program &, std::string_view backend, std::vector<SamplerBinding> &);
	// Packs finite float32/int32 arguments using the independently reflected layout.
	std::optional<std::string> Pack(const Program &, std::span<const Value>, std::vector<std::byte> &);
	// Writes explicit hlsl.vertex/hlsl.fragment ASH1 variants with caller-supplied compiler provenance.
	// This is transport metadata, not a device-compatibility or source-pixel-parity certificate.
	std::optional<std::string> BuildContainer(
		const Program &,
		std::string_view compilerIdentity,
		assets::ShaderData &,
		uint64_t maximumBytes = 128ull * 1024 * 1024
	);
	inline constexpr uint32_t MAXIMUM_ARGUMENTS = 64, MAXIMUM_SAMPLERS = 16;
	inline constexpr size_t MAXIMUM_SOURCE_BYTES = 1024 * 1024;
} // namespace engine::render::hlsl
