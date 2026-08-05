// The tools any program with worlds can answer.
//
// **The universe is the game and a world is a scene**, which is the mapping a
// script already sees through `game` and `workspace`. Nothing here invents a
// second vocabulary — `script/src/Services.cpp` established it at v0.6 and the
// explorer draws the same objects.
//
// **Every one of these runs on the caller's thread, inside a frame or a tick.**
// That is what makes them allowed to call `Universe::Enter` at all, and it is
// also the constraint: a tool holds up the loop it runs in, so none of them walk
// a world without a bound. The tree tools take a `depth` and a `limit` and
// default both to something small — a world of fifty thousand parts serialised
// whole is a reply nothing can read and a frame nothing can draw.

#include <engine/control/Surface.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace engine::control {

	using ecs::Classes;
	using ecs::ClassId;
	using ecs::PropertyDescriptor;
	using ecs::PropertyType;
	using ecs::Store;
	using nlohmann::json;
	using world::WorldId;

	namespace {
		// The run mode a world is in, as a word.
		//
		// **Derived from the runtime rather than stored**, because `RunMode` is
		// per-world state the editor keeps and a world that faulted is no longer
		// in the mode somebody asked for.
		const char *DescribeState(world::WorldState state) {
			switch (state) {
			case world::WorldState::Active:
				return "active";
			case world::WorldState::Idle:
				return "idle";
			case world::WorldState::Suspended:
				return "suspended";
			case world::WorldState::Faulted:
				// Named rather than folded into "unknown". A world whose tick
				// threw is the one state a reader most needs to be told about,
				// and the supervisor's restore is invisible without it.
				return "faulted";
			case world::WorldState::Remote:
				// Held by a supervised host, not by this process. Distinct from
				// "suspended", which is a world this process holds and has
				// chosen not to tick — see `world/Enums.hpp`.
				return "remote";
			}
			return "unknown";
		}

		// One property, converted for a reader rather than for a wire.
		//
		// A `Vector3` becomes three named numbers and not an array: the whole
		// point of this surface is that somebody who has never seen the engine
		// can read the reply, and `{"X":0,"Y":5,"Z":0}` needs no schema.
		json ReadProperty(Store &store, ecs::Entity instance, const PropertyDescriptor &property) {
			alignas(16) unsigned char bytes[sizeof(core::CFrame)] = {};
			if (property.Size > sizeof(bytes) ||
				!store.GetProperty(instance, property.Name, bytes, property.Size)) {
				return nullptr;
			}

			switch (property.Type) {
			case PropertyType::Bool:
				return *reinterpret_cast<const bool *>(bytes);
			case PropertyType::Int32:
				return *reinterpret_cast<const int32_t *>(bytes);
			case PropertyType::Int64:
				return *reinterpret_cast<const int64_t *>(bytes);
			case PropertyType::Float:
				return *reinterpret_cast<const float *>(bytes);
			case PropertyType::Double:
				return *reinterpret_cast<const double *>(bytes);
			case PropertyType::Name:
			case PropertyType::Enum:
				return std::string(reinterpret_cast<const core::Name *>(bytes)->Text());
			case PropertyType::Vector3: {
				const auto &value = *reinterpret_cast<const core::Vector3 *>(bytes);
				return json{{"X", value.X}, {"Y", value.Y}, {"Z", value.Z}};
			}
			case PropertyType::Color3: {
				const auto &value = *reinterpret_cast<const core::Color3 *>(bytes);
				return json{{"R", value.R}, {"G", value.G}, {"B", value.B}};
			}
			case PropertyType::CFrame: {
				const auto &value = *reinterpret_cast<const core::CFrame *>(bytes);
				return json{
					{"Position",
					 json{{"X", value.Position.X}, {"Y", value.Position.Y}, {"Z", value.Position.Z}}},
				};
			}
			case PropertyType::Reference:
				return reinterpret_cast<const ecs::Entity *>(bytes)->Id;
			case PropertyType::Opaque:
				break;
			}
			return nullptr;
		}

		// Every property a class declares, its own and its bases'.
		json ReadProperties(Store &store, ecs::Entity instance) {
			json out = json::object();
			const ClassId klass = store.ClassOf(instance);
			if (!klass.IsValid()) {
				return out;
			}

			for (const PropertyDescriptor &property : Classes::Describe(klass).Properties) {
				json value = ReadProperty(store, instance, property);
				if (!value.is_null()) {
					out[std::string(property.Name.Text())] = std::move(value);
				}
			}
			return out;
		}

		// One node of the instance tree, recursively.
		//
		// `budget` is shared across the whole walk rather than per level, so a
		// world that is wide rather than deep is truncated too.
		json ReadTree(Store &store, ecs::Entity instance, int depth, int &budget) {
			json node{
				{"id", instance.Id},
				{"name", std::string(store.InstanceNameOf(instance).Text())},
			};

			if (const ClassId klass = store.ClassOf(instance); klass.IsValid()) {
				node["class"] = std::string(Classes::Describe(klass).Name.Text());
			}

			if (depth <= 0) {
				// Said rather than implied: a caller that sees no `children` and
				// no `truncated` is entitled to believe the instance has none.
				size_t count = 0;
				store.EachChild(instance, [&](ecs::Entity) { count++; });
				if (count > 0) {
					node["childCount"] = count;
					node["truncated"] = true;
				}
				return node;
			}

			json children = json::array();
			store.EachChild(instance, [&](ecs::Entity child) {
				if (budget <= 0) {
					return;
				}
				budget--;
				children.push_back(ReadTree(store, child, depth - 1, budget));
			});

			if (!children.empty()) {
				node["children"] = std::move(children);
			}
			if (budget <= 0) {
				node["truncated"] = true;
			}
			return node;
		}

		// The `world` argument every tool shares.
		//
		// **Missing means the first one**, which is what a program with a single
		// world always means and what a program with several can be told about
		// by calling `world_list` first.
		WorldId WorldArgument(world::Universe &universe, const json &arguments, std::string &failure) {
			const std::vector<WorldId> worlds = universe.Worlds();
			if (!arguments.contains("world") || !arguments["world"].is_string()) {
				if (worlds.empty()) {
					failure = "this program has no worlds";
					return {};
				}
				return worlds.front();
			}

			const std::string wanted = arguments["world"].get<std::string>();
			const WorldId found = universe.Find(core::Name(wanted.c_str()));
			if (!found.IsValid()) {
				failure = "no world called '" + wanted + "' — call world_list";
			}
			return found;
		}

		json WorldSchema() {
			return json{
				{"type", "object"},
				{"properties",
				 json{{"world", json{{"type", "string"}, {"description", "Which scene, by name."}}}}},
			};
		}

		// Converts one JSON value into a property's storage and writes it.
		bool WriteProperty(
			Store &store,
			ecs::Entity instance,
			const PropertyDescriptor &property,
			const json &value,
			std::string &failure
		) {
			// Sized from the descriptor, exactly as the script bindings are, so
			// this cannot be the place a size mismatch is introduced.
			alignas(16) unsigned char bytes[sizeof(core::CFrame)] = {};
			if (property.Size > sizeof(bytes)) {
				failure = "property too large to write";
				return false;
			}

			const auto number = [](const json &from, float fallback) {
				return from.is_number() ? from.get<float>() : fallback;
			};

			switch (property.Type) {
			case PropertyType::Bool:
				*reinterpret_cast<bool *>(bytes) = value.is_boolean() && value.get<bool>();
				break;
			case PropertyType::Int32:
				*reinterpret_cast<int32_t *>(bytes) = value.is_number() ? value.get<int32_t>() : 0;
				break;
			case PropertyType::Int64:
				*reinterpret_cast<int64_t *>(bytes) = value.is_number() ? value.get<int64_t>() : 0;
				break;
			case PropertyType::Float:
				*reinterpret_cast<float *>(bytes) = number(value, 0.0f);
				break;
			case PropertyType::Double:
				*reinterpret_cast<double *>(bytes) = value.is_number() ? value.get<double>() : 0.0;
				break;
			case PropertyType::Name:
			case PropertyType::Enum: {
				if (!value.is_string()) {
					failure = "that property takes a string";
					return false;
				}
				const std::string text = value.get<std::string>();
				*reinterpret_cast<core::Name *>(bytes) = core::Name(text.c_str());
				break;
			}
			case PropertyType::Vector3:
				*reinterpret_cast<core::Vector3 *>(bytes) = core::Vector3{
					number(value.value("X", json(0.0)), 0.0f),
					number(value.value("Y", json(0.0)), 0.0f),
					number(value.value("Z", json(0.0)), 0.0f),
				};
				break;
			case PropertyType::Color3:
				*reinterpret_cast<core::Color3 *>(bytes) = core::Color3{
					number(value.value("R", json(0.0)), 0.0f),
					number(value.value("G", json(0.0)), 0.0f),
					number(value.value("B", json(0.0)), 0.0f),
				};
				break;
			case PropertyType::CFrame: {
				const json position = value.value("Position", value);
				*reinterpret_cast<core::CFrame *>(bytes) = core::CFrame{core::Vector3{
					number(position.value("X", json(0.0)), 0.0f),
					number(position.value("Y", json(0.0)), 0.0f),
					number(position.value("Z", json(0.0)), 0.0f),
				}};
				break;
			}
			case PropertyType::Reference:
			case PropertyType::Opaque:
				failure = "that property cannot be written through this surface yet";
				return false;
			}

			if (!store.SetProperty(instance, property.Name, bytes, property.Size)) {
				// The store refuses a write on an adopt-only world, which is what
				// a replica is. Said plainly rather than as a bare false.
				failure = "the world refused the write — it may be running or a replica";
				return false;
			}
			return true;
		}
	}

	void Surface::AddUniverseTools(world::Universe &universe, bool writable) {
		world::Universe *worlds = &universe;

		Add(Tool{
			"engine_info",
			"This program's own state: how many scenes it holds and what they are called. Call it "
			"first.",
			[] { return json{{"type", "object"}}; },
			[worlds](const json &, std::string &) {
				json names = json::array();
				for (const WorldId id : worlds->Worlds()) {
					names.push_back(std::string(worlds->NameOf(id).Text()));
				}
				return json{{"worlds", std::move(names)}, {"count", worlds->Count()}};
			},
		});

		Add(Tool{
			"world_list",
			"Every scene, with its state, its tick statistics and how many instances it holds. A "
			"world is a scene; the universe is the game.",
			[] { return json{{"type", "object"}}; },
			[worlds](const json &, std::string &) {
				json out = json::array();
				for (const WorldId id : worlds->Worlds()) {
					const world::WorldStatistics statistics = worlds->StatisticsOf(id);

					// `CountMatching` rather than a walk: every instance carries
					// `InstanceClass`, and counting a column is O(archetypes)
					// where walking the tree is O(instances) — per world, per
					// call, inside a frame.
					size_t instances = 0;
					worlds->Enter(id, [&](Store &store) {
						instances = store.CountMatching<ecs::InstanceClass>();
					});

					out.push_back(
						json{
							{"name", std::string(worlds->NameOf(id).Text())},
							{"remote", worlds->IsRemote(id)},
							{"state", DescribeState(worlds->StateOf(id))},
							{"tickRate", worlds->SettingsOf(id).TickRate},
							{"instances", instances},
							{"statistics",
							 json{
								 {"ticks", statistics.Ticks},
								 {"lastTickMs", statistics.LastTickMilliseconds},
								 {"slowestTickMs", statistics.SlowestTickMilliseconds},
								 {"faults", statistics.Faults},
								 {"droppedTicks", statistics.DroppedTicks},
							 }},
						}
					);
				}
				return out;
			},
		});

		Add(Tool{
			"world_tree",
			"The instance tree of one scene, as an author sees it in the explorer. Depth-limited and "
			"count-limited; ask for more when you know where you are going.",
			[] {
				json schema = WorldSchema();
				schema["properties"]["depth"] =
					json{{"type", "integer"}, {"description", "Levels to descend. Default 3."}};
				schema["properties"]["limit"] =
					json{{"type", "integer"}, {"description", "Instances at most. Default 200."}};
				return schema;
			},
			[worlds](const json &arguments, std::string &failure) -> json {
				const WorldId world = WorldArgument(*worlds, arguments, failure);
				if (!failure.empty()) {
					return nullptr;
				}

				const int depth = arguments.value("depth", 3);
				int budget = arguments.value("limit", 200);

				json roots = json::array();
				worlds->Enter(world, [&](Store &store) {
					store.EachRoot([&](ecs::Entity root) {
						if (budget <= 0) {
							return;
						}
						budget--;
						roots.push_back(ReadTree(store, root, depth, budget));
					});
				});

				return json{
					{"world", std::string(worlds->NameOf(world).Text())}, {"roots", std::move(roots)}
				};
			},
		});

		Add(Tool{
			"instance_get",
			"Every property of one instance, by the id world_tree reported.",
			[] {
				json schema = WorldSchema();
				schema["properties"]["id"] = json{{"type", "integer"}, {"description", "The entity id."}};
				schema["required"] = json::array({"id"});
				return schema;
			},
			[worlds](const json &arguments, std::string &failure) -> json {
				const WorldId world = WorldArgument(*worlds, arguments, failure);
				if (!failure.empty()) {
					return nullptr;
				}
				if (!arguments.contains("id")) {
					failure = "instance_get needs an `id` — take one from world_tree";
					return nullptr;
				}

				const ecs::Entity instance(arguments["id"].get<uint64_t>());
				json out;
				bool found = false;

				worlds->Enter(world, [&](Store &store) {
					if (!store.Alive(instance)) {
						return;
					}
					found = true;
					out = json{
						{"id", instance.Id},
						{"name", std::string(store.InstanceNameOf(instance).Text())},
						{"properties", ReadProperties(store, instance)},
					};
					if (const ClassId klass = store.ClassOf(instance); klass.IsValid()) {
						out["class"] = std::string(Classes::Describe(klass).Name.Text());
					}
				});

				if (!found) {
					failure = "no instance " + std::to_string(instance.Id) + " in that world";
					return nullptr;
				}
				return out;
			},
		});

		if (writable) {
			Add(Tool{
				"instance_set",
				"Writes one property of one instance. Vector3 and Color3 take an object of named "
				"components; an enum takes its member name as a string.",
				[] {
					json schema = WorldSchema();
					schema["properties"]["id"] = json{{"type", "integer"}};
					schema["properties"]["property"] = json{{"type", "string"}};
					schema["properties"]["value"] = json::object();
					schema["required"] = json::array({"id", "property", "value"});
					return schema;
				},
				[worlds](const json &arguments, std::string &failure) -> json {
					const WorldId world = WorldArgument(*worlds, arguments, failure);
					if (!failure.empty()) {
						return nullptr;
					}
					if (!arguments.contains("id") || !arguments.contains("property")) {
						failure = "instance_set needs `id`, `property` and `value`";
						return nullptr;
					}

					const ecs::Entity instance(arguments["id"].get<uint64_t>());
					const std::string name = arguments["property"].get<std::string>();
					const json value = arguments.value("value", json(nullptr));

					std::string problem;
					worlds->Enter(world, [&](Store &store) {
						if (!store.Alive(instance)) {
							problem = "no such instance";
							return;
						}

						const ClassId klass = store.ClassOf(instance);
						const PropertyDescriptor *found = nullptr;
						if (klass.IsValid()) {
							for (const PropertyDescriptor &property : Classes::Describe(klass).Properties) {
								if (std::string(property.Name.Text()) == name) {
									found = &property;
									break;
								}
							}
						}

						if (found == nullptr) {
							problem = "'" + name + "' is not a property of that class";
							return;
						}
						if (!found->Writable) {
							problem = "'" + name + "' is read-only";
							return;
						}
						WriteProperty(store, instance, *found, value, problem);
					});

					if (!problem.empty()) {
						failure = problem;
						return nullptr;
					}
					return json{{"ok", true}};
				},
			});
		}

		Add(Tool{
			"profile_frame",
			"The last frame as a flame graph: one span per scope with its depth, its parent, its "
			"inclusive time and its self time. Collection is switched on by the first call, so the "
			"first reply is empty and the next one is not.",
			[] { return json{{"type", "object"}}; },
			[this](const json &arguments, std::string &) -> json {
				if (!Profiling || !core::FrameGraph::IsEnabled()) {
					// Recorded as well as set, because a program that asserts
					// the graph's state every frame would otherwise undo this
					// before the next one — see `WantsProfiling`.
					Profiling = true;
					core::FrameGraph::SetEnabled(true);
					return json{
						{"spans", json::array()},
						{"note", "profiling was off and is now on — call again for the next frame"},
					};
				}

				const auto limit = static_cast<size_t>(std::max(1, arguments.value("limit", 200)));
				const std::vector<core::FrameSpan> &spans = core::FrameGraph::Spans();

				json out = json::array();
				for (size_t index = 0; index < spans.size() && index < limit; index++) {
					const core::FrameSpan &span = spans[index];
					out.push_back(
						json{
							{"name", std::string(span.Name)},
							{"depth", span.Depth},
							{"parent", span.Parent},
							{"startMs", span.StartMilliseconds},
							{"ms", span.Milliseconds},
							{"selfMs", span.SelfMilliseconds},
						}
					);
				}

				return json{
					{"frameMs", core::FrameGraph::FrameMilliseconds()},
					{"unmarkedMs", core::FrameGraph::UnmarkedMilliseconds()},
					{"spans", std::move(out)},
					{"truncated", spans.size() > limit},
				};
			},
		});
	}
}
