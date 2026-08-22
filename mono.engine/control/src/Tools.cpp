// The tools any program with worlds can answer.
//
// **The universe is the game and a world is a scene**, which is the mapping a
// script already sees through `game` and `workspace`. Nothing here invents a
// second vocabulary - `script/src/RunService.cpp` establishes it and the
// explorer draws the same objects.
//
// **Every one of these runs on the caller's thread, inside a frame or a tick.**
// That is what makes them allowed to call `Universe::Enter` at all, and it is
// also the constraint: a tool holds up the loop it runs in, so none of them walk
// a world without a bound. The tree tools take a `depth` and a `limit` and
// default both to something small - a world of fifty thousand parts serialised
// whole is a reply nothing can read and a frame nothing can draw.

#include "Catalogue.hpp"

#include <engine/control/Surface.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/NumberRange.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Schema.hpp>

#include <algorithm>
#include <new>
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
				// chosen not to tick - see `world/Enums.hpp`.
				return "remote";
			}
			return "unknown";
		}

		// How wide a property value can be, and therefore how big the buffers
		// below are.
		//
		// **Sized by the widest property type rather than by `CFrame`**, which is
		// what it was until a sequence became one. A `core::ColorSequence` is
		// twenty keypoints and does not fit in twenty-eight bytes, so the guard on
		// `property.Size` refused it - correctly, and silently, which is what
		// would have made `emitter.Color` read back as `null` over the control
		// surface with nothing in the log to say a property had been dropped.
		//
		// A `constexpr` rather than a `sizeof` spelled at each buffer, because
		// there are two of them and the read one being narrower than the write one
		// is a bug with no symptom on the write path.
		constexpr size_t WIDEST_PROPERTY =
			std::max(sizeof(core::ColorSequence), sizeof(core::NumberSequence));

		// One value, converted for a reader rather than for a wire.
		//
		// A `Vector3` becomes three named numbers and not an array: the whole
		// point of this surface is that somebody who has never seen the engine
		// can read the reply, and `{"X":0,"Y":5,"Z":0}` needs no schema.
		//
		// **The type and the bytes, never a descriptor**, which is what v0.12
		// changed and why: an ECS component *field* carries exactly these values
		// and is not a property, so a second switch for fields would be two
		// tables that agree until somebody edits one. Both script bindings took
		// the same split for the same reason.
		//
		// `PropertyType::String` is refused rather than handled, because the
		// bytes it would read are a live `std::string` and the callers that have
		// one take it down a path of their own.
		json ValueToJson(PropertyType type, const void *raw) {
			const auto *bytes = static_cast<const unsigned char *>(raw);

			switch (type) {
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
			case PropertyType::String:
				// Never reached - served by the branch above the buffer.
				return nullptr;
			case PropertyType::Vector3: {
				const auto &value = *reinterpret_cast<const core::Vector3 *>(bytes);
				return json{{"X", value.X}, {"Y", value.Y}, {"Z", value.Z}};
			}
			case PropertyType::Color3: {
				const auto &value = *reinterpret_cast<const core::Color3 *>(bytes);
				return json{{"R", value.R}, {"G", value.G}, {"B", value.B}};
			}
			case PropertyType::CFrame: {
				// **The rotation as well as the position.** Both halves were
				// missing here and in `ValueFromJson`, so reading a rotated
				// part's `CFrame` reported it upright and writing that value
				// back stood it up - which is what an editor does every time it
				// nudges a selection. `engine.control.marshalling` is what says
				// so now.
				//
				// The quaternion by its four components rather than as Euler
				// angles, for `CFrame.hpp`'s reason: reading angles back out of
				// a quaternion is ambiguous at the poles, so a round trip
				// through them is not one.
				const auto &value = *reinterpret_cast<const core::CFrame *>(bytes);
				return json{
					{"Position",
					 json{{"X", value.Position.X}, {"Y", value.Position.Y}, {"Z", value.Position.Z}}},
					{"Rotation",
					 json{
						 {"X", value.QuaternionX},
						 {"Y", value.QuaternionY},
						 {"Z", value.QuaternionZ},
						 {"W", value.QuaternionW},
					 }},
				};
			}
			// Named numbers, same rule as `Vector3` above: the reply is for
			// somebody who has never seen the engine, so `{"Scale":0.5,
			// "Offset":-8}` needs no schema where `[0.5,-8]` would.
			case PropertyType::Vector2: {
				const auto &value = *reinterpret_cast<const core::Vector2 *>(bytes);
				return json{{"X", value.X}, {"Y", value.Y}};
			}
			case PropertyType::UDim: {
				const auto &value = *reinterpret_cast<const core::UDim *>(bytes);
				return json{{"Scale", value.Scale}, {"Offset", value.Offset}};
			}
			case PropertyType::UDim2: {
				const auto &value = *reinterpret_cast<const core::UDim2 *>(bytes);
				return json{
					{"X", json{{"Scale", value.X.Scale}, {"Offset", value.X.Offset}}},
					{"Y", json{{"Scale", value.Y.Scale}, {"Offset", value.Y.Offset}}},
				};
			}
			case PropertyType::Rect: {
				const auto &value = *reinterpret_cast<const core::Rect *>(bytes);
				return json{
					{"Min", json{{"X", value.Min.X}, {"Y", value.Min.Y}}},
					{"Max", json{{"X", value.Max.X}, {"Y", value.Max.Y}}},
				};
			}
			// A curve is an array of named stops, by the same rule again: an array
			// of bare triples would need a schema to read and this does not.
			//
			// **Only `Count` stops, never the fixed array.** A sequence carries
			// twenty keypoint slots and uses two of them; writing the tail would
			// put eighteen zeroed stops at time zero in the reply, which reads as
			// a gradient that steps to nothing rather than as unused capacity.
			case PropertyType::NumberRange: {
				const auto &value = *reinterpret_cast<const core::NumberRange *>(bytes);
				return json{{"Min", value.Minimum}, {"Max", value.Maximum}};
			}
			case PropertyType::NumberSequence: {
				const auto &value = *reinterpret_cast<const core::NumberSequence *>(bytes);
				json stops = json::array();
				for (uint32_t index = 0; index < value.Count; index++) {
					const core::NumberKeypoint &stop = value.Keypoints[index];
					stops.push_back(
						json{{"Time", stop.Time}, {"Value", stop.Value}, {"Envelope", stop.Envelope}}
					);
				}
				return json{{"Keypoints", stops}};
			}
			case PropertyType::ColorSequence: {
				const auto &value = *reinterpret_cast<const core::ColorSequence *>(bytes);
				json stops = json::array();
				for (uint32_t index = 0; index < value.Count; index++) {
					const core::ColorKeypoint &stop = value.Keypoints[index];
					stops.push_back(
						json{
							{"Time", stop.Time},
							{"Value", json{{"R", stop.Value.R}, {"G", stop.Value.G}, {"B", stop.Value.B}}},
						}
					);
				}
				return json{{"Keypoints", stops}};
			}
			case PropertyType::Reference:
				return reinterpret_cast<const ecs::Entity *>(bytes)->Id;
			case PropertyType::Opaque:
				break;
			}
			return nullptr;
		}

		// One property of one instance, read through its conversion.
		json ReadProperty(Store &store, ecs::Entity instance, const PropertyDescriptor &property) {
			// **Before the shared buffer**, because a `PropertyType::String`
			// getter assigns into its destination rather than filling bytes -
			// and assigning a `std::string` into uninitialised storage is
			// undefined behaviour. Both script bindings take the same exception
			// in the same shape.
			if (property.Type == PropertyType::String) {
				std::string text;
				if (!store.GetProperty(instance, property, &text, sizeof(text))) {
					return nullptr;
				}
				return text;
			}

			alignas(16) unsigned char bytes[WIDEST_PROPERTY] = {};
			if (property.Size > sizeof(bytes) ||
				!store.GetProperty(instance, property, bytes, property.Size)) {
				return nullptr;
			}
			return ValueToJson(property.Type, bytes);
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

		// --- the storage underneath ------------------------------------------
		//
		// **The class tree is one view of a world and the components are the
		// other**, and a client that can only see the first cannot see anything
		// a game declared for itself. v0.12 gave a script `World:DefineComponent`
		// and a query over it; these are the same three questions asked over the
		// wire - what components exist, who carries them, and what is in one.
		//
		// **Only described components are readable**, exactly as they are from a
		// script. A C++ component has no field list at run time, so there is
		// nothing to build an object from - and it already has a property
		// surface, which `instance_get` answers.

		// One described component's fields, as a name-to-type object.
		json ReadSchema(const ecs::Schema &schema) {
			json fields = json::object();
			for (const ecs::FieldDescriptor &field : schema.Fields()) {
				fields[std::string(field.Spelling)] = field.Type == PropertyType::Enum
														  ? "Enum." + std::string(field.Enum.Text())
														  : std::string(ecs::Describe(field.Type));
			}
			return fields;
		}

		// The described component a `component` argument names.
		const ecs::Schema *SchemaArgument(const json &arguments, ecs::ComponentId &id, std::string &failure) {
			if (!arguments.contains("component") || !arguments["component"].is_string()) {
				failure = "which component? - call component_list";
				return nullptr;
			}

			const std::string wanted = arguments["component"].get<std::string>();
			id = ecs::Components::Find(core::Name(wanted.c_str()));

			if (!id.IsValid()) {
				failure = "no component called '" + wanted + "' - call component_list";
				return nullptr;
			}

			const ecs::Schema *schema = ecs::Schemas::Of(id);
			if (schema == nullptr) {
				// Two different mistakes, told apart. "There is no such
				// component" sends a reader looking for a typo, where the real
				// answer is often "that one is reached through its properties".
				failure = "'" + wanted +
						  "' is a component the engine declares, so it has no readable fields - use "
						  "instance_get on the instance that carries it";
			}
			return schema;
		}

		// The component ids a `components` array names.
		bool
		ComponentArgument(const json &arguments, std::vector<ecs::ComponentId> &out, std::string &failure) {
			if (!arguments.contains("components") || !arguments["components"].is_array() ||
				arguments["components"].empty()) {
				failure = "name at least one component - call component_list";
				return false;
			}

			for (const json &named : arguments["components"]) {
				if (!named.is_string()) {
					failure = "every component has to be named as a string";
					return false;
				}

				const std::string wanted = named.get<std::string>();
				const ecs::ComponentId id = ecs::Components::Find(core::Name(wanted.c_str()));
				if (!id.IsValid()) {
					// **Refused rather than answered with nothing.** A typo would
					// otherwise be an empty result, which reads exactly like a
					// world with nothing in it.
					failure = "no component called '" + wanted + "'";
					return false;
				}
				out.push_back(id);
			}
			return true;
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
				failure = "no world called '" + wanted + "' - call world_list";
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

		// Converts one JSON value into storage of that type.
		//
		// The write half of `ValueToJson`, split for the same reason and
		// refusing `PropertyType::String` for the same one: its destination is a
		// live `std::string` rather than bytes.
		//
		// @param type    What to read the value as.
		// @param value   The JSON.
		// @param raw     At least `Schemas::SizeOf(type)` bytes, zeroed.
		// @param failure Filled when the value cannot be read as that type.
		// @return `false` when it could not.
		bool ValueFromJson(PropertyType type, const json &value, void *raw, std::string &failure) {
			auto *bytes = static_cast<unsigned char *>(raw);

			const auto number = [](const json &from, float fallback) {
				return from.is_number() ? from.get<float>() : fallback;
			};

			switch (type) {
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
			case PropertyType::String:
				// Never reached - served by the branch above the buffer.
				failure = "a string property is written through its own path";
				return false;
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

				// **An absent `Rotation` is the identity, not what was there.**
				// Every other case in this switch defaults a missing field to
				// zero rather than reading the old value, and the argument is
				// the one `UDim2` states below: the read-modify-write belongs in
				// the caller, which can see both halves. `{"W": 1}` rather than
				// all zeroes because a quaternion of four zeroes is not a
				// rotation at all.
				const json rotation = value.value("Rotation", json::object());
				core::CFrame frame{core::Vector3{
					number(position.value("X", json(0.0)), 0.0f),
					number(position.value("Y", json(0.0)), 0.0f),
					number(position.value("Z", json(0.0)), 0.0f),
				}};
				frame.QuaternionX = number(rotation.value("X", json(0.0)), 0.0f);
				frame.QuaternionY = number(rotation.value("Y", json(0.0)), 0.0f);
				frame.QuaternionZ = number(rotation.value("Z", json(0.0)), 0.0f);
				frame.QuaternionW = number(rotation.value("W", json(1.0)), 1.0f);

				*reinterpret_cast<core::CFrame *>(bytes) = frame;
				break;
			}
			case PropertyType::Vector2:
				*reinterpret_cast<core::Vector2 *>(bytes) = core::Vector2{
					number(value.value("X", json(0.0)), 0.0f),
					number(value.value("Y", json(0.0)), 0.0f),
				};
				break;
			case PropertyType::UDim:
				*reinterpret_cast<core::UDim *>(bytes) = core::UDim{
					number(value.value("Scale", json(0.0)), 0.0f),
					number(value.value("Offset", json(0.0)), 0.0f),
				};
				break;
			case PropertyType::UDim2: {
				// **Missing axes default to zero rather than to what is
				// already there.** A caller sending only `{"X":...}` is
				// setting a size, and a half-write that kept the old Y would
				// be a value nobody authored - the read-modify-write belongs
				// in the caller, which can see both halves.
				const json x = value.value("X", json::object());
				const json y = value.value("Y", json::object());
				*reinterpret_cast<core::UDim2 *>(bytes) = core::UDim2{
					number(x.value("Scale", json(0.0)), 0.0f),
					number(x.value("Offset", json(0.0)), 0.0f),
					number(y.value("Scale", json(0.0)), 0.0f),
					number(y.value("Offset", json(0.0)), 0.0f),
				};
				break;
			}
			case PropertyType::Rect: {
				const json min = value.value("Min", json::object());
				const json max = value.value("Max", json::object());
				*reinterpret_cast<core::Rect *>(bytes) = core::Rect{
					number(min.value("X", json(0.0)), 0.0f),
					number(min.value("Y", json(0.0)), 0.0f),
					number(max.value("X", json(0.0)), 0.0f),
					number(max.value("Y", json(0.0)), 0.0f),
				};
				break;
			}
			case PropertyType::NumberRange:
				*reinterpret_cast<core::NumberRange *>(bytes) = core::NumberRange{
					number(value.value("Min", json(0.0)), 0.0f),
					number(value.value("Max", json(0.0)), 0.0f),
				};
				break;
			case PropertyType::NumberSequence: {
				// **Placement-new rather than a cast and an assign.** Every other
				// case here writes over a buffer that is already a valid object of
				// nothing at all, which is fine for four floats; a sequence is
				// three hundred bytes with a `Count` that decides which of them
				// mean anything, and assigning through a cast to storage that was
				// never constructed reads the old `Count` on the way past. Zeroed
				// storage makes that harmless today and would not the first time a
				// buffer was reused.
				auto *out = new (bytes) core::NumberSequence();
				const json stops = value.value("Keypoints", value);
				if (!stops.is_array()) {
					failure = "a NumberSequence takes {\"Keypoints\": [...]}";
					return false;
				}
				for (const json &stop : stops) {
					// Refused rather than truncated, for the reason
					// `NumberSequence.new` gives in the Luau binding: a gradient
					// silently missing its last stop is subtly wrong everywhere
					// and obviously wrong nowhere.
					if (!out->Add(
							core::NumberKeypoint{
								number(stop.value("Time", json(0.0)), 0.0f),
								number(stop.value("Value", json(0.0)), 0.0f),
								number(stop.value("Envelope", json(0.0)), 0.0f),
							}
						)) {
						failure = "a NumberSequence holds at most 20 keypoints";
						return false;
					}
				}
				break;
			}
			case PropertyType::ColorSequence: {
				auto *out = new (bytes) core::ColorSequence();
				const json stops = value.value("Keypoints", value);
				if (!stops.is_array()) {
					failure = "a ColorSequence takes {\"Keypoints\": [...]}";
					return false;
				}
				for (const json &stop : stops) {
					const json colour = stop.value("Value", json::object());
					if (!out->Add(
							core::ColorKeypoint{
								number(stop.value("Time", json(0.0)), 0.0f),
								core::Color3{
									number(colour.value("R", json(0.0)), 0.0f),
									number(colour.value("G", json(0.0)), 0.0f),
									number(colour.value("B", json(0.0)), 0.0f),
								},
							}
						)) {
						failure = "a ColorSequence holds at most 20 keypoints";
						return false;
					}
				}
				break;
			}
			case PropertyType::Reference:
			case PropertyType::Opaque:
				failure = "that value cannot be written through this surface yet";
				return false;
			}
			return true;
		}

		// Converts one JSON value into a property's storage and writes it.
		bool WriteProperty(
			Store &store,
			ecs::Entity instance,
			const PropertyDescriptor &property,
			const json &value,
			std::string &failure
		) {
			// The write half of the read path's exception - see `ReadProperty`.
			if (property.Type == PropertyType::String) {
				if (!value.is_string()) {
					failure = "that property takes a string";
					return false;
				}

				const std::string text = value.get<std::string>();
				if (!store.SetProperty(instance, property, &text, sizeof(text))) {
					failure = "the world refused the write - it may be running or a replica";
					return false;
				}
				return true;
			}

			// Sized from the descriptor, exactly as the script bindings are, so
			// this cannot be the place a size mismatch is introduced.
			alignas(16) unsigned char bytes[WIDEST_PROPERTY] = {};
			if (property.Size > sizeof(bytes)) {
				failure = "property too large to write";
				return false;
			}

			if (!ValueFromJson(property.Type, value, bytes, failure)) {
				return false;
			}

			if (!store.SetProperty(instance, property, bytes, property.Size)) {
				// The store refuses a write on an adopt-only world, which is what
				// a replica is. Said plainly rather than as a bare false.
				failure = "the world refused the write - it may be running or a replica";
				return false;
			}
			return true;
		}
	}

	json ComponentCatalogue() {
		json out = json::array();
		for (uint32_t index = 0; index < static_cast<uint32_t>(ecs::Components::Count()); index++) {
			const ecs::TypeDescriptor &type = ecs::Components::Describe(ecs::ComponentId(index));
			const std::string name(type.Name.Text());
			const size_t dot = name.find('.');

			out.push_back(
				json{
					{"name", name},
					{"module", dot == std::string::npos ? std::string() : name.substr(0, dot)},
					{"bytes", type.Size},
					{"tag", type.Kind == ecs::ComponentKind::Tag},
					{"saved", type.Serialisable},
					{"wireBytes", type.Wire.Present() ? type.Wire.Size : 0},
				}
			);
		}

		std::sort(out.begin(), out.end(), [](const json &a, const json &b) {
			return a.at("name").get<std::string>() < b.at("name").get<std::string>();
		});
		return json{{"components", std::move(out)}, {"count", ecs::Components::Count()}};
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

		// **The engine's own components, which `component_list` deliberately does
		// not show.** That tool reads `ecs::Schemas`, which is what a *game*
		// declared for itself; the engine's hundred and twenty-nine are reached
		// as properties through `instance_get`, and only where a class exposes
		// one. So a model could not ask what storage a world actually has, and
		// the answer was "read the C++".
		//
		// Every field here comes from the registry rather than from a list kept
		// beside it, for `componentdoc`'s reason: a hand-maintained copy of a
		// generated fact is the one that goes stale. `docs/ECS_COMPONENTS.md` is
		// the same table with a written purpose per row, which this cannot carry
		// because the purpose lives in a file and not in the process.
		//
		// **Takes no world**, unlike its neighbours. The component table is
		// per-process and sealed before any world ticks; asking it about a
		// particular scene would imply it could differ between two, which is
		// exactly the belief `Components::Seal` exists to prevent.
		Add(Tool{
			"engine_components",
			"Every component type this engine registers, with its size, whether it is a tag, "
			"whether a save file can carry it and whether replication has a compact form for it. "
			"A component is the storage under the class tree: an instance is an entity and a class "
			"is a set of these. Use component_list instead for the components a game declared for "
			"itself, and instance_get to read one off a particular instance.",
			[] { return json{{"type", "object"}}; },
			[](const json &, std::string &) { return ComponentCatalogue(); },
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
					// where walking the tree is O(instances) - per world, per
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
							{"physicsTickRate", worlds->SettingsOf(id).PhysicsTickRate},
							{"replicationTickRate", worlds->SettingsOf(id).ReplicationTickRate},
							{"instances", instances},
							{"statistics",
							 json{
								 {"ticks", statistics.Ticks},
								 {"lastTickMs", statistics.LastTickMilliseconds},
								 {"slowestTickMs", statistics.SlowestTickMilliseconds},
								 {"faults", statistics.Faults},
								 {"droppedTicks", statistics.DroppedTicks},
								 {"replicationTicks", statistics.ReplicationTicks},
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
					failure = "instance_get needs an `id` - take one from world_tree";
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

		// --- the storage underneath ------------------------------------------

		Add(Tool{
			"component_list",
			"Every component a game declared for itself, with its fields and how many entities in a "
			"scene carry it. A component is the storage under the class tree: an instance is an "
			"entity and a class is a set of these. Components the engine declares are not listed "
			"here - those are reached as properties, through instance_get.",
			[] { return WorldSchema(); },
			[worlds](const json &arguments, std::string &failure) -> json {
				const WorldId world = WorldArgument(*worlds, arguments, failure);
				if (!failure.empty()) {
					return nullptr;
				}

				json out = json::array();
				worlds->Enter(world, [&](Store &store) {
					for (const ecs::ComponentId id : ecs::Schemas::All()) {
						const ecs::Schema *schema = ecs::Schemas::Of(id);
						if (schema == nullptr) {
							continue;
						}

						const ecs::ComponentId terms[] = {id};
						out.push_back(
							json{
								{"name", std::string(schema->Name().Text())},
								{"fields", ReadSchema(*schema)},
								{"entities", store.CountMatching(terms)},
							}
						);
					}
				});

				return json{{"components", std::move(out)}};
			},
		});

		Add(Tool{
			"entity_query",
			"Every entity in a scene carrying all of the named components. This is the query a "
			"system runs, asked from outside: name the components and get the ids back, then read "
			"one with component_get or instance_get.",
			[] {
				json schema = WorldSchema();
				schema["properties"]["components"] = json{
					{"type", "array"},
					{"items", json{{"type", "string"}}},
					{"description", "Every one an entity must carry. Call component_list first."},
				};
				schema["properties"]["limit"] =
					json{{"type", "integer"}, {"description", "Ids at most. Default 200."}};
				schema["required"] = json::array({"components"});
				return schema;
			},
			[worlds](const json &arguments, std::string &failure) -> json {
				const WorldId world = WorldArgument(*worlds, arguments, failure);
				if (!failure.empty()) {
					return nullptr;
				}

				std::vector<ecs::ComponentId> terms;
				if (!ComponentArgument(arguments, terms, failure)) {
					return nullptr;
				}

				// Bounded for the reason `world_tree` is: a tool holds up the
				// loop it runs in, and fifty thousand ids is a reply nothing can
				// read and a frame nothing can draw.
				const auto limit = static_cast<size_t>(std::max(1, arguments.value("limit", 200)));

				json ids = json::array();
				size_t total = 0;

				worlds->Enter(world, [&](Store &store) {
					total = store.CountMatching(terms);
					store.EachMatching(terms, [&](ecs::Entity entity) {
						if (ids.size() < limit) {
							ids.push_back(entity.Id);
						}
					});
				});

				json out{{"entities", std::move(ids)}, {"total", total}};
				if (total > limit) {
					// Said rather than implied, exactly as `world_tree` says it.
					out["truncated"] = true;
				}
				return out;
			},
		});

		Add(Tool{
			"component_get",
			"The values one entity holds for one component, field by field. Null when the entity "
			"does not carry it - which is a different answer from carrying it with every field at "
			"zero.",
			[] {
				json schema = WorldSchema();
				schema["properties"]["id"] = json{{"type", "integer"}, {"description", "The entity id."}};
				schema["properties"]["component"] = json{{"type", "string"}};
				schema["required"] = json::array({"id", "component"});
				return schema;
			},
			[worlds](const json &arguments, std::string &failure) -> json {
				const WorldId world = WorldArgument(*worlds, arguments, failure);
				if (!failure.empty()) {
					return nullptr;
				}
				if (!arguments.contains("id") || !arguments["id"].is_number()) {
					failure = "which entity? - call entity_query or world_tree";
					return nullptr;
				}

				ecs::ComponentId id;
				const ecs::Schema *schema = SchemaArgument(arguments, id, failure);
				if (schema == nullptr) {
					return nullptr;
				}

				const ecs::Entity entity(arguments["id"].get<uint64_t>());
				json out = nullptr;

				worlds->Enter(world, [&](Store &store) {
					if (!store.HasComponent(entity, id)) {
						return;
					}

					const auto *bytes = static_cast<const unsigned char *>(store.GetComponent(entity, id));
					json fields = json::object();

					for (const ecs::FieldDescriptor &field : schema->Fields()) {
						fields[std::string(field.Spelling)] =
							field.Type == PropertyType::String
								? json(*reinterpret_cast<const std::string *>(bytes + field.Offset))
								: ValueToJson(field.Type, bytes + field.Offset);
					}
					out = json{
						{"id", entity.Id},
						{"component", std::string(schema->Name().Text())},
						{"fields", std::move(fields)}
					};
				});

				return out;
			},
		});

		if (writable) {
			Add(Tool{
				"component_set",
				"Writes fields of one component on one entity, adding it when the entity does not "
				"carry it yet. Fields left out keep what they had; a fresh component starts at "
				"zero. Vector3 and Color3 take an object of named components, as instance_set does.",
				[] {
					json schema = WorldSchema();
					schema["properties"]["id"] = json{{"type", "integer"}};
					schema["properties"]["component"] = json{{"type", "string"}};
					schema["properties"]["fields"] = json::object();
					schema["required"] = json::array({"id", "component", "fields"});
					return schema;
				},
				[worlds](const json &arguments, std::string &failure) -> json {
					const WorldId world = WorldArgument(*worlds, arguments, failure);
					if (!failure.empty()) {
						return nullptr;
					}
					if (!arguments.contains("id") || !arguments["id"].is_number()) {
						failure = "which entity? - call entity_query or world_tree";
						return nullptr;
					}
					if (!arguments.contains("fields") || !arguments["fields"].is_object()) {
						failure = "fields has to be an object of field names to values";
						return nullptr;
					}

					ecs::ComponentId id;
					const ecs::Schema *schema = SchemaArgument(arguments, id, failure);
					if (schema == nullptr) {
						return nullptr;
					}

					const ecs::Entity entity(arguments["id"].get<uint64_t>());
					json written = json::array();

					worlds->Enter(world, [&](Store &store) {
						if (!store.Alive(entity)) {
							failure = "no such entity in that scene";
							return;
						}

						// The component's own lifetime hooks, because a schema
						// holding a string field owns an allocation - the same
						// holder both script bindings carry, for the same
						// reason.
						const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(id);
						std::vector<unsigned char> value(schema->Size());
						descriptor.DefaultConstruct(value.data(), 1);

						// The current value first, so a field the caller left
						// out keeps it.
						if (const void *held = store.GetComponent(entity, id); held != nullptr) {
							descriptor.Destruct(value.data(), 1);
							descriptor.CopyConstruct(value.data(), held, 1);
						}

						for (const auto &field : arguments["fields"].items()) {
							const ecs::FieldDescriptor *found = schema->Find(std::string_view(field.key()));
							if (found == nullptr) {
								failure = "'" + std::string(schema->Name().Text()) + "' has no field '" +
										  field.key() + "'";
								break;
							}

							if (found->Type == PropertyType::String) {
								if (!field.value().is_string()) {
									failure = "'" + field.key() + "' takes a string";
									break;
								}
								*reinterpret_cast<std::string *>(value.data() + found->Offset) =
									field.value().get<std::string>();
							} else if (!ValueFromJson(
										   found->Type, field.value(), value.data() + found->Offset, failure
									   )) {
								failure = "'" + field.key() + "': " + failure;
								break;
							}
							written.push_back(field.key());
						}

						if (failure.empty()) {
							store.SetComponent(entity, id, value.data());
						}
						descriptor.Destruct(value.data(), 1);
					});

					if (!failure.empty()) {
						return nullptr;
					}
					return json{
						{"id", entity.Id},
						{"component", std::string(schema->Name().Text())},
						{"fields", std::move(written)},
					};
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
					// before the next one - see `WantsProfiling`.
					Profiling = true;
					core::FrameGraph::SetEnabled(true);
					return json{
						{"spans", json::array()},
						{"note", "profiling was off and is now on - call again for the next frame"},
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
