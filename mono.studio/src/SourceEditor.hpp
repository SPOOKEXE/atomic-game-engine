#pragma once

// World-backed source documents the Studio code editor can open.
//
// Program text lives in `script::SourceCache`, while shader text lives on a
// `scene::ShaderSource` row and must be written through `SetShaderSource` so its
// revision advances. This adapter keeps that storage difference out of the
// panel and gives every way into the editor one answer to "is this source?".
//
// @tier client

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace studio {

	// Which storage owns an open source document.
	//
	// @since v0.21
	enum class SourceDocumentKind : uint8_t {
		// A Luau or JavaScript program in the world's source cache.
		Program,

		// A GLSL fragment program on a `ShaderScript` instance.
		Shader,
	};

	// A source document copied out of its world for editing.
	//
	// @since v0.21
	struct SourceDocument {
		// Which storage receives the edited text.
		SourceDocumentKind Kind = SourceDocumentKind::Program;

		// The program's asset-relative path. Invalid for a shader, whose instance
		// is its identity.
		engine::core::Name Path;

		// The editable source text.
		std::string Text;
	};

	// Reports which source document an instance carries.
	//
	// @return The kind, or nothing when the instance is not editable source.
	// @since v0.21
	std::optional<SourceDocumentKind>
	SourceDocumentKindOf(const engine::ecs::Store &store, engine::ecs::Entity instance);

	// Copies an instance's active source into an editor document.
	//
	// A program whose path is missing, or whose file does not exist yet, is an
	// empty legal document. A shader's text comes directly from its component.
	//
	// @return The document, or nothing when the instance is not editable source.
	// @since v0.21
	std::optional<SourceDocument> ReadSourceDocument(engine::ecs::Store &store, engine::ecs::Entity instance);

	// Writes an edited document back through its storage owner's setter.
	//
	// An unsaved program receives a path under `Scripts/`; a shader advances its
	// revision through `scene::SetShaderSource`.
	//
	// @param store    The owning world.
	// @param instance The source instance.
	// @param kind     Which storage owns the text.
	// @param path     The program path, filled when an unsaved program is named.
	// @param text     The edited source.
	// @return `false` when the instance is gone or does not match `kind`.
	// @since v0.21
	bool WriteSourceDocument(
		engine::ecs::Store &store,
		engine::ecs::Entity instance,
		SourceDocumentKind kind,
		engine::core::Name &path,
		std::string_view text
	);

}
