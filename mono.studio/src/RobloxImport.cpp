#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Values.hpp>
#include <engine/gui/Services.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/spatial/CollisionGroups.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Prompts.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <map>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <studio/Complete.hpp>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/RobloxImport.hpp>
#include <studio/RojoSync.hpp>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace studio {

	namespace {
		using engine::bake::RobloxValueKind;
		using engine::ecs::PropertyType;

		const char *Describe(RobloxValueKind kind) {
			switch (kind) {
			case RobloxValueKind::Bool:
				return "Bool";
			case RobloxValueKind::Integer:
				return "Integer";
			case RobloxValueKind::Number:
				return "Number";
			case RobloxValueKind::Text:
				return "Text";
			case RobloxValueKind::Vector3:
				return "Vector3";
			case RobloxValueKind::Vector2:
				return "Vector2";
			case RobloxValueKind::Color3:
				return "Color3";
			case RobloxValueKind::CFrame:
				return "CFrame";
			case RobloxValueKind::UDim:
				return "UDim";
			case RobloxValueKind::UDim2:
				return "UDim2";
			case RobloxValueKind::Rect:
				return "Rect";
			case RobloxValueKind::NumberRange:
				return "NumberRange";
			}
			return "Unknown";
		}

		bool Compatible(PropertyType property, RobloxValueKind value) {
			switch (property) {
			case PropertyType::Bool:
				return value == RobloxValueKind::Bool;
			case PropertyType::Int32:
			case PropertyType::Int64:
			case PropertyType::Float:
			case PropertyType::Double:
				return value == RobloxValueKind::Integer || value == RobloxValueKind::Number;
			case PropertyType::Name:
			case PropertyType::String:
			case PropertyType::Enum:
				return value == RobloxValueKind::Text;
			case PropertyType::Vector3:
				return value == RobloxValueKind::Vector3;
			case PropertyType::Color3:
				return value == RobloxValueKind::Color3;
			case PropertyType::CFrame:
				return value == RobloxValueKind::CFrame;
			case PropertyType::Vector2:
				return value == RobloxValueKind::Vector2;
			case PropertyType::UDim:
				return value == RobloxValueKind::UDim;
			case PropertyType::UDim2:
				return value == RobloxValueKind::UDim2;
			case PropertyType::Rect:
				return value == RobloxValueKind::Rect;
			case PropertyType::NumberRange:
				return value == RobloxValueKind::NumberRange;
			case PropertyType::Reference:
			case PropertyType::NumberSequence:
			case PropertyType::ColorSequence:
			case PropertyType::Opaque:
				return false;
			}
			return false;
		}

		const engine::ecs::PropertyDescriptor *
		RobloxPropertyNamed(const engine::ecs::ClassInfo &info, std::string_view name) {
			const auto found =
				std::find_if(info.Properties.begin(), info.Properties.end(), [&](const auto &property) {
					return property.Spelling == name;
				});
			return found == info.Properties.end() ? nullptr : &*found;
		}

		using GapKey = std::tuple<std::string, std::string, std::string, std::string>;
		using SkipKey = std::tuple<std::string, std::string, std::string>;
		using ClassCounts = std::map<std::string, size_t, std::less<>>;

		void AddGap(std::map<GapKey, size_t> &gaps, const GapKey &key) {
			const auto [found, inserted] = gaps.try_emplace(key, 0);
			found->second++;
		}

		std::vector<RobloxPropertyGap> FinishGaps(const std::map<GapKey, size_t> &gaps) {
			std::vector<RobloxPropertyGap> out;
			out.reserve(gaps.size());
			for (const auto &[key, count] : gaps) {
				const auto &[className, propertyName, sourceType, expectedType] = key;
				out.push_back({className, propertyName, sourceType, expectedType, count});
			}
			return out;
		}

		std::vector<RobloxClassGap> FinishClassCounts(const ClassCounts &counts) {
			std::vector<RobloxClassGap> out;
			out.reserve(counts.size());
			for (const auto &[className, count] : counts) {
				out.push_back({className, count});
			}
			return out;
		}

		std::vector<RobloxPropertySkip> FinishSkippedProperties(const std::map<SkipKey, size_t> &skips) {
			std::vector<RobloxPropertySkip> out;
			out.reserve(skips.size());
			for (const auto &[key, count] : skips) {
				const auto &[className, propertyName, reason] = key;
				out.push_back({className, propertyName, reason, count});
			}
			return out;
		}

		std::string Lowercase(std::string_view text) {
			std::string lowered(text);
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return lowered;
		}

		bool ReadBytes(const std::filesystem::path &path, std::vector<std::byte> &out, std::string &error) {
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if (!input) {
				error = "could not open " + path.string();
				return false;
			}
			const std::streampos length = input.tellg();
			if (length < 0) {
				error = "could not measure " + path.string();
				return false;
			}
			input.seekg(0);
			out.resize(static_cast<size_t>(length));
			if (!out.empty() && !input.read(reinterpret_cast<char *>(out.data()), length)) {
				error = "could not read " + path.string();
				return false;
			}
			error.clear();
			return true;
		}

		bool ContainsFolded(std::string_view text, std::string_view filter) {
			if (filter.empty()) {
				return true;
			}
			return Lowercase(text).find(Lowercase(filter)) != std::string::npos;
		}

		using Replacement = std::pair<std::string, std::string>;
		using ReplacementTable = std::unordered_map<std::string, std::vector<Replacement>>;
		using ClassTable = std::unordered_map<std::string, engine::ecs::ClassId>;

		ClassTable
		ResolveClassMappings(const RobloxClassMappings &mappings, std::vector<std::string> *notes = nullptr) {
			ClassTable resolved;
			std::unordered_set<uint32_t> insertable;
			for (const engine::ecs::ClassId id : InsertableClasses()) {
				insertable.insert(id.Index);
			}
			for (const auto &[source, target] : mappings) {
				const engine::ecs::ClassId id = engine::ecs::Classes::Find(engine::core::Name(target));
				if (source.empty() || !id.IsValid() || !insertable.contains(id.Index)) {
					if (notes != nullptr && !source.empty()) {
						notes->push_back(source + " has an invalid engine class mapping to " + target);
					}
					continue;
				}
				resolved.emplace(source, id);
			}
			return resolved;
		}

		engine::ecs::ClassId ResolveClass(const ClassTable &mappings, std::string_view source) {
			const engine::ecs::ClassId native =
				engine::ecs::Classes::Find(engine::core::Name(std::string(source)));
			if (native.IsValid()) {
				return native;
			}
			const auto mapped = mappings.find(std::string(source));
			return mapped == mappings.end() ? FolderClass() : mapped->second;
		}

		bool UsesFolderFallback(const ClassTable &mappings, std::string_view source) {
			const engine::ecs::ClassId native =
				engine::ecs::Classes::Find(engine::core::Name(std::string(source)));
			return !native.IsValid() && !mappings.contains(std::string(source));
		}

		std::string ReplacementKey(std::string_view path, std::string_view property) {
			return std::string(path) + '\n' + std::string(property);
		}

		std::string ApplyAssetMappings(
			const ReplacementTable &replacements,
			std::string_view path,
			std::string_view property,
			std::string text
		) {
			const auto found = replacements.find(ReplacementKey(path, property));
			if (found == replacements.end()) {
				return text;
			}
			for (const auto &[source, local] : found->second) {
				if (source.empty() || local.empty()) {
					continue;
				}
				size_t at = 0;
				while ((at = text.find(source, at)) != std::string::npos) {
					text.replace(at, source.size(), local);
					at += local.size();
				}
			}
			return text;
		}

		bool ToGameValue(
			const engine::ecs::PropertyDescriptor &property,
			const engine::bake::RobloxValue &source,
			engine::game::PropertyValue &out
		) {
			using engine::bake::RobloxValueKind;
			using engine::ecs::PropertyType;
			if (!Compatible(property.Type, source.Kind())) {
				return false;
			}

			out = engine::game::PropertyValue{};
			out.Type = property.Type;
			switch (property.Type) {
			case PropertyType::Bool:
				out.Bool = source.As<bool>();
				return true;
			case PropertyType::Int32:
			case PropertyType::Int64:
			case PropertyType::Float:
			case PropertyType::Double: {
				const double number = source.Kind() == RobloxValueKind::Integer
										  ? static_cast<double>(source.As<int64_t>())
										  : source.As<double>();
				out.Int32 = static_cast<int32_t>(number);
				out.Int64 = static_cast<int64_t>(number);
				out.Float = static_cast<float>(number);
				out.Double = number;
				return true;
			}
			case PropertyType::String:
				out.String = source.As<std::string>();
				return true;
			case PropertyType::Name:
			case PropertyType::Enum:
				out.Name = engine::core::Name(source.As<std::string>());
				return property.Type != PropertyType::Enum ||
					   engine::ecs::EnumTable::Has(property.EnumName, out.Name);
			case PropertyType::Vector3:
				out.Vector3 = source.As<engine::core::Vector3>();
				return true;
			case PropertyType::Color3:
				out.Color3 = source.As<engine::core::Color3>();
				return true;
			case PropertyType::CFrame:
				out.CFrame = source.As<engine::core::CFrame>();
				return true;
			case PropertyType::Vector2:
				out.Vector2 = source.As<engine::core::Vector2>();
				return true;
			case PropertyType::UDim:
				out.UDim = source.As<engine::core::UDim>();
				return true;
			case PropertyType::UDim2:
				out.UDim2 = source.As<engine::core::UDim2>();
				return true;
			case PropertyType::Rect:
				out.Rect = source.As<engine::core::Rect>();
				return true;
			case PropertyType::NumberRange:
				out.NumberRange = source.As<engine::core::NumberRange>();
				return true;
			case PropertyType::Reference:
			case PropertyType::NumberSequence:
			case PropertyType::ColorSequence:
			case PropertyType::Opaque:
				return false;
			}
			return false;
		}

		const engine::ecs::PropertyDescriptor *RobloxPropertyNamed(
			const engine::ecs::Store &store, engine::ecs::Entity instance, std::string_view name
		) {
			const std::span<const engine::ecs::PropertyDescriptor> properties = store.PropertiesOf(instance);
			const auto found = std::find_if(properties.begin(), properties.end(), [&](const auto &property) {
				return property.Spelling == name;
			});
			return found == properties.end() ? nullptr : &*found;
		}

		struct BuildState {
			engine::ecs::Store &Store;
			const ReplacementTable &Replacements;
			const ClassTable &Classes;
			RobloxImportResult &Report;
			const RobloxImportOptions &Options;
			std::unordered_set<std::string> SourceKeys;
			ClassCounts FolderFallbackClasses;
			std::map<SkipKey, size_t> SkippedProperties;
			std::string Error;
		};

		void SkipProperty(
			BuildState &state,
			const engine::bake::RobloxInstance &instance,
			const engine::bake::RobloxProperty &property,
			std::string_view reason
		) {
			state.SkippedProperties[{instance.ClassName, property.Name, std::string(reason)}]++;
		}

		std::string UniqueSourceKey(BuildState &state, const std::string &path) {
			if (state.SourceKeys.insert(path).second) {
				return path;
			}
			for (size_t suffix = 2; suffix < 1'000'000; suffix++) {
				const std::string candidate = path + " (" + std::to_string(suffix) + ")";
				if (state.SourceKeys.insert(candidate).second) {
					return candidate;
				}
			}
			return {};
		}

		bool StageSource(engine::ecs::Store &store, std::string_view key, std::string source) {
			auto *cache = store.ResourceMutable<engine::script::SourceCache>();
			if (cache == nullptr) {
				store.SetResource(engine::script::SourceCache{});
				cache = store.ResourceMutable<engine::script::SourceCache>();
			}
			if (cache == nullptr) {
				return false;
			}
			cache->Set(engine::core::Name(std::string(key)), std::move(source));
			return true;
		}

		engine::ecs::Entity BuildRobloxInstance(
			BuildState &state,
			const engine::bake::RobloxInstance &node,
			const std::string &path,
			engine::ecs::Entity reuse = engine::ecs::NULL_ENTITY
		) {
			using engine::ecs::Entity;
			using engine::ecs::NULL_ENTITY;

			const bool script = node.ClassName == "Script" || node.ClassName == "LocalScript" ||
								node.ClassName == "ModuleScript";
			const engine::bake::RobloxProperty *sourceProperty = nullptr;
			if (script) {
				const auto found =
					std::find_if(node.Properties.begin(), node.Properties.end(), [](const auto &property) {
						return property.Name == "Source" &&
							   property.Value.Kind() == engine::bake::RobloxValueKind::Text;
					});
				if (found != node.Properties.end()) {
					sourceProperty = &*found;
				}
			}

			Entity instance = reuse;
			if (instance == NULL_ENTITY && sourceProperty != nullptr) {
				const std::string sourceKey = UniqueSourceKey(state, path);
				std::string source = ApplyAssetMappings(
					state.Replacements, path, sourceProperty->Name, sourceProperty->Value.As<std::string>()
				);
				if (!sourceKey.empty() && StageSource(state.Store, sourceKey, std::move(source))) {
					instance = node.ClassName == "ModuleScript"
								   ? engine::script::MakeModule(state.Store, sourceKey, node.Name)
								   : engine::script::MakeScript(
										 state.Store, sourceKey, node.Name, node.ClassName == "LocalScript"
									 );
					state.Report.Scripts++;
				}
			}

			if (instance == NULL_ENTITY) {
				const bool fallback = UsesFolderFallback(state.Classes, node.ClassName);
				const engine::ecs::ClassId classId = ResolveClass(state.Classes, node.ClassName);
				instance = state.Store.CreateInstance(classId, node.Name);
				if (instance != NULL_ENTITY && fallback) {
					state.FolderFallbackClasses[node.ClassName]++;
				}
			}
			if (instance == NULL_ENTITY) {
				state.Error = "the world refused instance " + path;
				return NULL_ENTITY;
			}
			state.Report.Instances++;

			for (const engine::bake::RobloxProperty &property : node.Properties) {
				if (&property == sourceProperty) {
					continue;
				}
				const engine::ecs::PropertyDescriptor *descriptor =
					RobloxPropertyNamed(state.Store, instance, property.Name);
				if (descriptor == nullptr) {
					SkipProperty(state, node, property, "no matching engine property");
					continue;
				}

				engine::bake::RobloxValue mapped = property.Value;
				if (mapped.Kind() == engine::bake::RobloxValueKind::Text) {
					mapped.Set(
						ApplyAssetMappings(state.Replacements, path, property.Name, mapped.As<std::string>())
					);
				}
				if (descriptor->Spelling == "CollisionGroup" &&
					mapped.Kind() == engine::bake::RobloxValueKind::Text) {
					// Roblox stores the group name on each part. The computed engine
					// property rightly refuses unknown names, so declare the imported
					// name before asking that property to apply it.
					const std::string &group = mapped.As<std::string>();
					if (group.empty() ||
						engine::spatial::CollisionGroups::Register(group) == engine::spatial::NO_GROUP) {
						SkipProperty(state, node, property, "collision group is empty or unavailable");
						continue;
					}
				}
				engine::game::PropertyValue value;
				if (!Compatible(descriptor->Type, mapped.Kind())) {
					SkipProperty(state, node, property, "source value type is incompatible");
					continue;
				}
				if (!ToGameValue(*descriptor, mapped, value)) {
					const char *reason = descriptor->Type == PropertyType::Enum
											 ? "engine enum does not contain value"
											 : "source value cannot be represented";
					SkipProperty(state, node, property, reason);
					continue;
				}
				if (!engine::game::WriteAuthoredProperty(state.Store, instance, *descriptor, value)) {
					SkipProperty(state, node, property, "engine property rejected value");
					continue;
				}
				state.Report.Properties++;
			}
			if (state.Options.DisableScripts &&
				(node.ClassName == "Script" || node.ClassName == "LocalScript")) {
				state.Store.Set(instance, engine::script::Disabled{});
				state.Report.DisabledScripts++;
			}

			for (const engine::bake::RobloxInstance &child : node.Children) {
				const std::string childPath = path + "/" + child.Name;
				const Entity built = BuildRobloxInstance(state, child, childPath);
				if (built != NULL_ENTITY) {
					state.Store.SetParent(built, instance);
				}
			}
			return instance;
		}

		struct RojoTrace {
			std::vector<const engine::bake::RobloxInstance *> Nodes;
			bool Ambiguous = false;
		};

		struct RobloxRojoPlan {
			std::vector<RobloxRojoSubject> Subjects;
			std::vector<RojoTrace> Traces;
		};

		bool IsRojoServiceRoot(std::string_view className) {
			if (className.size() >= 7 && className.substr(className.size() - 7) == "Service") {
				return true;
			}
			constexpr std::array<std::string_view, 10> ROOTS{
				"Workspace",
				"ReplicatedFirst",
				"ReplicatedStorage",
				"ServerStorage",
				"StarterGui",
				"StarterPack",
				"StarterPlayer",
				"Teams",
				"Lighting",
				"Chat",
			};
			return std::find(ROOTS.begin(), ROOTS.end(), className) != ROOTS.end();
		}

		bool IsSimpleRojoContainer(std::string_view className) {
			return className == "Folder" || className == "StarterPlayerScripts" ||
				   className == "StarterCharacterScripts" || className == "PlayerScripts";
		}

		bool IsPortableRojoName(std::string_view name) {
			if (name.empty() || name == "." || name == ".." || name.front() == '.' || name.front() == '$' ||
				name.back() == '.' || name.back() == ' ') {
				return false;
			}
			for (const unsigned char character : name) {
				if (character < 32 ||
					std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos) {
					return false;
				}
			}

			std::string reserved = Lowercase(name.substr(0, name.find('.')));
			if (reserved == "con" || reserved == "prn" || reserved == "aux" || reserved == "nul") {
				return false;
			}
			return !(
				reserved.size() == 4 && (reserved.substr(0, 3) == "com" || reserved.substr(0, 3) == "lpt") &&
				reserved[3] >= '1' && reserved[3] <= '9'
			);
		}

		const char *RojoScriptSuffix(std::string_view className) {
			if (className == "Script") {
				return ".server.luau";
			}
			if (className == "LocalScript") {
				return ".client.luau";
			}
			return className == "ModuleScript" ? ".luau" : nullptr;
		}

		void Invalidate(RobloxRojoSubject &subject, std::string reason) {
			if (!subject.Valid) {
				return;
			}
			subject.Valid = false;
			subject.Reason = std::move(reason);
		}

		RobloxRojoPlan BuildRobloxRojoPlan(const engine::bake::RobloxModel &model) {
			std::unordered_map<std::string, std::vector<RojoTrace>> scriptNodes;
			std::vector<const engine::bake::RobloxInstance *> ancestry;

			const auto collect =
				[&](const auto &self, const engine::bake::RobloxInstance &node, bool ambiguous) -> void {
				ancestry.push_back(&node);
				if (RojoScriptSuffix(node.ClassName) != nullptr) {
					std::string path;
					for (const engine::bake::RobloxInstance *part : ancestry) {
						if (!path.empty()) {
							path += '/';
						}
						path += part->Name;
					}
					scriptNodes[path].push_back(RojoTrace{ancestry, ambiguous});
				}

				if (node.Children.size() == 1) {
					self(self, node.Children.front(), ambiguous);
				} else if (!node.Children.empty()) {
					std::unordered_map<std::string, size_t> childNames;
					for (const engine::bake::RobloxInstance &child : node.Children) {
						childNames[child.Name]++;
					}
					for (const engine::bake::RobloxInstance &child : node.Children) {
						self(self, child, ambiguous || childNames[child.Name] > 1);
					}
				}
				ancestry.pop_back();
			};

			std::unordered_map<std::string, size_t> rootNames;
			if (model.Roots.size() > 1) {
				for (const engine::bake::RobloxInstance &root : model.Roots) {
					rootNames[root.Name]++;
				}
			}
			for (const engine::bake::RobloxInstance &root : model.Roots) {
				collect(collect, root, model.Roots.size() > 1 && rootNames[root.Name] > 1);
			}

			RobloxRojoPlan plan;
			plan.Subjects.reserve(model.Scripts.size());
			plan.Traces.resize(model.Scripts.size());
			for (size_t index = 0; index < model.Scripts.size(); index++) {
				const engine::bake::RobloxScript &script = model.Scripts[index];
				RobloxRojoSubject subject;
				subject.InstancePath = script.InstancePath;
				subject.ClassName = script.ClassName;

				const auto found = scriptNodes.find(script.InstancePath);
				if (found == scriptNodes.end() || found->second.empty()) {
					subject.Reason = "not attached to a decoded script instance";
					plan.Subjects.push_back(std::move(subject));
					continue;
				}
				if (found->second.size() != 1 || found->second.front().Ambiguous) {
					subject.Reason = "duplicate instance names make this hierarchy ambiguous";
					plan.Subjects.push_back(std::move(subject));
					continue;
				}

				const RojoTrace &trace = found->second.front();
				plan.Traces[index] = trace;
				if (trace.Nodes.back()->ClassName != script.ClassName) {
					subject.Reason = "decoded script class does not match its hierarchy";
					plan.Subjects.push_back(std::move(subject));
					continue;
				}
				if (trace.Nodes.size() < 2 || !IsRojoServiceRoot(trace.Nodes.front()->ClassName)) {
					subject.Reason = "not under a supported Roblox service root";
					plan.Subjects.push_back(std::move(subject));
					continue;
				}

				bool simple = true;
				for (size_t depth = 1; depth + 1 < trace.Nodes.size(); depth++) {
					if (!IsSimpleRojoContainer(trace.Nodes[depth]->ClassName)) {
						subject.Reason =
							"parent " + trace.Nodes[depth]->ClassName + " needs a complex Rojo hierarchy";
						simple = false;
						break;
					}
				}
				if (!simple) {
					plan.Subjects.push_back(std::move(subject));
					continue;
				}

				for (const engine::bake::RobloxInstance *part : trace.Nodes) {
					if (!IsPortableRojoName(part->Name)) {
						subject.Reason = part->Name + " is not a safe Rojo file name";
						simple = false;
						break;
					}
				}
				const std::string foldedName = Lowercase(trace.Nodes.back()->Name);
				if (simple && foldedName == "init") {
					subject.Reason = "init would turn its parent into the script";
					simple = false;
				}
				if (simple && script.ClassName == "ModuleScript" &&
					(foldedName.ends_with(".server") || foldedName.ends_with(".client"))) {
					subject.Reason = "the module name would change its Rojo script class";
					simple = false;
				}
				if (!simple) {
					plan.Subjects.push_back(std::move(subject));
					continue;
				}

				std::filesystem::path sourcePath("src");
				for (size_t depth = 0; depth + 1 < trace.Nodes.size(); depth++) {
					sourcePath /= trace.Nodes[depth]->Name;
				}
				sourcePath /= trace.Nodes.back()->Name + std::string(RojoScriptSuffix(script.ClassName));
				subject.SourcePath = std::move(sourcePath);
				subject.Valid = true;
				plan.Subjects.push_back(std::move(subject));
			}

			std::unordered_map<std::string, std::vector<size_t>> files;
			std::unordered_map<std::string, std::vector<size_t>> directories;
			std::unordered_map<std::string, std::string> directorySpellings;
			std::unordered_set<std::string> directoryCollisions;
			for (size_t index = 0; index < plan.Subjects.size(); index++) {
				const RobloxRojoSubject &subject = plan.Subjects[index];
				if (!subject.Valid) {
					continue;
				}
				files[Lowercase(subject.SourcePath.generic_string())].push_back(index);
				for (std::filesystem::path directory = subject.SourcePath.parent_path(); !directory.empty();
					 directory = directory.parent_path()) {
					const std::string spelling = directory.generic_string();
					const std::string key = Lowercase(spelling);
					directories[key].push_back(index);
					const auto [known, inserted] = directorySpellings.try_emplace(key, spelling);
					if (!inserted && known->second != spelling) {
						directoryCollisions.insert(key);
					}
					if (directoryCollisions.contains(key)) {
						for (const size_t conflict : directories[key]) {
							Invalidate(plan.Subjects[conflict], "case-only directory names collide on disk");
						}
					}
				}
			}
			for (const auto &[path, conflicts] : files) {
				if (conflicts.size() > 1) {
					for (const size_t conflict : conflicts) {
						Invalidate(plan.Subjects[conflict], "two scripts map to the same Rojo file");
					}
				}
				if (const auto directory = directories.find(path); directory != directories.end()) {
					for (const size_t conflict : conflicts) {
						Invalidate(plan.Subjects[conflict], "a script file collides with a Rojo directory");
					}
					for (const size_t conflict : directory->second) {
						Invalidate(plan.Subjects[conflict], "a Rojo directory collides with a script file");
					}
				}
			}
			return plan;
		}

		nlohmann::json RojoProjectDocument(const RobloxRojoPlan &plan, std::string_view projectName) {
			nlohmann::json document{
				{"name", projectName.empty() ? "RobloxPlace" : std::string(projectName)},
				{"tree", nlohmann::json{{"$className", "DataModel"}}},
			};
			for (size_t index = 0; index < plan.Subjects.size(); index++) {
				const RobloxRojoSubject &subject = plan.Subjects[index];
				if (!subject.Valid) {
					continue;
				}
				nlohmann::json *node = &document["tree"];
				for (const engine::bake::RobloxInstance *part : plan.Traces[index].Nodes) {
					nlohmann::json &child = (*node)[part->Name];
					if (!child.is_object()) {
						child = nlohmann::json::object();
					}
					child["$className"] = part->ClassName;
					node = &child;
				}
				(*node)["$path"] = subject.SourcePath.generic_string();
			}
			return document;
		}
	}

	RobloxImportAnalysis
	AnalyzeRobloxImport(const engine::bake::RobloxModel &model, const RobloxClassMappings &classMappings) {
		engine::game::RegisterGameClasses();
		engine::effects::RegisterEffectClasses();
		(void)FolderClass();
		const ClassTable mappedClasses = ResolveClassMappings(classMappings);
		ClassCounts classCounts;
		std::map<GapKey, size_t> missingProperties;
		std::map<GapKey, size_t> conflictingProperties;

		const auto visit = [&](const auto &self, const engine::bake::RobloxInstance &instance) -> void {
			classCounts[instance.ClassName]++;
			const engine::ecs::ClassId classId = ResolveClass(mappedClasses, instance.ClassName);
			if (classId.IsValid()) {
				const engine::ecs::ClassInfo &info = engine::ecs::Classes::Describe(classId);
				for (const engine::bake::RobloxProperty &property : instance.Properties) {
					const engine::ecs::PropertyDescriptor *descriptor =
						RobloxPropertyNamed(info, property.Name);
					if (descriptor == nullptr) {
						AddGap(
							missingProperties,
							{instance.ClassName, property.Name, Describe(property.Value.Kind()), {}}
						);
					} else if (!Compatible(descriptor->Type, property.Value.Kind())) {
						AddGap(
							conflictingProperties,
							{
								instance.ClassName,
								property.Name,
								Describe(property.Value.Kind()),
								engine::ecs::Describe(descriptor->Type),
							}
						);
					}
				}
			}

			for (const engine::bake::RobloxInstance &child : instance.Children) {
				self(self, child);
			}
		};

		for (const engine::bake::RobloxInstance &root : model.Roots) {
			visit(visit, root);
		}

		RobloxImportAnalysis analysis;
		analysis.Classes = classCounts.size();
		for (const auto &[className, count] : classCounts) {
			analysis.Instances += count;
			if (!engine::ecs::Classes::Find(engine::core::Name(className)).IsValid() &&
				!mappedClasses.contains(className)) {
				analysis.MissingClasses.push_back({className, count});
			}
		}
		analysis.MissingProperties = FinishGaps(missingProperties);
		analysis.ConflictingProperties = FinishGaps(conflictingProperties);
		return analysis;
	}

	std::vector<RobloxAssetChoice>
	RobloxAssetChoices(const engine::bake::RobloxModel &model, const RobloxAssetMappings &mappings) {
		std::map<std::string, RobloxAssetChoice, std::less<>> grouped;
		for (const engine::bake::RobloxAssetReference &asset : model.Assets) {
			auto [found, inserted] = grouped.try_emplace(asset.Identifier);
			RobloxAssetChoice &choice = found->second;
			if (inserted) {
				choice.Identifier = asset.Identifier;
				choice.SourceUri = asset.Uri;
				choice.Kind = asset.Kind;
				if (const auto mapping = mappings.find(asset.Identifier); mapping != mappings.end()) {
					choice.LocalAsset = mapping->second;
				}
			} else if (choice.Kind == engine::bake::RobloxAssetKind::Unknown) {
				choice.Kind = asset.Kind;
			}
			choice.Uses++;
		}

		std::vector<RobloxAssetChoice> out;
		out.reserve(grouped.size());
		for (auto &[identifier, choice] : grouped) {
			out.push_back(std::move(choice));
		}
		return out;
	}

	std::vector<RobloxRojoSubject> RobloxRojoSubjects(const engine::bake::RobloxModel &model) {
		return BuildRobloxRojoPlan(model).Subjects;
	}

	bool SetupRobloxRojoProject(
		const engine::bake::RobloxModel &model,
		const std::filesystem::path &destination,
		std::string_view projectName,
		RobloxRojoSetupResult &out,
		std::string &error
	) {
		RobloxRojoPlan plan = BuildRobloxRojoPlan(model);
		const size_t ready = static_cast<size_t>(
			std::count_if(plan.Subjects.begin(), plan.Subjects.end(), [](const RobloxRojoSubject &subject) {
				return subject.Valid;
			})
		);
		if (ready == 0) {
			error = "no recovered script has a simple Rojo hierarchy";
			return false;
		}

		std::error_code filesystemError;
		const bool destinationExists = std::filesystem::exists(destination, filesystemError);
		if (filesystemError) {
			error = "could not inspect " + destination.string() + ": " + filesystemError.message();
			return false;
		}
		if (destinationExists) {
			error = destination.string() + " already exists; no files were overwritten";
			return false;
		}
		const std::filesystem::path parent = destination.has_parent_path()
												 ? destination.parent_path()
												 : std::filesystem::current_path(filesystemError);
		const bool parentIsDirectory =
			!filesystemError && std::filesystem::is_directory(parent, filesystemError);
		if (filesystemError || !parentIsDirectory) {
			error = "Rojo project parent is not a directory: " + parent.string();
			return false;
		}

		std::filesystem::path staging = destination;
		staging += ".atomic-setup";
		const bool stagingExists = std::filesystem::exists(staging, filesystemError);
		if (filesystemError) {
			error = "could not inspect " + staging.string() + ": " + filesystemError.message();
			return false;
		}
		if (stagingExists) {
			error = staging.string() + " already exists; remove the stale setup folder first";
			return false;
		}
		if (!std::filesystem::create_directories(staging, filesystemError) || filesystemError) {
			error = "could not create " + staging.string();
			return false;
		}

		const auto fail = [&](std::string reason) {
			error = std::move(reason);
			std::error_code ignored;
			std::filesystem::remove_all(staging, ignored);
			return false;
		};

		for (size_t index = 0; index < plan.Subjects.size(); index++) {
			const RobloxRojoSubject &subject = plan.Subjects[index];
			if (!subject.Valid) {
				continue;
			}
			const std::filesystem::path target = staging / subject.SourcePath;
			std::filesystem::create_directories(target.parent_path(), filesystemError);
			if (filesystemError) {
				return fail("could not create " + target.parent_path().string());
			}
			std::ofstream source(target, std::ios::binary | std::ios::trunc);
			if (source) {
				source.write(
					model.Scripts[index].Source.data(),
					static_cast<std::streamsize>(model.Scripts[index].Source.size())
				);
			}
			source.close();
			if (!source) {
				return fail("could not write " + target.string());
			}
		}

		const std::filesystem::path stagedProject = staging / "default.project.json";
		std::ofstream project(stagedProject, std::ios::binary | std::ios::trunc);
		if (project) {
			project << RojoProjectDocument(plan, projectName).dump(2) << '\n';
		}
		project.close();
		if (!project) {
			return fail("could not write " + stagedProject.string());
		}

		std::filesystem::rename(staging, destination, filesystemError);
		if (filesystemError) {
			return fail("could not finish " + destination.string() + ": " + filesystemError.message());
		}

		RobloxRojoSetupResult candidate;
		candidate.ProjectFile = destination / "default.project.json";
		candidate.ScriptsWritten = ready;
		candidate.Subjects = std::move(plan.Subjects);
		out = std::move(candidate);
		error.clear();
		return true;
	}

	bool ImportRobloxPlace(
		engine::ecs::Store &store,
		const engine::bake::RobloxModel &model,
		const RobloxAssetMappings &assetMappings,
		const RobloxClassMappings &classMappings,
		RobloxImportResult &out,
		std::string &error,
		const RobloxImportOptions &options
	) {
		if (model.Roots.empty()) {
			error = "the Roblox file has no root instances";
			return false;
		}
		engine::script::RegisterScriptComponents();
		engine::game::RegisterGameClasses();
		engine::effects::RegisterEffectClasses();
		// Service roots are non-creatable fixtures. Install them before walking
		// source roots so a Roblox `GuiService` or `ChangeHistoryService` is
		// reused instead of being refused as a new instance.
		(void)engine::scene::InstallServices(store);
		(void)engine::gui::InstallGuiServices(store);
		(void)FolderClass();
		RobloxImportResult candidate;
		const ClassTable mappedClasses = ResolveClassMappings(classMappings, &candidate.Notes);

		ReplacementTable replacements;
		for (const engine::bake::RobloxAssetReference &asset : model.Assets) {
			const auto mapped = assetMappings.find(asset.Identifier);
			if (mapped == assetMappings.end() || mapped->second.empty()) {
				continue;
			}
			replacements[ReplacementKey(asset.InstancePath, asset.PropertyName)].emplace_back(
				asset.Uri, mapped->second
			);
		}

		BuildState state{store, replacements, mappedClasses, candidate, options, {}, {}, {}, {}};
		for (const engine::bake::RobloxInstance &root : model.Roots) {
			const engine::ecs::Entity existing = store.FindFirstRoot(root.Name);
			if (existing != engine::ecs::NULL_ENTITY) {
				candidate.ReusedRoots++;
				const engine::ecs::ClassId existingClass = store.ClassOf(existing);
				const engine::ecs::ClassId wantedClass = ResolveClass(mappedClasses, root.ClassName);
				if (wantedClass.IsValid() && existingClass != wantedClass) {
					candidate.Notes.push_back(
						root.Name + " reused an existing root of a different engine class"
					);
				}
			}
			BuildRobloxInstance(state, root, root.Name, existing);
			if (!state.Error.empty()) {
				error = state.Error;
				return false;
			}
		}
		candidate.FolderFallbackClasses = FinishClassCounts(state.FolderFallbackClasses);
		candidate.SkippedProperties = FinishSkippedProperties(state.SkippedProperties);

		out = std::move(candidate);
		error.clear();
		return true;
	}

	bool PortRobloxPlace(
		const std::filesystem::path &source,
		const std::filesystem::path &destination,
		const RobloxAssetMappings &assetMappings,
		const RobloxClassMappings &classMappings,
		RobloxWorldPortResult &out,
		std::string &error,
		const RobloxImportOptions &options
	) {
		std::vector<std::byte> bytes;
		if (!ReadBytes(source, bytes, error)) {
			return false;
		}

		engine::bake::RobloxModel model;
		if (!engine::bake::ReadRobloxFile(bytes, model, error)) {
			return false;
		}

		RobloxWorldPortResult candidate;
		candidate.Analysis = AnalyzeRobloxImport(model, classMappings);

		engine::world::Universe universe;
		const std::string stem = destination.stem().string();
		engine::world::WorldSettings settings;
		settings.Name = engine::core::Name(stem.empty() ? "RobloxPort" : stem);
		const engine::world::WorldId world = universe.Create(settings);
		if (!world.IsValid()) {
			error = "could not create the destination world";
			return false;
		}

		const engine::world::WorldStatus status = universe.Enter(world, [&](engine::ecs::Store &store) {
			engine::scene::InstallServices(store);
			ImportRobloxPlace(store, model, assetMappings, classMappings, candidate.Import, error, options);
		});
		if (status != engine::world::WorldStatus::Ok) {
			error = "the destination world became unavailable";
			return false;
		}
		if (!error.empty()) {
			return false;
		}
		if (!engine::game::ExportWorld(universe, world, destination, error)) {
			return false;
		}
		engine::world::Universe validation;
		if (!engine::game::ImportWorld(validation, destination, engine::core::Name{}, error).IsValid()) {
			error = "the written world did not round-trip: " + error;
			return false;
		}

		out = std::move(candidate);
		error.clear();
		return true;
	}

	bool LoadRobloxAssetMappings(RobloxAssetMappings &out, std::string &error) {
		nlohmann::json document;
		if (!ReadConfigDocument("roblox-assets.json", document, error)) {
			if (error.empty()) {
				out.clear();
				return true;
			}
			return false;
		}

		const auto mappings = document.find("mappings");
		if (mappings == document.end() || !mappings->is_object()) {
			error = ConfigPath("roblox-assets.json").string() + " has no mappings object";
			return false;
		}

		RobloxAssetMappings candidate;
		for (const auto &[identifier, value] : mappings->items()) {
			if (!identifier.empty() && value.is_string() && !value.get_ref<const std::string &>().empty()) {
				candidate.emplace(identifier, value.get<std::string>());
			}
		}
		out = std::move(candidate);
		error.clear();
		return true;
	}

	bool SaveRobloxAssetMappings(const RobloxAssetMappings &mappings, std::string &error) {
		nlohmann::json values = nlohmann::json::object();
		for (const auto &[identifier, localAsset] : mappings) {
			if (!identifier.empty() && !localAsset.empty()) {
				values[identifier] = localAsset;
			}
		}
		return WriteConfigDocument(
			"roblox-assets.json", nlohmann::json{{"version", 1}, {"mappings", std::move(values)}}, error
		);
	}

	bool LoadRobloxClassMappings(RobloxClassMappings &out, std::string &error) {
		nlohmann::json document;
		if (!ReadConfigDocument("roblox-classes.json", document, error)) {
			if (error.empty()) {
				out.clear();
				return true;
			}
			return false;
		}

		const auto mappings = document.find("mappings");
		if (mappings == document.end() || !mappings->is_object()) {
			error = ConfigPath("roblox-classes.json").string() + " has no mappings object";
			return false;
		}

		RobloxClassMappings candidate;
		for (const auto &[source, value] : mappings->items()) {
			if (!source.empty() && value.is_string() && !value.get_ref<const std::string &>().empty()) {
				candidate.emplace(source, value.get<std::string>());
			}
		}
		out = std::move(candidate);
		error.clear();
		return true;
	}

	bool SaveRobloxClassMappings(const RobloxClassMappings &mappings, std::string &error) {
		nlohmann::json values = nlohmann::json::object();
		for (const auto &[source, target] : mappings) {
			if (!source.empty() && !target.empty()) {
				values[source] = target;
			}
		}
		return WriteConfigDocument(
			"roblox-classes.json", nlohmann::json{{"version", 1}, {"mappings", std::move(values)}}, error
		);
	}

	const char *Describe(engine::bake::RobloxAssetKind kind) {
		using Kind = engine::bake::RobloxAssetKind;
		switch (kind) {
		case Kind::Unknown:
			return "Unknown";
		case Kind::Animation:
			return "Animation";
		case Kind::Audio:
			return "Audio";
		case Kind::Font:
			return "Font";
		case Kind::Mesh:
			return "Mesh";
		case Kind::Texture:
			return "Texture";
		case Kind::Video:
			return "Video";
		}
		return "Unknown";
	}

	void Editor::DrawRobloxImport() {
		if (!ShowRobloxImport) {
			return;
		}
		if (!ImGui::Begin("Roblox Import", &ShowRobloxImport)) {
			ImGui::End();
			return;
		}

		if (!RobloxMappingsLoaded) {
			std::string error;
			if (!LoadRobloxAssetMappings(RobloxMappings, error)) {
				RobloxImportStatus = std::move(error);
			}
			if (!LoadRobloxClassMappings(RobloxClassMap, error)) {
				RobloxImportStatus = std::move(error);
			}
			RobloxMappingsLoaded = true;
		}

		if (ImGui::Button("Open Roblox file...")) {
			ImGui::OpenPopup("Analyze Roblox File");
		}
		ImGui::SameLine();
		if (RobloxImportPath.empty()) {
			ImGui::TextDisabled(".rbxl, .rbxm, .rbxlx, or .rbxmx");
		} else {
			ImGui::TextUnformatted(RobloxImportPath.c_str());
		}

		const std::vector<std::string> extensions{".rbxl", ".rbxm", ".rbxlx", ".rbxmx"};
		if (engine::ui::FilePrompt("Analyze Roblox File", RobloxImportPath, "Analyze", extensions, true)) {
			std::vector<std::byte> bytes;
			std::string error;
			if (!ReadBytes(RobloxImportPath, bytes, error)) {
				RobloxImportStatus = std::move(error);
			} else {
				engine::bake::RobloxModel model;
				if (!engine::bake::ReadRobloxFile(bytes, model, error)) {
					RobloxImportStatus = std::move(error);
				} else {
					RobloxAnalysis = AnalyzeRobloxImport(model, RobloxClassMap);
					RobloxChoices = RobloxAssetChoices(model, RobloxMappings);
					RobloxRojoScripts = RobloxRojoSubjects(model);
					RobloxImportModel = std::move(model);
					RobloxImportApplied = false;
					RobloxSelectedScript = -1;
					RobloxRojoBrowsePath = std::filesystem::path(RobloxImportPath).parent_path().string();
					RobloxImportStatus = "analysis complete";
				}
			}
		}

		if (!RobloxImportStatus.empty()) {
			ImGui::SameLine();
			ImGui::TextUnformatted(RobloxImportStatus.c_str());
		}

		if (!RobloxImportModel) {
			ImGui::Separator();
			ImGui::TextWrapped(
				"analysis happens before any world changes. missing classes, property conflicts, scripts, "
				"and "
				"asset ids stay visible here."
			);
			ImGui::End();
			return;
		}

		const engine::bake::RobloxModel &model = *RobloxImportModel;
		ImGui::Separator();
		ImGui::Text(
			"%zu instances   %zu classes   %zu scripts   %zu asset ids   %zu lost values",
			RobloxAnalysis.Instances,
			RobloxAnalysis.Classes,
			model.Scripts.size(),
			RobloxChoices.size(),
			model.LostProperties.size()
		);
		const bool mayImport = Universe != nullptr && Active.IsValid() && ModeOf(Active) == RunMode::Edit &&
							   !RobloxImportApplied;
		ImGui::Checkbox("Import Roblox scripts disabled", &RobloxImportDisableScripts);
		ImGui::SameLine();
		ImGui::BeginDisabled(!mayImport);
		if (ImGui::Button("Import into current scene")) {
			RobloxImportResult report;
			std::string error;
			const engine::world::WorldStatus status = Universe->Enter(Active, [&](engine::ecs::Store &store) {
				ImportRobloxPlace(
					store,
					model,
					RobloxMappings,
					RobloxClassMap,
					report,
					error,
					RobloxImportOptions{.DisableScripts = RobloxImportDisableScripts}
				);
			});
			if (status != engine::world::WorldStatus::Ok) {
				RobloxImportStatus = "the active scene is unavailable";
			} else if (!error.empty()) {
				RobloxImportStatus = std::move(error);
			} else {
				RobloxImportApplied = true;
				MarkModified();
				RobloxImportStatus =
					std::to_string(report.Instances) + " instances imported, " +
					std::to_string(report.Scripts) + " scripts staged, " +
					std::to_string(report.DisabledScripts) + " scripts disabled, " +
					std::to_string(report.Properties) + " properties applied, " +
					std::to_string(report.FolderFallbackClasses.size()) + " fallback class groups, " +
					std::to_string(report.SkippedProperties.size()) + " skipped property groups";
			}
		}
		ImGui::EndDisabled();

		if (ImGui::BeginTabBar("##roblox-import-tabs")) {
			if (ImGui::BeginTabItem("Compatibility")) {
				ImGui::Text(
					"%zu missing classes   %zu missing properties   %zu type conflicts",
					RobloxAnalysis.MissingClasses.size(),
					RobloxAnalysis.MissingProperties.size(),
					RobloxAnalysis.ConflictingProperties.size()
				);

				std::string changedSource;
				std::string changedTarget;
				if (ImGui::BeginTable(
						"##roblox-class-gaps",
						3,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
						ImVec2(0.0f, engine::ui::Scaled(180.0f))
					)) {
					ImGui::TableSetupColumn("Missing class");
					ImGui::TableSetupColumn(
						"Instances", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(90.0f)
					);
					ImGui::TableSetupColumn("Engine class");
					ImGui::TableHeadersRow();
					for (const RobloxClassGap &gap : RobloxAnalysis.MissingClasses) {
						ImGui::PushID(gap.ClassName.c_str());
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(gap.ClassName.c_str());
						ImGui::TableNextColumn();
						ImGui::Text("%zu", gap.Instances);
						ImGui::TableNextColumn();
						const auto mapped = RobloxClassMap.find(gap.ClassName);
						const char *label =
							mapped == RobloxClassMap.end() ? "Folder (fallback)" : mapped->second.c_str();
						if (ImGui::Button(label)) {
							ImGui::OpenPopup("Choose Engine Class");
						}
						if (ImGui::BeginPopup("Choose Engine Class")) {
							if (const engine::ecs::ClassId chosen = DrawClassPicker("roblox-import-class");
								chosen.IsValid()) {
								changedSource = gap.ClassName;
								changedTarget =
									std::string(engine::ecs::Classes::Describe(chosen).Name.Text());
								ImGui::CloseCurrentPopup();
							}
							if (mapped != RobloxClassMap.end() && ImGui::Button("Use Folder fallback")) {
								changedSource = gap.ClassName;
								changedTarget.clear();
								ImGui::CloseCurrentPopup();
							}
							ImGui::EndPopup();
						}
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
				if (!changedSource.empty()) {
					if (changedTarget.empty()) {
						RobloxClassMap.erase(changedSource);
					} else {
						RobloxClassMap[changedSource] = changedTarget;
					}
					std::string error;
					if (!SaveRobloxClassMappings(RobloxClassMap, error)) {
						RobloxImportStatus = std::move(error);
					} else {
						RobloxImportStatus = "class mapping saved";
					}
					RobloxAnalysis = AnalyzeRobloxImport(model, RobloxClassMap);
				}

				const auto drawPropertyGaps = [&](const char *id,
												  const std::vector<RobloxPropertyGap> &gaps) {
					if (!ImGui::BeginTable(
							id,
							5,
							ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
							ImVec2(0.0f, engine::ui::Scaled(180.0f))
						)) {
						return;
					}
					ImGui::TableSetupColumn("Class");
					ImGui::TableSetupColumn("Property");
					ImGui::TableSetupColumn("Roblox type");
					ImGui::TableSetupColumn("Engine type");
					ImGui::TableSetupColumn(
						"Uses", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(65.0f)
					);
					ImGui::TableHeadersRow();
					for (const RobloxPropertyGap &gap : gaps) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(gap.ClassName.c_str());
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(gap.PropertyName.c_str());
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(gap.SourceType.c_str());
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(
							gap.ExpectedType.empty() ? "missing" : gap.ExpectedType.c_str()
						);
						ImGui::TableNextColumn();
						ImGui::Text("%zu", gap.Occurrences);
					}
					ImGui::EndTable();
				};
				ImGui::TextUnformatted("Missing properties");
				drawPropertyGaps("##roblox-missing-properties", RobloxAnalysis.MissingProperties);
				ImGui::TextUnformatted("Type conflicts");
				drawPropertyGaps("##roblox-conflicting-properties", RobloxAnalysis.ConflictingProperties);
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Assets")) {
				ImGui::SetNextItemWidth(engine::ui::Scaled(300.0f));
				ImGui::InputTextWithHint(
					"##roblox-asset-filter",
					"filter id, uri, kind, or mapping",
					RobloxImportFilter,
					sizeof(RobloxImportFilter)
				);
				if (ImGui::BeginTable(
						"##roblox-assets",
						6,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
							ImGuiTableFlags_Resizable,
						ImVec2(0.0f, 0.0f)
					)) {
					ImGui::TableSetupColumn("Asset id");
					ImGui::TableSetupColumn("Kind");
					ImGui::TableSetupColumn(
						"Uses", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(55.0f)
					);
					ImGui::TableSetupColumn("Source URI");
					ImGui::TableSetupColumn("Local asset");
					ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(105.0f));
					ImGui::TableHeadersRow();
					for (RobloxAssetChoice &choice : RobloxChoices) {
						const std::string kind = Describe(choice.Kind);
						const std::string_view filter(RobloxImportFilter);
						if (!ContainsFolded(choice.Identifier, filter) &&
							!ContainsFolded(choice.SourceUri, filter) && !ContainsFolded(kind, filter) &&
							!ContainsFolded(choice.LocalAsset, filter)) {
							continue;
						}
						ImGui::PushID(choice.Identifier.c_str());
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(choice.Identifier.c_str());
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(kind.c_str());
						ImGui::TableNextColumn();
						ImGui::Text("%zu", choice.Uses);
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(choice.SourceUri.c_str());
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(
							choice.LocalAsset.empty() ? "unmapped" : choice.LocalAsset.c_str()
						);
						ImGui::TableNextColumn();
						if (ImGui::SmallButton("Choose...")) {
							RobloxMappingIdentifier = choice.Identifier;
							RobloxMappingBrowsePath = choice.LocalAsset;
							ImGui::OpenPopup("Choose Local Roblox Asset");
						}
						if (!choice.LocalAsset.empty()) {
							ImGui::SameLine();
							if (ImGui::SmallButton("Clear")) {
								choice.LocalAsset.clear();
								RobloxMappings.erase(choice.Identifier);
								std::string error;
								if (!SaveRobloxAssetMappings(RobloxMappings, error)) {
									RobloxImportStatus = std::move(error);
								}
							}
						}
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Scripts")) {
				const size_t readyScripts = static_cast<size_t>(std::count_if(
					RobloxRojoScripts.begin(), RobloxRojoScripts.end(), [](const RobloxRojoSubject &subject) {
						return subject.Valid;
					}
				));
				ImGui::BeginDisabled(readyScripts == 0);
				if (ImGui::Button("Set up Rojo project...")) {
					if (RobloxRojoBrowsePath.empty()) {
						RobloxRojoBrowsePath = std::filesystem::path(RobloxImportPath).parent_path().string();
					}
					ImGui::OpenPopup("Set Up Roblox Rojo Project");
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::Text("%zu ready   %zu invalid", readyScripts, RobloxRojoScripts.size() - readyScripts);

				const float listHeight = engine::ui::Scaled(220.0f);
				if (ImGui::BeginTable(
						"##roblox-scripts",
						3,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
						ImVec2(0.0f, listHeight)
					)) {
					ImGui::TableSetupColumn("Path");
					ImGui::TableSetupColumn(
						"Class", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(110.0f)
					);
					ImGui::TableSetupColumn("Rojo subject");
					ImGui::TableHeadersRow();
					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(model.Scripts.size()));
					while (clipper.Step()) {
						for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++) {
							const engine::bake::RobloxScript &script =
								model.Scripts[static_cast<size_t>(index)];
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							if (ImGui::Selectable(
									script.InstancePath.c_str(),
									RobloxSelectedScript == index,
									ImGuiSelectableFlags_SpanAllColumns
								)) {
								RobloxSelectedScript = index;
							}
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(script.ClassName.c_str());
							ImGui::TableNextColumn();
							if (static_cast<size_t>(index) >= RobloxRojoScripts.size()) {
								ImGui::TextUnformatted("invalid: no hierarchy analysis");
							} else {
								const RobloxRojoSubject &subject =
									RobloxRojoScripts[static_cast<size_t>(index)];
								if (subject.Valid) {
									ImGui::TextUnformatted(subject.SourcePath.generic_string().c_str());
								} else {
									ImGui::Text("invalid: %s", subject.Reason.c_str());
								}
							}
						}
					}
					ImGui::EndTable();
				}
				if (RobloxSelectedScript >= 0 &&
					static_cast<size_t>(RobloxSelectedScript) < model.Scripts.size()) {
					const engine::bake::RobloxScript &script = model.Scripts[RobloxSelectedScript];
					ImGui::TextUnformatted(script.InstancePath.c_str());
					ImGui::BeginChild("##roblox-script-source", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
					ImGui::TextUnformatted(
						script.Source.c_str(), script.Source.c_str() + script.Source.size()
					);
					ImGui::EndChild();
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Lost properties")) {
				if (ImGui::BeginTable(
						"##roblox-lost-properties",
						4,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
							ImGuiTableFlags_Resizable,
						ImVec2(0.0f, 0.0f)
					)) {
					ImGui::TableSetupColumn("Path");
					ImGui::TableSetupColumn("Class");
					ImGui::TableSetupColumn("Property");
					ImGui::TableSetupColumn("Roblox type");
					ImGui::TableHeadersRow();
					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(model.LostProperties.size()));
					while (clipper.Step()) {
						for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++) {
							const engine::bake::RobloxLostProperty &lost =
								model.LostProperties[static_cast<size_t>(index)];
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(lost.InstancePath.c_str());
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(lost.ClassName.c_str());
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(lost.PropertyName.c_str());
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(lost.RobloxType.c_str());
						}
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		if (engine::ui::FilePrompt(
				"Choose Local Roblox Asset", RobloxMappingBrowsePath, "Use file", {}, true
			)) {
			for (RobloxAssetChoice &choice : RobloxChoices) {
				if (choice.Identifier == RobloxMappingIdentifier) {
					choice.LocalAsset = RobloxMappingBrowsePath;
					RobloxMappings[choice.Identifier] = choice.LocalAsset;
					break;
				}
			}
			std::string error;
			if (!SaveRobloxAssetMappings(RobloxMappings, error)) {
				RobloxImportStatus = std::move(error);
			} else {
				RobloxImportStatus = "asset mapping saved";
			}
		}

		if (engine::ui::FolderPrompt("Set Up Roblox Rojo Project", RobloxRojoBrowsePath, "Create project")) {
			std::string projectName = std::filesystem::path(RobloxImportPath).stem().string();
			if (projectName.empty()) {
				projectName = "RobloxPlace";
			}
			const std::string folderName = IsPortableRojoName(projectName) ? projectName : "RobloxPlace";
			const std::filesystem::path destination =
				std::filesystem::path(RobloxRojoBrowsePath) / (folderName + "-rojo");
			RobloxRojoSetupResult report;
			std::string error;
			if (!SetupRobloxRojoProject(model, destination, projectName, report, error)) {
				RobloxImportStatus = std::move(error);
			} else {
				RobloxImportStatus = std::to_string(report.ScriptsWritten) + " scripts written to " +
									 report.ProjectFile.string();
			}
		}

		ImGui::End();
	}
}
