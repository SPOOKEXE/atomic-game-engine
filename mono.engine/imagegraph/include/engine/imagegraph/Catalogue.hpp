#pragma once

// The reviewed source-backed node catalogue from the pinned public source, with typed inputs,
// outputs and source defaults.
//
// Catalogue types are named "pc.<node>". Each input is both an authored property (when its type has a
// document value) and a linkable input port with the same id. A catalogue node with no native executor
// still loads, saves, links and appears in editors; evaluating it reports UnsupportedExecution.

#include <engine/imagegraph/Document.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace engine::imagegraph {
	enum class SourceActualConnectability : uint8_t { Unknown, General };
	enum class SourceChoiceClamp : uint8_t { Unknown, Always, Default, Disabled };
	enum class SourceChoicesStatus : uint8_t { Unknown, Resolved };

	// Source widget rules. Unknown facts remain absent rather than taking a native default.
	struct CatalogueSourceBehavior {
		std::optional<bool> FractionalInterpolation;
		std::optional<bool> StrictSuggestion;
		SourceActualConnectability ActualConnectability = SourceActualConnectability::Unknown;
		SourceChoiceClamp ChoiceClamp = SourceChoiceClamp::Unknown;
	};
	struct CatalogueSourceChoice {
		int32_t SourceIndex = -1;
		std::string_view Label;
		bool Separator = false;
	};
	// Raw count includes separator slots. Entries retain their source indices.
	struct CatalogueSourceChoices {
		SourceChoicesStatus Status = SourceChoicesStatus::Unknown;
		std::optional<uint32_t> RawCount;
		std::span<const CatalogueSourceChoice> Entries;
	};

	// One source-declared input, in source declaration order.
	struct CatalogueInput {
		// Durable property and port id.
		std::string_view Id;
		// Source display name.
		std::string_view Name;
		// Pixel Composer input index, or -1 when the source computes it.
		int32_t SourceIndex = -1;
		// Source value constructor, such as "Slider" or "EScroll".
		std::string_view SourceKind;
		ValueType Type = ValueType::Any;
		// catalogue preview default in value text; surface expressions use a reference extent.
		std::string_view Default;
		// Enum labels separated by ';', empty when the source builds them at runtime.
		std::string_view Choices;
		// Accepted source array nesting before the processor selects one outer row.
		uint8_t ArrayDepth = 0;
		bool ArrayDepthKnown = true;
		std::optional<CatalogueSourceBehavior> SourceBehavior = std::nullopt;
		std::optional<CatalogueSourceChoices> SourceChoices = std::nullopt;
		// Source typeArray classification from declaration type/display, absent when unresolved.
		// This does not describe payload shape or processor ArrayDepth.
		std::optional<bool> SourceArrayClassification = std::nullopt;
		// preview defaults may substitute a project surface; only fixed pairs can seed local axes.
		bool SourceConstructorConstant = false;
	};

	// One source-declared output.
	struct CatalogueOutput {
		std::string_view Id;
		std::string_view Name;
		int32_t SourceIndex = -1;
		ValueType Type = ValueType::Any;
		// Initial source value in document ValueText, empty when unresolved. Its type may differ from Type.
		std::string_view ConstructorDefault = {};
		// Exact source constructor expression, including unresolved runtime objects.
		std::string_view ConstructorExpression = {};
	};

	struct CatalogueEntry {
		// Native node type, "pc.<node>".
		std::string_view Type;
		// Pixel Composer constructor name, such as "Node_Normal".
		std::string_view SourceNode;
		// Documentation title.
		std::string_view Title;
		// Documentation family, such as "filter" or "3d".
		std::string_view Family;
		// Source file inside the pinned checkout.
		std::string_view SourceFile;
		std::span<const CatalogueInput> Inputs;
		std::span<const CatalogueOutput> Outputs;
		// Source index of the first dynamic group, and inputs per group. Both are 0 without dynamic inputs.
		int32_t DynamicFixedLength = 0;
		int32_t DynamicGroupLength = 0;
		// Maximum number of dynamic groups, zero when source has no bounded family recipe.
		int32_t DynamicGroupLimit = 0;
		// One dynamic group. SourceIndex is the offset inside the group. A node instance declares
		// "<template id>_<group>" dynamic inputs, with group counted from 0.
		std::span<const CatalogueInput> DynamicTemplate;
		// Ports and authored properties derived from Inputs and Outputs.
		NodeSchema Schema;
	};

	// Every catalogue entry, sorted by source node name.
	std::span<const CatalogueEntry> Catalogue();
	// Finds an entry by native type.
	const CatalogueEntry *FindCatalogueEntry(std::string_view type);
	// Finds an entry by Pixel Composer constructor name.
	const CatalogueEntry *FindCatalogueSource(std::string_view sourceNode);
	// Finds an input by id.
	const CatalogueInput *FindCatalogueInput(const CatalogueEntry &entry, std::string_view id);
	// Finds an input by Pixel Composer input index.
	const CatalogueInput *FindCatalogueInputIndex(const CatalogueEntry &entry, int32_t sourceIndex);
	// Resolves a dynamic instance id "<template id>_<group>" to its template input and group number.
	const CatalogueInput *
	FindDynamicTemplate(const CatalogueEntry &entry, std::string_view id, size_t &group);
	// Accepts flat authored array leaves for known source processors and verified manual array inputs.
	bool
	CatalogueAuthoredArray(const CatalogueEntry &entry, const CatalogueInput &input, const ArrayValue &array);

	// Source enum numeric values; fractional doubles require verified source interpolation.
	bool CatalogueSourceEnumValue(const CatalogueInput &input, const Value &value);

	// Finite raw reals stored by verified ordinary source Int/ISlider and Bool/Active constructors.
	// Getter conversion is deferred until execution; legacy schemas and source attributes stay strict.
	bool CatalogueSourceRawValue(const CatalogueInput &input, const Value &value);

	// The typed source default of one input, or nullopt when the source computes it at runtime.
	std::optional<Value> CatalogueDefault(const CatalogueInput &input);
	// Number of enum labels, or 0 when the labels are not declared statically.
	size_t CatalogueChoiceCount(const CatalogueInput &input);
	// Whether the CPU evaluator has a native executor for this type.
	bool HasNativeExecutor(std::string_view type);
}
