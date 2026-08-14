#include <engine/bake/RobloxModel.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/game/Values.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <studio/RojoSync.hpp>
#include <toml++/toml.hpp>

namespace studio {

	namespace {
		using engine::core::Name;
		using engine::ecs::Classes;
		using engine::ecs::ClassId;
		using engine::ecs::Entity;
		using engine::ecs::NULL_ENTITY;
		using engine::ecs::PropertyType;
		using engine::ecs::Store;
		using nlohmann::json;

		// Whether a name ends with a suffix.
		bool EndsWith(std::string_view text, std::string_view suffix) {
			return text.size() >= suffix.size() &&
				   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		// What a file becomes, if anything.
		//
		// **`.server` and `.client` decide the class, not a guess from the
		// folder.** Rojo's convention puts the intent in the file name, and a
		// rule based on which directory it happened to sit in would disagree
		// with the same file moved.
		struct ScriptFile {
			bool IsScript = false;
			bool Local = false;

			// **A plain `.luau` is a module, which is Rojo's own rule.** Only the
			// suffixed files are programs the host runs; everything else in a
			// project is something a program requires. Mapping them all to
			// `Script` — which this did before `ModuleScript` existed — meant a
			// synced project executed every library it contained.
			bool Module = false;
			std::string Name;
		};

		ScriptFile ClassifyFile(const std::filesystem::path &file) {
			ScriptFile found;

			const std::string leaf = file.filename().string();
			if (!EndsWith(leaf, ".luau") && !EndsWith(leaf, ".lua")) {
				return found;
			}

			std::string stem = file.stem().string();
			found.IsScript = true;

			if (EndsWith(stem, ".server")) {
				stem.resize(stem.size() - 7);
			} else if (EndsWith(stem, ".client")) {
				stem.resize(stem.size() - 7);
				found.Local = true;
			} else {
				found.Module = true;
			}

			found.Name = stem;
			return found;
		}

		// What Rojo says a file becomes, for the one this engine cannot build.
		//
		// **Named by what it *would* be rather than skipped as unrecognised**,
		// and the difference matters to whoever reads the log: "not a script" is
		// what you say about a stray `.DS_Store`, and it is the wrong thing to
		// say about an `.rbxmx`, which Rojo maps and this engine has no reader
		// for. One is noise in the project and the other is a gap here.
		//
		// The list is `rojo.space/docs/v7/sync-details`. `D00104` carries what
		// closing it would take.
		const char *UnbuiltKind(const std::filesystem::path &file) {
			const std::string leaf = file.filename().string();

			// **One left, and it is a vendor decision before it is a feature.**
			// Everything else in Rojo's table is built — `BuildMapped` is the
			// dispatcher — so anything reaching here is a gap with a named cause
			// rather than an unrecognised file.
			//
			// `.toml` closed at v0.13 and `.rbxm` at v0.15, and the two went
			// differently on purpose: TOML's cost was a submodule because the
			// mapping was already the `*.json` one, and `.rbxm`'s was a binary
			// reader, which is why it lives in `bake` beside the other model
			// decoders rather than in this file.
			if (EndsWith(leaf, ".rbxmx")) {
				return "an XML Roblox model, and nothing here parses XML";
			}
			return nullptr;
		}

		// The `init` file a directory carries, which makes the directory itself
		// the script rather than a folder holding one.
		//
		// **Rojo's rule is that only one may be present**, and this engine has
		// to do something when an author breaks it. Refusing the whole sync
		// would be one mistake costing every other folder; picking silently
		// would be a directory whose class depends on which name happened to
		// sort first. So the order is fixed and written down — module, then
		// server, then client — and the extras are reported.
		struct InitFile {
			std::filesystem::path File;
			bool Local = false;
			bool Module = false;

			bool Present() const {
				return !File.empty();
			}
		};

		InitFile FindInit(const std::filesystem::path &directory, RojoSyncReport &report) {
			static const struct {
				const char *Leaf;
				bool Local;
				bool Module;
			} CANDIDATES[] = {
				{"init.luau", false, true},
				{"init.lua", false, true},
				{"init.server.luau", false, false},
				{"init.server.lua", false, false},
				{"init.client.luau", true, false},
				{"init.client.lua", true, false},
			};

			InitFile chosen;
			for (const auto &candidate : CANDIDATES) {
				const std::filesystem::path file = directory / candidate.Leaf;

				std::error_code kind;
				if (!std::filesystem::is_regular_file(file, kind)) {
					continue;
				}

				if (chosen.Present()) {
					report.Notes.push_back(
						directory.filename().string() + " has more than one init file — used " +
						chosen.File.filename().string() + " and ignored " + candidate.Leaf
					);
					continue;
				}

				chosen.File = file;
				chosen.Local = candidate.Local;
				chosen.Module = candidate.Module;
			}
			return chosen;
		}

		// Whether a file is *an* init file, whichever one the directory chose.
		//
		// The loop over a directory has to skip every one of them: the chosen
		// one was consumed by the directory itself, and an ignored one must not
		// come back as a child called `init`. That second case is the bug this
		// replaced — only `init.luau` was skipped, so `init.server.luau` became
		// a folder plus a stray `Script` named `init`.
		bool IsInitFile(const std::filesystem::path &file) {
			const std::string leaf = file.filename().string();
			return leaf == "init.luau" || leaf == "init.lua" || leaf == "init.server.luau" ||
				   leaf == "init.server.lua" || leaf == "init.client.luau" || leaf == "init.client.lua";
		}

		// --- the rest of Rojo's table ----------------------------------------
		//
		// `rojo.space/docs/v7/sync-details` maps nine more things than the
		// scripts above. Eight of them are built — the last of those, `.rbxm`,
		// through `bake` — and the one that is not needs an XML parser this
		// repository does not vendor. `D00104` carries what that would take.

		// One JSON value, read as a property of a declared type.
		//
		// **Accepts both shapes Rojo has used**, because a project in the wild
		// has either: the bare value — `"Size": [4, 1, 2]` — and the named-part
		// object — `"Size": {"X": 4, "Y": 1, "Z": 2}`. Neither is more correct
		// and refusing one would refuse half the projects.
		//
		// **Keyed on the property's declared type and never on the JSON's
		// shape.** An array of three numbers is a `Vector3` for a `Vector3`
		// property and nothing at all for a `bool` one — guessing from the value
		// would make `"Anchored": 1` mean something.
		bool ReadPropertyJson(
			const engine::ecs::PropertyDescriptor &property,
			const json &value,
			engine::game::PropertyValue &out
		) {
			out = engine::game::PropertyValue{};
			out.Type = property.Type;

			// A number out of either shape: `at` names the object member and
			// `index` the array slot, so one call covers both spellings.
			const auto number = [&value](const char *at, size_t index) -> float {
				if (value.is_object()) {
					const auto found = value.find(at);
					return found != value.end() && found->is_number() ? found->get<float>() : 0.0f;
				}
				if (value.is_array() && index < value.size() && value[index].is_number()) {
					return value[index].get<float>();
				}
				return 0.0f;
			};

			switch (property.Type) {
			case PropertyType::Bool:
				if (!value.is_boolean()) {
					return false;
				}
				out.Bool = value.get<bool>();
				return true;

			case PropertyType::Int32:
			case PropertyType::Int64:
			case PropertyType::Float:
			case PropertyType::Double:
				if (!value.is_number()) {
					return false;
				}
				out.Int32 = value.get<int32_t>();
				out.Int64 = value.get<int64_t>();
				out.Float = value.get<float>();
				out.Double = value.get<double>();
				return true;

			case PropertyType::String:
				if (!value.is_string()) {
					return false;
				}
				out.String = value.get<std::string>();
				return true;

			case PropertyType::Name:
			case PropertyType::Enum:
				// **An enum member is refused here rather than at the store.**
				// `ecs::EnumTable` is what decides membership, and letting a
				// typo through would land a name in a component that renders as
				// the default for reasons nobody can see — which is the whole
				// argument `PropertyType::Enum` was added on.
				if (!value.is_string()) {
					return false;
				}
				out.Name = Name(value.get<std::string>());
				if (property.Type == PropertyType::Enum &&
					!engine::ecs::EnumTable::Has(property.EnumName, out.Name)) {
					return false;
				}
				return true;

			case PropertyType::Vector3:
				out.Vector3 = engine::core::Vector3{number("X", 0), number("Y", 1), number("Z", 2)};
				return true;

			case PropertyType::Vector2:
				out.Vector2 = engine::core::Vector2{number("X", 0), number("Y", 1)};
				return true;

			case PropertyType::Color3:
				out.Color3 = engine::core::Color3{number("R", 0), number("G", 1), number("B", 2)};
				return true;

			case PropertyType::CFrame: {
				// Position only. A `.model.json` writes a `CFrame` as twelve
				// numbers — three of position and a nine-element rotation
				// matrix — and this engine's `CFrame` is a quaternion, so the
				// conversion is real work for a case no project in the seed
				// content uses. Reported by the caller rather than done wrong.
				const json position = value.is_object() ? value.value("Position", value) : value;
				const auto axis = [&position](const char *at, size_t index) -> float {
					if (position.is_object()) {
						const auto found = position.find(at);
						return found != position.end() && found->is_number() ? found->get<float>() : 0.0f;
					}
					return position.is_array() && index < position.size() && position[index].is_number()
							   ? position[index].get<float>()
							   : 0.0f;
				};
				out.CFrame =
					engine::core::CFrame(engine::core::Vector3{axis("X", 0), axis("Y", 1), axis("Z", 2)});
				return true;
			}

			case PropertyType::UDim:
				out.UDim = engine::core::UDim{number("Scale", 0), number("Offset", 1)};
				return true;

			case PropertyType::NumberRange:
				out.NumberRange = engine::core::NumberRange{number("Min", 0), number("Max", 1)};
				return true;

			default:
				// `UDim2`, `Rect`, the two sequences and a `Reference`. A
				// reference is the one that cannot be done at all from a file —
				// it names an instance, and a `.meta.json` has no way to say
				// which. The rest are shapes nothing has asked for.
				return false;
			}
		}

		// The descriptor an instance's class declares under that spelling.
		//
		// **One lookup, because two file formats ask the same question.** A
		// `.meta.json` patch and an `.rbxm` property are both a name off a file
		// matched against what the class actually has, and a second copy of this
		// loop is a second place for the two to start disagreeing about what
		// counts as a match.
		const engine::ecs::PropertyDescriptor *
		PropertyNamed(const Store &store, Entity instance, std::string_view spelling) {
			for (const engine::ecs::PropertyDescriptor &property : store.PropertiesOf(instance)) {
				if (property.Spelling == spelling) {
					return &property;
				}
			}
			return nullptr;
		}

		// Applies a `{"Name": ..., "Properties": {...}}` patch to one instance.
		//
		// **Every key is looked up in the class's own property list**, so a
		// patch naming something the class does not have is reported rather than
		// silently dropped — which is the difference between "this engine has no
		// `Reflectance`" and "your file has a typo", and an author needs to be
		// told which.
		void ApplyMetadata(Store &store, Entity instance, const json &document, RojoSyncReport &report) {
			if (!document.is_object()) {
				return;
			}

			if (const auto name = document.find("name"); name != document.end() && name->is_string()) {
				store.SetInstanceName(instance, name->get<std::string>());
			}

			const auto properties = document.find("properties");
			if (properties == document.end() || !properties->is_object()) {
				return;
			}

			for (const auto &entry : properties->items()) {
				const engine::ecs::PropertyDescriptor *found = PropertyNamed(store, instance, entry.key());
				if (found == nullptr) {
					report.Notes.push_back(entry.key() + " is not a property here — skipped");
					continue;
				}

				engine::game::PropertyValue value;
				if (!ReadPropertyJson(*found, entry.value(), value)) {
					report.Notes.push_back(
						entry.key() + " could not be read as a " + engine::ecs::Describe(found->Type) +
						" — skipped"
					);
					continue;
				}

				if (!engine::game::WriteProperty(store, instance, *found, value)) {
					report.Notes.push_back(entry.key() + " was refused by the world — skipped");
				}
			}
		}

		// Reads a JSON document off disk, or reports why not.
		bool ReadJsonFile(const std::filesystem::path &path, json &out, RojoSyncReport &report) {
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				report.Missing.push_back(path.string());
				return false;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();

			out = json::parse(buffer.str(), nullptr, false);
			if (out.is_discarded()) {
				// **Named rather than skipped.** A file somebody is editing by
				// hand and has broken is the one case where silence costs the
				// most: the instance simply does not appear, and nothing says
				// the file was even looked at.
				report.Notes.push_back(path.filename().string() + " is not valid JSON — skipped");
				return false;
			}
			return true;
		}

		// A TOML value as the equivalent `json` one.
		//
		// **A conversion rather than a second emitter, which is the whole reason
		// this row was cheap.** `LuauModuleFor` already turns a document into a
		// `ModuleScript` that returns it, and Rojo maps `*.toml` and `*.json` to
		// exactly the same thing — so the only part that was ever missing was a
		// parser, and everything downstream of this function is shared with the
		// JSON path unchanged. `D00104` predicted that and it held.
		//
		// **A date, a time or a date-time becomes its TOML spelling as a
		// string.** Those three have no JSON equivalent and no Luau one either,
		// and the alternatives are worse in ways an author would have to debug:
		// dropping them makes a key silently disappear, and inventing a table of
		// parts invents an interface this engine would then have to keep. The
		// text that round-trips back through a TOML parser is the one form that
		// loses nothing a reader cannot recover.
		json JsonFromToml(const toml::node &node) {
			if (const auto *table = node.as_table()) {
				json out = json::object();
				for (const auto &[key, value] : *table) {
					out[std::string(key.str())] = JsonFromToml(value);
				}
				return out;
			}
			if (const auto *array = node.as_array()) {
				json out = json::array();
				for (const auto &value : *array) {
					out.push_back(JsonFromToml(value));
				}
				return out;
			}
			if (const auto *text = node.as_string()) {
				return json(text->get());
			}
			if (const auto *integer = node.as_integer()) {
				return json(integer->get());
			}
			if (const auto *number = node.as_floating_point()) {
				return json(number->get());
			}
			if (const auto *boolean = node.as_boolean()) {
				return json(boolean->get());
			}

			// The three date and time types, named one at a time rather than
			// streamed through the base: `toml::node` is abstract and has no
			// `operator<<`, and the formatters are on the concrete values.
			std::ostringstream text;
			if (const auto *date = node.as_date()) {
				text << date->get();
			} else if (const auto *time = node.as_time()) {
				text << time->get();
			} else if (const auto *stamp = node.as_date_time()) {
				text << stamp->get();
			} else {
				// Anything a later toml++ adds. `nil` rather than a guess, and a
				// key that vanishes is the honest report of a type this
				// conversion has never seen.
				return json();
			}
			return json(text.str());
		}

		// Reads a TOML file, or reports why it could not be.
		bool ReadTomlFile(const std::filesystem::path &path, json &out, RojoSyncReport &report) {
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				report.Missing.push_back(path.string());
				return false;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();

			// **The non-throwing overload**, because a parse failure here is an
			// ordinary outcome — somebody is editing the file by hand — and this
			// sync reports rather than aborts. Same reason `ReadJsonFile` passes
			// `false` for its `allow_exceptions`.
			toml::parse_result parsed = toml::parse(buffer.str());
			if (!parsed) {
				// Named rather than skipped, and **with the parser's own
				// message**, which is the half a JSON note cannot give: TOML
				// fails on things that look right — a repeated key, a table
				// redefined, a bare string with a stray quote — and "is not
				// valid TOML" on its own sends an author back to stare at a file
				// they have already stared at.
				report.Notes.push_back(
					path.filename().string() + " is not valid TOML (" +
					std::string(parsed.error().description()) + ") — skipped"
				);
				return false;
			}

			out = JsonFromToml(parsed.table());
			return true;
		}

		// The whole text of a file, or nothing.
		bool ReadTextFile(const std::filesystem::path &path, std::string &out) {
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				return false;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();
			out = buffer.str();
			return true;
		}

		// The class a `$className` names, or an invalid id.
		//
		// **`Folder` is substituted rather than refused when a class is not
		// registered.** A project file is written against Roblox's whole class
		// tree and this engine has a fraction of it; refusing to sync a project
		// because it mentions `Team` would make the feature unusable against
		// every real project. The substitution is reported, so the answer is
		// "this became a folder" rather than silence.
		ClassId ClassFor(std::string_view name, RojoSyncReport &report) {
			if (name.empty()) {
				return FolderClass();
			}

			const ClassId found = Classes::Find(Name(std::string(name)));
			if (found.IsValid()) {
				return found;
			}

			report.Notes.push_back(std::string(name) + " is not a class here — made a Folder instead");
			return FolderClass();
		}

		// Reads a file into the world's program table.
		//
		// **The text goes into `SourceCache`, not onto the filesystem the engine
		// reads assets from.** A Rojo project lives wherever its author keeps it,
		// which is not under `Paths::Assets()` — and `ReadSource` checks the
		// world's table first for exactly this kind of reason. The upshot is a
		// synced game that runs and saves without anything being copied.
		bool StageProgram(Store &store, const std::filesystem::path &file, const std::string &key) {
			std::ifstream in(file, std::ios::binary);
			if (!in) {
				return false;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();

			auto *cache = store.ResourceMutable<engine::script::SourceCache>();
			if (cache == nullptr) {
				store.SetResource(engine::script::SourceCache{});
				cache = store.ResourceMutable<engine::script::SourceCache>();
			}
			if (cache == nullptr) {
				return false;
			}

			cache->Set(Name(key), buffer.str());
			return true;
		}

		// The same, for source this sync generated rather than read.
		//
		// **Split from `StageProgram` rather than given a "text or path"
		// argument**, because the two differ in what can go wrong: reading a
		// file can fail and a `std::string` cannot. A single function would have
		// had a failure path one of its callers could never take.
		bool StageProgramSource(Store &store, const std::string &key, const std::string &source) {
			auto *cache = store.ResourceMutable<engine::script::SourceCache>();
			if (cache == nullptr) {
				store.SetResource(engine::script::SourceCache{});
				cache = store.ResourceMutable<engine::script::SourceCache>();
			}
			if (cache == nullptr) {
				return false;
			}

			cache->Set(Name(key), source);
			return true;
		}

		void ReadNode(const std::string &name, const json &value, RojoNode &out) {
			out.Name = name;

			if (!value.is_object()) {
				return;
			}

			if (const auto found = value.find("$className"); found != value.end() && found->is_string()) {
				out.ClassName = found->get<std::string>();
			}
			if (const auto found = value.find("$path"); found != value.end() && found->is_string()) {
				out.Path = found->get<std::string>();
			}

			for (const auto &entry : value.items()) {
				// Anything beginning with `$` is a directive rather than a child.
				// `$properties`, `$ignoreUnknownInstances` and the rest are read
				// past rather than treated as instances named `$properties`.
				if (!entry.key().empty() && entry.key().front() == '$') {
					continue;
				}
				if (!entry.value().is_object()) {
					continue;
				}

				RojoNode child;
				ReadNode(entry.key(), entry.value(), child);
				out.Children.push_back(std::move(child));
			}
		}

		// A JSON document as a Luau chunk that returns it.
		//
		// **Rojo makes a `*.json` a `ModuleScript` returning a table**, and this
		// is that, emitted rather than parsed at run time — a module is source,
		// and generating source is what keeps `require` one mechanism instead of
		// two.
		//
		// **Every key is emitted as `["..."]` rather than as a bare
		// identifier.** A JSON key may be anything at all, and `{ foo-bar = 1 }`
		// is a syntax error where `{ ["foo-bar"] = 1 }` is not — so the bracket
		// form is used for all of them rather than a rule that has to decide.
		//
		// **Numbers go through `FormatNumber`.** `std::to_string` is `%f`, which
		// writes 1e-8 as "0.000000" — a value that round-trips through this
		// module as zero. `game::FormatNumber` is the shortest text that reads
		// back as the same double, and it exists because a document has numbers
		// that are not property values.
		void EmitLuauValue(const json &value, std::string &out, int depth) {
			// **A bound, because JSON nests as deep as a file says it does.** A
			// hand-written document twenty thousand levels deep would otherwise
			// recurse until the C stack ran out — a crash with no line number,
			// from a sync of a file somebody dropped in a folder.
			constexpr int MAXIMUM_DEPTH = 64;

			if (depth > MAXIMUM_DEPTH) {
				out += "nil";
				return;
			}

			switch (value.type()) {
			case json::value_t::null:
				out += "nil";
				return;
			case json::value_t::boolean:
				out += value.get<bool>() ? "true" : "false";
				return;
			case json::value_t::number_integer:
			case json::value_t::number_unsigned:
			case json::value_t::number_float:
				out += engine::game::FormatNumber(value.get<double>());
				return;
			case json::value_t::string: {
				// Through the JSON dumper, whose escaping is a superset of
				// Luau's for the characters that matter — quotes, backslashes
				// and control codes all come out in a form Luau reads back
				// identically. Writing a second escaper here would be a second
				// thing to get wrong about a tab.
				out += json(value.get<std::string>()).dump();
				return;
			}
			case json::value_t::array: {
				out += "{";
				for (size_t index = 0; index < value.size(); index++) {
					if (index > 0) {
						out += ", ";
					}
					EmitLuauValue(value[index], out, depth + 1);
				}
				out += "}";
				return;
			}
			case json::value_t::object: {
				out += "{";
				bool first = true;
				for (const auto &entry : value.items()) {
					if (!first) {
						out += ", ";
					}
					first = false;
					out += "[";
					out += json(entry.key()).dump();
					out += "] = ";
					EmitLuauValue(entry.value(), out, depth + 1);
				}
				out += "}";
				return;
			}
			default:
				out += "nil";
				return;
			}
		}

		std::string LuauModuleFor(const json &document) {
			std::string source = "-- generated from a .json or .toml by the Rojo sync\nreturn ";
			EmitLuauValue(document, source, 0);
			source += "\n";
			return source;
		}

		// Builds a `*.model.json` — a class, its properties and its children.
		//
		// **The same patch `ApplyMetadata` applies**, which is why the two share
		// it: a `.model.json` is a `.meta.json` with a class name and children,
		// and Rojo documents them with one property syntax on purpose.
		Entity BuildModel(
			Store &store, const json &document, const std::string &fallback, RojoSyncReport &report, int depth
		) {
			// The same bound `EmitLuauValue` takes, for the same reason.
			constexpr int MAXIMUM_DEPTH = 64;
			if (depth > MAXIMUM_DEPTH || !document.is_object()) {
				return NULL_ENTITY;
			}

			const std::string className = document.value("className", std::string("Folder"));
			const std::string name = document.value("name", fallback);

			const Entity node = store.CreateInstance(ClassFor(className, report), name);
			if (node == NULL_ENTITY) {
				return NULL_ENTITY;
			}
			report.Instances++;

			ApplyMetadata(store, node, document, report);

			if (const auto children = document.find("children");
				children != document.end() && children->is_array()) {
				for (const json &child : *children) {
					const Entity built = BuildModel(store, child, "Model", report, depth + 1);
					if (built != NULL_ENTITY) {
						store.SetParent(built, node);
					}
				}
			}
			return node;
		}

		// An instance whose whole content is one file's text.
		//
		// `*.txt` becomes a `StringValue` and `*.csv` a `LocalizationTable`,
		// which is Rojo's table exactly. See `scene::TextContent` for what a
		// `LocalizationTable` here does and does not do.
		Entity BuildTextValue(
			Store &store,
			const std::filesystem::path &file,
			const char *className,
			const std::string &name,
			RojoSyncReport &report
		) {
			std::string text;
			if (!ReadTextFile(file, text)) {
				report.Missing.push_back(file.string());
				return NULL_ENTITY;
			}

			const ClassId klass = Classes::Find(Name(className));
			if (!klass.IsValid()) {
				report.Notes.push_back(
					file.filename().string() + " wants a " + className + ", which is not registered"
				);
				return NULL_ENTITY;
			}

			const Entity node = store.CreateInstance(klass, name);
			if (node == NULL_ENTITY) {
				return NULL_ENTITY;
			}

			(void)store.SetProperty(node, Name("Value"), &text, sizeof(text));
			report.Instances++;
			return node;
		}

		// --- Roblox's binary model -------------------------------------------
		//
		// The last row of Rojo's table this engine could not build. The reader is
		// `bake::ReadRobloxModel`, which lives beside the other model decoders
		// because a binary format's parser is the largest attack surface a
		// content pipeline has and `bake/AGENTS.md` is where that is written
		// down. What is here is only the mapping onto instances.

		// The whole of a file, as bytes.
		//
		// **Split from `ReadTextFile` rather than sharing it**, because the two
		// differ in what the result is for: text goes into a `std::string` a
		// `StringValue` will hold, and this is a span handed to a parser that
		// treats every byte in it as hostile.
		bool ReadBinaryFile(const std::filesystem::path &path, std::vector<std::byte> &out) {
			std::ifstream in(path, std::ios::binary | std::ios::ate);
			if (!in) {
				return false;
			}

			const std::streampos size = in.tellg();
			if (size < 0) {
				return false;
			}
			in.seekg(0);

			out.resize(static_cast<size_t>(size));
			if (out.empty()) {
				return true;
			}
			return static_cast<bool>(in.read(reinterpret_cast<char *>(out.data()), size));
		}

		// One value out of a `.rbxm` as the property the class declares.
		//
		// **Keyed on the declared type and never on what the file stored**, which
		// is `ReadPropertyJson`'s rule one format along and matters more here: an
		// `.rbxm` states its own type for every value, and taking that as the
		// answer would let a file decide what a component holds.
		//
		// A number widens or narrows to whatever the property is, because Roblox
		// and this engine disagree about which of `Transparency` and `ZIndex` is
		// a float — and that disagreement is not something an author can fix in
		// their file.
		bool ReadPropertyRoblox(
			const engine::ecs::PropertyDescriptor &property,
			const engine::bake::RobloxValue &value,
			engine::game::PropertyValue &out
		) {
			using Kind = engine::bake::RobloxValueKind;

			out = engine::game::PropertyValue{};
			out.Type = property.Type;

			switch (property.Type) {
			case PropertyType::Bool:
				if (value.Kind != Kind::Bool) {
					return false;
				}
				out.Bool = value.Bool;
				return true;

			case PropertyType::Int32:
			case PropertyType::Int64:
			case PropertyType::Float:
			case PropertyType::Double: {
				if (value.Kind != Kind::Integer && value.Kind != Kind::Number) {
					return false;
				}
				const double number =
					value.Kind == Kind::Integer ? static_cast<double>(value.Integer) : value.Number;
				out.Int32 = static_cast<int32_t>(number);
				out.Int64 = static_cast<int64_t>(number);
				out.Float = static_cast<float>(number);
				out.Double = number;
				return true;
			}

			case PropertyType::String:
				if (value.Kind != Kind::Text) {
					return false;
				}
				out.String = value.Text;
				return true;

			case PropertyType::Name:
			case PropertyType::Enum:
				// **A `.rbxm` enum never reaches here**, because the reader
				// refuses one: it is a number naming a member of Roblox's table
				// and this engine names members by string. What can reach here is
				// a *string* landing on a property this engine declares as an
				// enum, and that is checked against `EnumTable` for
				// `ReadPropertyJson`'s reason.
				if (value.Kind != Kind::Text) {
					return false;
				}
				out.Name = Name(value.Text);
				if (property.Type == PropertyType::Enum &&
					!engine::ecs::EnumTable::Has(property.EnumName, out.Name)) {
					return false;
				}
				return true;

			case PropertyType::Vector3:
				if (value.Kind != Kind::Vector3) {
					return false;
				}
				out.Vector3 = value.Vector3;
				return true;

			case PropertyType::Vector2:
				if (value.Kind != Kind::Vector2) {
					return false;
				}
				out.Vector2 = value.Vector2;
				return true;

			case PropertyType::Color3:
				if (value.Kind != Kind::Color3) {
					return false;
				}
				out.Color3 = value.Color3;
				return true;

			case PropertyType::CFrame:
				// **The rotation survives, unlike the JSON path's.** A
				// `.model.json` writes a `CFrame` as twelve numbers and this
				// module reads only the three of its position; a `.rbxm` states
				// an orientation the reader has already turned into a
				// quaternion, so there is nothing left to approximate.
				if (value.Kind != Kind::CFrame) {
					return false;
				}
				out.CFrame = value.CFrame;
				return true;

			case PropertyType::UDim:
				if (value.Kind != Kind::UDim) {
					return false;
				}
				out.UDim = value.UDim;
				return true;

			case PropertyType::UDim2:
				if (value.Kind != Kind::UDim2) {
					return false;
				}
				out.UDim2 = value.UDim2;
				return true;

			case PropertyType::Rect:
				if (value.Kind != Kind::Rect) {
					return false;
				}
				out.Rect = value.Rect;
				return true;

			case PropertyType::NumberRange:
				if (value.Kind != Kind::NumberRange) {
					return false;
				}
				out.NumberRange = value.NumberRange;
				return true;

			default:
				// The two sequences and a `Reference`. The reader produces
				// neither, so this is the arm nothing reaches rather than a
				// refusal somebody will meet.
				return false;
			}
		}

		// What building one `.rbxm` is accumulating.
		struct RobloxImport {
			RojoSyncReport &Report;

			// The keys this file has already staged a program under, so that two
			// siblings of one name are two programs rather than one shared by
			// both.
			//
			// **Per import rather than per world**, which is what keeps a second
			// sync of an unchanged file idempotent: the same file lays down the
			// same keys, and only a collision *inside* one file needs a suffix.
			std::vector<std::string> Staged;

			// How many properties the file carried that this engine has no
			// declaration for.
			//
			// **Counted rather than reported one by one.** A `.meta.json` is
			// written by hand, so a key this engine does not have is a typo worth
			// naming; an `.rbxm` is written by Studio, which stores every property
			// of every class — a note each would be a hundred lines saying the
			// engine is smaller than Roblox, and would bury the notes that are
			// about this file.
			size_t Absent = 0;
		};

		// A source key nothing in this import already holds.
		//
		// **A suffix only on a collision inside one file**, because Roblox lets
		// two siblings share a name and this engine's `SourceCache` is keyed on
		// one string. Suffixing against the whole world instead would give the
		// same unchanged file a different key on every sync.
		std::string StagedKey(RobloxImport &import, const std::string &path) {
			std::string candidate = path;
			size_t attempt = 1;

			while (std::find(import.Staged.begin(), import.Staged.end(), candidate) != import.Staged.end()) {
				attempt++;
				candidate = path + " (" + std::to_string(attempt) + ")";
			}

			import.Staged.push_back(candidate);
			return candidate;
		}

		// Builds one instance out of a `.rbxm`, and everything under it.
		//
		// @param name The instance's name. The root's is the file's, which is
		//        every other row of Rojo's table's rule; a child's is its own.
		Entity BuildRobloxInstance(
			Store &store,
			const engine::bake::RobloxInstance &node,
			const std::string &name,
			const std::string &path,
			RobloxImport &import,
			int depth
		) {
			if (depth > static_cast<int>(engine::bake::MAXIMUM_ROBLOX_DEPTH)) {
				return NULL_ENTITY;
			}

			// **A script's program comes out of the file, not out of a path.**
			// Roblox stores `Source` on the instance and this engine stores a key
			// into the world's `SourceCache`, so the import stages the text under
			// a key derived from where the instance sits — which is what makes an
			// imported Tool's scripts run rather than exist.
			const bool module = node.ClassName == "ModuleScript";
			const bool local = node.ClassName == "LocalScript";
			const bool script = module || local || node.ClassName == "Script";

			const engine::bake::RobloxValue *source = nullptr;
			if (script) {
				for (const engine::bake::RobloxProperty &property : node.Properties) {
					if (property.Name == "Source" &&
						property.Value.Kind == engine::bake::RobloxValueKind::Text) {
						source = &property.Value;
						break;
					}
				}
			}

			Entity instance = NULL_ENTITY;
			if (script && source != nullptr) {
				const std::string key = StagedKey(import, path);
				if (StageProgramSource(store, key, source->Text)) {
					instance = module ? engine::script::MakeModule(store, key, name)
									  : engine::script::MakeScript(store, key, name, local);
					import.Report.Scripts++;
				}
			}

			if (instance == NULL_ENTITY) {
				instance = store.CreateInstance(ClassFor(node.ClassName, import.Report), name);
			}
			if (instance == NULL_ENTITY) {
				return NULL_ENTITY;
			}
			import.Report.Instances++;

			for (const engine::bake::RobloxProperty &property : node.Properties) {
				if (source != nullptr && &property.Value == source) {
					continue;
				}

				const engine::ecs::PropertyDescriptor *found = PropertyNamed(store, instance, property.Name);
				if (found == nullptr) {
					import.Absent++;
					continue;
				}

				engine::game::PropertyValue value;
				if (!ReadPropertyRoblox(*found, property.Value, value)) {
					import.Report.Notes.push_back(
						name + "." + property.Name + " is not a " + engine::ecs::Describe(found->Type) +
						" here — skipped"
					);
					continue;
				}

				if (!engine::game::WriteProperty(store, instance, *found, value)) {
					import.Report.Notes.push_back(
						name + "." + property.Name + " was refused by the world — skipped"
					);
				}
			}

			for (const engine::bake::RobloxInstance &child : node.Children) {
				const Entity built =
					BuildRobloxInstance(store, child, child.Name, path + "/" + child.Name, import, depth + 1);
				if (built != NULL_ENTITY) {
					store.SetParent(built, instance);
				}
			}
			return instance;
		}

		// Builds a `*.rbxm` into one instance.
		//
		// **One instance, because that is what Rojo's table maps a model file
		// to.** The container allows any number of roots and a file with several
		// is refused by name rather than wrapped in a folder somebody would then
		// have to explain — inventing a level the author did not write is the
		// kind of quiet wrongness this whole file is against.
		Entity BuildRobloxModel(
			Store &store,
			const std::filesystem::path &file,
			const std::string &key,
			const std::string &name,
			RojoSyncReport &report
		) {
			const std::string leaf = file.filename().string();

			std::vector<std::byte> bytes;
			if (!ReadBinaryFile(file, bytes)) {
				report.Missing.push_back(file.string());
				return NULL_ENTITY;
			}

			engine::bake::RobloxModel model;
			std::string failure;
			if (!engine::bake::ReadRobloxModel(bytes, model, failure)) {
				// **With the reader's own message.** "Is not a valid rbxm" sends
				// an author back to stare at a binary file; "wrong signature" tells
				// them they renamed an `.rbxmx`.
				report.Notes.push_back(leaf + " could not be read (" + failure + ") — skipped");
				return NULL_ENTITY;
			}

			for (const std::string &note : model.Notes) {
				report.Notes.push_back(leaf + ": " + note);
			}

			if (model.Roots.size() != 1) {
				report.Notes.push_back(
					leaf + " holds " + std::to_string(model.Roots.size()) +
					" instances at its top level, and a model file maps to one — skipped"
				);
				return NULL_ENTITY;
			}

			RobloxImport import{report, {}, 0};
			const Entity built = BuildRobloxInstance(store, model.Roots[0], name, key, import, 0);

			if (import.Absent > 0) {
				report.Notes.push_back(
					leaf + " carries " + std::to_string(import.Absent) +
					" property value(s) this engine has no property for — skipped"
				);
			}
			return built;
		}

		// **Declared before the dispatcher and defined after it**, because a
		// nested project builds a tree that builds directories that may hold
		// another nested project. The recursion is real and the cycle check in
		// `BuildProjectFile` is what bounds it.
		bool BuildProjectFile(
			Store &store, const std::filesystem::path &file, Entity parent, RojoSyncReport &report
		);

		// Whether a file is a sidecar rather than an instance.
		//
		// `*.meta.json` sets properties on the instance its *sibling* produced,
		// so it is consumed by that sibling and never becomes a node of its own.
		bool IsMetadataFile(const std::filesystem::path &file) {
			return EndsWith(file.filename().string(), ".meta.json");
		}

		// The `.meta.json` beside a file, if any, applied to what it produced.
		//
		// **Named from the *stem* rather than from the whole filename**, which
		// is Rojo's rule: `Button.meta.json` patches whatever `Button.luau`,
		// `Button.model.json` or `Button.txt` built. That is why one lookup
		// serves every mapping below rather than each having its own.
		void ApplySidecar(
			Store &store, const std::filesystem::path &file, Entity instance, RojoSyncReport &report
		) {
			if (instance == NULL_ENTITY) {
				return;
			}

			// The stem with every extension off: `Button.model.json` is
			// `Button`, and so is `Button.luau`.
			std::string stem = file.filename().string();
			if (const size_t dot = stem.find('.'); dot != std::string::npos) {
				stem.resize(dot);
			}

			const std::filesystem::path sidecar = file.parent_path() / (stem + ".meta.json");

			std::error_code missing;
			if (!std::filesystem::is_regular_file(sidecar, missing)) {
				return;
			}

			json document;
			if (ReadJsonFile(sidecar, document, report)) {
				ApplyMetadata(store, instance, document, report);
			}
		}

		// Everything in Rojo's table that is not a script and not a directory.
		//
		// **One function, because every one of them is "read a file, make an
		// instance, parent it"** — and because the order the suffixes are tested
		// in is a rule rather than an accident: `.meta.json` and `.model.json`
		// both end in `.json`, so the specific ones have to be asked first or a
		// model would become a module returning its own description.
		//
		// @return `true` when the file was this function's to handle, whether or
		//         not it produced an instance. A `false` sends it on to the
		//         script classifier.
		bool BuildMapped(
			Store &store,
			const std::filesystem::path &file,
			Entity parent,
			const std::string &keyPrefix,
			RojoSyncReport &report
		) {
			const std::string leaf = file.filename().string();

			// Consumed by whatever it sits beside. Never a node.
			if (IsMetadataFile(file)) {
				return true;
			}

			// A nested project. Followed rather than reported since v0.12; the
			// cycle check is `BuildProjectFile`'s and it is what makes the
			// recursion safe rather than merely possible.
			if (EndsWith(leaf, ".project.json")) {
				return BuildProjectFile(store, file, parent, report);
			}

			Entity node = NULL_ENTITY;
			std::string name = file.stem().string();

			if (EndsWith(leaf, ".model.json")) {
				// `Thing.model.json` is named `Thing`, not `Thing.model`.
				if (const size_t dot = name.rfind('.'); dot != std::string::npos) {
					name.resize(dot);
				}

				json document;
				if (ReadJsonFile(file, document, report)) {
					node = BuildModel(store, document, name, report, 0);
				}
			} else if (EndsWith(leaf, ".json")) {
				// A plain `*.json` is a `ModuleScript` returning a table.
				json document;
				if (ReadJsonFile(file, document, report)) {
					const std::string key = keyPrefix + leaf;
					if (StageProgramSource(store, key, LuauModuleFor(document))) {
						node = engine::script::MakeModule(store, key, name);
						report.Instances++;
						report.Scripts++;
					}
				}
			} else if (EndsWith(leaf, ".toml")) {
				// **The same instance a `*.json` produces**, because Rojo maps
				// them to the same thing. The document is converted to a `json`
				// and handed to the same emitter rather than getting a second
				// one — see `JsonFromToml`.
				json document;
				if (ReadTomlFile(file, document, report)) {
					const std::string key = keyPrefix + leaf;
					if (StageProgramSource(store, key, LuauModuleFor(document))) {
						node = engine::script::MakeModule(store, key, name);
						report.Instances++;
						report.Scripts++;
					}
				}
			} else if (EndsWith(leaf, ".rbxm")) {
				// **Named after the file, not after what the file called it.**
				// Every other row of Rojo's table takes the instance's name from
				// the path — a `.model.json`, a `.txt`, a script — and a model
				// file that kept its own would be the one place in a project
				// where renaming a file did nothing.
				node = BuildRobloxModel(store, file, keyPrefix + leaf, name, report);
			} else if (EndsWith(leaf, ".txt")) {
				node = BuildTextValue(store, file, "StringValue", name, report);
			} else if (EndsWith(leaf, ".csv")) {
				node = BuildTextValue(store, file, "LocalizationTable", name, report);
			} else {
				return false;
			}

			if (node != NULL_ENTITY) {
				store.SetParent(node, parent);
				ApplySidecar(store, file, node, report);
			}
			return true;
		}

		// Builds one directory into an instance tree.
		void BuildDirectory(
			Store &store,
			const std::filesystem::path &directory,
			Entity parent,
			const std::string &keyPrefix,
			RojoSyncReport &report
		) {
			// **A directory holding `default.project.json` *is* that project,
			// and is not also walked.** Rojo's rule, and the failure it prevents
			// is quiet rather than loud: every wally package ships one, mapping
			// its `lib/` or `src/` folder onto the module a game requires. A
			// sync that followed the project *and* walked the folder beside it
			// built both — one copy under the name the package publishes and one
			// under the folder's own — and two copies of a `ModuleScript` are
			// two modules with two states, which is `mono.studio/AGENTS.md`'s own
			// rule about a module being keyed by instance.
			//
			// Measured on a real project: `raceapet` synced 2012 scripts against
			// 1643 files that could be one, and the difference was every
			// installed package counted twice.
			std::error_code holds;
			const std::filesystem::path project = directory / "default.project.json";
			if (std::filesystem::is_regular_file(project, holds)) {
				BuildProjectFile(store, project, parent, report);
				return;
			}

			// **Sorted, because a directory walk is not ordered.** Two syncs of
			// one tree have to produce the same creation order or the entity ids
			// differ between them, and an id that moves is a saved reference that
			// points somewhere else.
			std::vector<std::filesystem::path> entries;
			std::error_code failed;
			for (const auto &entry : std::filesystem::directory_iterator(directory, failed)) {
				entries.push_back(entry.path());
			}
			if (failed) {
				return;
			}
			std::sort(entries.begin(), entries.end());

			for (const std::filesystem::path &entry : entries) {
				std::error_code kind;
				if (std::filesystem::is_directory(entry, kind)) {
					const std::string leaf = entry.filename().string();

					// An `init` file makes the directory itself the script
					// rather than a folder containing one — which is how a
					// program gets children without every path gaining a level.
					//
					// **Which class it becomes is the init file's suffix**,
					// exactly as it is for any other file: `init.luau` is a
					// module, `init.server.luau` a `Script`, `init.client.luau`
					// a `LocalScript`. Reading only `init.luau` — which this did
					// — made every `init.server.luau` project a folder plus a
					// stray script called `init`.
					Entity node = NULL_ENTITY;
					const InitFile init = FindInit(entry, report);

					if (init.Present()) {
						const std::string key = keyPrefix + leaf + "/" + init.File.filename().string();
						if (StageProgram(store, init.File, key)) {
							node = init.Module ? engine::script::MakeModule(store, key, leaf)
											   : engine::script::MakeScript(store, key, leaf, init.Local);
							report.Scripts++;
						}
					}

					if (node == NULL_ENTITY) {
						node = store.CreateInstance(FolderClass(), leaf);
					}
					if (node == NULL_ENTITY) {
						continue;
					}

					report.Instances++;
					store.SetParent(node, parent);

					// **`init.meta.json` patches the directory's own instance,
					// and may change its class outright** — which is Rojo's one
					// way of saying "this folder is really a `Model`". Applied
					// after the node exists and before its children are built,
					// so a child parented into it lands under the right thing.
					//
					// The class change is the one part not honoured: a class is
					// the archetype an entity was created in, and moving a live
					// row between class trees is not something `Store` offers.
					// Reported by `ApplyMetadata` as a property it does not
					// have, which is the honest answer.
					const std::filesystem::path metadata = entry / "init.meta.json";
					if (std::filesystem::is_regular_file(metadata, kind)) {
						json document;
						if (ReadJsonFile(metadata, document, report)) {
							ApplyMetadata(store, node, document, report);
						}
					}

					BuildDirectory(store, entry, node, keyPrefix + leaf + "/", report);
					continue;
				}

				if (IsInitFile(entry)) {
					// Consumed by the directory above, or reported there as one
					// init file too many. Either way it is not a child.
					continue;
				}

				// **The rest of Rojo's table, before the script test**, because
				// a `.model.json` is not a script and a `.meta.json` is not an
				// instance at all — and both end in an extension the script
				// classifier has never heard of.
				if (BuildMapped(store, entry, parent, keyPrefix, report)) {
					continue;
				}

				const ScriptFile file = ClassifyFile(entry);
				if (!file.IsScript) {
					// Named rather than skipped in silence, so an author whose
					// `.rbxm` did not appear knows why — and named by *what Rojo
					// says it is* where there is an answer, so a gap here reads
					// as a gap rather than as an unrecognised file.
					if (const char *kind_ = UnbuiltKind(entry); kind_ != nullptr) {
						report.Notes.push_back(entry.filename().string() + " is " + kind_ + " — skipped");
					} else {
						report.Notes.push_back(entry.filename().string() + " is not a script — skipped");
					}
					continue;
				}

				const std::string key = keyPrefix + entry.filename().string();
				if (!StageProgram(store, entry, key)) {
					report.Missing.push_back(entry.string());
					continue;
				}

				const Entity script = file.Module
										  ? engine::script::MakeModule(store, key, file.Name)
										  : engine::script::MakeScript(store, key, file.Name, file.Local);
				if (script == NULL_ENTITY) {
					continue;
				}

				store.SetParent(script, parent);
				report.Instances++;
				report.Scripts++;

				// **A script takes a sidecar like anything else.** Rojo's
				// `.meta.json` patches whatever the file of that stem produced,
				// and a script is the most common thing it produces —
				// `Disabled` on a `Script` is the first patch anybody writes.
				ApplySidecar(store, entry, script, report);
			}
		}

		// Builds whatever a `$path` names into an instance that already exists.
		//
		// **One implementation, because two things map a path onto a node.** A
		// node in a project's tree does it, and so does the *root* of a nested
		// project — a wally package is `{"tree": {"$path": "lib"}}` and nothing
		// else, so a nested build that only walked the root's children built
		// nothing at all for every package a game installs. That failure was
		// invisible while directories were also walked beside their project
		// file: the modules appeared, under the folder's name instead of the
		// package's, and both copies were there.
		//
		// @param path Relative to `root`. Empty does nothing, which is what a
		//        node with only children is.
		void BuildPathInto(
			Store &store,
			const std::filesystem::path &root,
			const std::string &path,
			Entity into,
			RojoSyncReport &report
		) {
			if (path.empty() || into == NULL_ENTITY) {
				return;
			}

			const std::filesystem::path source = root / path;
			std::error_code kind;

			if (std::filesystem::is_directory(source, kind)) {
				BuildDirectory(store, source, into, path + "/", report);
				return;
			}
			if (!std::filesystem::exists(source, kind)) {
				report.Missing.push_back(path);
				return;
			}

			// **The same dispatcher a directory walk uses**, so a `$path` naming
			// a `.model.json` builds the same instance it would have built one
			// folder up. Two answers to one mapping is the duplicate this whole
			// file is written against.
			//
			// The prefix is the file's *folder*, because `BuildMapped` appends
			// the leaf — passing the whole path would key a staged module under
			// `src/data.json/data.json`.
			const std::string folder = path.substr(0, path.find_last_of('/') + 1);
			if (BuildMapped(store, source, into, folder, report)) {
				return;
			}

			const ScriptFile file = ClassifyFile(source);
			if (!file.IsScript) {
				// The same accounting a directory walk does. A `$path` naming a
				// `.rbxm` used to produce nothing and say nothing, which is the
				// one outcome an author cannot act on.
				const char *unbuilt = UnbuiltKind(source);
				report.Notes.push_back(
					path + " is " + (unbuilt != nullptr ? unbuilt : "not a script") + " — skipped"
				);
				return;
			}

			if (!StageProgram(store, source, path)) {
				return;
			}

			const Entity script = file.Module
									  ? engine::script::MakeModule(store, path, file.Name)
									  : engine::script::MakeScript(store, path, file.Name, file.Local);
			if (script != NULL_ENTITY) {
				store.SetParent(script, into);
				report.Instances++;
				report.Scripts++;
			}
		}

		void BuildNode(
			Store &store,
			const RojoNode &node,
			const std::filesystem::path &root,
			Entity parent,
			RojoSyncReport &report
		) {
			// **An existing instance of that name is reused, not duplicated.**
			// `scene::InstallServices` has already put `Workspace` and the rest
			// into the world, and a sync that made a second `ReplicatedStorage`
			// beside the real one would produce a tree where half the game
			// cannot find the other half.
			Entity node_ = parent == NULL_ENTITY ? store.FindFirstRoot(node.Name)
												 : store.FindFirstChild(parent, node.Name);

			if (node_ == NULL_ENTITY) {
				node_ = store.CreateInstance(ClassFor(node.ClassName, report), node.Name);
				if (node_ == NULL_ENTITY) {
					return;
				}
				report.Instances++;
				if (parent != NULL_ENTITY) {
					store.SetParent(node_, parent);
				}
			}

			BuildPathInto(store, root, node.Path, node_, report);

			for (const RojoNode &child : node.Children) {
				BuildNode(store, child, root, node_, report);
			}
		}

		// A `*.project.json` found under a `$path`, followed.
		//
		// **Rojo lets a project include another and this now does too**, which
		// is what a package manager's output looks like: `Packages/` is a folder
		// of projects, and a sync that reported each one instead of building it
		// would build the half of a game an author wrote and none of the half
		// they installed.
		//
		// **The cycle check is the part that has to exist before the
		// recursion.** Two projects that include each other — or one that
		// includes itself, which is what a copy-pasted `$path` produces — would
		// otherwise recurse until the stack ran out, with no line number and no
		// file named. The set is of *canonical* paths, so two spellings of one
		// file are one entry.
		bool BuildProjectFile(
			Store &store, const std::filesystem::path &file, Entity parent, RojoSyncReport &report
		) {
			// **A file-local static would be wrong and a member would be
			// surface.** The set has to live exactly as long as one top-level
			// sync, and this is the only recursion in it — so it is a static
			// inside the function that opens and closes the scope, cleared by
			// the outermost call.
			static std::vector<std::filesystem::path> loading;

			std::error_code failed;
			const std::filesystem::path canonical = std::filesystem::weakly_canonical(file, failed);
			const std::filesystem::path key = failed ? file : canonical;

			if (std::find(loading.begin(), loading.end(), key) != loading.end()) {
				report.Notes.push_back(
					file.filename().string() + " includes itself — the second visit was skipped"
				);
				return true;
			}

			json document;
			if (!ReadJsonFile(file, document, report)) {
				return true;
			}

			RojoProject nested;
			std::string error;
			if (!ParseRojoProject(document.dump(), nested, error)) {
				report.Notes.push_back(file.filename().string() + " is not a project: " + error);
				return true;
			}

			loading.push_back(key);

			// **The root's own `$path` first, into the including node.** A
			// package's project file is a root with a path and no children —
			// `{"tree": {"$path": "lib"}}` — so a build that started at the
			// children built nothing for it. The root maps onto the node that
			// included it rather than onto a new instance: a nested project is a
			// subtree of the one that named it, and creating an extra level here
			// would put every package's contents one folder deeper than the name
			// a game requires.
			BuildPathInto(store, file.parent_path(), nested.Tree.Path, parent, report);

			// **Then the tree's own children, exactly as a top-level sync builds
			// them** — and under the *including* node, for the same reason.
			for (const RojoNode &child : nested.Tree.Children) {
				BuildNode(store, child, file.parent_path(), parent, report);
			}

			loading.pop_back();
			return true;
		}
	}

	engine::ecs::ClassId FolderClass() {
		// A function-local static, so the class exists before the first caller
		// reads an id from it and cannot be registered twice.
		static const ClassId folder = [] {
			// Through `PartClass` first, so the tree's root exists. A second
			// root would be a class tree nothing could compare across.
			engine::scene::EnsureClassTree();
			const ClassId instance = Classes::Find(Name("Instance"));
			return Classes::Register("Folder", instance, {});
		}();
		return folder;
	}

	bool ParseRojoProject(std::string_view json_, RojoProject &out, std::string &error) {
		json document = json::parse(json_, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			error = "not a JSON object";
			return false;
		}

		const auto tree = document.find("tree");
		if (tree == document.end() || !tree->is_object()) {
			error = "no 'tree' — this is not a Rojo project file";
			return false;
		}

		if (const auto name = document.find("name"); name != document.end() && name->is_string()) {
			out.Name = name->get<std::string>();
		}

		// The tree's own name is the project's, because the root of a place is
		// the `DataModel` and nothing else names it.
		ReadNode(out.Name.empty() ? "Game" : out.Name, *tree, out.Tree);
		return true;
	}

	bool SyncRojoProject(
		const RojoProject &project,
		const std::filesystem::path &root,
		Store &store,
		RojoSyncReport &report,
		std::string &error
	) {
		std::error_code failed;
		if (!std::filesystem::is_directory(root, failed)) {
			error = root.string() + " is not a directory";
			return false;
		}

		(void)FolderClass();
		engine::script::RegisterScriptComponents();

		// **The tree's own children, not the tree itself.** The root node is the
		// `DataModel`, which this engine models as the world rather than as an
		// instance in it — so its children become the world's roots.
		for (const RojoNode &child : project.Tree.Children) {
			BuildNode(store, child, root, NULL_ENTITY, report);
		}

		if (report.Instances == 0) {
			error = "the project named nothing this world could build";
			return false;
		}
		return true;
	}

	// --- the universe above them -------------------------------------------

	size_t RojoUniverseReport::Synced() const {
		return static_cast<size_t>(std::count_if(
			Worlds.begin(), Worlds.end(), [](const RojoWorldSync &world) { return world.Synced; }
		));
	}

	size_t RojoUniverseReport::Failed() const {
		return Worlds.size() - Synced();
	}

	bool ParseRojoUniverse(std::string_view json_, RojoUniverse &out, std::string &error) {
		json document = json::parse(json_, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			error = "not a JSON object";
			return false;
		}

		const auto worlds = document.find("worlds");
		if (worlds == document.end() || !worlds->is_object()) {
			error = "no 'worlds' — this is not a universe file";
			return false;
		}

		if (const auto name = document.find("name"); name != document.end() && name->is_string()) {
			out.Name = name->get<std::string>();
		}

		for (const auto &entry : worlds->items()) {
			if (entry.key().empty() || !entry.value().is_string()) {
				// **Skipped and not fatal**, which is the rule the whole layer
				// is built on: one malformed entry costs its own world. A
				// refusal here would be one typo taking every other world with
				// it, which is exactly what syncing them separately is for.
				continue;
			}
			out.Worlds.push_back(RojoUniverseWorld{entry.key(), entry.value().get<std::string>()});
		}

		if (out.Worlds.empty()) {
			error = "'worlds' names nothing";
			return false;
		}
		return true;
	}

	std::filesystem::path RojoProjectFor(const std::filesystem::path &root, const RojoUniverseWorld &world) {
		if (world.Path.empty()) {
			return {};
		}

		const std::filesystem::path candidate = root / world.Path;
		std::error_code kind;

		if (std::filesystem::is_regular_file(candidate, kind)) {
			return candidate;
		}
		if (!std::filesystem::is_directory(candidate, kind)) {
			return {};
		}

		// Rojo's own name first, so a subfolder stays a project every tool in
		// that ecosystem understands.
		for (const char *leaf : {"default.project.json", "main.default.json"}) {
			const std::filesystem::path project = candidate / leaf;
			if (std::filesystem::is_regular_file(project, kind)) {
				return project;
			}
		}
		return {};
	}

	bool SyncRojoUniverse(
		const RojoUniverse &universe,
		const std::filesystem::path &root,
		engine::world::Universe &worlds,
		RojoUniverseReport &report,
		std::string &error
	) {
		std::error_code failed;
		if (!std::filesystem::is_directory(root, failed)) {
			error = root.string() + " is not a directory";
			return false;
		}

		for (const RojoUniverseWorld &declared : universe.Worlds) {
			RojoWorldSync &result = report.Worlds.emplace_back();
			result.World = declared.Name;

			result.Project = RojoProjectFor(root, declared);
			if (result.Project.empty()) {
				result.Error = "no project file at " + declared.Path;
				continue;
			}

			std::ifstream in(result.Project, std::ios::binary);
			if (!in) {
				result.Error = "could not read " + result.Project.string();
				continue;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();

			RojoProject project;
			if (!ParseRojoProject(buffer.str(), project, result.Error)) {
				continue;
			}

			// **Found before created**, so a second sync builds into the world
			// the first one made rather than beside it. `Universe::Create`
			// already returns the world holding a name, and this says so at the
			// call site because the difference decides whether an author's
			// hand-placed instances survive.
			const engine::core::Name key(declared.Name);
			engine::world::WorldId id = worlds.Find(key);
			bool created = false;

			if (!id.IsValid()) {
				engine::world::WorldSettings settings;
				settings.Name = key;

				engine::world::WorldStatus status = engine::world::WorldStatus::Ok;
				id = worlds.Create(settings, &status);
				created = true;

				if (!id.IsValid()) {
					result.Error = "the driver refused to create a world called " + declared.Name;
					continue;
				}
			}

			const std::filesystem::path directory = result.Project.parent_path();
			std::string built;

			worlds.Enter(id, [&](Store &store) {
				// A world this call made has none of the services a place
				// needs, and `BuildNode` reuses an existing `Workspace` rather
				// than making a second — so installing them first is what stops
				// a synced world from having two.
				if (created) {
					engine::scene::InstallServices(store);
				}
				if (!SyncRojoProject(project, directory, store, result.Report, built)) {
					result.Error = built;
					return;
				}
				result.Synced = true;
			});

			if (!result.Synced && result.Error.empty()) {
				// `Enter` refused — the world is remote, faulted or held down.
				// Named rather than left as a silent failure, because "nothing
				// happened and nothing said why" is the report this layer exists
				// to avoid.
				result.Error = "could not enter world " + declared.Name;
			}
		}

		if (report.Synced() == 0) {
			error = "no world in the universe could be built";
			return false;
		}
		return true;
	}
}
