#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Services.hpp>
#include <engine/scene/ImageGraph.hpp>
#include <engine/script/PlayerGui.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::script {
	namespace {
		using ecs::Entity;
		using ecs::Store;

		struct CloneNode {
			Entity Source;
			Entity Copy;
		};

		bool CollectClonePairs(Store &store, Entity source, Entity copy, std::vector<CloneNode> &pairs) {
			pairs.push_back({source, copy});
			std::vector<Entity> sourceChildren;
			std::vector<Entity> copyChildren;
			store.EachChild(source, [&](Entity child) {
				if (store.Get<ecs::NotArchivable>(child) == nullptr) sourceChildren.push_back(child);
			});
			store.EachChild(copy, [&](Entity child) { copyChildren.push_back(child); });
			if (sourceChildren.size() != copyChildren.size()) return false;
			for (size_t index = 0; index < sourceChildren.size(); ++index)
				if (!CollectClonePairs(store, sourceChildren[index], copyChildren[index], pairs))
					return false;
			return true;
		}

		std::string CopyGraphKey(std::string_view oldKey, Entity copy, uint32_t attempt) {
			std::string key(oldKey.substr(0, 96));
			key += "-pg-" + std::to_string(copy.Id);
			if (attempt != 0) key += "-" + std::to_string(attempt);
			return key;
		}

		bool RemapReference(std::string &value, const std::unordered_map<std::string, std::string> &keys) {
			constexpr std::string_view prefix = "imagegraph-instance://";
			if (!value.starts_with(prefix)) return true;
			const size_t hash = value.find('#', prefix.size());
			if (hash == std::string::npos) return true;
			const auto found = keys.find(value.substr(prefix.size(), hash - prefix.size()));
			if (found == keys.end()) return true;
			value.replace(prefix.size(), hash - prefix.size(), found->second);
			return true;
		}

		bool RemapProperties(
			Store &store, Entity instance, const std::unordered_map<std::string, std::string> &keys
		) {
			for (const ecs::PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (!property.Writable || property.Kind != ecs::PropertyKind::Field) continue;
				if (property.Type == ecs::PropertyType::Name) {
					core::Name value;
					if (!store.GetProperty(instance, property, &value, sizeof(value))) continue;
					if (!value.IsValid()) continue;
					std::string text(value.Text());
					if (!RemapReference(text, keys) || text == value.Text()) continue;
					const core::Name remapped(text);
					if (!store.SetProperty(instance, property, &remapped, sizeof(remapped))) return false;
				} else if (property.Type == ecs::PropertyType::String) {
					std::string value;
					if (!store.GetProperty(instance, property, &value, sizeof(value))) continue;
					const std::string original = value;
					if (!RemapReference(value, keys) || value == original) continue;
					if (!store.SetProperty(instance, property, &value, sizeof(value))) return false;
				}
			}
			return true;
		}

		bool PreparePlayerGuiClone(Store &store, Entity source, Entity copy) {
			std::vector<CloneNode> pairs;
			if (!CollectClonePairs(store, source, copy, pairs)) return false;

			std::unordered_map<std::string, std::string> remappedKeys;
			for (const CloneNode &node : pairs) {
				const auto *graph = store.Get<scene::ImageGraph>(node.Source);
				if (graph == nullptr || !graph->InstanceKey.IsValid()) continue;
				const std::string oldKey(graph->InstanceKey.Text());
				if (remappedKeys.contains(oldKey)) return false;
				bool assigned = false;
				for (uint32_t attempt = 0; attempt < 1024 && !assigned; ++attempt) {
					const core::Name newKey(CopyGraphKey(oldKey, node.Copy, attempt));
					assigned = scene::SetImageGraphInstanceKey(store, node.Copy, newKey);
					if (assigned) remappedKeys.emplace(oldKey, std::string(newKey.Text()));
				}
				if (!assigned) return false;
			}

			if (!remappedKeys.empty())
				for (const CloneNode &node : pairs)
					if (!RemapProperties(store, node.Copy, remappedKeys)) return false;
			return true;
		}
	}

	size_t ResetPlayerGui(ecs::Store &store, ecs::Entity player) {
		return gui::ResetPlayerGui(store, player, &PreparePlayerGuiClone);
	}
}
