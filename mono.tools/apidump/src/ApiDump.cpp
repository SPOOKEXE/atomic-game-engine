#include <Luau/BuiltinDefinitions.h>
#include <Luau/Config.h>
#include <Luau/Frontend.h>
#include <Luau/ToString.h>
#include <Luau/Type.h>
#include <Luau/TypePack.h>
#include <algorithm>
#include <apidump/ApiDump.hpp>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <unordered_map>

namespace apidump {

	namespace {

		using Json = nlohmann::ordered_json;

		// The checker reads nothing from disk: the declarations arrive as text.
		struct NoFiles : Luau::FileResolver {
			std::optional<Luau::SourceCode> readSource(const Luau::ModuleName &) override {
				return std::nullopt;
			}
		};

		struct StrictConfig : Luau::ConfigResolver {
			Luau::Config Strict;

			StrictConfig() {
				Strict.mode = Luau::Mode::Strict;
			}

			const Luau::Config &
			getConfig(const Luau::ModuleName &, const Luau::TypeCheckLimits &) const override {
				return Strict;
			}
		};

		bool IsIdentifierCharacter(const char character) {
			return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
		}

		// `Enum_Material` is how a definition file has to spell `Enum.Material`;
		// the dump uses the spelling a script writes.
		std::string ScriptSpelling(std::string text) {
			constexpr std::string_view PREFIX = "Enum_";
			size_t at = 0;
			while ((at = text.find(PREFIX, at)) != std::string::npos) {
				if (at == 0 || !IsIdentifierCharacter(text[at - 1])) {
					text[at + 4] = '.';
				}
				at += PREFIX.size();
			}
			return text;
		}

		Luau::ToStringOptions TextOptions() {
			Luau::ToStringOptions options;
			options.functionTypeArguments = true;
			options.hideFunctionSelfArgument = true;
			options.maxTableLength = 100000;
			options.maxTypeLength = 100000;
			options.compositeTypesSingleLineLimit = 1000;
			return options;
		}

		std::string TypeText(const Luau::TypeId type) {
			Luau::ToStringOptions options = TextOptions();
			return ScriptSpelling(Luau::toString(type, options));
		}

		std::string PackText(const Luau::TypePackId pack) {
			Luau::ToStringOptions options = TextOptions();
			return ScriptSpelling(Luau::toString(pack, options));
		}

		// One callable shape. A method is called with a colon and its `self` is
		// already dropped from `Parameters`.
		struct Signature {
			Json Parameters = Json::array();
			std::string Returns;
			bool Method = false;
		};

		std::string ReturnText(const Luau::TypePackId returns) {
			const auto [types, tail] = Luau::flatten(returns);
			if (types.size() == 1 && !tail) {
				return TypeText(types.front());
			}
			if (types.empty() && !tail) {
				return "()";
			}
			std::string text = "(";
			for (size_t index = 0; index < types.size(); index++) {
				text += (index == 0 ? "" : ", ") + TypeText(types[index]);
			}
			if (tail) {
				text += std::string(types.empty() ? "" : ", ") + PackText(*tail);
			}
			return text + ")";
		}

		// Every overload a type can be called with, or none when it is not a
		// function.
		void CollectSignatures(const Luau::TypeId type, std::vector<Signature> &into) {
			const Luau::TypeId followed = Luau::follow(type);

			if (const auto *intersection = Luau::get<Luau::IntersectionType>(followed)) {
				for (const Luau::TypeId part : intersection->parts) {
					CollectSignatures(part, into);
				}
				return;
			}

			const auto *function = Luau::get<Luau::FunctionType>(followed);
			if (function == nullptr) {
				return;
			}

			Signature signature;
			const auto [arguments, tail] = Luau::flatten(function->argTypes);

			const auto nameOf = [&](const size_t index) -> std::string {
				if (index < function->argNames.size() && function->argNames[index]) {
					return function->argNames[index]->name;
				}
				return "arg" + std::to_string(index + 1);
			};

			size_t first = 0;
			if (function->hasSelf || (!function->argNames.empty() && function->argNames[0] &&
									  function->argNames[0]->name == "self")) {
				signature.Method = true;
				first = 1;
			}

			for (size_t index = first; index < arguments.size(); index++) {
				signature.Parameters.push_back(
					{{"name", nameOf(index)}, {"type", TypeText(arguments[index])}}
				);
			}
			if (tail) {
				std::string variadic = PackText(*tail);
				if (variadic.starts_with("...")) {
					variadic.erase(0, 3);
				}
				signature.Parameters.push_back({{"name", "..."}, {"type", variadic}});
			}

			signature.Returns = ReturnText(function->retTypes);
			into.push_back(std::move(signature));
		}

		std::vector<Signature> Signatures(const Luau::TypeId type) {
			std::vector<Signature> found;
			CollectSignatures(type, found);
			return found;
		}

		// A member found by name on an extern type or one of its parents.
		const Luau::Property *FindProperty(const Luau::ExternType *type, const std::string &name) {
			while (type != nullptr) {
				if (const auto found = type->props.find(name); found != type->props.end()) {
					return &found->second;
				}
				type = type->parent ? Luau::get<Luau::ExternType>(Luau::follow(*type->parent)) : nullptr;
			}
			return nullptr;
		}

		// The handler parameters of a signal type, read off its `Connect`. An
		// event is a property whose type has one.
		std::optional<Json> EventParameters(const Luau::TypeId type) {
			const auto *signal = Luau::get<Luau::ExternType>(Luau::follow(type));
			if (signal == nullptr) {
				return std::nullopt;
			}
			const Luau::Property *connect = FindProperty(signal, "Connect");
			if (connect == nullptr || !connect->readTy) {
				return std::nullopt;
			}
			const std::vector<Signature> connects = Signatures(*connect->readTy);
			if (connects.empty()) {
				return std::nullopt;
			}

			// The handler is Connect's one parameter; its own parameters are the
			// event's.
			const auto [arguments, tail] =
				Luau::flatten(Luau::get<Luau::FunctionType>(Luau::follow(*connect->readTy))->argTypes);
			for (const Luau::TypeId argument : arguments) {
				const std::vector<Signature> handler = Signatures(argument);
				if (!handler.empty()) {
					return handler.front().Parameters;
				}
			}
			return Json::array();
		}

		Json FunctionEntry(const std::string_view kind, const std::string &name, const Signature &signature) {
			return Json{
				{"kind", kind},
				{"name", name},
				{"parameters", signature.Parameters},
				{"returns", signature.Returns},
			};
		}

		// Members of an extern type, own only, sorted by name. Metamethods go to
		// `operators` instead.
		void DescribeExtern(
			const Luau::ExternType &type,
			const std::map<std::string, const ReflectedProperty *> &reflected,
			Json &members,
			Json &operators
		) {
			std::set<std::string> written;

			for (const auto &[name, property] : type.props) {
				if (!property.readTy) {
					continue;
				}
				const Luau::TypeId value = *property.readTy;
				const std::vector<Signature> signatures = Signatures(value);

				if (!signatures.empty()) {
					const bool method = signatures.front().Method;
					for (const Signature &signature : signatures) {
						members.push_back(FunctionEntry(method ? "method" : "callback", name, signature));
					}
				} else if (const std::optional<Json> parameters = EventParameters(value)) {
					members.push_back(
						{{"kind", "event"},
						 {"name", name},
						 {"type", TypeText(value)},
						 {"parameters", *parameters}}
					);
				} else {
					Json entry{
						{"kind", "property"},
						{"name", name},
						{"type", TypeText(value)},
						{"writable", !property.isReadOnly()},
					};
					if (const auto found = reflected.find(name); found != reflected.end()) {
						entry["writable"] = found->second->Writable && !property.isReadOnly();
						entry["valueType"] = found->second->Type;
						entry["storage"] = found->second->Kind;
					}
					members.push_back(std::move(entry));
				}
				written.insert(name);
			}

			// Reflected properties the declarations did not carry, so the dump is
			// never narrower than the class table.
			for (const auto &[name, property] : reflected) {
				if (written.contains(name)) {
					continue;
				}
				members.push_back({
					{"kind", "property"},
					{"name", name},
					{"type", property->Type},
					{"writable", property->Writable},
					{"valueType", property->Type},
					{"storage", property->Kind},
				});
			}

			std::stable_sort(members.begin(), members.end(), [](const Json &left, const Json &right) {
				return left["name"].get<std::string>() < right["name"].get<std::string>();
			});

			if (type.metatable) {
				if (const auto *table = Luau::get<Luau::TableType>(Luau::follow(*type.metatable))) {
					for (const auto &[name, property] : table->props) {
						if (!property.readTy) {
							continue;
						}
						for (const Signature &signature : Signatures(*property.readTy)) {
							Json entry = FunctionEntry("operator", name, signature);
							entry.erase("kind");
							operators.push_back(std::move(entry));
						}
					}
				}
			}
		}

		// Library members: functions with signatures and values with types.
		void DescribeTable(const Luau::TableType &table, Json &functions, Json &values) {
			for (const auto &[name, property] : table.props) {
				if (!property.readTy) {
					continue;
				}
				const std::vector<Signature> signatures = Signatures(*property.readTy);
				if (signatures.empty()) {
					values.push_back({{"name", name}, {"type", TypeText(*property.readTy)}});
					continue;
				}
				for (const Signature &signature : signatures) {
					Json entry = FunctionEntry("function", name, signature);
					entry.erase("kind");
					functions.push_back(std::move(entry));
				}
			}
		}

		bool IsEnumExtern(const std::string &name) {
			return name == "EnumItem" || name.starts_with("Enum_");
		}

		bool DerivesFrom(const Luau::ExternType *type, const std::string_view base) {
			while (type != nullptr) {
				if (type->name == base) {
					return true;
				}
				type = type->parent ? Luau::get<Luau::ExternType>(Luau::follow(*type->parent)) : nullptr;
			}
			return false;
		}

		std::string ParentName(const Luau::ExternType &type) {
			if (!type.parent) {
				return {};
			}
			// Luau roots every undeclared parent at its own `userdata`.
			const auto *parent = Luau::get<Luau::ExternType>(Luau::follow(*type.parent));
			return parent != nullptr && parent->name != "userdata" ? parent->name : std::string{};
		}

		// Arrays of scalars stay on one line; everything else opens up to the
		// member level, so a diff of the checked-in dump is one line per member.
		void Print(const Json &value, const int depth, std::string &out) {
			const auto indent = [&](const int level) { out.append(static_cast<size_t>(level), '\t'); };

			const bool scalarArray =
				value.is_array() &&
				std::all_of(value.begin(), value.end(), [](const Json &item) { return item.is_primitive(); });

			if (value.is_primitive() || scalarArray || depth >= 4 || value.empty()) {
				out += value.dump(-1, ' ', false, Json::error_handler_t::replace);
				return;
			}

			const bool object = value.is_object();
			out += object ? "{\n" : "[\n";
			size_t index = 0;
			for (auto item = value.begin(); item != value.end(); ++item, ++index) {
				indent(depth + 1);
				if (object) {
					out += Json(item.key()).dump() + ": ";
				}
				Print(item.value(), depth + 1, out);
				out += index + 1 < value.size() ? ",\n" : "\n";
			}
			indent(depth);
			out += object ? "}" : "]";
		}

	}

	Result Generate(const std::string_view luauDeclarations, const Reflection &reflection) {
		Result result;

		NoFiles files;
		StrictConfig configs;
		Luau::FrontendOptions options;
		options.runLintChecks = false;

		Luau::Frontend frontend(Luau::SolverMode::New, &files, &configs, options);
		Luau::registerBuiltinGlobals(frontend, frontend.globals);

		// Luau's own globals and types, so only what the engine adds or replaces
		// is written.
		std::unordered_map<std::string, Luau::TypeId> builtinGlobals;
		for (const auto &[symbol, binding] : frontend.globals.globalScope->bindings) {
			builtinGlobals.emplace(symbol.c_str(), binding.typeId);
		}
		std::set<std::string> builtinTypes;
		for (const auto &[name, binding] : frontend.globals.globalScope->exportedTypeBindings) {
			builtinTypes.insert(name);
		}
		for (const auto &[name, binding] : frontend.globals.globalScope->privateTypeBindings) {
			builtinTypes.insert(name);
		}

		const Luau::LoadDefinitionFileResult loaded = frontend.loadDefinitionFile(
			frontend.globals, frontend.globals.globalScope, std::string(luauDeclarations), "@engine", false
		);
		if (!loaded.success) {
			for (const Luau::ParseError &error : loaded.parseResult.errors) {
				result.Errors.push_back(
					"line " + std::to_string(error.getLocation().begin.line + 1) + ": " + error.what()
				);
			}
			if (loaded.module != nullptr) {
				for (const Luau::TypeError &error : loaded.module->errors) {
					result.Errors.push_back(
						"line " + std::to_string(error.location.begin.line + 1) + ": " + Luau::toString(error)
					);
				}
			}
			if (result.Errors.empty()) {
				result.Errors.emplace_back("the declarations did not load");
			}
			return result;
		}

		// Exported and private aliases alike, sorted so the dump is stable.
		std::map<std::string, Luau::TypeFun> types;
		// Private aliases (`type Services = ...`) stay in the definition
		// module's own scope rather than the global one.
		const Luau::ScopePtr definitionScope =
			loaded.module != nullptr ? loaded.module->getModuleScope() : nullptr;
		std::vector<const std::unordered_map<Luau::Name, Luau::TypeFun> *> sources{
			&frontend.globals.globalScope->exportedTypeBindings,
			&frontend.globals.globalScope->privateTypeBindings,
		};
		if (definitionScope != nullptr) {
			sources.push_back(&definitionScope->privateTypeBindings);
			sources.push_back(&definitionScope->exportedTypeBindings);
		}
		for (const auto *bindings : sources) {
			for (const auto &[name, binding] : *bindings) {
				if (!builtinTypes.contains(name)) {
					types.emplace(name, binding);
				}
			}
		}
		const auto &globals = frontend.globals.globalScope->bindings;

		std::map<std::string, const ReflectedClass *> reflectedClasses;
		for (const ReflectedClass &klass : reflection.Classes) {
			reflectedClasses.emplace(klass.Name, &klass);
		}

		// Own properties only: a property the parent already has is inherited.
		const auto ownProperties = [&](const ReflectedClass &klass) {
			std::map<std::string, const ReflectedProperty *> own;
			const auto parent = reflectedClasses.find(klass.Parent);
			for (const ReflectedProperty &property : klass.Properties) {
				bool inherited = false;
				if (parent != reflectedClasses.end()) {
					for (const ReflectedProperty &above : parent->second->Properties) {
						inherited = inherited || above.Name == property.Name;
					}
				}
				if (!inherited) {
					own.emplace(property.Name, &property);
				}
			}
			return own;
		};

		// Library globals by name, so a datatype can carry its constructors.
		std::map<std::string, const Luau::TableType *> libraries;
		for (const auto &[symbol, binding] : globals) {
			const std::string name = symbol.c_str();
			if (const auto builtin = builtinGlobals.find(name);
				builtin != builtinGlobals.end() && builtin->second == binding.typeId) {
				continue;
			}
			// A named table is an alias (`game: DataModel`), so a value.
			if (const auto *table = Luau::get<Luau::TableType>(Luau::follow(binding.typeId));
				table != nullptr && name != "Enum" && !table->name && !table->syntheticName) {
				libraries.emplace(name, table);
			}
		}

		Json classes = Json::array();
		Json datatypes = Json::array();
		std::set<std::string> written;

		// Classes in class-table order, so the dump reads like the tree.
		for (const ReflectedClass &klass : reflection.Classes) {
			Json members = Json::array();
			Json operators = Json::array();
			const auto own = ownProperties(klass);

			const auto found = types.find(klass.Name);
			const Luau::ExternType *declared =
				found != types.end() ? Luau::get<Luau::ExternType>(Luau::follow(found->second.type))
									 : nullptr;
			if (declared != nullptr) {
				DescribeExtern(*declared, own, members, operators);
			} else {
				DescribeExtern(
					Luau::ExternType("", {}, {}, {}, {}, nullptr, "", {}), own, members, operators
				);
			}

			classes.push_back({
				{"name", klass.Name},
				{"superclass", klass.Parent.empty() ? Json(nullptr) : Json(klass.Parent)},
				{"tags", klass.Tags},
				{"members", members},
			});
			written.insert(klass.Name);
		}

		// Then whatever the declarations add: instance classes the table does
		// not register, and every datatype.
		for (const auto &[name, binding] : types) {
			if (builtinTypes.contains(name) || written.contains(name) || IsEnumExtern(name)) {
				continue;
			}
			const auto *declared = Luau::get<Luau::ExternType>(Luau::follow(binding.type));
			if (declared == nullptr) {
				continue;
			}

			Json members = Json::array();
			Json operators = Json::array();
			DescribeExtern(*declared, {}, members, operators);
			const std::string parent = ParentName(*declared);

			if (DerivesFrom(declared, "Instance")) {
				classes.push_back({
					{"name", name},
					{"superclass", parent.empty() ? Json(nullptr) : Json(parent)},
					{"tags", Json::array({"NotCreatable", "DeclarationOnly"})},
					{"members", members},
				});
				continue;
			}

			Json entry{{"name", name}};
			if (!parent.empty()) {
				entry["superclass"] = parent;
			}
			if (FindProperty(declared, "Connect") != nullptr) {
				entry["tags"] = Json::array({"Signal"});
			}
			entry["members"] = members;
			if (!operators.empty()) {
				entry["operators"] = operators;
			}
			if (const auto library = libraries.find(name); library != libraries.end()) {
				Json constructors = Json::array();
				Json constants = Json::array();
				DescribeTable(*library->second, constructors, constants);
				entry["constructors"] = constructors;
				entry["constants"] = constants;
			}
			datatypes.push_back(std::move(entry));
		}

		Json enums = Json::array();
		std::set<std::string> reflectedEnums;
		for (const ReflectedEnum &set : reflection.Enums) {
			Json items = Json::array();
			for (size_t index = 0; index < set.Items.size(); index++) {
				items.push_back({{"name", set.Items[index]}, {"value", index}});
			}
			enums.push_back({{"name", set.Name}, {"items", items}});
			reflectedEnums.insert(set.Name);
		}

		// Enum sets only the declarations know about.
		{
			const Luau::TypeId enumGlobal = Luau::getGlobalBinding(frontend.globals, "Enum");
			if (const auto *table = Luau::get<Luau::TableType>(Luau::follow(enumGlobal))) {
				for (const auto &[name, property] : table->props) {
					if (reflectedEnums.contains(name) || !property.readTy) {
						continue;
					}
					Json items = Json::array();
					if (const auto *set = Luau::get<Luau::TableType>(Luau::follow(*property.readTy))) {
						size_t index = 0;
						for (const auto &[item, unused] : set->props) {
							items.push_back({{"name", item}, {"value", index++}});
						}
					}
					enums.push_back({{"name", name}, {"items", items}});
				}
			}
		}

		// Services: everything `GetService` answers, with the catalogue's
		// capability requirements where it has a row.
		std::map<std::string, const ReflectedService *> catalogue;
		for (const ReflectedService &service : reflection.Services) {
			catalogue.emplace(service.Name, &service);
		}
		Json services = Json::array();
		std::set<std::string> listed;
		const auto addService = [&](const std::string &name, const std::string &type) {
			if (!listed.insert(name).second) {
				return;
			}
			Json entry{{"name", name}, {"type", type}};
			if (const auto row = catalogue.find(name); row != catalogue.end()) {
				entry["capabilities"] = row->second->Capabilities;
				entry["availability"] = row->second->Availability;
				entry["languages"] = row->second->Languages;
			} else {
				entry["capabilities"] = Json::array();
				entry["availability"] = "always";
				entry["languages"] = Json::array({"luau", "javascript"});
			}
			services.push_back(std::move(entry));
		};
		if (const auto alias = types.find("Services"); alias != types.end()) {
			if (const auto *table = Luau::get<Luau::TableType>(Luau::follow(alias->second.type))) {
				for (const auto &[name, property] : table->props) {
					addService(name, property.readTy ? TypeText(*property.readTy) : name);
				}
			}
		}
		for (const ReflectedService &service : reflection.Services) {
			addService(service.Name, service.Name);
		}

		// A service the declarations type as a plain object (`RunService`) is
		// still a service.
		for (Json &datatype : datatypes) {
			for (const Json &service : services) {
				if (service["type"] == datatype["name"]) {
					if (!datatype.contains("tags")) {
						datatype["tags"] = Json::array();
					}
					datatype["tags"].push_back("Service");
					break;
				}
			}
		}

		std::map<std::string, std::vector<std::string>> globalTags;
		for (const ReflectedGlobalTag &tag : reflection.GlobalTags) {
			globalTags[tag.Name] = tag.Tags;
		}

		Json globalEntries = Json::array();
		std::vector<std::pair<std::string, Luau::TypeId>> engineGlobals;
		for (const auto &[symbol, binding] : globals) {
			const std::string name = symbol.c_str();
			if (const auto builtin = builtinGlobals.find(name);
				builtin != builtinGlobals.end() && builtin->second == binding.typeId) {
				continue;
			}
			engineGlobals.emplace_back(name, binding.typeId);
		}
		std::sort(engineGlobals.begin(), engineGlobals.end());

		std::set<std::string> datatypeNames;
		for (const Json &datatype : datatypes) {
			datatypeNames.insert(datatype["name"].get<std::string>());
		}

		for (const auto &[name, type] : engineGlobals) {
			Json entry{{"name", name}};

			if (name == "Enum") {
				entry["kind"] = "enums";
			} else if (const auto library = libraries.find(name); library != libraries.end()) {
				entry["kind"] = "library";
				if (datatypeNames.contains(name)) {
					entry["datatype"] = name;
				} else {
					Json functions = Json::array();
					Json values = Json::array();
					DescribeTable(*library->second, functions, values);
					entry["functions"] = functions;
					entry["values"] = values;
				}
			} else if (const std::vector<Signature> signatures = Signatures(type); !signatures.empty()) {
				entry["kind"] = "function";
				Json overloads = Json::array();
				for (const Signature &signature : signatures) {
					overloads.push_back(
						{{"parameters", signature.Parameters}, {"returns", signature.Returns}}
					);
				}
				entry["overloads"] = overloads;
			} else {
				entry["kind"] = "value";
				entry["type"] = TypeText(type);
			}

			if (const auto tags = globalTags.find(name); tags != globalTags.end()) {
				entry["tags"] = tags->second;
			}
			globalEntries.push_back(std::move(entry));
		}

		// Plain type aliases (`DataModel`, `ComputeRequest`), by their shape.
		Json aliases = Json::array();
		for (const auto &[name, binding] : types) {
			if (builtinTypes.contains(name) || Luau::get<Luau::ExternType>(Luau::follow(binding.type))) {
				continue;
			}
			Json entry{{"name", name}};
			if (const auto *table = Luau::get<Luau::TableType>(Luau::follow(binding.type));
				table != nullptr && binding.typeParams.empty()) {
				Json fields = Json::array();
				for (const auto &[field, property] : table->props) {
					if (property.readTy) {
						fields.push_back({{"name", field}, {"type", TypeText(*property.readTy)}});
					}
				}
				entry["fields"] = fields;
			} else {
				Luau::ToStringOptions expanded = TextOptions();
				expanded.hideTableAliasExpansions = false;
				entry["type"] = ScriptSpelling(Luau::toString(binding.type, expanded));
			}
			aliases.push_back(std::move(entry));
		}

		Json dump;
		dump["_comment"] = Json::array({
			"Generated by mono.tools/bindings. Do not edit by hand.",
			"",
			"The engine API dump: every class, datatype, enum, service and global a",
			"script can reach, with types. Reflection supplies storage, writability and",
			"capabilities; engine.d.luau, read through the Luau checker, supplies",
			"signatures, events, constructors and operators.",
		});
		dump["version"] = DUMP_VERSION;
		dump["classes"] = std::move(classes);
		dump["datatypes"] = std::move(datatypes);
		dump["enums"] = std::move(enums);
		dump["services"] = std::move(services);
		dump["globals"] = std::move(globalEntries);
		dump["types"] = std::move(aliases);

		Print(dump, 0, result.Json);
		result.Json += "\n";
		result.Ok = true;
		return result;
	}

}
