#pragma once

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>

#include <optional>
#include <variant>

namespace engine::imagegraphio {
	// Empty input records preserve constructor defaults according to source applyDeserialize.
	// Native execution may still report an unresolved default for a source-only node.
	struct PxcxNodeCreate {
		std::string NodeId;
		std::string SourceType;
		imagegraph::Vector2 Position;
	};
	// A complete source serializer record. This also admits groups and their boundary nodes.
	// Constructors with runtime defaults require an actual source record, not guessed values.
	struct PxcxNodeInsert {
		std::string RecordJson;
	};
	struct PxcxNodeClone {
		std::string TemplateId;
		std::string NodeId;
		imagegraph::Vector2 Position;
	};
	struct PxcxNodeDelete {
		std::string NodeId;
		// Group deletion includes its descendants when explicitly selected.
		bool IncludeGroupChildren = false;
	};
	struct PxcxSourceConnection {
		std::string NodeId;
		uint32_t OutputIndex = 0;
		// The source serializer retains output tags along with endpoint identity.
		std::optional<int64_t> Tag = std::nullopt;
	};
	struct PxcxLinkEdit {
		std::string NodeId;
		uint32_t InputIndex = 0;
		// Empty disconnects. The inactive authored value and unknown metadata survive.
		std::optional<PxcxSourceConnection> From;
	};
	struct PxcxDynamicInputInsert {
		std::string NodeId;
		uint32_t GroupIndex = 0;
		// One or more complete source input groups, as a JSON array of input records.
		std::string RecordsJson;
	};
	struct PxcxDynamicInputDelete {
		std::string NodeId;
		uint32_t GroupIndex = 0;
		uint32_t GroupCount = 1;
	};
	using PxcxStructureEdit = std::variant<
		PxcxNodeCreate,
		PxcxNodeInsert,
		PxcxNodeClone,
		PxcxNodeDelete,
		PxcxLinkEdit,
		PxcxDynamicInputInsert,
		PxcxDynamicInputDelete>;

	// Ordered source structural transaction. Creation may precede links in the same transaction.
	// Uses the pinned save version, retains all unaffected records, metadata and thumbnail bytes,
	// validates the container and native projection before publication, and preserves exact no-op bytes.
	// JSON payload and transaction work have bounded aggregate budgets. Failure never replaces out.
	// This proves native read/write acceptance, not licensed executable writer compatibility.
	[[nodiscard]] bool WritePxcxStructureEdits(
		const PxcxImport &imported,
		std::span<const std::byte> expectedSource,
		std::span<const PxcxStructureEdit> edits,
		std::vector<std::byte> &out,
		imagegraph::Diagnostic &diagnostic
	);

	// Saves an edited native projection back into its retained source archive. Source catalogue
	// node creation, links, dynamic sockets, positions, values, rendering attributes, source group
	// boundaries and existing source key tracks share one atomic write. Any field without a proved inverse
	// fails the final projection check.
	[[nodiscard]] bool WritePxcxProjection(
		const PxcxImport &imported,
		const imagegraph::Document &authored,
		imagegraph::FrameTime captureTime,
		std::vector<std::byte> &out,
		imagegraph::Diagnostic &diagnostic
	);
}
