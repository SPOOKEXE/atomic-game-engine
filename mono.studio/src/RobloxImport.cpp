#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/game/Values.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Prompts.hpp>

#include <algorithm>
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
			if (!Compatible(property.Type, source.Kind)) {
				return false;
			}

			out = engine::game::PropertyValue{};
			out.Type = property.Type;
			switch (property.Type) {
			case PropertyType::Bool:
				out.Bool = source.Bool;
				return true;
			case PropertyType::Int32:
			case PropertyType::Int64:
			case PropertyType::Float:
			case PropertyType::Double: {
				const double number = source.Kind == RobloxValueKind::Integer
										  ? static_cast<double>(source.Integer)
										  : source.Number;
				out.Int32 = static_cast<int32_t>(number);
				out.Int64 = static_cast<int64_t>(number);
				out.Float = static_cast<float>(number);
				out.Double = number;
				return true;
			}
			case PropertyType::String:
				out.String = source.Text;
				return true;
			case PropertyType::Name:
			case PropertyType::Enum:
				out.Name = engine::core::Name(source.Text);
				return property.Type != PropertyType::Enum ||
					   engine::ecs::EnumTable::Has(property.EnumName, out.Name);
			case PropertyType::Vector3:
				out.Vector3 = source.Vector3;
				return true;
			case PropertyType::Color3:
				out.Color3 = source.Color3;
				return true;
			case PropertyType::CFrame:
				out.CFrame = source.CFrame;
				return true;
			case PropertyType::Vector2:
				out.Vector2 = source.Vector2;
				return true;
			case PropertyType::UDim:
				out.UDim = source.UDim;
				return true;
			case PropertyType::UDim2:
				out.UDim2 = source.UDim2;
				return true;
			case PropertyType::Rect:
				out.Rect = source.Rect;
				return true;
			case PropertyType::NumberRange:
				out.NumberRange = source.NumberRange;
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
			RobloxImportResult &Report;
			const RobloxImportOptions &Options;
			std::unordered_set<std::string> SourceKeys;
			std::string Error;
		};

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
							   property.Value.Kind == engine::bake::RobloxValueKind::Text;
					});
				if (found != node.Properties.end()) {
					sourceProperty = &*found;
				}
			}

			Entity instance = reuse;
			if (instance == NULL_ENTITY && sourceProperty != nullptr) {
				const std::string sourceKey = UniqueSourceKey(state, path);
				std::string source = ApplyAssetMappings(
					state.Replacements, path, sourceProperty->Name, sourceProperty->Value.Text
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
				engine::ecs::ClassId classId = engine::ecs::Classes::Find(engine::core::Name(node.ClassName));
				if (!classId.IsValid()) {
					classId = FolderClass();
				}
				instance = state.Store.CreateInstance(classId, node.Name);
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
					state.Report.SkippedProperties++;
					continue;
				}

				engine::bake::RobloxValue mapped = property.Value;
				if (mapped.Kind == engine::bake::RobloxValueKind::Text) {
					mapped.Text =
						ApplyAssetMappings(state.Replacements, path, property.Name, std::move(mapped.Text));
				}
				engine::game::PropertyValue value;
				if (!ToGameValue(*descriptor, mapped, value) ||
					!engine::game::WriteAuthoredProperty(state.Store, instance, *descriptor, value)) {
					state.Report.SkippedProperties++;
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
	}

	RobloxImportAnalysis AnalyzeRobloxImport(const engine::bake::RobloxModel &model) {
		engine::game::RegisterGameClasses();
		engine::effects::RegisterEffectClasses();
		(void)FolderClass();
		std::map<std::string, size_t, std::less<>> classCounts;
		std::map<GapKey, size_t> missingProperties;
		std::map<GapKey, size_t> conflictingProperties;

		const auto visit = [&](const auto &self, const engine::bake::RobloxInstance &instance) -> void {
			classCounts[instance.ClassName]++;
			const engine::ecs::ClassId classId =
				engine::ecs::Classes::Find(engine::core::Name(instance.ClassName));
			if (classId.IsValid()) {
				const engine::ecs::ClassInfo &info = engine::ecs::Classes::Describe(classId);
				for (const engine::bake::RobloxProperty &property : instance.Properties) {
					const engine::ecs::PropertyDescriptor *descriptor =
						RobloxPropertyNamed(info, property.Name);
					if (descriptor == nullptr) {
						AddGap(
							missingProperties,
							{instance.ClassName, property.Name, Describe(property.Value.Kind), {}}
						);
					} else if (!Compatible(descriptor->Type, property.Value.Kind)) {
						AddGap(
							conflictingProperties,
							{
								instance.ClassName,
								property.Name,
								Describe(property.Value.Kind),
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
			if (!engine::ecs::Classes::Find(engine::core::Name(className)).IsValid()) {
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

	bool ImportRobloxPlace(
		engine::ecs::Store &store,
		const engine::bake::RobloxModel &model,
		const RobloxAssetMappings &mappings,
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
		(void)FolderClass();

		ReplacementTable replacements;
		for (const engine::bake::RobloxAssetReference &asset : model.Assets) {
			const auto mapped = mappings.find(asset.Identifier);
			if (mapped == mappings.end() || mapped->second.empty()) {
				continue;
			}
			replacements[ReplacementKey(asset.InstancePath, asset.PropertyName)].emplace_back(
				asset.Uri, mapped->second
			);
		}

		RobloxImportResult candidate;
		BuildState state{store, replacements, candidate, options, {}, {}};
		for (const engine::bake::RobloxInstance &root : model.Roots) {
			const engine::ecs::Entity existing = store.FindFirstRoot(root.Name);
			if (existing != engine::ecs::NULL_ENTITY) {
				candidate.ReusedRoots++;
				const engine::ecs::ClassId existingClass = store.ClassOf(existing);
				const engine::ecs::ClassId wantedClass =
					engine::ecs::Classes::Find(engine::core::Name(root.ClassName));
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
					RobloxAnalysis = AnalyzeRobloxImport(model);
					RobloxChoices = RobloxAssetChoices(model, RobloxMappings);
					RobloxImportModel = std::move(model);
					RobloxImportApplied = false;
					RobloxSelectedScript = -1;
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
				RobloxImportStatus = std::to_string(report.Instances) + " instances imported, " +
									 std::to_string(report.Scripts) + " scripts staged, " +
									 std::to_string(report.DisabledScripts) + " scripts disabled, " +
									 std::to_string(report.Properties) + " properties applied";
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

				if (ImGui::BeginTable(
						"##roblox-class-gaps",
						2,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
						ImVec2(0.0f, engine::ui::Scaled(180.0f))
					)) {
					ImGui::TableSetupColumn("Missing class");
					ImGui::TableSetupColumn(
						"Instances", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(90.0f)
					);
					ImGui::TableHeadersRow();
					for (const RobloxClassGap &gap : RobloxAnalysis.MissingClasses) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(gap.ClassName.c_str());
						ImGui::TableNextColumn();
						ImGui::Text("%zu", gap.Instances);
					}
					ImGui::EndTable();
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
				const float listHeight = engine::ui::Scaled(220.0f);
				if (ImGui::BeginTable(
						"##roblox-scripts",
						2,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
						ImVec2(0.0f, listHeight)
					)) {
					ImGui::TableSetupColumn("Path");
					ImGui::TableSetupColumn(
						"Class", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(110.0f)
					);
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

		ImGui::End();
	}
}
