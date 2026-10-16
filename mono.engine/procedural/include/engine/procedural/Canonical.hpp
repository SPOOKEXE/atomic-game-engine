#pragma once

// Canonical recipe values and versioned bytes for the bounded placement slice.
// Digests are opaque here; the asset-owning host verifies them.
// @tier L3 · shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace engine::procedural {

	// Result of validating, encoding or decoding bounded procedural data.
	enum class Status : uint8_t {
		Ok,				  // Operation completed.
		InvalidInput,	  // An argument or field is invalid.
		InvalidRecipe,	  // Recipe structure or canonical bytes are invalid.
		InvalidCoverage,  // Source coverage is incomplete or inconsistent.
		MissingSource,	  // A required named source is absent.
		LimitExceeded,	  // A configured count or byte limit was exceeded.
		AllocationFailed, // Required storage could not be allocated.
		Canceled,		  // Generation stopped before publishing a result.
		StaleArtifact,	  // Decoded artifact key differs from the expected key.
	};

	// Identifies a refused field without copying unbounded authored content.
	struct Diagnostic {
		Status Code = Status::Ok; // Reason for refusal, or Ok on success.
		std::string Field{};	  // Bounded stable field or identity name.
	};

	// Opaque 256-bit content digest; the owning host must verify its provenance.
	struct Digest {
		std::array<uint8_t, 32> Bytes{}; // Digest bytes, not proof of source authenticity.
		// Compares digest bytes exactly.
		bool operator==(const Digest &) const = default;
	};

	// Fixed-size numeric parameter values encoded as finite floating-point numbers.
	struct Vector2 {
		std::array<double, 2> Values{}; // Components in authored order.
	};
	struct Vector3 {
		std::array<double, 3> Values{}; // Components in authored order.
	};
	struct Vector4 {
		std::array<double, 4> Values{}; // Components in authored order.
	};
	// Stable UTF-8 parameter text, preserved exactly without normalization.
	struct StableText {
		std::string Value{}; // Text value.
	};
	// Opaque bounded bytes carried as a recipe parameter.
	struct ByteBuffer {
		std::vector<uint8_t> Value{}; // Byte payload.
	};

	// Supported recipe parameter value forms.
	using ParameterData =
		std::variant<bool, int64_t, uint64_t, double, Vector2, Vector3, Vector4, StableText, ByteBuffer>;

	// A named, typed recipe value; names are stable across serialization boundaries.
	struct Parameter {
		std::string Name{};	   // Stable parameter identity.
		ParameterData Value{}; // Authored parameter value.
	};

	// An operation in a recipe, identified independently of declaration order.
	struct OperationNode {
		std::string Id{};					 // Stable node identity.
		std::string Kind{};					 // Stable operation kind.
		std::vector<Parameter> Parameters{}; // Named values owned by this node.
	};

	// A typed connection between named operation nodes and ports.
	struct OperationLink {
		std::string FromNode{}; // Stable source node identity.
		std::string FromPort{}; // Stable source port name.
		std::string ToNode{};	// Stable destination node identity.
		std::string ToPort{};	// Stable destination port name.
	};

	// Authored procedural definition with stable names and an explicit schema version.
	struct Recipe {
		std::string Name{};					 // Stable recipe identity.
		std::string Generator{};			 // Stable generator kind.
		uint32_t SchemaVersion = 0;			 // Generator parameter schema version.
		uint64_t RootSeed = 0;				 // Root for named deterministic random streams.
		std::vector<Parameter> Parameters{}; // Generator-level named values.
		std::vector<OperationNode> Nodes{};	 // Authored operations.
		std::vector<OperationLink> Links{};	 // Typed operation connections.
	};

	// Hard ceilings applied before canonical serialization allocates output storage.
	struct CanonicalLimits {
		size_t MaximumParameters = 64;	 // Maximum values per named collection.
		size_t MaximumNodes = 8;		 // Maximum operation nodes.
		size_t MaximumLinks = 16;		 // Maximum typed operation links.
		size_t MaximumNameBytes = 128;	 // Maximum bytes in each stable name.
		size_t MaximumValueBytes = 4096; // Maximum encoded bytes in one value.
		size_t MaximumBytes = 32768;	 // Maximum complete canonical recipe size.
	};

	// Stable names are nonempty UTF-8 without control characters. No normalization
	// is applied, so callers must preserve the same spelling across boundaries.
	bool ValidStableName(std::string_view name);
	// Orders stable names by unsigned UTF-8 bytes for canonical output.
	bool StableNameLess(std::string_view left, std::string_view right);

	// Emits REC1, fixed-width little-endian fields, stable text type tags,
	// sorted named parameters, authored operations and typed links.
	// Finite floating values are required; negative zero has positive-zero bytes.
	// Refusal preserves out and gives a bounded diagnostic field.
	Status EncodeRecipe(
		const Recipe &recipe,
		std::vector<uint8_t> &out,
		Diagnostic &diagnostic,
		const CanonicalLimits &limits = {}
	);

	// Accepts only canonical REC1 bytes within the same hard limits. Unknown
	// tags, trailing bytes, non-finite values and noncanonical ordering refuse.
	// Refusal preserves out.
	Status DecodeRecipe(
		std::span<const uint8_t> bytes,
		Recipe &out,
		Diagnostic &diagnostic,
		const CanonicalLimits &limits = {}
	);
}
