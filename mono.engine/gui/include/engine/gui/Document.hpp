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
		static constexpr uint32_t HARD_MAXIMUM_NODES = 4096;
		static constexpr uint32_t HARD_MAXIMUM_DEPTH = 64;
		static constexpr uint32_t HARD_MAXIMUM_CHILDREN = 1024;
		static constexpr uint32_t HARD_MAXIMUM_PROPERTIES = 64;
		static constexpr uint32_t HARD_MAXIMUM_STRING_BYTES = 65536;
		static constexpr uint32_t HARD_MAXIMUM_EXTENSIONS = 64;
		static constexpr uint32_t HARD_MAXIMUM_COMPONENTS = 16;
		static constexpr uint32_t HARD_MAXIMUM_COMPONENT_BYTES = 1024 * 1024;
		static constexpr uint32_t HARD_MAXIMUM_THEMES = 64;
		static constexpr uint32_t HARD_MAXIMUM_ISSUES = 64;
		static constexpr uint32_t HARD_MAXIMUM_BINARY_BYTES = 16 * 1024 * 1024;

		uint32_t MaximumNodes = 4096;
		uint32_t MaximumDepth = 64;
		uint32_t MaximumChildren = 1024;
		uint32_t MaximumProperties = 64;
		uint32_t MaximumStringBytes = 65536;
		uint32_t MaximumExtensions = 64;
		uint32_t MaximumComponents = HARD_MAXIMUM_COMPONENTS;
		uint32_t MaximumComponentBytes = HARD_MAXIMUM_COMPONENT_BYTES;
		uint32_t MaximumThemes = 64;
		uint32_t MaximumBinaryBytes = HARD_MAXIMUM_BINARY_BYTES;
	};

	// One typed property. References carry a document-local stable id rather
	// than an ecs::Entity, whose number has no meaning in another world.
	struct DocumentProperty {
		std::string Name;
		ecs::PropertyType Type = ecs::PropertyType::Opaque;
		ecs::AttributeValue Value;
		std::string Reference;
		uint32_t SourceLine = 0;
	};

	// An unknown source property retained only by adapters that can round-trip it.
	struct DocumentExtension {
		std::string Name;
		std::string Payload;
		uint32_t SourceLine = 0;
	};

	// One authored component whose state is not represented by the public
	// property surface. Bytes use that component's registered stable codec.
	struct DocumentComponent {
		std::string Name;
		std::vector<std::byte> Bytes;
	};

	// One authored node. Id is a document-local identity. Adapters may preserve
	// their own stable id; ExportDocument only guarantees it within one export.
	struct DocumentNode {
		std::string Id;
		std::string Class;
		std::string Name;
		std::vector<DocumentProperty> Properties;
		std::vector<DocumentExtension> Extensions;
		std::vector<DocumentComponent> Components;
		std::vector<DocumentNode> Children;
	};

	// A theme is a document resource rather than a GUI tree node. A collector
	// references it by Id through its Theme property, so a reusable theme can
	// live outside the exported visual subtree without losing its tokens.
	struct DocumentTheme {
		std::string Id;
		std::string Name;
		StyleSet Tokens;
	};

	// The canonical format passed between adapters and the engine.
	struct UiDocument {
		uint32_t Version = UI_DOCUMENT_VERSION;
		std::vector<DocumentNode> Roots;
		std::vector<DocumentTheme> Themes;
	};

	// A local, source-addressable validation failure.
	struct DocumentIssue {
		std::string Path;
		std::string Message;
		uint32_t SourceLine = 0;
	};

	struct DocumentReport {
		std::vector<DocumentIssue> Issues;
		bool Truncated = false;

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
