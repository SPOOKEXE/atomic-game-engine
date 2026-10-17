#pragma once

// The checked, engine-neutral interchange description for authored GUI trees.
//
// This is deliberately a description rather than a parser. An adapter for a
// design tool first produces this bounded form. Import validates it before
// creating instances and rolls back newly created roots if a write is refused.
// Keeping that seam here lets Figma, Roblox, and template adapters agree.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/gui/Style.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// The canonical document version accepted by this engine build.
	inline constexpr uint32_t UI_DOCUMENT_VERSION = 3;

	// Hard limits checked before an adapter reserves, recurses, or mutates a Store.
	struct DocumentLimits {
		// Absolute cap on imported nodes.
		static constexpr uint32_t HARD_MAXIMUM_NODES = 4096;
		// Absolute cap on document tree depth.
		static constexpr uint32_t HARD_MAXIMUM_DEPTH = 64;
		// Absolute cap on children per node.
		static constexpr uint32_t HARD_MAXIMUM_CHILDREN = 1024;
		// Absolute cap on properties per node.
		static constexpr uint32_t HARD_MAXIMUM_PROPERTIES = 64;
		// Absolute cap on one UTF-8 string.
		static constexpr uint32_t HARD_MAXIMUM_STRING_BYTES = 65536;
		// Absolute cap on preserved extension values.
		static constexpr uint32_t HARD_MAXIMUM_EXTENSIONS = 64;
		// Absolute cap on encoded components per node.
		static constexpr uint32_t HARD_MAXIMUM_COMPONENTS = 16;
		// Absolute cap on encoded component bytes.
		static constexpr uint32_t HARD_MAXIMUM_COMPONENT_BYTES = 1024 * 1024;
		// Absolute cap on document theme resources.
		static constexpr uint32_t HARD_MAXIMUM_THEMES = 64;
		// Absolute cap on reported validation problems.
		static constexpr uint32_t HARD_MAXIMUM_ISSUES = 64;
		// Absolute cap on the whole encoded document.
		static constexpr uint32_t HARD_MAXIMUM_BINARY_BYTES = 16 * 1024 * 1024;

		// Allowed number of document nodes.
		uint32_t MaximumNodes = 4096;
		// Allowed maximum document tree depth.
		uint32_t MaximumDepth = 64;
		// Allowed children per document node.
		uint32_t MaximumChildren = 1024;
		// Allowed properties per document node.
		uint32_t MaximumProperties = 64;
		// Allowed bytes in one UTF-8 string.
		uint32_t MaximumStringBytes = 65536;
		// Allowed preserved extension values.
		uint32_t MaximumExtensions = 64;
		// Allowed encoded components per node.
		uint32_t MaximumComponents = HARD_MAXIMUM_COMPONENTS;
		// Allowed bytes in one encoded component.
		uint32_t MaximumComponentBytes = HARD_MAXIMUM_COMPONENT_BYTES;
		// Allowed document theme resources.
		uint32_t MaximumThemes = 64;
		// Allowed bytes in the complete encoded document.
		uint32_t MaximumBinaryBytes = HARD_MAXIMUM_BINARY_BYTES;
	};

	// One typed property. References carry a document-local stable id rather
	// than an ecs::Entity, whose number has no meaning in another world.
	struct DocumentProperty {
		// Stable property name.
		std::string Name;
		// Declared type of Value.
		ecs::PropertyType Type = ecs::PropertyType::Opaque;
		// Typed property payload.
		ecs::AttributeValue Value;
		// Document-local id when Type is a reference.
		std::string Reference;
		// One-based source line reported by an adapter.
		uint32_t SourceLine = 0;
	};

	// An unknown source property retained only by adapters that can round-trip it.
	struct DocumentExtension {
		// Unknown source property name.
		std::string Name;
		// Adapter-owned round-trip payload.
		std::string Payload;
		// One-based source line reported by an adapter.
		uint32_t SourceLine = 0;
	};

	// One authored component whose state is not represented by the public
	// property surface. Bytes use that component's registered stable codec.
	struct DocumentComponent {
		// Stable registered component name.
		std::string Name;
		// Component payload in its registered codec format.
		std::vector<std::byte> Bytes;
	};

	// One authored node. Id is a document-local identity. Adapters may preserve
	// their own stable id; ExportDocument only guarantees it within one export.
	struct DocumentNode {
		// Document-local stable node identity.
		std::string Id;
		// Registered GUI class name.
		std::string Class;
		// Authored instance name.
		std::string Name;
		// Declared instance properties.
		std::vector<DocumentProperty> Properties;
		// Preserved adapter-specific values.
		std::vector<DocumentExtension> Extensions;
		// Encoded authored components.
		std::vector<DocumentComponent> Components;
		// Child nodes in authored order.
		std::vector<DocumentNode> Children;
	};

	// A theme is a document resource rather than a GUI tree node. A collector
	// references it by Id through its Theme property, so a reusable theme can
	// live outside the exported visual subtree without losing its tokens.
	struct DocumentTheme {
		// Document-local stable theme identity.
		std::string Id;
		// Authored theme name.
		std::string Name;
		// Typed token values in this theme.
		StyleSet Tokens;
	};

	// The canonical format passed between adapters and the engine.
	struct UiDocument {
		// Wire-format version.
		uint32_t Version = UI_DOCUMENT_VERSION;
		// Root GUI nodes in authored order.
		std::vector<DocumentNode> Roots;
		// Shared document theme resources.
		std::vector<DocumentTheme> Themes;
	};

	// A local, source-addressable validation failure.
	struct DocumentIssue {
		// Source path to the invalid value.
		std::string Path;
		// Human-readable validation failure.
		std::string Message;
		// One-based source line when available.
		uint32_t SourceLine = 0;
	};

	// Bounded validation output for a document operation.
	struct DocumentReport {
		// Reported validation or conversion failures.
		std::vector<DocumentIssue> Issues;
		// Whether the issue limit stopped reporting.
		bool Truncated = false;

		// Whether no problems were reported.
		bool Ok() const {
			return Issues.empty();
		}
	};

	// Validates the complete document without touching a Store.
	bool
	ValidateDocument(const UiDocument &document, DocumentReport &report, const DocumentLimits &limits = {});

	// Writes the complete document as one bounded, little-endian interchange
	// value. Validation happens before bytes are appended to writer.
	bool EncodeDocument(
		const UiDocument &document,
		core::ByteWriter &writer,
		DocumentReport &report,
		const DocumentLimits &limits = {}
	);

	// Reads exactly one canonical document. On failure document is left alone.
	bool DecodeDocument(
		core::ByteReader &reader,
		UiDocument &document,
		DocumentReport &report,
		const DocumentLimits &limits = {}
	);

	// Creates the document's GUI trees and returns their roots in document
	// order. Validation and the creation plan precede all writes. A later Store
	// refusal destroys every newly created node and leaves preexisting instances
	// and the caller's roots untouched.
	bool ImportDocument(
		ecs::Store &store,
		const UiDocument &document,
		std::vector<ecs::Entity> &roots,
		DocumentReport &report,
		const DocumentLimits &limits = {},
		std::vector<ecs::Entity> *themes = nullptr
	);

	// Reads and validates a document before passing it to the Store import
	// transaction. A decoding failure leaves roots and store untouched.
	bool DecodeAndImportDocument(
		ecs::Store &store,
		core::ByteReader &reader,
		std::vector<ecs::Entity> &roots,
		DocumentReport &report,
		const DocumentLimits &limits = {},
		std::vector<ecs::Entity> *themes = nullptr
	);

	// Exports one GUI subtree with stable class and property names. Generated node
	// ids and references are stable within this one exported document only.
	// ThemeBinding targets become document theme resources; any other reference
	// outside the exported subtree makes the export fail.
	bool
	ExportDocument(const ecs::Store &store, ecs::Entity root, UiDocument &document, DocumentReport &report);

}
