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
		std::optional<Name> NewSourcePath(Store &store, Entity instance, bool javaScript) {
			const Name name = store.InstanceNameOf(instance);
			const std::string leaf = name.IsValid() ? std::string(name.Text()) : "Script";
			const char *extension = javaScript ? ".js" : ".luau";
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
			const auto available = NewSourcePath(
				store,
				instance,
				engine::script::ActiveLanguageOf(store, instance) == engine::script::Language::JavaScript
			);
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

	bool WriteProgramDocument(
		Store &store, Entity instance, bool javaScript, bool separate, Name &path, std::string_view text
	) {
		if (SourceDocumentKindOf(store, instance) != SourceDocumentKind::Program) return false;

		Name current;
		if (javaScript) {
			if (const auto *component = store.Get<engine::script::JavaScriptSourceContainer>(instance))
				current = component->Path;
		} else {
			if (const auto *component = store.Get<engine::script::LuaSourceContainer>(instance))
				current = component->Path;
		}
		if (current != path) return false;
		if (current.IsValid()) {
			store.Each<const engine::script::LuaSourceContainer>([&](Entity owner, const auto &source) {
				if (source.Path == current && (owner != instance || javaScript)) separate = true;
			});
			store.Each<const engine::script::JavaScriptSourceContainer>([&](Entity owner,
																			const auto &source) {
				if (source.Path == current && (owner != instance || !javaScript)) separate = true;
			});
		}
		Name destination = current;
		if (!destination.IsValid() || separate) {
			const auto available = NewSourcePath(store, instance, javaScript);
			if (!available) return false;
			destination = *available;
		}
		if (javaScript) {
			auto component = store.Get<engine::script::JavaScriptSourceContainer>(instance) == nullptr
								 ? engine::script::JavaScriptSourceContainer{}
								 : *store.Get<engine::script::JavaScriptSourceContainer>(instance);
			component.Path = destination;
			store.Set(instance, component);
		} else {
			auto component = store.Get<engine::script::LuaSourceContainer>(instance) == nullptr
								 ? engine::script::LuaSourceContainer{}
								 : *store.Get<engine::script::LuaSourceContainer>(instance);
			component.Path = destination;
			store.Set(instance, component);
		}
		if (store.Resource<engine::script::SourceCache>() == nullptr)
			store.SetResource(engine::script::SourceCache{});
		store.ResourceMutable<engine::script::SourceCache>()->Set(destination, text);
		path = destination;
		return true;
	}

	bool ReadOpenedSource(
		Store &store,
		Entity instance,
		bool shader,
		bool javaScript,
		Name path,
		std::string &text,
		bool &readable
	) {
		text.clear();
		readable = false;
		if (SourceDocumentKindOf(store, instance) !=
			(shader ? SourceDocumentKind::Shader : SourceDocumentKind::Program))
			return false;
		if (shader) {
			text = store.Get<engine::scene::ShaderSource>(instance)->Code;
			readable = true;
			return true;
		}
		Name current;
		if (javaScript) {
			if (const auto *source = store.Get<engine::script::JavaScriptSourceContainer>(instance))
				current = source->Path;
		} else {
			if (const auto *source = store.Get<engine::script::LuaSourceContainer>(instance))
				current = source->Path;
		}
		if (current != path) return false;
		if (!path.IsValid()) {
			readable = true;
			return true;
		}
		std::string error;
		readable = engine::script::ReadSource(store, path, text, error);
		return true;
	}

}
