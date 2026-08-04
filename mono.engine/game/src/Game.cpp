#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>

#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
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
		constexpr std::string_view WORLD_ROOT = "World";

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
		// and skipped when it matches — which costs one throwaway instance per
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
		// forward — a spotlight naming the part it follows, declared later in
		// the tree — and a writer that assigned ids as it went could not
		// resolve one.
		struct Numbering {
			std::unordered_map<uint64_t, uint32_t> Ids;
			uint32_t Next = 1;

			void Walk(const Store &store, Entity instance) {
				Ids.emplace(Key(instance), Next++);
				store.EachChild(instance, [&](Entity child) { Walk(store, child); });
			}

			uint32_t Of(Entity instance) const {
				const auto found = Ids.find(Key(instance));
				return found == Ids.end() ? 0 : found->second;
			}

			static uint64_t Key(Entity instance) {
				// The whole id, which already carries the generation in its
				// high bits — so a recycled slot is not mistaken for the
				// instance that used to live in it. Reading the two halves
				// apart would be this file depending on a layout `Entity`
				// deliberately does not expose.
				return instance.Id;
			}
		};

		void WriteInstance(
			XmlWriter &writer, Store &store, Entity instance, const Numbering &numbering, Defaults &defaults
		) {
			const ClassId id = store.ClassOf(instance);
			if (!id.IsValid()) {
				// Not every entity is an instance — a resource-carrying row, a
				// predicted entity — and a document holds instances. Skipped
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
					// error: `Anchored` is an archetype, so a part that has been
					// anchored has no `RigidBody` for a dynamic-only property to
					// read from.
					continue;
				}

				if (descriptor.Type == PropertyType::Reference) {
					// **Only a reference inside this document is written.** One
					// pointing at an instance in another world is meaningless in
					// a file — `Entity` is a handle within one world and says so
					// — and writing a dangling id would resolve to whatever
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
					if (TypeFromTag(child->Attribute("type"), declared) && declared != descriptor->Type) {
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
						// yet — a reference pointing forward in the tree is
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
			if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
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

		void WriteWorldAttributes(XmlWriter &writer, const world::WorldSettings &settings) {
			writer.Attribute("name", settings.Name.IsValid() ? settings.Name.Text() : "");
			writer.Attribute("tickRate", FormatNumber(settings.TickRate));
			writer.Attribute("idleTickRate", FormatNumber(settings.IdleTickRate));
			writer.Attribute("faultLimit", std::to_string(settings.FaultLimit));
		}

		world::WorldSettings ReadWorldAttributes(const XmlElement &element) {
			world::WorldSettings settings;
			settings.Name = core::Name(TextOf(element, "name", "World"));
			settings.TickRate = NumberOf(element, "tickRate", 60.0);
			settings.IdleTickRate = NumberOf(element, "idleTickRate", 2.0);
			settings.FaultLimit = CountOf(element, "faultLimit", 3);
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
			const std::string &text, std::string_view expectedRoot, XmlDocument &document, std::string &error
		) {
			uint32_t line = 0;
			const XmlStatus status = ParseXml(text, document, {}, &line);
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
			if (format == 0 || format > FORMAT_VERSION) {
				error = "this is a format " + std::to_string(format) + " file and this build reads " +
						std::to_string(FORMAT_VERSION);
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
		script::ScriptClass();
	}

	void WriteWorldBody(XmlWriter &writer, Store &store) {
		RegisterGameClasses();

		WriteSources(writer, store);

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
				// by hand or exported from a partial selection — and leaving
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
		Store &store, ecs::Scheduler &scheduler, const script::RuntimeLimits &limits, std::string &error
	) {
		error.clear();

		std::shared_ptr<script::Runtime> runtime = script::MakeRuntime(store, script::Language::Luau, limits);

		const size_t ran = runtime->RunWorldScripts();
		if (!runtime->LastError().empty()) {
			error = runtime->LastError();
		}

		// **The fixed tick delta, never wall time.** A script integrating
		// against a real clock puts the scene in a different place on a busy
		// machine, and the recording stops replaying — the desync rule 5 names,
		// arriving through the call a script uses most.
		scheduler.Add("script-heartbeat", ecs::Phase::Simulation, [runtime](Store &world) {
			if (!runtime->Heartbeat(world.Time().Delta)) {
				// Logged per tick rather than swallowed. A world that silently
				// stopped animating is a bug report with nothing in it.
				ENGINE_ERROR("heartbeat: {}", runtime->LastError());
			}
		});

		if (ran > 0) {
			ENGINE_INFO("world '{}': {} script(s) started", store.Name(), ran);
		}

		return runtime;
	}

	std::string WriteGame(world::Universe &universe, core::Name name) {
		XmlWriter writer;

		writer.Open(GAME_ROOT);
		writer.Attribute("format", std::to_string(FORMAT_VERSION));
		writer.Attribute("name", name.IsValid() ? name.Text() : "Game");

		const world::UniverseSettings &settings = universe.Settings();
		writer.Open("Universe");
		writer.Attribute(
			"mode", settings.Mode == world::ExecutionMode::WorldSerial ? "WorldSerial" : "WorldParallel"
		);
		writer.Attribute("catchUp", std::to_string(settings.MaximumCatchUpTicks));
		writer.Attribute("busBudget", std::to_string(settings.BusBudgetPerTick));
		writer.Close();

		for (const world::WorldId id : universe.Worlds()) {
			world::WorldSettings worldSettings;
			worldSettings.Name = universe.NameOf(id);

			writer.Open(WORLD_ROOT);
			WriteWorldAttributes(writer, worldSettings);

			if (universe.IsRemote(id)) {
				// **A name and a host, not content.** A world held by a
				// supervised host has no store here, so its instances are not
				// this process's to write — and an empty `<World>` would be a
				// save file that quietly deleted somebody's scene.
				writer.Attribute("host", universe.HostOf(id).Text());
				writer.Close();
				continue;
			}

			universe.Enter(id, [&](Store &store) { WriteWorldBody(writer, store); });
			writer.Close();
		}

		writer.Close();
		return writer.Finish();
	}

	bool SaveGame(
		world::Universe &universe, core::Name name, const std::filesystem::path &path, std::string &error
	) {
		return WriteFile(path, WriteGame(universe, name), error);
	}

	bool LoadGame(
		world::Universe &universe, const std::filesystem::path &path, GameInfo &out, std::string &error
	) {
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
		// empty universe rather than a hybrid — `ecs::Store::Load`'s rule, one
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
				world::UniverseSettings settings = universe.Settings();
				settings.MaximumCatchUpTicks = static_cast<int>(
					CountOf(*child, "catchUp", static_cast<uint32_t>(settings.MaximumCatchUpTicks))
				);
				settings.BusBudgetPerTick = CountOf(*child, "busBudget", settings.BusBudgetPerTick);

				// **Mode is applied and the rest is not.** `SetMode` is the one
				// setting `Universe` lets a caller change after construction,
				// because it is a tuning knob that changes no result. The
				// others are read into `out` so a caller that is *building* a
				// universe can honour them, and are not forced onto one that
				// already exists.
				universe.SetMode(
					TextOf(*child, "mode", "WorldParallel") == "WorldSerial"
						? world::ExecutionMode::WorldSerial
						: world::ExecutionMode::WorldParallel
				);
				out.Universe = settings;
				continue;
			}

			if (child->Name != WORLD_ROOT) {
				continue;
			}

			const world::WorldSettings settings = ReadWorldAttributes(*child);

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
			universe.Enter(id, [&](Store &store) {
				if (!ReadWorldBody(document, *child, store, worldError)) {
					return;
				}
			});

			if (!worldError.empty()) {
				error = "world '" + std::string(settings.Name.Text()) + "': " + worldError;
				for (const world::WorldId created : universe.Worlds()) {
					universe.Destroy(created);
				}
				return false;
			}

			out.Worlds.push_back(settings.Name);
		}

		return true;
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

		world::WorldSettings settings;
		settings.Name = name;
		WriteWorldAttributes(writer, settings);

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
		XmlDocument parsed;
		if (!Parse(std::string(document), WORLD_ROOT, parsed, error)) {
			return world::WorldId{};
		}

		const XmlElement &root = *parsed.Root();
		world::WorldSettings settings = ReadWorldAttributes(root);
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

}
