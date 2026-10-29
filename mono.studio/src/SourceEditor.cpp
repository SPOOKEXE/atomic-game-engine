#include "SourceEditor.hpp"

#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Classes;
	using engine::ecs::Entity;
	using engine::ecs::Store;

	namespace {
		std::optional<Name> NewSourcePath(Store &store, Entity instance) {
			const Name name = store.InstanceNameOf(instance);
			const std::string leaf = name.IsValid() ? std::string(name.Text()) : "Script";
			const char *extension =
				engine::script::ActiveLanguageOf(store, instance) == engine::script::Language::JavaScript
					? ".js"
					: ".luau";
			std::vector<Name> referenced;
			store.Each<const engine::script::LuaSourceContainer>([&](Entity, const auto &source) {
				if (source.Path.IsValid()) referenced.push_back(source.Path);
			});
			store.Each<const engine::script::JavaScriptSourceContainer>([&](Entity, const auto &source) {
				if (source.Path.IsValid()) referenced.push_back(source.Path);
			});
			const auto *cache = store.Resource<engine::script::SourceCache>();
			for (size_t suffix = 0;; ++suffix) {
				const std::string text =
					"Scripts/" + leaf + (suffix == 0 ? "" : "_" + std::to_string(suffix)) + extension;
				const Name candidate(text);
				if (std::find(referenced.begin(), referenced.end(), candidate) != referenced.end() ||
					(cache != nullptr && cache->Find(candidate) != nullptr))
					continue;
				std::error_code error;
				const bool exists = std::filesystem::exists(engine::core::Paths::Assets() / text, error);
				if (error) return std::nullopt;
				if (!exists) return candidate;
			}
		}
	}

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
			const auto available = NewSourcePath(store, instance);
			if (!available) return false;
			path = *available;
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
