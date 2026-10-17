#include "PlayerGuiState.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace engine::gui {
	namespace {
		using ecs::Entity;
		using ecs::Store;

		struct ComponentBaseline {
			ecs::ComponentId Component;
			std::vector<std::byte> Bytes;
			Entity Source;
			uint64_t StoreIdentity = 0;
			uint64_t Version = 0;
		};

		struct AttributeBaseline {
			core::Name Name;
			std::vector<std::byte> Bytes;
			uint64_t Revision = 0;
		};

		struct NodeCopy {
			Entity Source;
			Entity Copy;
			std::string Path;
			core::Name Name;
			Entity ParentCopy;
			ecs::ClassId Class;
			std::vector<ComponentBaseline> Components;
			std::vector<ecs::ComponentId> Changed;
			Entity AttributeSource;
			std::vector<AttributeBaseline> Attributes;
		};

		struct CollectorCopy {
			core::Name Origin;
			Entity Source;
			Entity Copy;
			std::vector<NodeCopy> Nodes;
		};

		// Only RefreshPlayerGuiProjection writes this private, unobserved cache.
		// Tombstones remember local Destroy until the next spawn.
		struct ProjectionState {
			Entity Player;
			Entity Character;
			std::vector<CollectorCopy> Collectors;
		};

		template <class T> void WriteLocal(core::ByteWriter &, const void *, size_t) {}
		template <class T> void ReadLocal(core::ByteReader &, void *destination, size_t count) {
			auto *values = static_cast<T *>(destination);
			for (size_t index = 0; index < count; ++index)
				values[index] = {};
		}

		void WriteOrigins(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *values = static_cast<const PlayerGuiTemplateOrigin *>(source);
			for (size_t index = 0; index < count; ++index)
				writer.WriteName(values[index].Path);
		}
		void ReadOrigins(core::ByteReader &reader, void *destination, size_t count) {
			auto *values = static_cast<PlayerGuiTemplateOrigin *>(destination);
			for (size_t index = 0; index < count; ++index)
				values[index].Path = reader.ReadName();
		}

		Entity Controller(Store &store, Entity player) {
			Entity found;
			store.Each<const ProjectionState>([&](Entity entity, const ProjectionState &state) {
				if (state.Player == player) found = entity;
			});
			return found;
		}

		bool AuthoredComponent(const ecs::TypeDescriptor &type) {
			const auto name = type.Name.Text();
			return (type.Kind == ecs::ComponentKind::Tag || (type.Serialisable && type.Write != nullptr)) &&
				   !name.starts_with("playergui.") && name != "ecs.InstanceProjection" &&
				   name != "ecs.ClientLocal" && name != "ecs.DirtyBits" && name != "ecs.Hierarchy" &&
				   name != "ecs.InstanceName" && name != "ecs.InstanceClass" && name != "gui.Resolved" &&
				   name != "gui.ScrollState" && name != "gui.SpatialCanvas" &&
				   name != "gui.GuiServiceState" && name != "gui.PlayerGuiTemplateOrigin";
		}

		void SyncComponents(Store &store, NodeCopy &node, bool initial) {
			const auto ids = store.ComponentsOf(node.Source);
			const std::vector<ecs::ComponentId> components(ids.begin(), ids.end());
			for (const auto component : components) {
				const auto &type = ecs::Components::Describe(component);
				if (!AuthoredComponent(type)) continue;
				auto baseline =
					std::find_if(node.Components.begin(), node.Components.end(), [&](const auto &held) {
						return held.Component == component;
					});
				// Observe before reading: enabling tracking can relocate the source row.
				store.Observe(component);
				const auto identity = store.Identity();
				const auto version = store.ComponentChangeVersion(component);
				if (baseline != node.Components.end() && baseline->Source == node.Source &&
					baseline->StoreIdentity == identity && baseline->Version == version)
					continue;
				const void *value = store.GetComponent(node.Source, component);
				core::ByteWriter writer;
				if (type.Write != nullptr) type.Write(writer, value, 1);
				if (type.Kind != ecs::ComponentKind::Tag && writer.Bytes().empty()) continue;
				const bool unchanged =
					baseline != node.Components.end() && std::ranges::equal(baseline->Bytes, writer.Bytes());
				if (!unchanged) {
					if (!initial && store.Alive(node.Copy)) store.SetComponent(node.Copy, component, value);
					node.Changed.push_back(component);
				}
				if (baseline == node.Components.end()) {
					node.Components.push_back(
						{component,
						 writer.TakeBytes(),
						 node.Source,
						 identity,
						 store.ComponentChangeVersion(component)}
					);
				} else {
					if (!unchanged) baseline->Bytes = writer.TakeBytes();
					baseline->Source = node.Source;
					baseline->StoreIdentity = identity;
					baseline->Version = store.ComponentChangeVersion(component);
				}
			}
			for (auto it = node.Components.begin(); it != node.Components.end();) {
				if (store.HasComponent(node.Source, it->Component)) {
					++it;
					continue;
				}
				if (store.Alive(node.Copy)) store.RemoveComponent(node.Copy, it->Component);
				it = node.Components.erase(it);
			}
		}

		void SyncAttributes(Store &store, NodeCopy &node) {
			const auto names = ecs::AttributeNames(store, node.Source);
			for (const auto name : names) {
				auto baseline =
					std::find_if(node.Attributes.begin(), node.Attributes.end(), [&](const auto &held) {
						return held.Name == name;
					});
				const uint64_t revision = ecs::AttributeRevision(store, node.Source, name);
				if (node.AttributeSource == node.Source && revision != 0 &&
					baseline != node.Attributes.end() && baseline->Revision == revision)
					continue;
				ecs::AttributeValue value;
				if (!ecs::GetAttribute(store, node.Source, name, value)) continue;
				// Reuse the attribute codec, with a fixed key so a fresh server source
				// compares by value instead of overwriting retained local attributes.
				ecs::AttributeTable single;
				single.Entities[0].emplace(name.Id(), value);
				core::ByteWriter writer;
				ecs::Components::Describe(ecs::Components::Of<ecs::AttributeTable>())
					.Write(writer, &single, 1);
				const bool unchanged =
					baseline != node.Attributes.end() && std::ranges::equal(baseline->Bytes, writer.Bytes());
				if (!unchanged && store.Alive(node.Copy))
					(void)ecs::SetAttribute(store, node.Copy, name, value);
				if (baseline == node.Attributes.end())
					node.Attributes.push_back({name, writer.TakeBytes(), revision});
				else {
					baseline->Revision = revision;
					if (!unchanged) baseline->Bytes = writer.TakeBytes();
				}
			}
			for (auto it = node.Attributes.begin(); it != node.Attributes.end();) {
				if (std::ranges::find(names, it->Name) != names.end()) {
					++it;
					continue;
				}
				if (store.Alive(node.Copy)) (void)ecs::SetAttribute(store, node.Copy, it->Name, {});
				it = node.Attributes.erase(it);
			}
			node.AttributeSource = node.Source;
		}

		struct SourceNode {
			Entity Source;
			std::string Path;
			std::string Parent;
		};

		void SourceNodes(
			const Store &store,
			Entity source,
			std::string path,
			std::string parent,
			std::vector<SourceNode> &out
		) {
			out.push_back({source, path, std::move(parent)});
			size_t index = 0;
			store.EachChild(source, [&](Entity child) {
				if (ecs::IsClientLocalInstance(store, child) || store.Has<ecs::NotArchivable>(child)) return;
				// Sibling position only disambiguates equal names inside this copy.
				const auto childPath = path + "/" + std::string(store.InstanceNameOf(child).Text()) + ":" +
									   std::to_string(index++);
				SourceNodes(store, child, childPath, path, out);
			});
		}

		void
		PairCloned(Store &store, Entity source, Entity copy, std::string path, CollectorCopy &collector) {
			// Empty custom codecs identify runtime rows rebuilt by the receiving view.
			const auto copiedIds = store.ComponentsOf(copy);
			const std::vector<ecs::ComponentId> copied(copiedIds.begin(), copiedIds.end());
			for (const auto component : copied) {
				const auto &type = ecs::Components::Describe(component);
				const auto name = type.Name.Text();
				if (name == "ecs.Hierarchy" || name == "ecs.InstanceName" || name == "ecs.InstanceClass" ||
					name == "ecs.ClientLocal")
					continue;
				core::ByteWriter writer;
				if (AuthoredComponent(type) && type.Write != nullptr)
					type.Write(writer, store.GetComponent(copy, component), 1);
				if (!AuthoredComponent(type) ||
					(type.Kind != ecs::ComponentKind::Tag && writer.Bytes().empty()))
					store.RemoveComponent(copy, component);
			}
			store.Set(copy, PlayerGuiCopy{source, true});
			store.Set(source, PlayerGuiSource{true, copy});
			NodeCopy node;
			node.Source = source;
			node.Copy = copy;
			node.Path = path;
			node.Name = store.InstanceNameOf(source);
			node.ParentCopy = store.ParentOf(copy);
			node.Class = store.ClassOf(source);
			SyncComponents(store, node, true);
			SyncAttributes(store, node);
			collector.Nodes.push_back(std::move(node));
			std::vector<Entity> sources, copies;
			store.EachChild(source, [&](Entity child) {
				if (!store.Has<ecs::NotArchivable>(child)) sources.push_back(child);
			});
			store.EachChild(copy, [&](Entity child) { copies.push_back(child); });
			for (size_t index = 0; index < std::min(sources.size(), copies.size()); ++index) {
				const auto childPath = path + "/" + std::string(store.InstanceNameOf(sources[index]).Text()) +
									   ":" + std::to_string(index);
				PairCloned(store, sources[index], copies[index], childPath, collector);
			}
		}

		void CloneCollector(Store &store, Entity target, CollectorCopy &collector) {
			collector.Copy = store.ClonePredictedInstance(collector.Source);
			collector.Nodes.clear();
			if (collector.Copy == ecs::NULL_ENTITY) return;
			store.Set(collector.Copy, ecs::ClientLocal{});
			store.SetParent(collector.Copy, target);
			PairCloned(store, collector.Source, collector.Copy, "", collector);
		}

		void RebindCollector(Store &store, CollectorCopy &collector) {
			std::vector<SourceNode> sources;
			SourceNodes(store, collector.Source, "", "", sources);
			for (const auto &source : sources) {
				auto node =
					std::find_if(collector.Nodes.begin(), collector.Nodes.end(), [&](const auto &held) {
						return held.Source == source.Source;
					});
				if (node == collector.Nodes.end())
					node =
						std::find_if(collector.Nodes.begin(), collector.Nodes.end(), [&](const auto &held) {
							return !store.Alive(held.Source) && held.Path == source.Path;
						});
				if (node == collector.Nodes.end()) {
					const auto parent =
						std::find_if(collector.Nodes.begin(), collector.Nodes.end(), [&](const auto &held) {
							return held.Path == source.Parent;
						});
					if (parent == collector.Nodes.end() || !store.Alive(parent->Copy)) continue;
					const Entity copy = store.CreatePredictedInstance(
						store.ClassOf(source.Source), store.InstanceNameOf(source.Source).Text()
					);
					if (copy == ecs::NULL_ENTITY) continue;
					const auto sourceIds = store.ComponentsOf(source.Source);
					const std::vector<ecs::ComponentId> components(sourceIds.begin(), sourceIds.end());
					for (const auto component : components) {
						const auto &type = ecs::Components::Describe(component);
						if (!AuthoredComponent(type)) continue;
						const auto *value = store.GetComponent(source.Source, component);
						core::ByteWriter writer;
						if (type.Write != nullptr) type.Write(writer, value, 1);
						if (type.Kind != ecs::ComponentKind::Tag && writer.Bytes().empty()) continue;
						store.SetComponent(copy, component, value);
					}
					store.SetParent(copy, parent->Copy);
					store.Set(copy, PlayerGuiCopy{source.Source, true});
					store.Set(source.Source, PlayerGuiSource{true, copy});
					NodeCopy added;
					added.Source = source.Source;
					added.Copy = copy;
					added.Path = source.Path;
					added.Name = store.InstanceNameOf(source.Source);
					added.ParentCopy = parent->Copy;
					added.Class = store.ClassOf(source.Source);
					SyncComponents(store, added, true);
					SyncAttributes(store, added);
					collector.Nodes.push_back(std::move(added));
					continue;
				}
				node->Source = source.Source;
				node->Path = source.Path;
				const auto *sourceBinding = store.Get<PlayerGuiSource>(source.Source);
				const Entity liveCopy = store.Alive(node->Copy) ? node->Copy : ecs::NULL_ENTITY;
				if (sourceBinding == nullptr || !sourceBinding->Active || sourceBinding->Copy != liveCopy)
					store.Set(source.Source, PlayerGuiSource{true, liveCopy});
				if (store.Alive(node->Copy)) {
					const auto *binding = store.Get<PlayerGuiCopy>(node->Copy);
					if (binding == nullptr || binding->Source != source.Source)
						store.Set(node->Copy, PlayerGuiCopy{source.Source, true});
				}
			}
			for (auto &node : collector.Nodes) {
				const bool present = std::ranges::any_of(sources, [&](const auto &source) {
					return source.Source == node.Source;
				});
				if (!present && store.Alive(node.Copy)) store.DestroyInstance(node.Copy);
				if (!present || !store.Alive(node.Copy)) continue;
				const auto name = store.InstanceNameOf(node.Source);
				if (node.Name != name) {
					store.SetInstanceName(node.Copy, name.Text());
					node.Name = name;
				}
				const auto sourceParent = store.ParentOf(node.Source);
				const auto localParent = FindPlayerGuiCopy(store, sourceParent);
				const Entity parent = localParent != ecs::NULL_ENTITY ? localParent : sourceParent;
				if (node.ParentCopy != parent) {
					store.SetParent(node.Copy, parent);
					node.ParentCopy = parent;
				}
				const auto sourceClass = store.ClassOf(node.Source);
				if (node.Class != sourceClass) {
					store.Set(node.Copy, ecs::InstanceClass{sourceClass});
					node.Class = sourceClass;
				}
				SyncComponents(store, node, false);
				SyncAttributes(store, node);
			}
		}

		void RemapChangedReferences(Store &store, CollectorCopy &collector) {
			for (auto &node : collector.Nodes) {
				if (!node.Changed.empty() && store.Alive(node.Copy) && store.Alive(node.Source)) {
					for (const auto &property : store.PropertiesOf(node.Copy)) {
						if (property.Type != ecs::PropertyType::Reference || !property.Writable ||
							property.Get == nullptr || property.Set == nullptr || property.Writes == nullptr)
							continue;
						bool changed = false;
						for (const auto component : property.Writes->Ids())
							changed |= std::ranges::find(node.Changed, component) != node.Changed.end();
						if (!changed) continue;
						Entity reference;
						if (!property.Get(store, node.Source, &reference)) continue;
						const auto copy = FindPlayerGuiCopy(store, reference);
						if (copy != ecs::NULL_ENTITY) reference = copy;
						const auto restore =
							property.RestoreReference != nullptr ? property.RestoreReference : property.Set;
						restore(store, node.Copy, &reference);
					}
				}
				node.Changed.clear();
			}
		}
	}

	void RegisterPlayerGuiComponents() {
		ecs::Components::Register<PlayerGuiTemplateOrigin>(
			"gui.PlayerGuiTemplateOrigin", WriteOrigins, ReadOrigins
		);
		ecs::Components::Register<PlayerGuiSource>(
			"playergui.Source", WriteLocal<PlayerGuiSource>, ReadLocal<PlayerGuiSource>
		);
		ecs::Components::Register<ProjectionState>(
			"playergui.Projection", WriteLocal<ProjectionState>, ReadLocal<ProjectionState>
		);
	}

	bool IsPlayerGuiSource(const Store &store, Entity instance) {
		for (Entity current = instance; current != ecs::NULL_ENTITY; current = store.ParentOf(current)) {
			const auto *source = store.Get<PlayerGuiSource>(current);
			if (source != nullptr && source->Active) return true;
		}
		return false;
	}

	bool IsPlayerGuiCopy(const Store &store, Entity instance) {
		for (Entity current = instance; current != ecs::NULL_ENTITY; current = store.ParentOf(current)) {
			const auto *copy = store.Get<PlayerGuiCopy>(current);
			if (copy != nullptr && copy->Active) return true;
		}
		return false;
	}

	Entity FindPlayerGuiCopy(const Store &store, Entity source) {
		const auto *binding = store.Get<PlayerGuiSource>(source);
		return binding != nullptr && binding->Active && store.Alive(binding->Copy) ? binding->Copy
																				   : ecs::NULL_ENTITY;
	}

	Entity PlayerGuiSourceOf(const Store &store, Entity copy) {
		const auto *binding = store.Get<PlayerGuiCopy>(copy);
		return binding != nullptr && binding->Active ? binding->Source : ecs::NULL_ENTITY;
	}

	size_t RefreshPlayerGuiProjection(Store &store, Entity player, Entity character) {
		ENGINE_PROFILE_CAT("player gui projection", core::ProfileCategory::ECS);
		const Entity target = store.FindFirstChild(player, "PlayerGui");
		if (target == ecs::NULL_ENTITY) return 0;
		Entity controller = Controller(store, player);
		if (controller == ecs::NULL_ENTITY) {
			controller = store.CreatePredicted();
			if (controller == ecs::NULL_ENTITY) return 0;
			store.Set(controller, ecs::ClientLocal{});
			store.Set(controller, ProjectionState{player, character, {}});
		}
		auto *state = store.GetUnobserved<ProjectionState>(controller);
		const bool respawn = character != ecs::NULL_ENTITY && state->Character != ecs::NULL_ENTITY &&
							 character != state->Character;
		if (character != ecs::NULL_ENTITY) state->Character = character;
		if (respawn) {
			std::vector<Entity> clearing;
			store.EachChild(target, [&](Entity child) {
				const auto *layer = store.Get<Layer>(child);
				if (ecs::IsClientLocalInstance(store, child) && layer != nullptr && layer->ResetOnSpawn)
					clearing.push_back(child);
			});
			for (const Entity child : clearing)
				store.DestroyInstance(child);
		}
		std::vector<Entity> sources;
		store.EachChild(target, [&](Entity child) {
			if (!ecs::IsClientLocalInstance(store, child)) sources.push_back(child);
		});
		size_t created = 0;
		for (const Entity source : sources) {
			const auto *tag = store.Get<PlayerGuiSource>(source);
			if (tag == nullptr || !tag->Active) store.Set(source, PlayerGuiSource{true, {}});
			const auto *origin = store.Get<PlayerGuiTemplateOrigin>(source);
			const core::Name key =
				origin != nullptr && origin->Path.IsValid()
					? origin->Path
					: core::Name(
						  std::string("PlayerGui/") + std::string(store.InstanceNameOf(source).Text())
					  );
			auto collector =
				std::find_if(state->Collectors.begin(), state->Collectors.end(), [&](const auto &held) {
					return held.Source == source;
				});
			if (collector == state->Collectors.end())
				collector =
					std::find_if(state->Collectors.begin(), state->Collectors.end(), [&](const auto &held) {
						return held.Origin == key;
					});
			if (collector == state->Collectors.end()) {
				state->Collectors.push_back({key, source, {}, {}});
				CloneCollector(store, target, state->Collectors.back());
				created += store.Alive(state->Collectors.back().Copy);
				continue;
			}
			collector->Source = source;
			if (!store.Alive(collector->Copy)) {
				if (respawn) {
					CloneCollector(store, target, *collector);
					created += store.Alive(collector->Copy);
				}
				continue;
			}
			RebindCollector(store, *collector);
		}
		for (auto &collector : state->Collectors) {
			if (!store.Alive(collector.Source) && !respawn && store.Alive(collector.Copy))
				store.DestroyInstance(collector.Copy);
			RemapChangedReferences(store, collector);
		}
		return created;
	}
}
