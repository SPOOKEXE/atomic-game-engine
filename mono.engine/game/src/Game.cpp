#include "UniversePaths.hpp"

#include <engine/bakegraph/Document.hpp>
#include <engine/core/Chars.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Values.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Clock.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scripthost/Runtime.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::game {

	namespace {
		using ecs::Classes;
		using ecs::ClassId;
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		using ecs::PropertyDescriptor;
		using ecs::PropertyType;
		using ecs::Store;

		constexpr std::string_view GAME_ROOT = "Game";
		constexpr std::string_view UNIVERSE_ROOT = "UniverseManifest";
		constexpr std::string_view WORLD_ROOT = "World";
		constexpr size_t MAXIMUM_WORLD_BYTES = 256u * 1024u * 1024u;
		constexpr uint32_t UNIVERSE_FORMAT_VERSION = 1;
		constexpr size_t MAXIMUM_UNIVERSE_WORLDS = 4096;

		// A world's settings, as of format 2. See `WriteWorldProperties`.
		constexpr std::string_view WORLD_PROPERTIES = "WorldProperties";
		constexpr std::string_view RENDERING_PROFILES = "RenderingProfiles";

		// A world's bake pipelines. See `WriteAssetPipelines`.
		//
		// **Not `Pipelines`**, which is the render half's element and is still
		// recognised by the reader below. Two kinds of pipeline under one tag
		// would make a file's meaning depend on which build wrote it.
		constexpr std::string_view ASSET_PIPELINES = "AssetPipelines";

		// One instance and its subtree, on its own. See
		// `WriteInstanceDocument`.
		constexpr std::string_view INSTANCE_ROOT = "Instance";

		// Whether an instance belongs to the viewer rather than to the game.
		//
		// See `scene::TransientComponent`. Asked here rather than by class,
		// because "is this a camera" is the wrong question - a script may make
		// a camera that *is* content, and a viewer may make something that is
		// not a camera.
		bool IsTransient(const Store &store, Entity instance) {
			return store.Has<scene::TransientComponent>(instance);
		}

		// Written as an attribute and never as a property, because the tree
		// carries it. A `Parent` property in the file would be a second answer
		// to a question the nesting already answers, and rule 2 is about
		// exactly that.
		bool IsStructural(core::Name property) {
			static const core::Name PARENT("Parent");
			static const core::Name NAME("Name");
			return property == PARENT || property == NAME;
		}

		// The class defaults, one scratch instance per class, built on demand.
		//
		// **This is what makes a save file readable.** A `Part` exposes fifteen
		// properties and a scene sets three; writing all fifteen turns a file
		// into a wall and a one-property change into a diff nobody can read. So
		// each property is compared against a fresh instance of the same class
		// and skipped when it matches - which costs one throwaway instance per
		// class in the file, not per instance.
		//
		// The scratch store is this object's own. Creating the probe in the
		// world being saved would put an instance into somebody's scene for the
		// duration of a save, and a save that mutates what it is saving is the
		// worst possible kind of bug to find.
		class Defaults {
		  public:
			Defaults() : Scratch("game.defaults") {}

			// The default value of one property for a class, or nothing when
			// the class's own instances cannot answer.
			const PropertyValue *Of(ClassId id, const PropertyDescriptor &descriptor) {
				const Entity probe = ProbeFor(id);
				if (probe == NULL_ENTITY) {
					return nullptr;
				}

				const uint64_t key =
					(static_cast<uint64_t>(id.Index) << 32) | static_cast<uint64_t>(descriptor.Name.Id());

				if (const auto found = Values.find(key); found != Values.end()) {
					return found->second.Known ? &found->second.Value : nullptr;
				}

				Cached entry;
				entry.Known = ReadProperty(Scratch, probe, descriptor, entry.Value);
				const auto inserted = Values.emplace(key, entry).first;
				return inserted->second.Known ? &inserted->second.Value : nullptr;
			}

		  private:
			struct Cached {
				PropertyValue Value;
				bool Known = false;
			};

			Entity ProbeFor(ClassId id) {
				if (const auto found = Probes.find(id.Index); found != Probes.end()) {
					return found->second;
				}
				const Entity probe = Scratch.CreateInstance(id);
				Probes.emplace(id.Index, probe);
				return probe;
			}

			Store Scratch;
			std::unordered_map<uint32_t, Entity> Probes;
			std::unordered_map<uint64_t, Cached> Values;
		};

		// Every instance in a world, in the order a document writes them, with
		// each one's document-local id.
		//
		// Built before anything is written, because a reference may point
		// forward - a spotlight naming the part it follows, declared later in
		// the tree - and a writer that assigned ids as it went could not
		// resolve one.
		struct Numbering {
			std::unordered_map<uint64_t, uint32_t> Ids;
			uint32_t Next = 1;

			void Walk(const Store &store, Entity instance) {
				if (IsTransient(store, instance)) {
					return;
				}

				Ids.emplace(Key(instance), Next++);
				store.EachChild(instance, [&](Entity child) { Walk(store, child); });
			}

			uint32_t Of(Entity instance) const {
				const auto found = Ids.find(Key(instance));
				return found == Ids.end() ? 0 : found->second;
			}

			static uint64_t Key(Entity instance) {
				// The whole id, which already carries the generation in its
				// high bits - so a recycled slot is not mistaken for the
				// instance that used to live in it. Reading the two halves
				// apart would be this file depending on a layout `Entity`
				// deliberately does not expose.
				return instance.Id;
			}
		};

		void WriteInstance(
			XmlWriter &writer, Store &store, Entity instance, const Numbering &numbering, Defaults &defaults
		) {
			// **The viewer's own instances are not the file's.** A camera is
			// made by whoever is looking - the editor for its viewport, a client
			// for its player, one each when several people edit together - and
			// writing one out would put somebody's point of view into everyone
			// else's copy. Skipped with its whole subtree, because anything
			// parented to a camera belongs to that camera.
			if (IsTransient(store, instance)) {
				return;
			}

			const ClassId id = store.ClassOf(instance);
			if (!id.IsValid()) {
				// Not every entity is an instance - a resource-carrying row, a
				// predicted entity - and a document holds instances. Skipped
				// rather than written as a class nobody can read back.
				return;
			}

			const ecs::ClassInfo &info = Classes::Describe(id);

			writer.Open("Item");
			writer.Attribute("class", info.Name.Text());

			if (const core::Name name = store.InstanceNameOf(instance); name.IsValid()) {
				writer.Attribute("name", name.Text());
			}
			writer.Attribute("id", std::to_string(numbering.Of(instance)));

			for (const PropertyDescriptor &descriptor : info.Properties) {
				if (IsStructural(descriptor.Name) || !descriptor.Writable) {
					continue;
				}

				PropertyValue value;
				if (!ReadProperty(store, instance, descriptor, value)) {
					// The instance does not carry what the getter reads. Not an
					// error: whether a part is simulated is an archetype, so an
					// anchored part has no `scene::Motion` for a dynamic-only
					// property to read from.
					continue;
				}

				if (descriptor.Type == PropertyType::Reference) {
					// **Only a reference inside this document is written.** One
					// pointing at an instance in another world is meaningless in
					// a file - `Entity` is a handle within one world and says so
					// - and writing a dangling id would resolve to whatever
					// happened to take that number on load.
					const uint32_t target =
						value.Reference == NULL_ENTITY ? 0 : numbering.Of(value.Reference);
					if (target == 0) {
						continue;
					}

					writer.Open("Property");
					writer.Attribute("name", descriptor.Name.Text());
					writer.Attribute("type", TypeTag(descriptor.Type));
					writer.Text(std::to_string(target));
					writer.Close();
					continue;
				}

				if (const PropertyValue *fallback = defaults.Of(id, descriptor);
					fallback != nullptr && ValuesEqual(value, *fallback)) {
					continue;
				}

				writer.Open("Property");
				writer.Attribute("name", descriptor.Name.Text());
				writer.Attribute("type", TypeTag(descriptor.Type));
				writer.Text(FormatValue(value));
				writer.Close();
			}

			store.EachChild(instance, [&](Entity child) {
				WriteInstance(writer, store, child, numbering, defaults);
			});

			writer.Close();
		}

		// A reference waiting for the instance it names to exist.
		struct PendingReference {
			Entity Instance;
			core::Name Property;
			uint32_t Target = 0;
		};

		bool ReadInstance(
			const XmlDocument &document,
			const XmlElement &element,
			Store &store,
			Entity parent,
			std::unordered_map<uint32_t, Entity> &byId,
			std::vector<PendingReference> &pending,
			std::string &error
		) {
			const std::string_view className = element.Attribute("class");
			const ClassId id = Classes::Find(core::Name(className));
			if (!id.IsValid()) {
				// **Refused rather than skipped**, and the file names what it
				// wanted. A game file holding a class this build does not have
				// is a file from a newer engine or a file for a game with a
				// module this program did not link, and silently dropping the
				// instance would open a world that is missing things nobody can
				// name.
				error = "no class named '" + std::string(className) + "'";
				return false;
			}

			const Entity instance = store.CreateInstance(id, element.Attribute("name"));
			if (instance == NULL_ENTITY) {
				error = "the world refused an instance of '" + std::string(className) + "'";
				return false;
			}

			if (parent != NULL_ENTITY) {
				store.SetParent(instance, parent);
			}

			if (element.HasAttribute("id")) {
				const std::string_view text = element.Attribute("id");
				uint32_t local = 0;
				if (std::from_chars(text.data(), text.data() + text.size(), local).ec == std::errc{} &&
					local != 0) {
					byId.emplace(local, instance);
				}
			}

			const ecs::ClassInfo &info = Classes::Describe(id);

			for (const uint32_t childIndex : element.Children) {
				const XmlElement *child = document.At(childIndex);
				if (child == nullptr) {
					continue;
				}

				if (child->Name == "Item") {
					if (!ReadInstance(document, *child, store, instance, byId, pending, error)) {
						return false;
					}
					continue;
				}

				if (child->Name != "Property") {
					continue;
				}

				const core::Name property(child->Attribute("name"));

				const PropertyDescriptor *descriptor = nullptr;
				for (const PropertyDescriptor &candidate : info.Properties) {
					if (candidate.Name == property) {
						descriptor = &candidate;
						break;
					}
				}

				if (descriptor == nullptr) {
					// **Ignored rather than refused, and the asymmetry with an
					// unknown class is deliberate.** A property this build does
					// not have is a field that was removed or renamed, and
					// dropping it loses one value; a class this build does not
					// have loses a whole subtree. Those deserve different
					// answers, and treating them the same would either make
					// every engine update reject old files or make a missing
					// module invisible.
					ENGINE_WARN(
						"game file: '{}' has no property '{}', ignored",
						info.Name.Text(),
						property.IsValid() ? property.Text() : "?"
					);
					continue;
				}

				PropertyType type = descriptor->Type;
				if (child->HasAttribute("type")) {
					PropertyType declared = PropertyType::Opaque;
					// **Compared as tags rather than as enum members.** `Name` and
					// `String` are both written `string`, because whether the
					// engine interns text is a storage decision and a file holds
					// text either way - so moving a property between them must
					// not turn every saved game into a load error.
					if (TypeFromTag(child->Attribute("type"), declared) &&
						TypeTag(declared) != TypeTag(descriptor->Type)) {
						error = "'" + std::string(info.Name.Text()) + "." + std::string(property.Text()) +
								"' is a " + std::string(TypeTag(descriptor->Type)) + " and the file has a " +
								std::string(TypeTag(declared));
						return false;
					}
					type = descriptor->Type;
				}

				if (type == PropertyType::Reference) {
					uint32_t target = 0;
					const std::string &text = child->Text;
					if (std::from_chars(text.data(), text.data() + text.size(), target).ec == std::errc{} &&
						target != 0) {
						// Deferred, because the instance it names may not exist
						// yet - a reference pointing forward in the tree is
						// ordinary, and resolving in one pass would silently
						// drop every one of them.
						pending.push_back(PendingReference{instance, property, target});
					}
					continue;
				}

				PropertyValue value;
				std::string reason;
				if (!ParseValue(type, child->Text, value, reason)) {
					error = "'" + std::string(info.Name.Text()) + "." + std::string(property.Text()) +
							"': " + reason;
					return false;
				}

				if (!WriteProperty(store, instance, *descriptor, value)) {
					error = "'" + std::string(info.Name.Text()) + "." + std::string(property.Text()) +
							"' refused the value '" + child->Text + "'";
					return false;
				}
			}

			return true;
		}

		void WriteSources(XmlWriter &writer, const Store &store) {
			const auto *cache = store.Resource<script::SourceCache>();
			if (cache == nullptr || cache->Count() == 0) {
				return;
			}

			writer.Open("Sources");
			for (const script::SourceRow &row : cache->Rows) {
				writer.Open("Source");
				writer.Attribute("path", row.Path.IsValid() ? row.Path.Text() : "");

				// CDATA rather than escaped text. A program is the one thing in
				// this format a person reads in the file, and `&lt;` on every
				// comparison makes it unreadable. `XmlWriter::Verbatim` handles
				// the `]]>` case, which is ordinary code rather than a corner.
				writer.Verbatim(row.Text);
				writer.Close();
			}
			writer.Close();
		}

		// Every script path a subtree names, in walk order.
		//
		// Asked through the property surface rather than by reaching for the
		// `Source` component, so a class that grows a second way to carry a
		// program is covered by declaring the property rather than by editing
		// this.
		void CollectSourcePaths(const Store &store, Entity instance, std::vector<core::Name> &out) {
			static const core::Name SOURCE("Source");

			const ClassId container = Classes::Find(core::Name("LuaSourceContainer"));
			if (container.IsValid() && store.IsA(instance, container)) {
				core::Name path;
				if (store.GetProperty(instance, SOURCE, &path, sizeof(path)) && path.IsValid()) {
					out.push_back(path);
				}
			}

			store.EachChild(instance, [&](Entity child) { CollectSourcePaths(store, child, out); });
		}

		// `WriteSources`, restricted to a set of paths.
		void WriteSourcesFor(XmlWriter &writer, const Store &store, const std::vector<core::Name> &paths) {
			if (paths.empty()) {
				return;
			}

			const auto *cache = store.Resource<script::SourceCache>();
			if (cache == nullptr || cache->Count() == 0) {
				return;
			}

			bool opened = false;
			for (const script::SourceRow &row : cache->Rows) {
				if (std::find(paths.begin(), paths.end(), row.Path) == paths.end()) {
					continue;
				}

				if (!opened) {
					writer.Open("Sources");
					opened = true;
				}

				writer.Open("Source");
				writer.Attribute("path", row.Path.IsValid() ? row.Path.Text() : "");
				writer.Verbatim(row.Text);
				writer.Close();
			}

			if (opened) {
				writer.Close();
			}
		}

		// `ReadSources`, into a world that already has scripts of its own.
		void MergeSources(const XmlDocument &document, const XmlElement &element, Store &store) {
			script::SourceCache cache;
			if (const auto *existing = store.Resource<script::SourceCache>(); existing != nullptr) {
				cache = *existing;
			}

			for (const uint32_t index : element.Children) {
				const XmlElement *source = document.At(index);
				if (source == nullptr || source->Name != "Source") {
					continue;
				}

				const std::string_view path = source->Attribute("path");
				if (path.empty()) {
					continue;
				}

				// **The incoming text wins a collision**, because the thing
				// being moved is the thing being asked for. Two worlds holding
				// different programs at one path is already a name clash the
				// author has to resolve; silently keeping the old one would
				// move a script whose body changed on arrival.
				cache.Set(core::Name(path), source->Text);
			}

			if (cache.Count() > 0) {
				store.SetResource(cache);
			}
		}

		void WriteRenderingProfiles(XmlWriter &writer, const graph::PipelineSet &profiles) {
			if (profiles.Count() == 0) {
				return;
			}

			writer.Open(RENDERING_PROFILES);
			writer.Verbatim(graph::Write(profiles));
			writer.Close();
		}

		void ReadRenderingProfiles(const XmlElement &element, graph::PipelineSet &profiles) {
			core::Name offender;
			const graph::PipelineDocumentStatus status = graph::Read(element.Text, profiles, offender);
			if (status == graph::PipelineDocumentStatus::Ok) {
				return;
			}

			ENGINE_WARN(
				"rendering profiles: {} at '{}'; the universe loads with the standard graph",
				graph::Describe(status),
				offender.IsValid() ? offender.Text() : ""
			);
			profiles.Clear();
		}

		// A malformed render graph loses authored rendering data, not the world's
		// instances. The standard graph is a safe fallback and the document stays
		// repairable in Studio.
		void ReadPipelines(const XmlElement &element, Store &store) {
			graph::PipelineSet set;
			core::Name offender;
			const graph::PipelineDocumentStatus status = graph::Read(element.Text, set, offender);
			if (status != graph::PipelineDocumentStatus::Ok) {
				ENGINE_WARN(
					"world pipelines: {} at '{}'; the world loads with the standard graph",
					graph::Describe(status),
					offender.IsValid() ? offender.Text() : ""
				);
				return;
			}

			if (set.Count() > 0) {
				store.SetResource(std::move(set));
			}
		}

		core::Name
		MigrateWorldPipelines(Store &store, core::Name worldName, graph::PipelineSet &renderingProfiles) {
			const graph::PipelineSet *legacy = store.Resource<graph::PipelineSet>();
			if (legacy == nullptr || legacy->Count() == 0) {
				return {};
			}

			core::Name selected;
			for (const core::Name name : legacy->Names()) {
				const graph::PipelineDocument *document = legacy->Find(name);
				if (document == nullptr) {
					continue;
				}

				core::Name destination = name;
				if (const graph::PipelineDocument *existing = renderingProfiles.Find(name);
					existing != nullptr && graph::Write(*existing) != graph::Write(*document)) {
					const std::string base = std::string(worldName.Text()) + "/" + std::string(name.Text());
					destination = core::Name(base);
					for (uint32_t suffix = 2; renderingProfiles.Find(destination) != nullptr; suffix++) {
						destination = core::Name(base + " " + std::to_string(suffix));
					}
				}

				renderingProfiles.Set(destination, *document);
				if (!selected.IsValid() || name == core::Name("main")) {
					selected = destination;
				}
			}

			store.RemoveResource<graph::PipelineSet>();
			return selected;
		}

		// A world's asset pipelines, as one block of text.
		//
		// **Embedded rather than re-encoded as elements**, which is the shape
		// the render half used and the reason it used it: `bake::PipelineSet`
		// already has a line-oriented format that round trips and refuses a
		// malformed line, and restating it as XML would be a second grammar for
		// one thing - with the interesting failure that the two could disagree
		// about what a pipeline is while both parsing.
		//
		// CDATA for `WriteSources`'s reason: this is meant to be read in the
		// file, and a document full of `&quot;` is not.
		//
		// **A world writes this and an instance document does not**, unlike
		// `<Sources>`, which travels with the instance that names it. No
		// instance owns a bake pipeline - it names an asset, and the asset is
		// already baked - so copying a part into another world has nothing to
		// carry and must not invent something.
		//
		// **No format bump for the new element.** A world with no pipelines
		// writes no block, so every file this build produces for existing
		// content is byte-identical to the last one's; and an older build skips
		// a child it does not recognise, so a file with the block still loads
		// there minus its pipelines. Moving `FORMAT_VERSION` would turn that
		// into a refusal and buy nothing - the version is for a change that
		// makes an old *reading* wrong, which is what `2` was.
		void WriteAssetPipelines(XmlWriter &writer, const Store &store) {
			const auto *set = store.Resource<bake::PipelineSet>();
			if (set == nullptr || set->Count() == 0) {
				return;
			}

			writer.Open(ASSET_PIPELINES);
			writer.Verbatim(bake::Write(*set));
			writer.Close();
		}

		// **A document naming a node kind this build does not have loses the
		// pipelines and keeps the world**, which is the decision `D00102` left
		// open and is the render half's answer to the same question.
		//
		// `bake::Read` refuses an unknown kind as a malformed line - the node
		// vocabulary is a closed list, so `node bevel` from a newer editor is
		// indistinguishable from a typo and neither is worth guessing at. The
		// cost of that refusal is set here: a world whose parts all loaded and
		// whose pipelines did not is recoverable, because a pipeline is a recipe
		// for content that is already baked and sitting in the store. Refusing
		// the document would lose the parts too, which is a worse answer to the
		// same file, and dropping the offending pipeline while keeping its
		// neighbours would be this function deciding that half a saved set is a
		// set - it is not, it is a file from a build somebody should be told to
		// upgrade from.
		void ReadAssetPipelines(const XmlElement &element, Store &store) {
			bake::PipelineSet set;
			std::string offender;

			const bake::DocumentStatus status = bake::Read(element.Text, set, offender);
			if (status != bake::DocumentStatus::Ok) {
				ENGINE_WARN(
					"world asset pipelines: {} at '{}' - the world loads and bakes nothing",
					bake::Describe(status),
					offender
				);
				return;
			}

			if (set.Count() > 0) {
				store.SetResource(set);
			}
		}

		void ReadSources(const XmlDocument &document, const XmlElement &element, Store &store) {
			script::SourceCache cache;

			for (const uint32_t index : element.Children) {
				const XmlElement *source = document.At(index);
				if (source == nullptr || source->Name != "Source") {
					continue;
				}

				const std::string_view path = source->Attribute("path");
				if (path.empty()) {
					continue;
				}
				cache.Set(core::Name(path), source->Text);
			}

			if (cache.Count() > 0) {
				store.SetResource(cache);
			}
		}

		std::string_view
		TextOf(const XmlElement &element, std::string_view attribute, std::string_view fallback) {
			return element.Attribute(attribute, fallback);
		}

		double NumberOf(const XmlElement &element, std::string_view attribute, double fallback) {
			const std::string_view text = element.Attribute(attribute);
			if (text.empty()) {
				return fallback;
			}
			double value = 0.0;
			if (core::FromChars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
				return fallback;
			}
			return value;
		}

		uint32_t CountOf(const XmlElement &element, std::string_view attribute, uint32_t fallback) {
			const std::string_view text = element.Attribute(attribute);
			if (text.empty()) {
				return fallback;
			}
			uint32_t value = 0;
			if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
				return fallback;
			}
			return value;
		}

		void WriteUniverseSettings(XmlWriter &writer, const world::UniverseSettings &settings) {
			writer.Open("Universe");
			writer.Attribute(
				"mode", settings.Mode == world::ExecutionMode::WorldSerial ? "WorldSerial" : "WorldParallel"
			);
			writer.Attribute("catchUp", std::to_string(settings.MaximumCatchUpTicks));
			writer.Attribute("worldParallelFloor", FormatNumber(settings.WorldParallelFloorMilliseconds));
			writer.Attribute("busBudget", std::to_string(settings.BusBudgetPerTick));
			writer.Attribute("channelQueue", std::to_string(settings.ChannelQueueLimit));
			writer.Attribute("channelsPerWorld", std::to_string(settings.ChannelsPerWorld));
			writer.Close();
		}

		world::UniverseSettings
		ReadUniverseSettings(const XmlElement &element, const world::UniverseSettings &fallback) {
			world::UniverseSettings settings = fallback;
			settings.Mode = TextOf(element, "mode", "WorldParallel") == "WorldSerial"
								? world::ExecutionMode::WorldSerial
								: world::ExecutionMode::WorldParallel;
			settings.MaximumCatchUpTicks = static_cast<int>(
				CountOf(element, "catchUp", static_cast<uint32_t>(settings.MaximumCatchUpTicks))
			);
			settings.WorldParallelFloorMilliseconds = static_cast<float>(
				NumberOf(element, "worldParallelFloor", settings.WorldParallelFloorMilliseconds)
			);
			settings.BusBudgetPerTick = CountOf(element, "busBudget", settings.BusBudgetPerTick);
			settings.ChannelQueueLimit = CountOf(element, "channelQueue", settings.ChannelQueueLimit);
			settings.ChannelsPerWorld = CountOf(element, "channelsPerWorld", settings.ChannelsPerWorld);
			return settings;
		}

		void ApplyUniverseSettings(world::Universe &universe, const world::UniverseSettings &settings) {
			universe.SetMode(settings.Mode);
			universe.SetMaximumCatchUpTicks(settings.MaximumCatchUpTicks);
			universe.SetWorldParallelFloorMilliseconds(settings.WorldParallelFloorMilliseconds);
			universe.SetBusBudgetPerTick(settings.BusBudgetPerTick);
			universe.SetChannelQueueLimit(settings.ChannelQueueLimit);
			universe.SetChannelsPerWorld(settings.ChannelsPerWorld);
		}

		// The first child of an element with a given name, or null.
		const XmlElement *
		ChildNamed(const XmlDocument &document, const XmlElement &element, std::string_view name) {
			for (const uint32_t index : element.Children) {
				const XmlElement *child = document.At(index);
				if (child != nullptr && child->Name == name) {
					return child;
				}
			}
			return nullptr;
		}

		// **What a world *is*, and it stays on the element.** A name is the
		// world's identity - it is what a bus envelope, a subscription and a
		// teleport carry, and what `<World>` is looked up by while reading a
		// game - so it reads as part of the tag rather than as one of the
		// settings underneath it. The same split `<Item name=... >` already
		// makes against its `<Property>` children.
		void WriteWorldAttributes(XmlWriter &writer, const world::WorldSettings &settings) {
			writer.Attribute("name", settings.Name.IsValid() ? settings.Name.Text() : "");
		}

		// **What a world is *configured like*, and it is an element.**
		// `<Universe>` has carried the universe's settings this way since the
		// format existed; three tunables crammed onto the `<World>` tag was the
		// odd one out, and it was the version with nowhere to put a fourth.
		void WriteWorldProperties(XmlWriter &writer, const world::WorldSettings &settings) {
			writer.Open(WORLD_PROPERTIES);
			writer.Attribute("tickRate", FormatNumber(settings.TickRate));
			writer.Attribute("idleTickRate", FormatNumber(settings.IdleTickRate));
			writer.Attribute("physicsTickRate", FormatNumber(settings.PhysicsTickRate));
			writer.Attribute("scriptTickRate", FormatNumber(settings.ScriptTickRate));
			writer.Attribute("replicationTickRate", FormatNumber(settings.ReplicationTickRate));
			writer.Attribute(
				"globalSimulatedNetworkLatency", FormatNumber(settings.GlobalSimulatedNetworkLatency)
			);
			writer.Attribute("faultLimit", std::to_string(settings.FaultLimit));
			writer.Attribute(
				"isolation", settings.IsolationLevel == world::Isolation::Dedicated ? "Dedicated" : "Shared"
			);
			writer.Attribute(
				"renderingProfile",
				settings.RenderingProfile.IsValid() ? settings.RenderingProfile.Text() : ""
			);
			writer.Close();
		}

		world::WorldSettings ReadWorldAttributes(const XmlDocument &document, const XmlElement &element) {
			world::WorldSettings settings;
			settings.Name = core::Name(TextOf(element, "name", "World"));

			// **The child if it is there, the element if it is not.** That one
			// line is the whole of format 1 compatibility: an old file has the
			// numbers on `<World>` and a new one has them under
			// `<WorldProperties>`, and both are read by pointing the same three
			// lookups at whichever element carried them.
			//
			// Not keyed off the format number, deliberately. A reader that
			// branched on `format == 1` would be a reader that breaks on a file
			// somebody hand-edited into the new shape without touching the
			// version - and the shape is the thing being read either way.
			const XmlElement *properties = ChildNamed(document, element, WORLD_PROPERTIES);
			const XmlElement &source = properties != nullptr ? *properties : element;

			settings.TickRate = NumberOf(source, "tickRate", 60.0);
			settings.IdleTickRate = NumberOf(source, "idleTickRate", 2.0);

			// Zero for a file that predates them, which is the same "follow the
			// tick rate" every world in this repository already means.
			settings.PhysicsTickRate = NumberOf(source, "physicsTickRate", 0.0);
			settings.ScriptTickRate = NumberOf(source, "scriptTickRate", 0.0);
			settings.ReplicationTickRate = NumberOf(source, "replicationTickRate", 0.0);

			settings.GlobalSimulatedNetworkLatency = NumberOf(source, "globalSimulatedNetworkLatency", 0.0);
			settings.FaultLimit = CountOf(source, "faultLimit", 3);
			settings.IsolationLevel = TextOf(source, "isolation", "Shared") == "Dedicated"
										  ? world::Isolation::Dedicated
										  : world::Isolation::Shared;
			settings.RenderingProfile = core::Name(TextOf(source, "renderingProfile", "Default PBR"));
			return settings;
		}

		bool WriteFile(const std::filesystem::path &path, const std::string &text, std::string &error) {
			// The parent directory, because "Save As" into a folder that does
			// not exist yet is a thing somebody does once per project and a
			// failure there reads as the editor refusing to save.
			std::error_code code;
			if (path.has_parent_path()) {
				std::filesystem::create_directories(path.parent_path(), code);
			}

			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file) {
				error = "could not write " + path.string();
				return false;
			}

			file << text;
			if (!file) {
				error = "could not finish writing " + path.string();
				return false;
			}
			return true;
		}

		bool ReadFile(const std::filesystem::path &path, std::string &out, std::string &error) {
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				error = "could not open " + path.string();
				return false;
			}

			std::ostringstream contents;
			contents << file.rdbuf();
			out = contents.str();
			return true;
		}

		bool Parse(
			const std::string &text,
			std::string_view expectedRoot,
			XmlDocument &document,
			std::string &error,
			uint32_t maximumFormat = FORMAT_VERSION,
			size_t maximumBytes = XmlLimits{}.MaximumBytes
		) {
			uint32_t line = 0;
			XmlLimits limits;
			limits.MaximumBytes = maximumBytes;
			const XmlStatus status = ParseXml(text, document, limits, &line);
			if (status != XmlStatus::Ok) {
				error = std::string("line ") + std::to_string(line) + ": " + Describe(status);
				return false;
			}

			const XmlElement *root = document.Root();
			if (root == nullptr || root->Name != expectedRoot) {
				error = "expected a <" + std::string(expectedRoot) + "> document";
				return false;
			}

			const uint32_t format = CountOf(*root, "format", 0);
			if (format == 0 || format > maximumFormat) {
				error = "this is a format " + std::to_string(format) + " file and this build reads " +
						std::to_string(maximumFormat);
				return false;
			}

			return true;
		}
	}

	void RegisterGameClasses() {
		// `scene` first: `script`'s tree derives from `Instance`, which `scene`
		// registers, and `ScriptClass` reaches for it through `PartClass`
		// anyway. Naming both is what makes the order impossible to get wrong
		// from outside.
		scene::RegisterSceneClasses();

		// **The 2D tree, because a game file carries one.** A server authors a
		// `ScreenGui` and saves it; a loader that had not registered the class
		// would refuse a perfectly good file with "no class named ScreenGui",
		// which reads as a corrupt save rather than as a missing registration.
		gui::RegisterGuiClasses();

		// `Decal` and `Texture` are authored face images, not renderer state. A
		// world document may name either, so their class tree belongs beside the
		// other document-wide registrations rather than behind a client setup
		// path that happens to draw ribbons.
		effects::RegisterEffectClasses();

		script::ScriptClass();

		// **The bake pipelines a world carries, named here rather than by the
		// module that owns the type.** `bakegraph` links `core` and nothing
		// else - that is the whole point of it, and `ecs::Components::Register`
		// is out of its reach, so the alternative to this line is a link edge
		// that undoes the split. It is the exception `scene`'s registration
		// rule is stated against rather than a violation of it.
		//
		// Registered rather than left to `SetResource`, for the reason every
		// other line in this function exists: a resource is keyed by a component
		// id, and one first minted by `SetResource` takes the compiler's
		// spelling of the type - a world that saves, loads, and quietly has no
		// pipelines because the two spellings never met.
		//
		// No writer or reader: a pipeline set is authored content that travels
		// in the world document, not state that travels in a replication
		// snapshot. Registering it with serialisers would claim it belonged on
		// the wire.
		ecs::Components::Register<bake::PipelineSet>("bake.PipelineSet");

		// **The render pipelines a world carries.** Registered here rather than
		// left to a caller for the reason every other line in this function
		// exists: a resource is keyed by a component id, and one first minted by
		// `SetResource` takes the compiler's spelling of the type - a world that
		// saves, loads, and quietly has no pipelines because the two spellings
		// never met. Naming it beside the classes is what makes the order
		// impossible to get wrong from outside.
		graph::RegisterPipelineComponents();
	}

	void WriteWorldBody(XmlWriter &writer, Store &store) {
		RegisterGameClasses();

		WriteSources(writer, store);
		WriteAssetPipelines(writer, store);

		Numbering numbering;
		store.EachRoot([&](Entity root) { numbering.Walk(store, root); });

		Defaults defaults;
		store.EachRoot([&](Entity root) { WriteInstance(writer, store, root, numbering, defaults); });
	}

	bool
	ReadWorldBody(const XmlDocument &document, const XmlElement &element, Store &store, std::string &error) {
		RegisterGameClasses();

		std::unordered_map<uint32_t, Entity> byId;
		std::vector<PendingReference> pending;

		for (const uint32_t index : element.Children) {
			const XmlElement *child = document.At(index);
			if (child == nullptr) {
				continue;
			}

			if (child->Name == "Sources") {
				ReadSources(document, *child, store);
				continue;
			}

			if (child->Name == ASSET_PIPELINES) {
				ReadAssetPipelines(*child, store);
				continue;
			}

			if (child->Name == "Pipelines") {
				ReadPipelines(*child, store);
				continue;
			}

			if (child->Name != "Item") {
				continue;
			}

			if (!ReadInstance(document, *child, store, NULL_ENTITY, byId, pending, error)) {
				return false;
			}
		}

		// Second pass. Every instance exists now, so a reference pointing
		// forward resolves exactly as one pointing backward does.
		for (const PendingReference &reference : pending) {
			const auto found = byId.find(reference.Target);
			if (found == byId.end()) {
				// Dangling. A warning rather than a refusal: the file names an
				// instance that is not in it, which is a file that was edited
				// by hand or exported from a partial selection - and leaving
				// the property at its default is what a missing target means.
				ENGINE_WARN(
					"game file: reference '{}' names instance {} which is not in this world",
					reference.Property.Text(),
					reference.Target
				);
				continue;
			}

			const Entity target = found->second;
			store.SetProperty(reference.Instance, reference.Property, &target, sizeof(Entity));
		}

		return true;
	}

	std::shared_ptr<script::Runtime> StartWorldScripts(
		Store &store,
		ecs::Scheduler &scheduler,
		const script::RuntimeLimits &limits,
		std::string &error,
		const script::Debugger *breakpoints,
		double scriptTickRate
	) {
		error.clear();

		std::shared_ptr<script::Runtime> runtime = script::MakeRuntime(store, script::Language::Luau, limits);

		// **Before the scripts run, and that ordering is the whole point.** A
		// script's top level has already executed by the time this function
		// returns, so breakpoints adopted afterwards would silently never fire
		// on it - which is the failure a debugger can least afford.
		if (breakpoints != nullptr) {
			runtime->Debug().Adopt(*breakpoints);
		}

		const size_t ran = runtime->RunWorldScripts();
		script::SetScriptTickRate(store, scriptTickRate);
		if (!runtime->LastError().empty()) {
			error = runtime->LastError();
		}

		// **The fixed tick delta, never wall time.** A script integrating
		// against a real clock puts the scene in a different place on a busy
		// machine, and the recording stops replaying - the desync rule 5 names,
		// arriving through the call a script uses most.
		scheduler.Add("script-heartbeat", ecs::Phase::Simulation, [runtime](Store &world) {
			std::optional<float> delta = script::TakeScriptUpdate(world);
			while (delta) {
				if (!runtime->Heartbeat(*delta)) {
					// Logged per tick rather than swallowed. A world that silently
					// stopped animating is a bug report with nothing in it.
					ENGINE_ERROR("heartbeat: {}", runtime->LastError());
					return;
				}

				// A rate above the world clock may need several barriers this tick.
				// Zero returns the world delta and deliberately stops at one barrier.
				if (*delta >= world.Time().Delta) {
					return;
				}
				delta = script::TakeScriptUpdate(world);
			}
		});

		if (ran > 0) {
			ENGINE_INFO("world '{}': {} script(s) started", store.Name(), ran);
		}

		return runtime;
	}

	std::string
	WriteGame(world::Universe &universe, core::Name name, const graph::PipelineSet &renderingProfiles) {
		XmlWriter writer;

		writer.Open(GAME_ROOT);
		writer.Attribute("format", std::to_string(FORMAT_VERSION));
		writer.Attribute("name", name.IsValid() ? name.Text() : "Game");

		WriteUniverseSettings(writer, universe.Settings());

		WriteRenderingProfiles(writer, renderingProfiles);

		for (const world::WorldId id : universe.Worlds()) {
			// **A replica is not authored here, so it is not written here.**
			// A game file is authored content - that is the whole reason it is a
			// different format from a snapshot - and a world whose rows arrived
			// from somebody else's authority is a *view* of a game rather than
			// part of one. Writing it would put a second copy of the same scene
			// in the file under a second name, and loading that file would give
			// an author two scenes to edit where they had made one.
			//
			// Asked of the store rather than of the caller, because the store
			// already knows: `Store::AdoptOnly` is set on every world a
			// `replication::Connector` or the studio's own client view writes
			// into, and it is set for a reason that is exactly this one - the
			// rows are not this process's to mint. A flag passed in by each
			// caller would be the same fact recorded in three programs.
			if (!universe.IsRemote(id)) {
				bool replica = false;
				universe.Enter(id, [&replica](Store &store) { replica = store.AdoptOnly(); });
				if (replica) {
					continue;
				}
			}

			// **What the world is, not what a default-constructed one would
			// be.** This used to build a fresh `WorldSettings` and fill in only
			// the name, so every `<World>` in every file claimed 60Hz - a scene
			// authored at 30 saved as 60 and loaded as 60, and the number that
			// was lost was one nobody would think to check. `SettingsOf` exists
			// because moving these into an element made the lie legible.
			const world::WorldSettings worldSettings = universe.SettingsOf(id);

			writer.Open(WORLD_ROOT);
			WriteWorldAttributes(writer, worldSettings);

			// **Every attribute before the first child element**, which is now
			// a rule this loop has to keep rather than a thing it got for free.
			// `WriteWorldProperties` opens a child, and an `Attribute` call
			// after that lands on the child instead of on `<World>` - so the
			// host of a remote world is written here, above it.
			const bool remote = universe.IsRemote(id);
			if (remote) {
				// **A name and a host, not content.** A world held by a
				// supervised host has no store here, so its instances are not
				// this process's to write - and an empty `<World>` would be a
				// save file that quietly deleted somebody's scene.
				writer.Attribute("host", universe.HostOf(id).Text());
			}

			WriteWorldProperties(writer, worldSettings);

			if (!remote) {
				universe.Enter(id, [&](Store &store) { WriteWorldBody(writer, store); });
			}

			writer.Close();
		}

		writer.Close();
		return writer.Finish();
	}

	std::string WriteGame(world::Universe &universe, core::Name name) {
		return WriteGame(universe, name, graph::PipelineSet{});
	}

	namespace {
		constexpr size_t MAXIMUM_UNIVERSE_CDNS = 64;

		struct ManifestWorld {
			std::filesystem::path RelativePath;
			world::WorldSettings Settings;
			core::Name Host;
			bool Remote = false;
		};

		bool AuthoredLocalWorld(world::Universe &universe, world::WorldId id) {
			if (universe.IsRemote(id)) {
				return false;
			}
			bool replica = false;
			universe.Enter(id, [&replica](Store &store) { replica = store.AdoptOnly(); });
			return !replica;
		}

		bool AddManifestWorldFile(
			const std::filesystem::path &base,
			std::string_view text,
			std::unordered_set<std::string> &seen,
			std::vector<ManifestWorld> &worlds,
			std::string &error
		) {
			std::filesystem::path relative;
			std::filesystem::path absolute;
			if (!detail::ResolveWorldReference(base, text, relative, absolute, error)) {
				return false;
			}
			const std::string key = relative.generic_string();
			if (!seen.insert(key).second) {
				return true;
			}
			if (worlds.size() >= MAXIMUM_UNIVERSE_WORLDS) {
				error = "universe names too many worlds";
				return false;
			}
			worlds.push_back(
				ManifestWorld{
					.RelativePath = std::move(relative),
					.Settings = {},
					.Host = {},
					.Remote = false,
				}
			);
			return true;
		}

		bool DiscoverManifestWorlds(
			const std::filesystem::path &base,
			std::unordered_set<std::string> &seen,
			std::vector<ManifestWorld> &worlds,
			std::string &error
		) {
			std::vector<std::filesystem::path> discovered;
			if (!detail::DiscoverWorldReferences(base, discovered, error)) {
				return false;
			}
			for (const std::filesystem::path &relative : discovered) {
				if (seen.contains(relative.generic_string())) {
					continue;
				}
				if (!AddManifestWorldFile(base, relative.generic_string(), seen, worlds, error)) {
					return false;
				}
			}
			return true;
		}

		void EmptyUniverse(world::Universe &universe) {
			for (const world::WorldId existing : universe.Worlds()) {
				universe.Destroy(existing);
			}
		}

		core::Name ImportedWorldName(world::Universe &universe, core::Name authored) {
			if (!universe.Find(authored).IsValid()) {
				return authored;
			}
			const std::string base(authored.Text());
			for (int attempt = 2; attempt < 1000; attempt++) {
				const core::Name candidate(base + " " + std::to_string(attempt));
				if (!universe.Find(candidate).IsValid()) {
					return candidate;
				}
			}
			return {};
		}
	}

	bool SaveUniverse(
		world::Universe &universe,
		core::Name name,
		const graph::PipelineSet &renderingProfiles,
		const std::filesystem::path &path,
		const UniverseFileOptions &options,
		std::string &error
	) {
		error.clear();
		XmlWriter writer;
		writer.Open(UNIVERSE_ROOT);
		writer.Attribute("format", std::to_string(UNIVERSE_FORMAT_VERSION));
		writer.Attribute("name", name.IsValid() ? name.Text() : "Universe");
		writer.Attribute("recursive", options.RecursiveWorldDiscovery ? "true" : "false");
		WriteUniverseSettings(writer, universe.Settings());
		WriteRenderingProfiles(writer, renderingProfiles);
		writer.Open("Permissions");
		writer.Attribute("http", options.HttpEnabled ? "true" : "false");
		writer.Close();
		if (!options.PublisherKey.empty() || !options.Cdns.empty()) {
			writer.Open("Content");
			writer.Attribute("publisher", options.PublisherKey);
			for (const UniverseCdn &cdn : options.Cdns) {
				if (cdn.Name.empty() || cdn.Location.empty()) {
					continue;
				}
				writer.Open("Cdn");
				writer.Attribute("name", cdn.Name);
				writer.Attribute("location", cdn.Location);
				writer.Close();
			}
			writer.Close();
		}
		if (options.DataStore.Enabled) {
			std::filesystem::path relative;
			std::filesystem::path absolute;
			if (!detail::ResolveDirectoryReference(
					path.parent_path(), options.DataStore.Root.generic_string(), relative, absolute, error
				)) {
				return false;
			}
			writer.Open("DataStore");
			writer.Attribute("backend", options.DataStore.Backend);
			writer.Attribute("path", relative.generic_string());
			writer.Close();
		}
		const std::filesystem::path assetsDirectory = path.parent_path() / "assets";
		std::error_code assetsError;
		std::filesystem::create_directories(assetsDirectory, assetsError);
		if (assetsError) {
			error = "could not create the universe assets directory";
			return false;
		}
		writer.Open("Assets");
		writer.Attribute("path", "assets");
		writer.Close();

		const std::filesystem::path worldDirectory = path.parent_path() / (path.stem().string() + ".worlds");
		size_t localOrdinal = 0;
		for (const world::WorldId id : universe.Worlds()) {
			const world::WorldSettings settings = universe.SettingsOf(id);
			if (universe.IsRemote(id)) {
				writer.Open("RemoteWorld");
				WriteWorldAttributes(writer, settings);
				writer.Attribute("host", universe.HostOf(id).Text());
				WriteWorldProperties(writer, settings);
				writer.Close();
				continue;
			}
			if (!AuthoredLocalWorld(universe, id)) {
				continue;
			}

			localOrdinal++;
			const std::filesystem::path worldPath =
				worldDirectory / (std::to_string(localOrdinal) + "-" +
								  detail::SafeWorldFileStem(settings.Name) + std::string(WORLD_EXTENSION));
			const std::string document = WriteWorldDocument(universe, id, error);
			if (document.empty()) {
				return false;
			}
			if (!WriteFile(worldPath, document, error)) {
				return false;
			}

			writer.Open("WorldFile");
			writer.Attribute("path", worldPath.lexically_relative(path.parent_path()).generic_string());
			writer.Close();
		}

		writer.Close();
		return WriteFile(path, writer.Finish(), error);
	}

	bool SaveGame(
		world::Universe &universe, core::Name name, const std::filesystem::path &path, std::string &error
	) {
		return SaveGame(universe, name, graph::PipelineSet{}, path, error);
	}

	bool SaveGame(
		world::Universe &universe,
		core::Name name,
		const graph::PipelineSet &renderingProfiles,
		const std::filesystem::path &path,
		std::string &error
	) {
		if (path.extension() == UNIVERSE_EXTENSION) {
			return SaveUniverse(universe, name, renderingProfiles, path, UniverseFileOptions{}, error);
		}
		return WriteFile(path, WriteGame(universe, name, renderingProfiles), error);
	}

	bool LoadUniverse(
		world::Universe &universe, const std::filesystem::path &path, GameInfo &out, std::string &error
	) {
		out = GameInfo{};
		std::string text;
		if (!ReadFile(path, text, error)) {
			return false;
		}

		XmlDocument document;
		if (!Parse(text, UNIVERSE_ROOT, document, error, UNIVERSE_FORMAT_VERSION)) {
			return false;
		}

		const XmlElement &root = *document.Root();
		out.Name = core::Name(TextOf(root, "name", "Universe"));
		out.RecursiveWorldDiscovery = TextOf(root, "recursive", "false") == "true";
		out.Universe = universe.Settings();
		const std::filesystem::path base = path.parent_path();
		std::unordered_set<std::string> seen;
		std::vector<ManifestWorld> worlds;
		bool assetsDeclared = false;
		bool dataStoreDeclared = false;

		for (const uint32_t index : root.Children) {
			const XmlElement *child = document.At(index);
			if (child == nullptr) {
				continue;
			}
			if (child->Name == "Universe") {
				out.Universe = ReadUniverseSettings(*child, universe.Settings());
				continue;
			}
			if (child->Name == RENDERING_PROFILES) {
				ReadRenderingProfiles(*child, out.RenderingProfiles);
				continue;
			}
			if (child->Name == "Permissions") {
				out.HttpEnabled = TextOf(*child, "http", "false") == "true";
				continue;
			}
			if (child->Name == "Content") {
				out.PublisherKey = std::string(child->Attribute("publisher"));
				for (const uint32_t sourceIndex : child->Children) {
					const XmlElement *source = document.At(sourceIndex);
					if (source == nullptr || source->Name != "Cdn") {
						continue;
					}
					if (out.Cdns.size() >= MAXIMUM_UNIVERSE_CDNS) {
						error = "universe names too many content origins";
						EmptyUniverse(universe);
						return false;
					}
					UniverseCdn cdn{
						.Name = std::string(source->Attribute("name")),
						.Location = std::string(source->Attribute("location")),
					};
					if (cdn.Name.empty() || cdn.Location.empty()) {
						error = "universe content origin needs a name and location";
						EmptyUniverse(universe);
						return false;
					}
					out.Cdns.push_back(std::move(cdn));
				}
				continue;
			}
			if (child->Name == "DataStore") {
				if (dataStoreDeclared) {
					error = "universe declares more than one DataStore";
					EmptyUniverse(universe);
					return false;
				}
				std::filesystem::path relative;
				std::filesystem::path absolute;
				if (!detail::ResolveDirectoryReference(
						base, child->Attribute("path"), relative, absolute, error
					)) {
					EmptyUniverse(universe);
					return false;
				}
				out.DataStore.Enabled = true;
				out.DataStore.Backend = TextOf(*child, "backend", "binary");
				out.DataStore.Root = std::move(relative);
				dataStoreDeclared = true;
				continue;
			}
			if (child->Name == "Assets") {
				if (assetsDeclared) {
					error = "universe declares more than one assets directory";
					EmptyUniverse(universe);
					return false;
				}
				std::filesystem::path relative;
				if (!detail::ResolveDirectoryReference(
						base, child->Attribute("path"), relative, out.Assets, error
					)) {
					EmptyUniverse(universe);
					return false;
				}
				assetsDeclared = true;
				continue;
			}
			if (child->Name == "WorldFile") {
				if (!AddManifestWorldFile(base, child->Attribute("path"), seen, worlds, error)) {
					EmptyUniverse(universe);
					return false;
				}
				continue;
			}
			if (child->Name == "RemoteWorld") {
				if (worlds.size() >= MAXIMUM_UNIVERSE_WORLDS) {
					error = "universe names too many worlds";
					EmptyUniverse(universe);
					return false;
				}
				worlds.push_back(
					ManifestWorld{
						.RelativePath = {},
						.Settings = ReadWorldAttributes(document, *child),
						.Host = core::Name(child->Attribute("host")),
						.Remote = true,
					}
				);
			}
		}

		if (out.RecursiveWorldDiscovery && !DiscoverManifestWorlds(base, seen, worlds, error)) {
			EmptyUniverse(universe);
			return false;
		}

		EmptyUniverse(universe);
		ApplyUniverseSettings(universe, out.Universe);
		for (const ManifestWorld &entry : worlds) {
			if (entry.Remote) {
				world::WorldStatus status = world::WorldStatus::Ok;
				const world::WorldId created = universe.CreateRemote(entry.Settings, entry.Host, &status);
				if (!created.IsValid()) {
					error = "could not create remote world '" + std::string(entry.Settings.Name.Text()) + "'";
					EmptyUniverse(universe);
					return false;
				}
				out.Worlds.push_back(entry.Settings.Name);
				continue;
			}

			std::filesystem::path relative;
			std::filesystem::path absolute;
			if (!detail::ResolveWorldReference(
					base, entry.RelativePath.generic_string(), relative, absolute, error
				)) {
				EmptyUniverse(universe);
				return false;
			}
			std::string worldError;
			const world::WorldId created = ImportWorld(universe, absolute, core::Name{}, worldError);
			if (!created.IsValid()) {
				error = "world '" + relative.generic_string() + "': " + worldError;
				EmptyUniverse(universe);
				return false;
			}
			out.Worlds.push_back(universe.NameOf(created));
		}
		return true;
	}

	size_t ImportUniverse(
		world::Universe &universe, const std::filesystem::path &path, GameInfo &out, std::string &error
	) {
		if (path.extension() == UNIVERSE_EXTENSION) {
			world::Universe staged;
			GameInfo stagedInfo;
			if (!LoadUniverse(staged, path, stagedInfo, error)) {
				out = std::move(stagedInfo);
				return 0;
			}

			out = std::move(stagedInfo);
			out.Worlds.clear();
			size_t imported = 0;
			for (const world::WorldId source : staged.Worlds()) {
				const world::WorldSettings sourceSettings = staged.SettingsOf(source);
				const core::Name destinationName = ImportedWorldName(universe, sourceSettings.Name);
				if (!destinationName.IsValid()) {
					error = "could not find a free name to import '" +
							std::string(sourceSettings.Name.Text()) + "' under";
					return imported;
				}

				world::WorldId created;
				if (staged.IsRemote(source)) {
					world::WorldSettings renamed = sourceSettings;
					renamed.Name = destinationName;
					created = universe.CreateRemote(renamed, staged.HostOf(source));
				} else {
					std::string documentError;
					const std::string worldDocument = WriteWorldDocument(staged, source, documentError);
					if (worldDocument.empty()) {
						error = documentError;
						return imported;
					}
					created = ReadWorldDocument(universe, worldDocument, destinationName, documentError);
					if (!created.IsValid()) {
						error = documentError;
						return imported;
					}
				}

				if (!created.IsValid()) {
					error = "could not create world '" + std::string(destinationName.Text()) + "'";
					return imported;
				}
				out.Worlds.push_back(destinationName);
				imported++;
			}
			return imported;
		}

		out = GameInfo{};

		std::string text;
		if (!ReadFile(path, text, error)) {
			return 0;
		}

		XmlDocument document;
		if (!Parse(text, GAME_ROOT, document, error)) {
			return 0;
		}

		const XmlElement &root = *document.Root();
		out.Name = core::Name(TextOf(root, "name", "Game"));

		// **Nothing is destroyed, which is the whole difference from
		// `LoadGame`.** That one empties the universe first because a universe
		// half of one game and half of another is `ecs::Store::Load`'s hazard
		// one layer up. This is the other operation: an author bringing a
		// colleague's scenes into the game they already have open, which is the
		// same thing `ImportWorld` does for one world.
		//
		// **The universe's own settings are read and not applied**, for the
		// reason `LoadGame` applies only the mode: they belong to the universe
		// being imported *into*, and a file arriving with a different bus
		// budget must not retune somebody else's game.
		size_t imported = 0;

		for (const uint32_t index : root.Children) {
			const XmlElement *child = document.At(index);
			if (child == nullptr) {
				continue;
			}

			if (child->Name == RENDERING_PROFILES) {
				ReadRenderingProfiles(*child, out.RenderingProfiles);
				continue;
			}

			if (child->Name != WORLD_ROOT) {
				continue;
			}

			world::WorldSettings settings = ReadWorldAttributes(document, *child);

			// **A clash gets a suffix rather than a refusal.** Two worlds
			// cannot share a name, and importing a game that shares one scene
			// name with the open game is the ordinary case rather than the
			// exception - being told "Lobby is taken" and made to guess a free
			// name is a worse answer than being given one. `ImportWorld` and
			// `DuplicateWorld` both already do this.
			if (universe.Find(settings.Name).IsValid()) {
				const std::string base(settings.Name.Text());
				core::Name chosen;
				for (int attempt = 2; attempt < 1000; attempt++) {
					const core::Name candidate(base + " " + std::to_string(attempt));
					if (!universe.Find(candidate).IsValid()) {
						chosen = candidate;
						break;
					}
				}

				if (!chosen.IsValid()) {
					error = "could not find a free name to import '" + base + "' under";
					return imported;
				}
				settings.Name = chosen;
			}

			world::WorldStatus status = world::WorldStatus::Ok;
			const world::WorldId id = universe.Create(settings, &status);
			if (!id.IsValid()) {
				error = "could not create world '" + std::string(settings.Name.Text()) + "'";
				return imported;
			}

			std::string worldError;
			core::Name migratedProfile;
			universe.Enter(id, [&](Store &store) {
				ReadWorldBody(document, *child, store, worldError);
				if (worldError.empty()) {
					migratedProfile = MigrateWorldPipelines(store, settings.Name, out.RenderingProfiles);
				}
			});

			if (!worldError.empty()) {
				// **Only this world is undone.** The ones already imported are
				// good, and throwing them away because the fourth scene names a
				// class this build lacks would lose work that read perfectly.
				error = worldError;
				universe.Destroy(id);
				return imported;
			}
			if (migratedProfile.IsValid()) {
				(void)universe.SetRenderingProfile(id, migratedProfile);
			}

			out.Worlds.push_back(settings.Name);
			imported++;
		}

		return imported;
	}

	bool LoadGame(
		world::Universe &universe, const std::filesystem::path &path, GameInfo &out, std::string &error
	) {
		if (path.extension() == UNIVERSE_EXTENSION) {
			return LoadUniverse(universe, path, out, error);
		}
		out = GameInfo{};

		std::string text;
		if (!ReadFile(path, text, error)) {
			return false;
		}

		XmlDocument document;
		if (!Parse(text, GAME_ROOT, document, error)) {
			return false;
		}

		const XmlElement &root = *document.Root();
		out.Name = core::Name(TextOf(root, "name", "Game"));

		// Everything first, then nothing left over on a failure. The universe
		// is emptied up front so that a load which fails halfway leaves an
		// empty universe rather than a hybrid - `ecs::Store::Load`'s rule, one
		// layer up.
		for (const world::WorldId existing : universe.Worlds()) {
			universe.Destroy(existing);
		}

		for (const uint32_t index : root.Children) {
			const XmlElement *child = document.At(index);
			if (child == nullptr) {
				continue;
			}

			if (child->Name == "Universe") {
				const world::UniverseSettings settings = ReadUniverseSettings(*child, universe.Settings());

				// These are authored tuning, so opening a game applies the same
				// values its file reports. Federation alone remains
				// construction-time host policy and is not in this file.
				ApplyUniverseSettings(universe, settings);
				out.Universe = settings;
				continue;
			}

			if (child->Name == RENDERING_PROFILES) {
				ReadRenderingProfiles(*child, out.RenderingProfiles);
				continue;
			}

			if (child->Name != WORLD_ROOT) {
				continue;
			}

			const world::WorldSettings settings = ReadWorldAttributes(document, *child);

			world::WorldStatus status = world::WorldStatus::Ok;
			const world::WorldId id = universe.Create(settings, &status);
			if (!id.IsValid()) {
				error = "could not create world '" + std::string(settings.Name.Text()) + "'";
				for (const world::WorldId created : universe.Worlds()) {
					universe.Destroy(created);
				}
				return false;
			}

			std::string worldError;
			core::Name migratedProfile;
			universe.Enter(id, [&](Store &store) {
				if (!ReadWorldBody(document, *child, store, worldError)) {
					return;
				}
				migratedProfile = MigrateWorldPipelines(store, settings.Name, out.RenderingProfiles);
			});

			if (!worldError.empty()) {
				error = "world '" + std::string(settings.Name.Text()) + "': " + worldError;
				for (const world::WorldId created : universe.Worlds()) {
					universe.Destroy(created);
				}
				return false;
			}
			if (migratedProfile.IsValid()) {
				(void)universe.SetRenderingProfile(id, migratedProfile);
			}

			out.Worlds.push_back(settings.Name);
		}

		return true;
	}

	std::string WriteInstanceDocument(Store &store, Entity instance) {
		RegisterGameClasses();

		if (!store.Alive(instance) || !store.ClassOf(instance).IsValid()) {
			return {};
		}

		XmlWriter writer;
		writer.Open(INSTANCE_ROOT);
		writer.Attribute("format", std::to_string(FORMAT_VERSION));

		// **Only the scripts this subtree actually names.** `WriteWorldBody`
		// writes the whole `SourceCache` because it is writing the whole world;
		// doing that here would carry every program in the source world into
		// the destination on a move that took one part with it.
		std::vector<core::Name> paths;
		CollectSourcePaths(store, instance, paths);
		WriteSourcesFor(writer, store, paths);

		Numbering numbering;
		numbering.Walk(store, instance);

		Defaults defaults;
		WriteInstance(writer, store, instance, numbering, defaults);

		writer.Close();
		return writer.Finish();
	}

	Entity ReadInstanceDocument(Store &store, std::string_view document, Entity parent, std::string &error) {
		RegisterGameClasses();

		XmlDocument parsed;
		if (!Parse(std::string(document), INSTANCE_ROOT, parsed, error)) {
			return NULL_ENTITY;
		}

		const XmlElement &root = *parsed.Root();

		std::unordered_map<uint32_t, Entity> byId;
		std::vector<PendingReference> pending;
		uint32_t rootId = 0;

		for (const uint32_t index : root.Children) {
			const XmlElement *child = parsed.At(index);
			if (child == nullptr) {
				continue;
			}

			if (child->Name == "Sources") {
				// **Merged rather than set.** `ReadSources` replaces the
				// resource outright, which is right when it is filling an empty
				// world and catastrophic here - pasting one part into a world
				// would delete every script that world already had.
				MergeSources(parsed, *child, store);
				continue;
			}

			if (child->Name != "Item") {
				continue;
			}

			if (!ReadInstance(parsed, *child, store, parent, byId, pending, error)) {
				return NULL_ENTITY;
			}

			rootId = CountOf(*child, "id", 0);
		}

		for (const PendingReference &reference : pending) {
			const auto found = byId.find(reference.Target);
			if (found == byId.end()) {
				// **A reference out of the subtree, and it dangles.** Moving a
				// part that something outside it pointed at cannot carry that
				// pointer across - the target is a handle in the world being
				// left. Left at its default and said out loud, which is the
				// same answer `ReadWorldBody` gives a dangling id.
				ENGINE_WARN(
					"instance document: reference '{}' names instance {} which is not in this subtree",
					reference.Property.Text(),
					reference.Target
				);
				continue;
			}

			const Entity target = found->second;
			store.SetProperty(reference.Instance, reference.Property, &target, sizeof(Entity));
		}

		const auto found = byId.find(rootId);
		return found == byId.end() ? NULL_ENTITY : found->second;
	}

	std::string WriteWorldDocument(world::Universe &universe, world::WorldId world, std::string &error) {
		error.clear();

		if (universe.IsRemote(world)) {
			error = "that world is held by another host, so its content is not this process's to write";
			return {};
		}

		const core::Name name = universe.NameOf(world);
		if (!name.IsValid()) {
			error = "no such world";
			return {};
		}

		XmlWriter writer;
		writer.Open(WORLD_ROOT);
		writer.Attribute("format", std::to_string(FORMAT_VERSION));

		// The world's own settings, for `WriteGame`'s reason: an exported world
		// that came back at a tick rate it was never authored at is a scene
		// that behaves differently on the round trip.
		const world::WorldSettings settings = universe.SettingsOf(world);
		WriteWorldAttributes(writer, settings);
		WriteWorldProperties(writer, settings);

		const world::WorldStatus status =
			universe.Enter(world, [&](Store &store) { WriteWorldBody(writer, store); });
		if (status != world::WorldStatus::Ok) {
			error = "no such world";
			return {};
		}

		writer.Close();
		return writer.Finish();
	}

	world::WorldId ReadWorldDocument(
		world::Universe &universe, std::string_view document, core::Name rename, std::string &error
	) {
		// A standalone world can hold hundreds of thousands of imported instances.
		// Keep the parser bounded, but do not let the writer produce a normal world
		// that its paired reader refuses at the generic 64 MiB document limit.
		XmlDocument parsed;
		if (!Parse(std::string(document), WORLD_ROOT, parsed, error, FORMAT_VERSION, MAXIMUM_WORLD_BYTES)) {
			return world::WorldId{};
		}

		const XmlElement &root = *parsed.Root();
		world::WorldSettings settings = ReadWorldAttributes(parsed, root);
		if (rename.IsValid()) {
			settings.Name = rename;
		}

		if (universe.Find(settings.Name).IsValid()) {
			error = "a world called '" + std::string(settings.Name.Text()) + "' is already in this universe";
			return world::WorldId{};
		}

		const world::WorldId id = universe.Create(settings);
		if (!id.IsValid()) {
			error = "could not create world '" + std::string(settings.Name.Text()) + "'";
			return world::WorldId{};
		}

		std::string worldError;
		universe.Enter(id, [&](Store &store) { ReadWorldBody(parsed, root, store, worldError); });

		if (!worldError.empty()) {
			error = worldError;
			universe.Destroy(id);
			return world::WorldId{};
		}

		return id;
	}

	bool ExportWorld(
		world::Universe &universe, world::WorldId world, const std::filesystem::path &path, std::string &error
	) {
		const std::string document = WriteWorldDocument(universe, world, error);
		if (document.empty()) {
			return false;
		}
		return WriteFile(path, document, error);
	}

	world::WorldId ImportWorld(
		world::Universe &universe, const std::filesystem::path &path, core::Name rename, std::string &error
	) {
		std::string text;
		if (!ReadFile(path, text, error)) {
			return world::WorldId{};
		}
		return ReadWorldDocument(universe, text, rename, error);
	}

	bool PrepareWorldImport(
		const std::filesystem::path &path,
		PreparedWorldImport &out,
		std::string &error,
		const WorldImportProgress &progress
	) {
		out = {};
		error.clear();
		RegisterGameClasses();

		std::error_code sizeError;
		const uintmax_t size = std::filesystem::file_size(path, sizeError);
		if (sizeError) {
			error = "could not inspect " + path.string();
			return false;
		}
		if (size > MAXIMUM_WORLD_BYTES) {
			error = "world document exceeds the 256 MiB limit";
			return false;
		}

		std::ifstream file(path, std::ios::binary);
		if (!file) {
			error = "could not open " + path.string();
			return false;
		}
		std::string text;
		text.resize(static_cast<size_t>(size));
		constexpr size_t READ_BLOCK = 1024u * 1024u;
		size_t offset = 0;
		while (offset < text.size()) {
			const size_t bytes = std::min(READ_BLOCK, text.size() - offset);
			file.read(text.data() + offset, static_cast<std::streamsize>(bytes));
			if (!file) {
				error = "could not finish reading " + path.string();
				return false;
			}
			offset += bytes;
			if (progress) {
				progress(
					WorldImportPhase::Read, text.empty() ? 1.0f : static_cast<float>(offset) / text.size()
				);
			}
		}

		if (progress) {
			progress(WorldImportPhase::Decode, 0.0f);
		}
		XmlDocument parsed;
		if (!Parse(text, WORLD_ROOT, parsed, error, FORMAT_VERSION, MAXIMUM_WORLD_BYTES)) {
			return false;
		}
		const XmlElement &root = *parsed.Root();
		out.Settings = ReadWorldAttributes(parsed, root);

		if (progress) {
			progress(WorldImportPhase::Build, 0.0f);
		}
		ecs::Store staged("game.world-import");
		if (!ReadWorldBody(parsed, root, staged, error)) {
			out = {};
			return false;
		}

		if (progress) {
			progress(WorldImportPhase::Encode, 0.0f);
		}
		core::ByteWriter writer;
		if (!staged.Save(writer)) {
			error = "world contains data that cannot cross the import boundary";
			out = {};
			return false;
		}
		out.Snapshot.assign(writer.Bytes().begin(), writer.Bytes().end());
		if (progress) {
			progress(WorldImportPhase::Encode, 1.0f);
		}
		return true;
	}

	world::WorldId CommitWorldImport(
		world::Universe &universe, const PreparedWorldImport &prepared, core::Name rename, std::string &error
	) {
		error.clear();
		world::WorldSettings settings = prepared.Settings;
		if (rename.IsValid()) {
			settings.Name = rename;
		}
		if (!settings.Name.IsValid() || prepared.Snapshot.empty()) {
			error = "prepared world is empty";
			return {};
		}
		if (universe.Find(settings.Name).IsValid()) {
			error = "a world called '" + std::string(settings.Name.Text()) + "' is already in this universe";
			return {};
		}

		const world::WorldId id = universe.Create(settings);
		if (!id.IsValid()) {
			error = "could not create world '" + std::string(settings.Name.Text()) + "'";
			return {};
		}
		bool restored = false;
		universe.Enter(id, [&](Store &store) {
			core::ByteReader reader(prepared.Snapshot);
			restored = store.LoadContents(reader) && reader.AtEnd();
		});
		if (!restored) {
			universe.Destroy(id);
			error = "could not restore the prepared world";
			return {};
		}
		return id;
	}

}
