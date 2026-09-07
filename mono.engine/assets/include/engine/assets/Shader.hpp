#pragma once

// Cooked shader transport and reflection metadata. Structural validity does not
// certify SPIR-V, resource contracts, driver compatibility or safe execution.
// @tier L8 shared

#include <engine/assets/ContentHash.hpp>
#include <engine/core/Bytes.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::assets {

	// One canonical named feature value. Interpretation belongs to the cook profile.
	struct ShaderFeature {
		// Stable declaration name; collection order is bytewise lexical name order.
		std::string Name;
		// Canonical feature text or little-endian specialization bytes, according to the declaration.
		std::string Value;
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderFeature &) const = default;
	};

	// A source/include/publication dependency, bound to its content root.
	struct ShaderDependency {
		// Stable declaration name; collection order is bytewise lexical name order.
		std::string Name;
		// Verified publication content root, not a digest of concatenated asset bytes.
		ContentHash Root;
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderDependency &) const = default;
	};

	// A typed specialization value with canonical little-endian bytes.
	struct ShaderSpecialization {
		// Stable declaration name; collection order is bytewise lexical name order.
		std::string Name;
		// Closed scalar/vector/matrix type spelling; unsupported active types are refused.
		std::string Type;
		// Module specialization constant identifier, unique within the variant.
		uint32_t ConstantId = 0;
		// Canonical feature text or little-endian specialization bytes, according to the declaration.
		std::vector<std::byte> Value;
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderSpecialization &) const = default;
	};

	// A reflected descriptor or push-constant block; no backend index is inferred here.
	struct ShaderResource {
		// Stable declaration name; collection order is bytewise lexical name order.
		std::string Name;
		// Descriptor category or push-constants, serialized by its stable spelling.
		std::string Kind;
		// Declared descriptor set; mapping it to a device is admission work.
		uint32_t Set = 0;
		// Binding within the declared set.
		uint32_t Binding = 0;
		// Declared read, write or read-write access.
		std::string Access;
		// Declared image shape, or none for a non-image resource.
		std::string Dimension;
		// Declared storage/sample format, empty when the interface does not constrain one.
		std::string Format;
		// Minimum bytes the declared buffer block requires.
		uint64_t MinimumBytes = 0;
		// Static descriptor-array length; zero exactly when RuntimeArray is true.
		uint32_t DescriptorCount = 1;
		// Whether descriptor-array length is chosen at admission.
		bool RuntimeArray = false;
		// Image sample scalar kind: none, float, int, uint or depth.
		std::string SampleType = "none";
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderResource &) const = default;
	};

	// A material uniform member. Offsets are relative to the named resource block.
	struct ShaderParameter {
		// Stable declaration name; collection order is bytewise lexical name order.
		std::string Name;
		// Name of the resource block containing this parameter.
		std::string Resource;
		// Closed scalar/vector/matrix type spelling; unsupported active types are refused.
		std::string Type;
		// Member offset in bytes relative to its resource block.
		uint32_t Offset = 0;
		// Owned backend bytes or reflected member extent, as declared by the enclosing type.
		uint32_t Bytes = 0;
		// Required power-of-two alignment of the member offset.
		uint32_t Alignment = 1;
		// Declared member element count; one also represents a non-array.
		uint32_t ArrayCount = 1;
		// Byte stride between array elements; zero for a non-array.
		uint32_t ArrayStride = 0;
		// Byte stride between matrix columns, or rows when RowMajor is true.
		uint32_t MatrixStride = 0;
		// Whether matrix stride advances rows rather than columns.
		bool RowMajor = false;
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderParameter &) const = default;
	};

	// One stage input/output. Type spells its complete scalar/vector/matrix shape.
	struct ShaderInterfaceVariable {
		// Stable declaration name; collection order is bytewise lexical name order.
		std::string Name;
		// Closed scalar/vector/matrix type spelling; unsupported active types are refused.
		std::string Type;
		// First stage-interface location.
		uint32_t Location = 0;
		// First occupied scalar component within each location.
		uint32_t Component = 0;
		// Occupied location range, including matrix columns and array elements.
		uint32_t LocationCount = 1;
		// Declared smooth, flat or noperspective interpolation.
		std::string Interpolation = "smooth";
		// Whether interpolation samples the covered centroid.
		bool Centroid = false;
		// Whether interpolation is evaluated per sample.
		bool Sample = false;
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderInterfaceVariable &) const = default;
	};

	// A cook pass report, retained in execution order rather than sorted by name.
	struct ShaderOptimization {
		// Stable declaration name; collection order is bytewise lexical name order.
		std::string Name;
		// Static instruction count before this cook pass.
		uint32_t BeforeInstructions = 0;
		// Static instruction count after this cook pass.
		uint32_t AfterInstructions = 0;
		// Whether module words changed during this pass.
		bool Changed = false;
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderOptimization &) const = default;
	};

	// A backend form and its own entry point and target. Hashes cover these exact bytes on disk.
	struct ShaderPayload {
		// Payload form, spirv or msl; payloads sort lexically by this field.
		std::string Backend;
		// Entry point belonging to these backend bytes.
		std::string EntryPoint;
		// Backend language/version target used to generate this payload.
		std::string Target;
		// Owned backend bytes or reflected member extent, as declared by the enclosing type.
		std::vector<std::byte> Bytes;
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderPayload &) const = default;
	};

	// One demanded compiled variant and the metadata its consumer must independently validate.
	struct ShaderVariant {
		// Stable declaration name; collection order is bytewise lexical name order.
		std::string Name;
		// Declared vertex, fragment or compute stage; checked against code later.
		std::string Stage;
		// Sorted unique static feature names and their values.
		std::vector<ShaderFeature> Features;
		// Sorted unique specialization names with unique module constant identifiers.
		std::vector<ShaderSpecialization> Specializations;
		// Sorted unique resource declarations, with no conflicting descriptor bindings.
		std::vector<ShaderResource> Resources;
		// Sorted uniform members; overlapping ranges within one block are refused.
		std::vector<ShaderParameter> Parameters;
		// Sorted stage input declarations.
		std::vector<ShaderInterfaceVariable> Inputs;
		// Sorted stage output declarations.
		std::vector<ShaderInterfaceVariable> Outputs;
		// Sorted unique capability spellings; device support is checked at admission.
		std::vector<std::string> RequiredCapabilities;
		// Declared local invocation count along X, one for non-compute stages.
		uint32_t WorkgroupX = 1;
		// Declared local invocation count along Y, one for non-compute stages.
		uint32_t WorkgroupY = 1;
		// Declared local invocation count along Z, one for non-compute stages.
		uint32_t WorkgroupZ = 1;
		// Static counts, never GPU time or a bound on executed instructions.
		//@{
		uint32_t Instructions = 0;
		// Static arithmetic instruction estimate.
		uint32_t ArithmeticInstructions = 0;
		// Static image and texture instruction estimate.
		uint32_t TextureInstructions = 0;
		// Static memory instruction estimate.
		uint32_t MemoryInstructions = 0;
		// Static control-flow instruction estimate.
		uint32_t ControlFlowInstructions = 0;
		//@}
		std::vector<ShaderOptimization> Optimizations;
		// One SPIR-V payload and optionally one MSL payload, sorted by backend spelling.
		std::vector<ShaderPayload> Payloads;
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderVariant &) const = default;
	};

	// Immutable cooked bytes; no process-local names, compiler objects or device handles.
	struct ShaderData {
		// Exact source compiler identity/version used for this cook.
		std::string CompilerVersion;
		// Exact optimizer identity/version, or an explicit none token.
		std::string OptimizerVersion;
		// Exact MSL translator identity/version, or an explicit none token.
		std::string TranslatorVersion;
		// Named shader interface and packing ABI.
		std::string ShaderAbi;
		// SPIR-V target environment requested by the cook.
		std::string TargetEnvironment;
		// Source language recorded by the cook, initially glsl.
		std::string SourceLanguage = "glsl";
		// Named demand/capability profile used when selecting variants.
		std::string CapabilityProfile = "portable";
		// Validation/cook policy identity, carried into incremental cache keys.
		std::string PolicyVersion = "1";
		// Explicit final or preview cook profile identity.
		std::string CookProfile = "final";
		// Canonical sorted compiler options used for this cook.
		std::vector<ShaderFeature> CompilerOptions;
		// Canonical sorted optimizer options used for this cook.
		std::vector<ShaderFeature> OptimizerOptions;
		// Canonical sorted translator options used for this cook.
		std::vector<ShaderFeature> TranslatorOptions;
		// Canonical sorted dependency names and verified publication roots.
		std::vector<ShaderDependency> Dependencies;
		// Nonempty sorted variant list; duplicate stage/feature/specialization selections refuse.
		std::vector<ShaderVariant> Variants;

		// Checks the container shape only. Successful parsing is not executable admission.
		bool IsValid() const;
		// Compares all authored metadata and owned bytes.
		bool operator==(const ShaderData &) const = default;
	};

	// Canonical ASH1 container. Limits bound both serialized bytes and parsed collection sizes.
	class Shader {
	  public:
		// Little-endian ASH1 signature.
		static constexpr uint32_t MAGIC = 0x31485341;
		// Container encoding version; existing versions are never reinterpreted.
		static constexpr uint16_t VERSION = 1;
		// Maximum encoded container bytes, including its length-delimited header.
		static constexpr size_t MAXIMUM_BYTES = 64 * 1024 * 1024;
		// Maximum bytes in one backend payload.
		static constexpr size_t MAXIMUM_PAYLOAD_BYTES = 16 * 1024 * 1024;
		// Maximum demanded variants in one module bundle.
		static constexpr size_t MAXIMUM_VARIANTS = 256;
		// Maximum resources, members or variables in each interface collection.
		static constexpr size_t MAXIMUM_DECLARATIONS = 128;
		// Maximum options, dependencies, features, specializations or capabilities per collection.
		static constexpr size_t MAXIMUM_KEYS = 64;
		// Maximum retained cook pass reports per variant.
		static constexpr size_t MAXIMUM_OPTIMIZATIONS = 32;
		// Maximum bytes in any metadata string.
		static constexpr size_t MAXIMUM_NAME = 1024;

		// Appends one complete container; invalid input leaves the writer unchanged.
		static bool Write(core::ByteWriter &writer, const ShaderData &data);

		// Reads exactly one length-delimited container. Refusal leaves output unchanged and fails the reader.
		static bool Read(core::ByteReader &reader, ShaderData &out);

		// Hashes canonical stage/resource/member/input/output/local-size metadata, not code or labels.
		// Invalid interface structure returns the zero sentinel.
		static ContentHash InterfaceHash(const ShaderVariant &variant);
	};
}
