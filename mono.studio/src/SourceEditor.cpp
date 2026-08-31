#include "SourceEditor.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Classes;
	using engine::ecs::Entity;
	using engine::ecs::Store;

	std::optional<SourceDocumentKind> SourceDocumentKindOf(const Store &store, const Entity instance) {
		if (!store.Alive(instance)) {
			return std::nullopt;
		}

		if (store.Get<engine::scene::ShaderSource>(instance) != nullptr) {
			return SourceDocumentKind::Shader;
		}

		const engine::ecs::ClassId program = Classes::Find(Name("LuaSourceContainer"));
		if (program.IsValid() && store.IsA(instance, program)) {
			return SourceDocumentKind::Program;
		}
		return std::nullopt;
	}

	std::optional<SourceDocument> ReadSourceDocument(Store &store, const Entity instance) {
		const std::optional<SourceDocumentKind> kind = SourceDocumentKindOf(store, instance);
		if (!kind.has_value()) {
			return std::nullopt;
		}

		SourceDocument document;
		document.Kind = *kind;

		if (*kind == SourceDocumentKind::Shader) {
			document.Text = store.Get<engine::scene::ShaderSource>(instance)->Code;
			return document;
		}

		document.Path = engine::script::ActiveSourceOf(store, instance);
		if (!document.Path.IsValid()) {
			return document;
		}

		std::string error;
		if (!engine::script::ReadSource(store, document.Path, document.Text, error)) {
			document.Text.clear();
		}
		return document;
	}

	bool WriteSourceDocument(
		Store &store,
		const Entity instance,
		const SourceDocumentKind kind,
		Name &path,
		const std::string_view text
	) {
		if (SourceDocumentKindOf(store, instance) != kind) {
			return false;
		}

		if (kind == SourceDocumentKind::Shader) {
			return engine::scene::SetShaderSource(store, instance, text);
		}

		if (!path.IsValid()) {
			const Name name = store.InstanceNameOf(instance);
			const std::string leaf = name.IsValid() ? std::string(name.Text()) : "Script";
			path = Name("Scripts/" + leaf + ".luau");
			engine::script::SetSourcePath(store, instance, path);
		}

		auto *cache = store.ResourceMutable<engine::script::SourceCache>();
		if (cache == nullptr) {
			store.SetResource(engine::script::SourceCache{});
			cache = store.ResourceMutable<engine::script::SourceCache>();
		}
		cache->Set(path, text);
		return true;
	}

}
