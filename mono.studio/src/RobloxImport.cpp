#include "ImportedValues.hpp"

#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/Values.hpp>
#include <engine/gui/Services.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/spatial/CollisionGroups.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Prompts.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <studio/Complete.hpp>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/RobloxImport.hpp>
#include <studio/RojoSync.hpp>
#include <sys/resource.h>
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
			case RobloxValueKind::NumberSequence:
				return "NumberSequence";
			case RobloxValueKind::ColorSequence:
				return "ColorSequence";
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
			case PropertyType::NumberSequence:
				return value == RobloxValueKind::NumberSequence;
			case PropertyType::ColorSequence:
				return value == RobloxValueKind::ColorSequence;
			case PropertyType::NumberRange:
				return value == RobloxValueKind::NumberRange;
			case PropertyType::Reference:
			case PropertyType::Opaque:
				return false;
			}
			return false;
		}

		// Views into the decoded model and static type names, so counting a
		// gap per property occurrence allocates nothing; `FinishGaps` copies
		// only the distinct groups.
		using GapKey = std::tuple<std::string_view, std::string_view, std::string_view, std::string_view>;
		using SkipKey = std::tuple<std::string, std::string, std::string>;
		using SubstitutionKey = std::tuple<std::string, std::string, std::string>;
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
				out.push_back(
					{std::string(className),
					 std::string(propertyName),
					 std::string(sourceType),
					 std::string(expectedType),
					 count}
				);
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

		void AddSubstitution(
			std::map<SubstitutionKey, size_t> &substitutions,
			std::string_view source,
			std::string_view target,
			std::string_view note
		) {
			substitutions[{std::string(source), std::string(target), std::string(note)}]++;
		}

		std::vector<RobloxClassSubstitution>
		FinishSubstitutions(const std::map<SubstitutionKey, size_t> &substitutions) {
			std::vector<RobloxClassSubstitution> out;
			out.reserve(substitutions.size());
			for (const auto &[key, count] : substitutions) {
				const auto &[source, target, note] = key;
				out.push_back({source, target, count, note});
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

		struct BuiltinClassSubstitution {
			std::string_view Source;
			std::string_view Target;
			std::string_view Note;
		};

		constexpr std::array BUILTIN_CLASS_SUBSTITUTIONS{
			BuiltinClassSubstitution{"Hat", "Accessory", "accessory attachment behaviour is approximated"},
			BuiltinClassSubstitution{"Seat", "Part", "seat behaviour is approximated as a part"},
			BuiltinClassSubstitution{
				"VehicleSeat", "Part", "vehicle seat behaviour is approximated as a part"
			},
			BuiltinClassSubstitution{"TrussPart", "Part", "truss geometry is approximated as a box"},
			BuiltinClassSubstitution{"UnionOperation", "Part", "union geometry is approximated as a box"},
			BuiltinClassSubstitution{
				"Sky", "SkyboxTextures", "sky settings are approximated as six textures"
			},
		};

		const BuiltinClassSubstitution *BuiltinSubstitution(std::string_view source) {
			const auto found = std::find_if(
				BUILTIN_CLASS_SUBSTITUTIONS.begin(),
				BUILTIN_CLASS_SUBSTITUTIONS.end(),
				[source](const BuiltinClassSubstitution &substitution) {
					return substitution.Source == source;
				}
			);
			return found == BUILTIN_CLASS_SUBSTITUTIONS.end() ? nullptr : &*found;
		}

		// A Roblox part whose drawn shape is not a box, rebuilt as a `MeshPart`
		// on a builtin mesh with matching collision.
		//
		// The builtin meshes are not oriented like Roblox's, so the part frame is
		// turned and its size permuted to match: Roblox's wedge is tall at +Z and
		// the builtin at -Z, Roblox's cylinder runs along X and the builtin along Y.
		struct PartShape {
			enum class Turn : uint8_t { None, HalfY, QuarterY, QuarterZ };
			std::string_view Mesh;
			std::string_view Collision;
			Turn Rotation = Turn::None;
			BuiltinClassSubstitution Report;
			// Taken from a `SpecialMesh` child, which the shape consumes. An
			// empty `Mesh` means the child's own `MeshId` and `TextureId`.
			bool FromSpecialMesh = false;
		};

		constexpr PartShape WEDGE_SHAPE{
			"engine.Wedge",
			"Hull",
			PartShape::Turn::HalfY,
			{"WedgePart", "MeshPart", "drawn and collided as the builtin wedge"}
		};
		constexpr PartShape CORNER_WEDGE_SHAPE{
			"engine.CornerWedge",
			"Hull",
			PartShape::Turn::QuarterY,
			{"CornerWedgePart", "MeshPart", "drawn and collided as the builtin corner wedge, apex at +X -Z"}
		};
		constexpr PartShape BALL_SHAPE{
			"engine.Sphere",
			"Sphere",
			PartShape::Turn::None,
			{"Part (Ball)", "MeshPart", "drawn as the builtin sphere"}
		};
		constexpr PartShape CYLINDER_SHAPE{
			"engine.Cylinder",
			"Cylinder",
			PartShape::Turn::QuarterZ,
			{"Part (Cylinder)", "MeshPart", "drawn as the builtin cylinder, turned onto Roblox's X axis"}
		};
		constexpr PartShape PART_WEDGE_SHAPE{
			"engine.Wedge",
			"Hull",
			PartShape::Turn::HalfY,
			{"Part (Wedge)", "MeshPart", "drawn and collided as the builtin wedge"}
		};
		constexpr PartShape PART_CORNER_WEDGE_SHAPE{
			"engine.CornerWedge",
			"Hull",
			PartShape::Turn::QuarterY,
			{"Part (CornerWedge)",
			 "MeshPart",
			 "drawn and collided as the builtin corner wedge, apex at +X -Z"}
		};

		// A `SpecialMesh` only changes what a part looks like; it still
		// collides as its box in Roblox, so these keep a box collider.
		constexpr PartShape SPECIAL_SPHERE_SHAPE{
			"engine.Sphere",
			"Box",
			PartShape::Turn::None,
			{"SpecialMesh (Sphere)",
			 "MeshPart",
			 "parent drawn as the builtin sphere; Scale and Offset ignored"},
			true
		};
		constexpr PartShape SPECIAL_CYLINDER_SHAPE{
			"engine.Cylinder",
			"Box",
			PartShape::Turn::QuarterZ,
			{"SpecialMesh (Cylinder)",
			 "MeshPart",
			 "parent drawn as the builtin cylinder; Scale and Offset ignored"},
			true
		};
		constexpr PartShape SPECIAL_WEDGE_SHAPE{
			"engine.Wedge",
			"Box",
			PartShape::Turn::HalfY,
			{"SpecialMesh (Wedge)",
			 "MeshPart",
			 "parent drawn as the builtin wedge; Scale and Offset ignored"},
			true
		};
		constexpr PartShape SPECIAL_FILE_MESH_SHAPE{
			"",
			"Box",
			PartShape::Turn::None,
			{"SpecialMesh (FileMesh)", "MeshPart", "mesh fitted to the part size; Scale and Offset ignored"},
			true
		};

		const engine::bake::RobloxInstance *SpecialMeshOf(const engine::bake::RobloxInstance &node) {
			for (const engine::bake::RobloxInstance &child : node.Children) {
				if (child.ClassName == "SpecialMesh") {
					return &child;
				}
			}
			return nullptr;
		}

		std::string_view TextProperty(const engine::bake::RobloxInstance &node, std::string_view name) {
			for (const engine::bake::RobloxProperty &property : node.Properties) {
				if (property.Name == name && property.Value.Kind() == RobloxValueKind::Text) {
					return property.Value.As<std::string>();
				}
			}
			return {};
		}

		const PartShape *PartShapeOf(const engine::bake::RobloxInstance &node) {
			if (node.ClassName == "WedgePart") return &WEDGE_SHAPE;
			if (node.ClassName == "CornerWedgePart") return &CORNER_WEDGE_SHAPE;
			if (node.ClassName != "Part") return nullptr;
			const std::string_view shape = TextProperty(node, "Shape");
			if (shape == "Ball") return &BALL_SHAPE;
			if (shape == "Cylinder") return &CYLINDER_SHAPE;
			if (shape == "Wedge") return &PART_WEDGE_SHAPE;
			if (shape == "CornerWedge") return &PART_CORNER_WEDGE_SHAPE;
			const engine::bake::RobloxInstance *special = SpecialMeshOf(node);
			if (special == nullptr) return nullptr;
			const std::string_view meshType = TextProperty(*special, "MeshType");
			if (meshType == "Sphere") return &SPECIAL_SPHERE_SHAPE;
			if (meshType == "Cylinder") return &SPECIAL_CYLINDER_SHAPE;
			if (meshType == "Wedge") return &SPECIAL_WEDGE_SHAPE;
			if (meshType == "FileMesh" && !TextProperty(*special, "MeshId").empty())
				return &SPECIAL_FILE_MESH_SHAPE;
			return nullptr;
		}

		engine::core::CFrame ShapeRotation(PartShape::Turn turn) {
			constexpr float HALF_PI = 1.57079632679f;
			switch (turn) {
			case PartShape::Turn::HalfY:
				return engine::core::CFrame::FromAxisAngle({0.0f, 1.0f, 0.0f}, 2.0f * HALF_PI);
			case PartShape::Turn::QuarterY:
				return engine::core::CFrame::FromAxisAngle({0.0f, 1.0f, 0.0f}, -HALF_PI);
			case PartShape::Turn::QuarterZ:
				return engine::core::CFrame::FromAxisAngle({0.0f, 0.0f, 1.0f}, HALF_PI);
			case PartShape::Turn::None:
				break;
			}
			return {};
		}

		// The Roblox value re-expressed in the turned mesh frame. Properties
		// other than the part frame, its pivot and its size pass through.
		engine::bake::RobloxProperty
		ShapedProperty(const PartShape &shape, const engine::bake::RobloxProperty &property) {
			engine::bake::RobloxProperty out = property;
			if (shape.Rotation == PartShape::Turn::None) {
				return out;
			}
			const engine::core::CFrame turn = ShapeRotation(shape.Rotation);
			if (property.Value.Kind() == RobloxValueKind::CFrame && property.Name == "CFrame") {
				out.Value.Set(property.Value.As<engine::core::CFrame>() * turn);
			} else if (property.Value.Kind() == RobloxValueKind::CFrame && property.Name == "PivotOffset") {
				out.Value.Set(turn.Inverse() * property.Value.As<engine::core::CFrame>());
			} else if (property.Value.Kind() == RobloxValueKind::Vector3 && property.Name == "Size") {
				const engine::core::Vector3 size = property.Value.As<engine::core::Vector3>();
				if (shape.Rotation == PartShape::Turn::QuarterY) {
					out.Value.Set(engine::core::Vector3{size.Z, size.Y, size.X});
				} else if (shape.Rotation == PartShape::Turn::QuarterZ) {
					out.Value.Set(engine::core::Vector3{size.Y, size.X, size.Z});
				}
			}
			return out;
		}

		// Legacy surface joints. Roblox rebuilds them from part surfaces at run
		// time, and the parts they join are references that cannot cross.
		constexpr std::array<std::string_view, 9> LEGACY_JOINTS{
			"Snap",
			"Glue",
			"Rotate",
			"RotateP",
			"RotateV",
			"ManualWeld",
			"ManualGlue",
			"ManualSurfaceJointInstance",
			"VelocityMotor",
		};

		bool DroppedClass(const ClassTable &mappings, std::string_view className) {
			return !mappings.contains(std::string(className)) &&
				   std::find(LEGACY_JOINTS.begin(), LEGACY_JOINTS.end(), className) != LEGACY_JOINTS.end();
		}

		constexpr std::string_view DROPPED_JOINT_NOTE = "legacy surface joint, rebuilt by Roblox at run time";

		// Roblox bookkeeping with no content for this engine: identity, legacy
		// surface inputs, cached physics and mesh state. Skipped without a
		// report line, since nothing an author made is lost with them.
		bool BookkeepingProperty(std::string_view name) {
			static const std::unordered_set<std::string_view> NAMES{
				"ModelInPrimary",
				"ModelMeshCFrame",
				"ModelMeshData",
				"ModelMeshSize",
				"LODData",
				"PhysicsData",
				"PhysicalConfigData",
				"UniqueId",
				"HistoryId",
				"SourceAssetId",
				"ScriptGuid",
				"Capabilities",
				"DefinesCapabilities",
				"Sandboxed",
				"formFactorRaw",
				"FormFactor",
				"CollisionGroupId",
				"Velocity",
				"RotVelocity",
				"AssemblyLinearVelocity",
				"AssemblyAngularVelocity",
				"RootPriority",
				"NeedsPivotMigration",
				"AcquisitionMethod",
			};
			if (NAMES.contains(name)) {
				return true;
			}
			const auto endsWith = [name](std::string_view suffix) {
				return name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
			};
			return endsWith("SurfaceInput") || endsWith("ParamA") || endsWith("ParamB");
		}

		// Source properties the importer reads itself instead of copying.
		bool ConsumedProperty(
			const engine::bake::RobloxInstance &node, const PartShape *shape, std::string_view name
		) {
			if (BookkeepingProperty(name)) {
				return true;
			}
			// A block is a plain part, so a `Part`'s shape is always accounted for.
			if ((shape != nullptr || node.ClassName == "Part") && name == "Shape") {
				return true;
			}
			return node.ClassName == "Terrain" &&
				   (name == "SmoothGrid" || name == "PhysicsGrid" || name == "MaterialColors" ||
					name == "WaterColor" || name == "WaterTransparency");
		}

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

		struct ClassResolution {
			engine::ecs::ClassId Id;
			const BuiltinClassSubstitution *Substitution = nullptr;
		};

		ClassResolution ResolveClass(const ClassTable &mappings, std::string_view source) {
			const engine::ecs::ClassId native =
				engine::ecs::Classes::Find(engine::core::Name(std::string(source)));
			if (native.IsValid()) {
				return {native};
			}
			const auto mapped = mappings.find(std::string(source));
			if (mapped != mappings.end()) {
				return {mapped->second};
			}
			const BuiltinClassSubstitution *substitution = BuiltinSubstitution(source);
			if (substitution == nullptr) {
				return {FolderClass()};
			}
			const engine::ecs::ClassId target =
				engine::ecs::Classes::Find(engine::core::Name(std::string(substitution->Target)));
			return target.IsValid() ? ClassResolution{target, substitution} : ClassResolution{FolderClass()};
		}

		// `ResolveClass`, with a non-box part shape taking precedence over the
		// native class unless the author mapped the source class themselves.
		ClassResolution ResolveNode(const ClassTable &mappings, const engine::bake::RobloxInstance &node) {
			const PartShape *shape = mappings.contains(node.ClassName) ? nullptr : PartShapeOf(node);
			if (shape != nullptr) {
				const engine::ecs::ClassId meshPart =
					engine::ecs::Classes::Find(engine::core::Name("MeshPart"));
				if (meshPart.IsValid()) {
					return {meshPart, &shape->Report};
				}
			}
			return ResolveClass(mappings, node.ClassName);
		}

		bool UsesFolderFallback(const ClassTable &mappings, const engine::bake::RobloxInstance &node) {
			const engine::ecs::ClassId native =
				engine::ecs::Classes::Find(engine::core::Name(std::string(node.ClassName)));
			return !native.IsValid() && !mappings.contains(node.ClassName) &&
				   BuiltinSubstitution(node.ClassName) == nullptr && PartShapeOf(node) == nullptr;
		}

		// A root of a class this engine lacks and with nothing inside: a
		// Roblox service shell such as `Selection` or `PhysicsService`. Only
		// services sit at a place's root, so importing it would add an empty
		// Folder and nothing an author made.
		bool EmptyUnknownRoot(const ClassTable &mappings, const engine::bake::RobloxInstance &root) {
			return root.Children.empty() && UsesFolderFallback(mappings, root);
		}

		bool IsBasePart(engine::ecs::ClassId classId) {
			const engine::ecs::ClassId basePart = engine::ecs::Classes::Find(engine::core::Name("BasePart"));
			return basePart.IsValid() && classId.IsValid() && engine::ecs::Classes::IsA(classId, basePart);
		}

		std::string_view ImportedPropertyName(
			std::string_view sourceClass, engine::ecs::ClassId classId, std::string_view propertyName
		) {
			if (sourceClass != "Sky") {
				return propertyName;
			}
			const engine::ecs::ClassId skybox =
				engine::ecs::Classes::Find(engine::core::Name("SkyboxTextures"));
			if (classId != skybox) {
				return propertyName;
			}
			if (propertyName == "SkyboxFt") return "Front";
			if (propertyName == "SkyboxBk") return "Back";
			if (propertyName == "SkyboxLf") return "Left";
			if (propertyName == "SkyboxRt") return "Right";
			if (propertyName == "SkyboxUp") return "Up";
			return propertyName == "SkyboxDn" ? "Down" : propertyName;
		}

		std::string_view SurfaceAppearancePropertyName(std::string_view propertyName) {
			if (propertyName == "ColorMap") return "TextureID";
			return propertyName == "Color" ? "SurfaceColor" : propertyName;
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
				return source.Kind() == RobloxValueKind::Integer
						   ? ReadImportedNumber(property.Type, source.As<int64_t>(), out)
						   : ReadImportedNumber(property.Type, source.As<double>(), out);
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
			case PropertyType::NumberSequence: {
				if (source.Kind() != RobloxValueKind::NumberSequence) {
					return false;
				}
				return ReadImportedSequence(
					source.As<engine::bake::RobloxNumberSequence>(), out.NumberSequence
				);
			}
			case PropertyType::ColorSequence: {
				if (source.Kind() != RobloxValueKind::ColorSequence) {
					return false;
				}
				return ReadImportedSequence(
					source.As<engine::bake::RobloxColorSequence>(), out.ColorSequence
				);
			}
			case PropertyType::NumberRange:
				out.NumberRange = source.As<engine::core::NumberRange>();
				return true;
			case PropertyType::Reference:
			case PropertyType::Opaque:
				return false;
			}
			return false;
		}

		// A decoded value as an attribute, by the kind the file stored.
		bool ToAttribute(const engine::bake::RobloxValue &source, engine::ecs::AttributeValue &out) {
			using engine::ecs::PropertyType;
			switch (source.Kind()) {
			case RobloxValueKind::Bool:
				out.Type = PropertyType::Bool;
				out.Bool = source.As<bool>();
				return true;
			case RobloxValueKind::Integer:
				out.Type = PropertyType::Int64;
				out.Int64 = source.As<int64_t>();
				return true;
			case RobloxValueKind::Number:
				out.Type = PropertyType::Double;
				out.Double = source.As<double>();
				return std::isfinite(out.Double);
			case RobloxValueKind::Text:
				out.Type = PropertyType::String;
				out.String = source.As<std::string>();
				return true;
			case RobloxValueKind::Vector3:
				out.Type = PropertyType::Vector3;
				out.Vector3 = source.As<engine::core::Vector3>();
				return true;
			case RobloxValueKind::Vector2:
				out.Type = PropertyType::Vector2;
				out.Vector2 = source.As<engine::core::Vector2>();
				return true;
			case RobloxValueKind::Color3:
				out.Type = PropertyType::Color3;
				out.Color3 = source.As<engine::core::Color3>();
				return true;
			case RobloxValueKind::CFrame:
				out.Type = PropertyType::CFrame;
				out.CFrame = source.As<engine::core::CFrame>();
				return true;
			case RobloxValueKind::UDim:
				out.Type = PropertyType::UDim;
				out.UDim = source.As<engine::core::UDim>();
				return true;
			case RobloxValueKind::UDim2:
				out.Type = PropertyType::UDim2;
				out.UDim2 = source.As<engine::core::UDim2>();
				return true;
			case RobloxValueKind::Rect:
				out.Type = PropertyType::Rect;
				out.Rect = source.As<engine::core::Rect>();
				return true;
			case RobloxValueKind::NumberRange:
				out.Type = PropertyType::NumberRange;
				out.NumberRange = source.As<engine::core::NumberRange>();
				return true;
			case RobloxValueKind::NumberSequence:
				out.Type = PropertyType::NumberSequence;
				return ReadImportedSequence(
					source.As<engine::bake::RobloxNumberSequence>(), out.NumberSequence
				);
			case RobloxValueKind::ColorSequence:
				out.Type = PropertyType::ColorSequence;
				return ReadImportedSequence(
					source.As<engine::bake::RobloxColorSequence>(), out.ColorSequence
				);
			}
			return false;
		}

		// Lets a set of strings be searched with a string_view, without
		// building a string for the lookup.
		struct TransparentHash {
			using is_transparent = void;
			size_t operator()(std::string_view text) const {
				return std::hash<std::string_view>{}(text);
			}
		};

		struct BuildState {
			engine::ecs::Store &Store;
			const ReplacementTable &Replacements;
			const ClassTable &Classes;
			RobloxImportResult &Report;
			const RobloxImportOptions &Options;
			std::unordered_set<std::string> SourceKeys;
			ClassCounts FolderFallbackClasses;
			// Skips counted under one combined key (class, property and reason
			// joined by NUL), built in a reused buffer. A large place skips
			// millions of properties and only the distinct groups allocate.
			std::unordered_map<std::string, size_t, TransparentHash, std::equal_to<>> SkippedProperties;
			std::string SkipKeyBuffer;
			std::map<SubstitutionKey, size_t> Substitutions;
			std::string Error;
			// Property lookup by spelling per descriptor list, so a place with
			// millions of properties does not scan a class's list for each.
			using PropertyIndex =
				std::unordered_map<std::string_view, const engine::ecs::PropertyDescriptor *>;
			std::unordered_map<const engine::ecs::PropertyDescriptor *, PropertyIndex> PropertyIndices;
			// The index for the instance being filled, since its properties
			// arrive one after another.
			engine::ecs::Entity IndexedInstance = engine::ecs::NULL_ENTITY;
			const PropertyIndex *InstanceIndex = nullptr;
			// Class resolution by Roblox class name, and whether it fell back to
			// Folder. Looking a class up takes the registry lock and interns a
			// name, which a place with half a million instances should do once
			// per class, not once per instance.
			std::unordered_map<std::string, std::pair<ClassResolution, bool>> ResolvedClasses;
		};

		// `ResolveNode` and `UsesFolderFallback`, remembered per class name for
		// every instance whose class alone decides it (no non-box shape).
		std::pair<ClassResolution, bool>
		ResolveCached(BuildState &state, const engine::bake::RobloxInstance &node) {
			if (!state.Classes.contains(node.ClassName) && PartShapeOf(node) != nullptr) {
				return {ResolveNode(state.Classes, node), false};
			}
			const auto found = state.ResolvedClasses.find(node.ClassName);
			if (found != state.ResolvedClasses.end()) {
				return found->second;
			}
			const std::pair<ClassResolution, bool> resolved{
				ResolveNode(state.Classes, node), UsesFolderFallback(state.Classes, node)
			};
			state.ResolvedClasses.emplace(node.ClassName, resolved);
			return resolved;
		}

		const engine::ecs::PropertyDescriptor *
		RobloxPropertyNamed(BuildState &state, engine::ecs::Entity instance, std::string_view name) {
			if (instance != state.IndexedInstance || state.InstanceIndex == nullptr) {
				const std::span<const engine::ecs::PropertyDescriptor> properties =
					state.Store.PropertiesOf(instance);
				auto [index, inserted] = state.PropertyIndices.try_emplace(properties.data());
				if (inserted) {
					for (const engine::ecs::PropertyDescriptor &property : properties) {
						index->second.try_emplace(property.Spelling, &property);
					}
				}
				state.IndexedInstance = instance;
				state.InstanceIndex = &index->second;
			}
			const auto found = state.InstanceIndex->find(name);
			return found == state.InstanceIndex->end() ? nullptr : found->second;
		}

		void RecordSkip(
			BuildState &state,
			std::string_view className,
			std::string_view propertyName,
			std::string_view reason
		) {
			std::string &key = state.SkipKeyBuffer;
			key.assign(className).append(1, '\0').append(propertyName).append(1, '\0').append(reason);
			const auto found = state.SkippedProperties.find(std::string_view(key));
			if (found != state.SkippedProperties.end()) {
				found->second++;
			} else {
				state.SkippedProperties.emplace(key, 1);
			}
		}

		void SkipProperty(
			BuildState &state,
			const engine::bake::RobloxInstance &instance,
			const engine::bake::RobloxProperty &property,
			std::string_view reason
		) {
			RecordSkip(state, instance.ClassName, property.Name, reason);
		}

		void ApplyRobloxProperty(
			BuildState &state,
			const engine::bake::RobloxInstance &source,
			std::string_view path,
			engine::ecs::Entity instance,
			const engine::bake::RobloxProperty &property,
			std::string_view targetName
		) {
			const engine::ecs::PropertyDescriptor *descriptor =
				RobloxPropertyNamed(state, instance, targetName);
			if (descriptor == nullptr) {
				SkipProperty(state, source, property, "no matching engine property");
				return;
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
					SkipProperty(state, source, property, "collision group is empty or unavailable");
					return;
				}
			}
			engine::game::PropertyValue value;
			if (!Compatible(descriptor->Type, mapped.Kind())) {
				SkipProperty(state, source, property, "source value type is incompatible");
				return;
			}
			if (!ToGameValue(*descriptor, mapped, value)) {
				const char *reason = descriptor->Type == PropertyType::Enum
										 ? "engine enum does not contain value"
										 : "source value cannot be represented";
				SkipProperty(state, source, property, reason);
				return;
			}
			if (!engine::game::WriteAuthoredProperty(state.Store, instance, *descriptor, value)) {
				SkipProperty(state, source, property, "engine property rejected value");
				return;
			}
			state.Report.Properties++;
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
		);

		const engine::bake::RobloxValue *PropertyValueOf(
			const engine::bake::RobloxInstance &node, std::string_view name, RobloxValueKind kind
		) {
			for (const engine::bake::RobloxProperty &property : node.Properties) {
				if (property.Name == name && property.Value.Kind() == kind) {
					return &property.Value;
				}
			}
			return nullptr;
		}

		// Rebuilds a Roblox terrain's voxels as anchored box parts in a
		// `TerrainVoxels` model beside the Terrain instance. A malformed grid
		// is a note, not a failed import.
		void BuildRobloxTerrain(
			BuildState &state,
			const engine::bake::RobloxInstance &terrain,
			const std::string &path,
			engine::ecs::Entity parent
		) {
			const engine::bake::RobloxValue *grid =
				PropertyValueOf(terrain, "SmoothGrid", RobloxValueKind::Text);
			if (grid == nullptr) {
				return;
			}
			RobloxTerrainVoxels voxels;
			std::string failure;
			if (!DecodeRobloxSmoothGrid(
					grid->As<std::string>(), state.Options.MaximumTerrainParts, voxels, failure
				)) {
				state.Report.Notes.push_back(path + ": " + failure);
				return;
			}
			state.Report.TerrainChunks += voxels.Chunks;
			state.Report.TerrainVoxels += voxels.SolidVoxels;
			state.Report.TerrainTruncated = state.Report.TerrainTruncated || voxels.Truncated;
			state.Report.TerrainVoxelStuds = std::max(state.Report.TerrainVoxelStuds, voxels.VoxelStuds);
			if (voxels.UnknownMaterialVoxels > 0) {
				state.Report.Notes.push_back(
					path + ": " + std::to_string(voxels.UnknownMaterialVoxels) +
					" terrain voxels use material ids this importer has no name for; drawn grey"
				);
			}
			if (voxels.VoxelStuds > 4.0f) {
				state.Report.Notes.push_back(
					path + ": terrain coarsened to " + std::to_string(static_cast<int>(voxels.VoxelStuds)) +
					"-stud cells to fit " + std::to_string(state.Options.MaximumTerrainParts) + " box parts"
				);
			}
			if (voxels.Truncated) {
				state.Report.Notes.push_back(
					path + ": terrain needed more than " + std::to_string(state.Options.MaximumTerrainParts) +
					" box parts and was cut off"
				);
			}
			if (voxels.Boxes.empty()) {
				return;
			}

			const engine::bake::RobloxValue *colors =
				PropertyValueOf(terrain, "MaterialColors", RobloxValueKind::Text);
			const std::string_view materialColors =
				colors != nullptr ? std::string_view(colors->As<std::string>()) : "";
			const engine::bake::RobloxValue *waterColor =
				PropertyValueOf(terrain, "WaterColor", RobloxValueKind::Color3);
			const engine::bake::RobloxValue *waterTransparency =
				PropertyValueOf(terrain, "WaterTransparency", RobloxValueKind::Number);

			engine::bake::RobloxInstance container;
			container.ClassName = "Model";
			container.Name = "TerrainVoxels";
			const std::string containerPath = path + "Voxels";
			const engine::ecs::Entity model = BuildRobloxInstance(state, container, containerPath);
			if (model == engine::ecs::NULL_ENTITY) {
				return;
			}
			state.Store.SetParent(model, parent);

			const auto add = [](engine::bake::RobloxInstance &part, std::string name, auto value) {
				engine::bake::RobloxProperty property;
				property.Name = std::move(name);
				property.Value.Set(std::move(value));
				part.Properties.push_back(std::move(property));
			};
			for (const RobloxTerrainBox &box : voxels.Boxes) {
				const bool water = box.Material == 1;
				engine::bake::RobloxInstance part;
				part.ClassName = "Part";
				part.Name = std::string(RobloxTerrainMaterialName(box.Material));
				add(part, "Size", box.Maximum - box.Minimum);
				add(part, "CFrame", engine::core::CFrame((box.Minimum + box.Maximum) * 0.5f));
				add(part, "Anchored", true);
				add(part,
					"Color",
					water && waterColor != nullptr
						? waterColor->As<engine::core::Color3>()
						: RobloxTerrainMaterialColor(box.Material, materialColors));
				if (water) {
					add(part,
						"Transparency",
						waterTransparency != nullptr ? waterTransparency->As<double>() : 0.3);
					add(part, "CanCollide", false);
					add(part, "CastShadow", false);
				}
				const engine::ecs::Entity built =
					BuildRobloxInstance(state, part, containerPath + "/" + part.Name);
				if (built == engine::ecs::NULL_ENTITY) {
					return;
				}
				state.Store.SetParent(built, model);
				state.Report.TerrainParts++;
			}
		}

		engine::ecs::Entity BuildRobloxInstance(
			BuildState &state,
			const engine::bake::RobloxInstance &node,
			const std::string &path,
			engine::ecs::Entity reuse
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

			const PartShape *shape = state.Classes.contains(node.ClassName) ? nullptr : PartShapeOf(node);
			if (instance == NULL_ENTITY) {
				const auto [resolution, fallback] = ResolveCached(state, node);
				instance = state.Store.CreateInstance(resolution.Id, node.Name);
				if (instance != NULL_ENTITY && fallback) {
					state.FolderFallbackClasses[node.ClassName]++;
				}
				if (instance != NULL_ENTITY && resolution.Substitution != nullptr) {
					AddSubstitution(
						state.Substitutions,
						resolution.Substitution->Source,
						resolution.Substitution->Target,
						resolution.Substitution->Note
					);
				}
			}
			if (instance == NULL_ENTITY) {
				state.Error = "the world refused instance " + path;
				return NULL_ENTITY;
			}
			state.Report.Instances++;

			const engine::ecs::ClassId instanceClass = state.Store.ClassOf(instance);
			const bool shaped = shape != nullptr &&
								instanceClass == engine::ecs::Classes::Find(engine::core::Name("MeshPart"));
			for (const engine::bake::RobloxProperty &property : node.Properties) {
				if (&property == sourceProperty ||
					ConsumedProperty(node, shaped ? shape : nullptr, property.Name)) {
					continue;
				}
				ApplyRobloxProperty(
					state,
					node,
					path,
					instance,
					shaped ? ShapedProperty(*shape, property) : property,
					ImportedPropertyName(node.ClassName, instanceClass, property.Name)
				);
			}
			if (shaped) {
				const auto text = [](std::string_view name, std::string_view value) {
					engine::bake::RobloxProperty property;
					property.Name = std::string(name);
					property.Value.Set(std::string(value));
					return property;
				};
				ApplyRobloxProperty(
					state, node, path, instance, text("CollisionShape", shape->Collision), "CollisionShape"
				);
				if (!shape->Mesh.empty()) {
					ApplyRobloxProperty(state, node, path, instance, text("MeshId", shape->Mesh), "MeshId");
				}
				if (shape->Collision == "Hull") {
					ApplyRobloxProperty(
						state,
						node,
						path,
						instance,
						text("CollisionGeometry", shape->Mesh),
						"CollisionGeometry"
					);
				}
				const engine::bake::RobloxInstance *special =
					shape->FromSpecialMesh ? SpecialMeshOf(node) : nullptr;
				if (special != nullptr && shape->Mesh.empty()) {
					// Read through the child's own path so asset mappings apply.
					const std::string specialPath = path + "/" + special->Name;
					for (const engine::bake::RobloxProperty &property : special->Properties) {
						if (property.Name == "MeshId" || property.Name == "TextureId") {
							ApplyRobloxProperty(
								state,
								*special,
								specialPath,
								instance,
								property,
								property.Name == "MeshId" ? "MeshId" : "TextureID"
							);
						}
					}
				}
			}
			for (const std::string &tag : node.Tags) {
				if (engine::scene::AddTag(state.Store, instance, engine::core::Name(tag))) {
					state.Report.Tags++;
				} else {
					RecordSkip(
						state, node.ClassName, "Tags", "instance cannot hold tags, or the tag table is full"
					);
				}
			}
			for (const engine::bake::RobloxProperty &attribute : node.Attributes) {
				engine::ecs::AttributeValue value;
				if (!ToAttribute(attribute.Value, value)) {
					RecordSkip(
						state,
						node.ClassName,
						"Attributes." + attribute.Name,
						"attribute value cannot be represented"
					);
				} else if (!engine::ecs::SetAttribute(
							   state.Store, instance, engine::core::Name(attribute.Name), value
						   )) {
					RecordSkip(
						state, node.ClassName, "Attributes." + attribute.Name, "engine refused the attribute"
					);
				} else {
					state.Report.Attributes++;
				}
			}
			if (state.Options.DisableScripts &&
				(node.ClassName == "Script" || node.ClassName == "LocalScript")) {
				state.Store.Set(instance, engine::script::Disabled{});
				state.Report.DisabledScripts++;
			}

			const bool specialConsumed = shaped && shape->FromSpecialMesh;
			for (const engine::bake::RobloxInstance &child : node.Children) {
				if (specialConsumed && &child == SpecialMeshOf(node)) {
					state.Report.Instances++;
					continue;
				}
				const std::string childPath = path + "/" + child.Name;
				if (child.ClassName == "SurfaceAppearance" && IsBasePart(instanceClass)) {
					state.Report.Instances++;
					AddSubstitution(
						state.Substitutions,
						"SurfaceAppearance",
						engine::ecs::Classes::Describe(instanceClass).Name.Text(),
						"surface properties were applied to the parent; material behaviour is approximated"
					);
					for (const engine::bake::RobloxProperty &property : child.Properties) {
						ApplyRobloxProperty(
							state,
							child,
							childPath,
							instance,
							property,
							SurfaceAppearancePropertyName(property.Name)
						);
					}
					continue;
				}
				if (DroppedClass(state.Classes, child.ClassName)) {
					AddSubstitution(state.Substitutions, child.ClassName, "(dropped)", DROPPED_JOINT_NOTE);
					continue;
				}
				// A singleton the world already made, such as `Workspace.Terrain`,
				// takes the source's properties instead of gaining a twin.
				Entity existing = NULL_ENTITY;
				const ClassResolution childClass = ResolveCached(state, child).first;
				if (childClass.Id.IsValid() && !engine::ecs::Classes::Describe(childClass.Id).Creatable) {
					existing = state.Store.FindFirstChildOfClass(instance, childClass.Id);
				}
				const Entity built = BuildRobloxInstance(state, child, childPath, existing);
				if (built != NULL_ENTITY && existing == NULL_ENTITY) {
					state.Store.SetParent(built, instance);
				}
				if (built != NULL_ENTITY && child.ClassName == "Terrain" && state.Options.ConvertTerrain) {
					BuildRobloxTerrain(state, child, childPath, instance);
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
		ClassCounts missingClasses;
		std::map<GapKey, size_t> missingProperties;
		std::map<GapKey, size_t> conflictingProperties;
		std::map<SubstitutionKey, size_t> substitutions;

		// Property lookup by spelling, built once per class instead of a
		// linear scan per property occurrence.
		using PropertyIndex = std::unordered_map<std::string_view, const engine::ecs::PropertyDescriptor *>;
		std::unordered_map<uint32_t, PropertyIndex> indices;
		const auto propertyNamed = [&](engine::ecs::ClassId classId, std::string_view name) {
			auto [found, inserted] = indices.try_emplace(classId.Index);
			if (inserted) {
				for (const engine::ecs::PropertyDescriptor &property :
					 engine::ecs::Classes::Describe(classId).Properties) {
					found->second.try_emplace(property.Spelling, &property);
				}
			}
			const auto property = found->second.find(name);
			return property == found->second.end() ? nullptr : property->second;
		};

		const auto visit = [&](const auto &self,
							   const engine::bake::RobloxInstance &instance,
							   engine::ecs::ClassId parentClass) -> void {
			classCounts[instance.ClassName]++;
			if (DroppedClass(mappedClasses, instance.ClassName)) {
				AddSubstitution(substitutions, instance.ClassName, "(dropped)", DROPPED_JOINT_NOTE);
				return;
			}
			const ClassResolution resolution = ResolveNode(mappedClasses, instance);
			const PartShape *shape =
				mappedClasses.contains(instance.ClassName) ? nullptr : PartShapeOf(instance);
			if (resolution.Substitution != nullptr) {
				AddSubstitution(
					substitutions,
					resolution.Substitution->Source,
					resolution.Substitution->Target,
					resolution.Substitution->Note
				);
			}
			const bool surfaceAppearance =
				instance.ClassName == "SurfaceAppearance" && IsBasePart(parentClass);
			const engine::ecs::ClassId classId = surfaceAppearance ? parentClass : resolution.Id;
			if (!surfaceAppearance && UsesFolderFallback(mappedClasses, instance)) {
				missingClasses[instance.ClassName]++;
			}
			if (surfaceAppearance) {
				AddSubstitution(
					substitutions,
					"SurfaceAppearance",
					engine::ecs::Classes::Describe(parentClass).Name.Text(),
					"surface properties were applied to the parent; material behaviour is approximated"
				);
			}
			if (classId.IsValid()) {
				for (const engine::bake::RobloxProperty &property : instance.Properties) {
					if (ConsumedProperty(instance, shape, property.Name)) {
						continue;
					}
					const engine::ecs::PropertyDescriptor *descriptor = propertyNamed(
						classId,
						surfaceAppearance ? SurfaceAppearancePropertyName(property.Name)
										  : ImportedPropertyName(instance.ClassName, classId, property.Name)
					);
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

			const engine::bake::RobloxInstance *consumed =
				shape != nullptr && shape->FromSpecialMesh ? SpecialMeshOf(instance) : nullptr;
			for (const engine::bake::RobloxInstance &child : instance.Children) {
				if (&child == consumed) {
					classCounts[child.ClassName]++;
					continue;
				}
				self(self, child, classId);
			}
		};

		for (const engine::bake::RobloxInstance &root : model.Roots) {
			if (EmptyUnknownRoot(mappedClasses, root)) {
				classCounts[root.ClassName]++;
				AddSubstitution(
					substitutions,
					"empty service roots",
					"(skipped)",
					"service shells this engine has no class for"
				);
				continue;
			}
			visit(visit, root, {});
		}

		RobloxImportAnalysis analysis;
		analysis.Classes = classCounts.size();
		for (const auto &[className, count] : classCounts) {
			analysis.Instances += count;
		}
		for (const auto &[className, count] : missingClasses) {
			analysis.MissingClasses.push_back({className, count});
		}
		analysis.MissingProperties = FinishGaps(missingProperties);
		analysis.ConflictingProperties = FinishGaps(conflictingProperties);
		analysis.Substitutions = FinishSubstitutions(substitutions);

		// Counted under views into the model first; a place can lose millions
		// of values and only the distinct groups need strings of their own.
		std::map<std::tuple<std::string_view, std::string_view, std::string_view, std::string_view>, size_t>
			counted;
		for (const engine::bake::RobloxLostProperty &property : model.LostProperties) {
			counted[{property.ClassName, property.PropertyName, property.RobloxType, property.Reason}] +=
				property.Count;
		}
		std::map<SkipKey, size_t> lost;
		for (const auto &[key, count] : counted) {
			const auto &[className, propertyName, type, reason] = key;
			lost[{
				std::string(className),
				std::string(propertyName),
				std::string(type) + ": " + std::string(reason)
			}] += count;
		}
		analysis.LostProperties = FinishSkippedProperties(lost);
		analysis.ScriptFindings = LintRobloxScripts(model);
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

		BuildState state{
			store, replacements, mappedClasses, candidate, options, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}
		};
		std::vector<std::string_view> skippedRoots;
		for (const engine::bake::RobloxInstance &root : model.Roots) {
			const engine::ecs::Entity existing = store.FindFirstRoot(root.Name);
			if (existing == engine::ecs::NULL_ENTITY && EmptyUnknownRoot(mappedClasses, root)) {
				skippedRoots.push_back(root.ClassName);
				continue;
			}
			if (existing != engine::ecs::NULL_ENTITY) {
				candidate.ReusedRoots++;
				const engine::ecs::ClassId existingClass = store.ClassOf(existing);
				const ClassResolution wantedClass = ResolveClass(mappedClasses, root.ClassName);
				if (wantedClass.Id.IsValid() && existingClass != wantedClass.Id) {
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
		if (!skippedRoots.empty()) {
			std::string note = std::to_string(skippedRoots.size()) +
							   " empty Roblox service roots this engine has no class for were skipped:";
			for (const std::string_view name : skippedRoots) {
				note += " " + std::string(name);
			}
			candidate.Notes.push_back(std::move(note));
		}
		candidate.FolderFallbackClasses = FinishClassCounts(state.FolderFallbackClasses);
		std::map<SkipKey, size_t> skipped;
		for (const auto &[key, count] : state.SkippedProperties) {
			const size_t first = key.find('\0');
			const size_t second = key.find('\0', first + 1);
			skipped[{
				key.substr(0, first), key.substr(first + 1, second - first - 1), key.substr(second + 1)
			}] += count;
		}
		candidate.SkippedProperties = FinishSkippedProperties(skipped);
		candidate.Substitutions = FinishSubstitutions(state.Substitutions);

		out = std::move(candidate);
		error.clear();
		return true;
	}

	namespace {
		// Writes `universe`'s world to `destination` and reads it back, so a
		// port only succeeds when the result loads.
		bool ExportAndValidate(
			engine::world::Universe &universe,
			engine::world::WorldId world,
			const std::filesystem::path &destination,
			std::string &error
		) {
			if (!engine::game::ExportWorld(universe, world, destination, error)) {
				return false;
			}
			engine::world::Universe validation;
			if (!engine::game::ImportWorld(validation, destination, engine::core::Name{}, error).IsValid()) {
				error = "the written world did not round-trip: " + error;
				return false;
			}
			return true;
		}

		bool
		ReadRojoProjectFile(const std::filesystem::path &source, RojoProject &project, std::string &error) {
			std::ifstream in(source, std::ios::binary);
			if (!in) {
				error = "could not read " + source.string();
				return false;
			}
			std::ostringstream text;
			text << in.rdbuf();
			if (!ParseRojoProject(text.str(), project, error)) {
				error = "not a Rojo project: " + error;
				return false;
			}
			return true;
		}

		// A Rojo project through the same port, optionally on top of a place.
		bool PortRojoProject(
			const std::filesystem::path &source,
			const std::filesystem::path &destination,
			RobloxWorldPortResult &out,
			std::string &error,
			const RobloxImportOptions &options
		) {
			engine::world::Universe universe;
			engine::world::WorldSettings settings;
			const std::string stem = destination.stem().string();
			settings.Name = engine::core::Name(stem.empty() ? "RojoPort" : stem);
			const engine::world::WorldId world = universe.Create(settings);
			if (!world.IsValid()) {
				error = "could not create the destination world";
				return false;
			}

			RobloxWorldPortResult candidate;
			bool built = false;
			const engine::world::WorldStatus status = universe.Enter(world, [&](engine::ecs::Store &store) {
				built = ImportRobloxRojoProject(store, source, options, candidate, error);
			});
			if (status != engine::world::WorldStatus::Ok || !built) {
				if (error.empty()) error = "the destination world became unavailable";
				return false;
			}
			if (!ExportAndValidate(universe, world, destination, error)) {
				return false;
			}
			out = std::move(candidate);
			error.clear();
			return true;
		}
	}

	bool ImportRobloxRojoProject(
		engine::ecs::Store &store,
		const std::filesystem::path &project,
		const RobloxImportOptions &options,
		RobloxWorldPortResult &out,
		std::string &error
	) {
		RojoProject parsed;
		if (!ReadRojoProjectFile(project, parsed, error)) {
			return false;
		}

		// What the store held before this call, so the lint below covers only
		// what this sync staged and not scripts a live world already had.
		std::unordered_map<std::string, std::string> before;
		if (const auto *cache = store.Resource<engine::script::SourceCache>(); cache != nullptr) {
			for (const engine::script::SourceRow &row : cache->Entries()) {
				before.emplace(std::string(row.Path.Text()), row.Text);
			}
		}

		RobloxWorldPortResult candidate;
		engine::scene::InstallServices(store);
		engine::gui::InstallGuiServices(store);
		if (!options.BasePlace.empty()) {
			std::vector<std::byte> bytes;
			engine::bake::RobloxModel place;
			if (!ReadBytes(options.BasePlace, bytes, error) ||
				!engine::bake::ReadRobloxFile(bytes, place, error)) {
				error = options.BasePlace.string() + ": " + error;
				return false;
			}
			RobloxAssetMappings assets;
			RobloxClassMappings classes;
			std::string ignored;
			(void)LoadRobloxAssetMappings(assets, ignored);
			(void)LoadRobloxClassMappings(classes, ignored);
			candidate.Analysis = AnalyzeRobloxImport(place, classes);
			if (!ImportRobloxPlace(store, place, assets, classes, candidate.Import, error, options)) {
				return false;
			}
		}

		RojoSyncReport report;
		if (!SyncRojoProject(parsed, project.parent_path(), store, report, error)) {
			return false;
		}
		candidate.Import.Instances += report.Instances;
		candidate.Import.Scripts += report.Scripts;
		candidate.MissingPaths = report.Missing;
		candidate.Import.Notes.insert(candidate.Import.Notes.end(), report.Notes.begin(), report.Notes.end());

		// Every program this call staged, the place's and the project's,
		// linted once. A row whose text did not change was already there.
		engine::bake::RobloxModel scripts;
		if (const auto *cache = store.Resource<engine::script::SourceCache>(); cache != nullptr) {
			for (const engine::script::SourceRow &row : cache->Entries()) {
				const auto prior = before.find(std::string(row.Path.Text()));
				if (prior != before.end() && prior->second == row.Text) {
					continue;
				}
				scripts.Scripts.push_back({std::string(row.Path.Text()), "Script", row.Text});
			}
		}
		candidate.Analysis.ScriptFindings = LintRobloxScripts(scripts);
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
		RobloxPortTimings timings;
		auto mark = std::chrono::steady_clock::now();
		// Seconds since the last call, for the phase that just ended.
		const auto lap = [&mark]() {
			const auto now = std::chrono::steady_clock::now();
			const double seconds = std::chrono::duration<double>(now - mark).count();
			mark = now;
			return seconds;
		};
		// Process peak resident set so far, in MiB.
		const auto peak = []() {
			rusage usage{};
			getrusage(RUSAGE_SELF, &usage);
			return static_cast<double>(usage.ru_maxrss) / 1024.0;
		};

		if (source.extension() == ".json") {
			return PortRojoProject(source, destination, out, error, options);
		}

		std::vector<std::byte> bytes;
		if (!ReadBytes(source, bytes, error)) {
			return false;
		}

		engine::bake::RobloxModel model;
		if (!engine::bake::ReadRobloxFile(bytes, model, error)) {
			return false;
		}
		decltype(bytes){}.swap(bytes);

		timings.Read = lap();
		timings.PeakAfterRead = peak();
		RobloxWorldPortResult candidate;
		candidate.Analysis = AnalyzeRobloxImport(model, classMappings);
		timings.Analyze = lap();
		timings.PeakAfterAnalyze = peak();

		// A large place holds gigabytes at each stage, so every stage is
		// released before the next one starts: the file bytes after decoding,
		// the decoded tree after import, the built world after export.
		std::optional<engine::world::Universe> universe(std::in_place);
		const std::string stem = destination.stem().string();
		engine::world::WorldSettings settings;
		settings.Name = engine::core::Name(stem.empty() ? "RobloxPort" : stem);
		const engine::world::WorldId world = universe->Create(settings);
		if (!world.IsValid()) {
			error = "could not create the destination world";
			return false;
		}

		const engine::world::WorldStatus status = universe->Enter(world, [&](engine::ecs::Store &store) {
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
		model = {};
		timings.Import = lap();
		timings.PeakAfterImport = peak();
		if (!engine::game::ExportWorld(*universe, world, destination, error)) {
			return false;
		}
		universe.reset();
		timings.Export = lap();
		timings.PeakAfterExport = peak();
		engine::world::Universe validation;
		if (!engine::game::ImportWorld(validation, destination, engine::core::Name{}, error).IsValid()) {
			error = "the written world did not round-trip: " + error;
			return false;
		}

		timings.Validate = lap();
		timings.PeakAfterValidate = peak();
		candidate.Timings = timings;
		out = std::move(candidate);
		error.clear();
		return true;
	}

	std::string FormatRobloxPortReport(const RobloxWorldPortResult &report, size_t rows) {
		const RobloxImportAnalysis &analysis = report.Analysis;
		const RobloxImportResult &import = report.Import;
		std::string out;
		const auto line = [&out](std::string text) {
			out += text;
			out += '\n';
		};
		// Prints the first `rows` entries of a section, largest first.
		const auto section = [&](std::string_view title, auto entries, auto count, auto describe) {
			if (entries.empty()) {
				return;
			}
			std::stable_sort(entries.begin(), entries.end(), [&](const auto &left, const auto &right) {
				return count(left) > count(right);
			});
			line(std::string(title) + " (" + std::to_string(entries.size()) + ")");
			for (size_t index = 0; index < entries.size() && index < rows; index++) {
				line("  " + std::to_string(count(entries[index])) + "  " + describe(entries[index]));
			}
			if (entries.size() > rows) {
				line("  ... " + std::to_string(entries.size() - rows) + " more");
			}
		};

		if (analysis.Instances > 0)
			line(
				std::to_string(analysis.Instances) + " instances, " + std::to_string(analysis.Classes) +
				" classes, " + std::to_string(analysis.MissingClasses.size()) + " missing classes, " +
				std::to_string(analysis.MissingProperties.size()) + " missing properties, " +
				std::to_string(analysis.ConflictingProperties.size()) + " type conflicts, " +
				std::to_string(analysis.Substitutions.size()) + " approximations"
			);
		line(
			"imported " + std::to_string(import.Instances) + " instances, " +
			std::to_string(import.Properties) + " properties, " + std::to_string(import.Scripts) +
			" scripts (" + std::to_string(import.DisabledScripts) + " disabled), " +
			std::to_string(import.Tags) + " tags, " + std::to_string(import.Attributes) + " attributes"
		);
		const RobloxPortTimings &time = report.Timings;
		const bool timed = time.Read + time.Analyze + time.Import + time.Export + time.Validate > 0.0;
		char timing[160];
		std::snprintf(
			timing,
			sizeof(timing),
			"time: read %.2fs, analyze %.2fs, import %.2fs, export %.2fs, validate %.2fs",
			time.Read,
			time.Analyze,
			time.Import,
			time.Export,
			time.Validate
		);
		if (timed) line(timing);
		std::snprintf(
			timing,
			sizeof(timing),
			"peak MiB after: read %.0f, analyze %.0f, import %.0f, export %.0f, validate %.0f",
			time.PeakAfterRead,
			time.PeakAfterAnalyze,
			time.PeakAfterImport,
			time.PeakAfterExport,
			time.PeakAfterValidate
		);
		if (timed) line(timing);
		if (import.TerrainChunks > 0) {
			line(
				"terrain: " + std::to_string(import.TerrainChunks) + " chunks, " +
				std::to_string(import.TerrainVoxels) + " solid voxels, " +
				std::to_string(import.TerrainParts) + " box parts of " +
				std::to_string(static_cast<int>(import.TerrainVoxelStuds)) + "-stud cells" +
				(import.TerrainTruncated ? " (cut off at the part cap)" : "")
			);
		}

		section(
			"missing classes, imported as Folder",
			analysis.MissingClasses,
			[](const RobloxClassGap &gap) { return gap.Instances; },
			[](const RobloxClassGap &gap) { return gap.ClassName; }
		);
		section(
			"approximated classes",
			analysis.Substitutions,
			[](const RobloxClassSubstitution &entry) { return entry.Instances; },
			[](const RobloxClassSubstitution &entry) {
				return entry.SourceClass + " -> " + entry.TargetClass + ": " + entry.Note;
			}
		);
		section(
			"missing properties",
			analysis.MissingProperties,
			[](const RobloxPropertyGap &gap) { return gap.Occurrences; },
			[](const RobloxPropertyGap &gap) {
				return gap.ClassName + "." + gap.PropertyName + " (" + gap.SourceType + ")";
			}
		);
		section(
			"type conflicts",
			analysis.ConflictingProperties,
			[](const RobloxPropertyGap &gap) { return gap.Occurrences; },
			[](const RobloxPropertyGap &gap) {
				return gap.ClassName + "." + gap.PropertyName + " " + gap.SourceType + " -> " +
					   gap.ExpectedType;
			}
		);
		section(
			"lost values",
			analysis.LostProperties,
			[](const RobloxPropertySkip &skip) { return skip.Occurrences; },
			[](const RobloxPropertySkip &skip) {
				return skip.ClassName + "." + skip.PropertyName + " (" + skip.Reason + ")";
			}
		);
		section(
			"refused during import",
			import.SkippedProperties,
			[](const RobloxPropertySkip &skip) { return skip.Occurrences; },
			[](const RobloxPropertySkip &skip) {
				return skip.ClassName + "." + skip.PropertyName + " (" + skip.Reason + ")";
			}
		);

		// Script findings grouped by what was named, counting scripts.
		struct Group {
			std::string Label;
			size_t Scripts = 0;
			std::string Example;
		};
		std::map<std::string, Group> groups;
		for (const RobloxScriptFinding &finding : analysis.ScriptFindings) {
			Group &group = groups[finding.Kind + " " + finding.Name];
			if (group.Scripts++ == 0) {
				group.Label = finding.Kind + " " + finding.Name + ": " + finding.Message;
				group.Example = finding.InstancePath + ":" + std::to_string(finding.Line);
			}
		}
		std::vector<Group> grouped;
		for (auto &[key, group] : groups) {
			grouped.push_back(std::move(group));
		}
		section(
			"script uses the engine lacks (scripts affected)",
			grouped,
			[](const Group &group) { return group.Scripts; },
			[](const Group &group) { return group.Label + "  e.g. " + group.Example; }
		);

		for (const std::string &missing : report.MissingPaths) {
			line("no such path: " + missing);
		}
		for (const std::string &note : import.Notes) {
			line("note: " + note);
		}
		return out;
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
			"%zu instances   %zu classes   %zu scripts   %zu asset ids   %zu lost value groups",
			RobloxAnalysis.Instances,
			RobloxAnalysis.Classes,
			model.Scripts.size(),
			RobloxChoices.size(),
			model.LostProperties.size()
		);
		// **A new world, never the open one.** A place is a whole scene with
		// its own services, and writing it into the current world would mix
		// two scenes and change service properties no undo puts back. A world
		// of its own is saved as its own `.aworld`, and removing it takes the
		// whole import back. Allowed during a run: Stop restores only the
		// running world and a world added since is in no snapshot.
		const bool mayImport = Universe != nullptr && !RobloxImportApplied;
		ImGui::Checkbox("Import Roblox scripts disabled", &RobloxImportDisableScripts);
		ImGui::SameLine();
		ImGui::Checkbox("Terrain as parts", &RobloxImportConvertTerrain);
		ImGui::SameLine();
		ImGui::BeginDisabled(!mayImport);
		if (ImGui::Button("Import as new world")) {
			// Named after the place file, numbered when the name is taken.
			const std::string stem = std::filesystem::path(RobloxImportPath).stem().string();
			const std::string base = stem.empty() ? std::string("Roblox Place") : stem;
			std::string name = base;
			for (size_t number = 2;; number++) {
				const auto worlds = Universe->Worlds();
				if (std::none_of(worlds.begin(), worlds.end(), [&](WorldId id) {
						return Universe->NameOf(id).Text() == name;
					})) {
					break;
				}
				name = base + " " + std::to_string(number);
			}

			RobloxImportResult report;
			std::string error;
			const WorldId world = AddWorld(engine::core::Name(name));
			engine::world::WorldStatus status = engine::world::WorldStatus::Ok;
			if (world.IsValid()) {
				status = Universe->Enter(world, [&](engine::ecs::Store &store) {
					RobloxImportOptions options;
					options.DisableScripts = RobloxImportDisableScripts;
					options.ConvertTerrain = RobloxImportConvertTerrain;
					ImportRobloxPlace(store, model, RobloxMappings, RobloxClassMap, report, error, options);
				});
			}
			if (!world.IsValid()) {
				RobloxImportStatus = "could not create a world for the place";
			} else if (status != engine::world::WorldStatus::Ok || !error.empty()) {
				// A half-built world is not left behind.
				RobloxImportStatus = error.empty() ? std::string("the new world became unavailable") : error;
				RemoveWorld(world);
			} else {
				RobloxImportApplied = true;
				MarkModified();
				Touch(world);
				PendingActivate = world;
				RobloxImportStatus =
					"imported into new world '" + name + "': " + std::to_string(report.Instances) +
					" instances, " + std::to_string(report.Scripts) + " scripts staged, " +
					std::to_string(report.DisabledScripts) + " scripts disabled, " +
					std::to_string(report.Properties) + " properties applied, " +
					std::to_string(report.FolderFallbackClasses.size()) + " fallback class groups, " +
					std::to_string(report.Substitutions.size()) + " approximated class groups, " +
					std::to_string(report.SkippedProperties.size()) + " skipped property groups, " +
					std::to_string(report.TerrainParts) + " terrain parts";
				for (const std::string &note : report.Notes) {
					RobloxImportStatus += "; " + note;
				}
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
				if (!RobloxAnalysis.Substitutions.empty()) {
					ImGui::TextUnformatted("Approximations");
					if (ImGui::BeginTable(
							"##roblox-substitutions", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
						)) {
						ImGui::TableSetupColumn("Roblox class");
						ImGui::TableSetupColumn("Engine class");
						ImGui::TableSetupColumn(
							"Uses", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(65.0f)
						);
						ImGui::TableSetupColumn("Approximation");
						ImGui::TableHeadersRow();
						for (const RobloxClassSubstitution &substitution : RobloxAnalysis.Substitutions) {
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(substitution.SourceClass.c_str());
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(substitution.TargetClass.c_str());
							ImGui::TableNextColumn();
							ImGui::Text("%zu", substitution.Instances);
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(substitution.Note.c_str());
						}
						ImGui::EndTable();
					}
				}
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
						5,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
							ImGuiTableFlags_Resizable,
						ImVec2(0.0f, 0.0f)
					)) {
					ImGui::TableSetupColumn("First path");
					ImGui::TableSetupColumn("Class");
					ImGui::TableSetupColumn("Property");
					ImGui::TableSetupColumn("Roblox type");
					ImGui::TableSetupColumn(
						"Uses", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(65.0f)
					);
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
							ImGui::TextUnformatted(
								lost.RobloxType.data(), lost.RobloxType.data() + lost.RobloxType.size()
							);
							ImGui::TableNextColumn();
							ImGui::Text("%zu", lost.Count);
						}
					}
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}

			const std::string lintLabel = "Script lint (" +
										  std::to_string(RobloxAnalysis.ScriptFindings.size()) +
										  ")###roblox-script-lint";
			if (ImGui::BeginTabItem(lintLabel.c_str())) {
				if (ImGui::BeginTable(
						"##roblox-script-findings",
						4,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
							ImGuiTableFlags_Resizable,
						ImVec2(0.0f, 0.0f)
					)) {
					ImGui::TableSetupColumn("Script");
					ImGui::TableSetupColumn(
						"Line", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(50.0f)
					);
					ImGui::TableSetupColumn("Name");
					ImGui::TableSetupColumn("Problem");
					ImGui::TableHeadersRow();
					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(RobloxAnalysis.ScriptFindings.size()));
					while (clipper.Step()) {
						for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++) {
							const RobloxScriptFinding &finding =
								RobloxAnalysis.ScriptFindings[static_cast<size_t>(index)];
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(finding.InstancePath.c_str());
							ImGui::TableNextColumn();
							ImGui::Text("%zu", finding.Line);
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(finding.Name.c_str());
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(finding.Message.c_str());
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
