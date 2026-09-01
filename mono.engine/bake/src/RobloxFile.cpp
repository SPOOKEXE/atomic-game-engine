#include <engine/bake/RobloxModel.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <rbxl/rbxl.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::bake {

	namespace {
		constexpr size_t MAXIMUM_ROBLOX_FILE_BYTES = 256ull * 1024ull * 1024ull;

		std::string Lowercase(std::string_view text) {
			std::string lowered(text);
			std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return lowered;
		}

		bool IsScript(std::string_view className) {
			return className == "Script" || className == "LocalScript" || className == "ModuleScript";
		}

		RobloxAssetKind AssetKindFor(std::string_view className, std::string_view propertyName) {
			const std::string hint = Lowercase(std::string(className) + "." + std::string(propertyName));
			if (hint.find("animation") != std::string::npos) {
				return RobloxAssetKind::Animation;
			}
			if (hint.find("sound") != std::string::npos || hint.find("audio") != std::string::npos) {
				return RobloxAssetKind::Audio;
			}
			if (hint.find("font") != std::string::npos) {
				return RobloxAssetKind::Font;
			}
			if (hint.find("mesh") != std::string::npos) {
				return RobloxAssetKind::Mesh;
			}
			if (hint.find("video") != std::string::npos) {
				return RobloxAssetKind::Video;
			}
			if (hint.find("texture") != std::string::npos || hint.find("image") != std::string::npos ||
				hint.find("decal") != std::string::npos || hint.find("skybox") != std::string::npos ||
				hint.find("template") != std::string::npos || hint.find("graphic") != std::string::npos) {
				return RobloxAssetKind::Texture;
			}
			return RobloxAssetKind::Unknown;
		}

		bool IsDigits(std::string_view text) {
			return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char character) {
				return std::isdigit(character);
			});
		}

		std::string NumericIdentifier(std::string_view uri) {
			const std::string lowered = Lowercase(uri);
			size_t begin = std::string::npos;
			const size_t scheme = lowered.find("rbxassetid://");
			if (scheme != std::string::npos) {
				begin = scheme + std::string_view("rbxassetid://").size();
			} else {
				const size_t query = lowered.find("id=");
				if (query != std::string::npos) {
					begin = query + 3;
				}
			}

			if (begin == std::string::npos || begin >= uri.size()) {
				return IsDigits(uri) ? std::string(uri) : std::string{};
			}

			size_t end = begin;
			while (end < uri.size() && std::isdigit(static_cast<unsigned char>(uri[end]))) {
				end++;
			}
			return end == begin ? std::string{} : std::string(uri.substr(begin, end - begin));
		}

		void AddAsset(
			RobloxModel &model,
			std::string_view uri,
			RobloxAssetKind kind,
			std::string_view path,
			std::string_view className,
			std::string_view propertyName
		) {
			if (uri.empty()) {
				return;
			}

			const std::string identifier = NumericIdentifier(uri);
			model.Assets.push_back(
				RobloxAssetReference{
					identifier.empty() ? std::string(uri) : identifier,
					std::string(uri),
					kind,
					std::string(path),
					std::string(className),
					std::string(propertyName),
				}
			);
		}

		void FindEmbeddedAssets(
			RobloxModel &model,
			std::string_view text,
			RobloxAssetKind kind,
			std::string_view path,
			std::string_view className,
			std::string_view propertyName
		) {
			const std::string lowered = Lowercase(text);
			static constexpr std::string_view PREFIXES[] = {
				"rbxassetid://",
				"https://www.roblox.com/asset/?id=",
				"http://www.roblox.com/asset/?id=",
				"https://assetdelivery.roblox.com/v1/asset/?id=",
				"http://assetdelivery.roblox.com/v1/asset/?id=",
			};

			for (const std::string_view prefix : PREFIXES) {
				size_t at = 0;
				while ((at = lowered.find(prefix, at)) != std::string::npos) {
					size_t end = at + prefix.size();
					while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
						end++;
					}
					if (end > at + prefix.size()) {
						AddAsset(model, text.substr(at, end - at), kind, path, className, propertyName);
					}
					at = std::max(end, at + 1);
				}
			}
		}

		engine::core::CFrame ToCFrame(const rbxl::CFrame &source) {
			glm::mat3 rotation(1.0f);
			for (size_t row = 0; row < 3; row++) {
				for (size_t column = 0; column < 3; column++) {
					rotation[column][row] = source.rotation[row * 3 + column];
				}
			}
			return engine::core::CFrame(
				engine::core::Vector3{source.position.x, source.position.y, source.position.z},
				glm::quat_cast(rotation)
			);
		}

		bool ConvertValue(const rbxl::Variant &source, RobloxValue &out) {
			using Type = rbxl::VariantType;
			switch (rbxl::variantTypeOf(source)) {
			case Type::String:
				out.Kind = RobloxValueKind::Text;
				out.Text = std::get<std::string>(source);
				return true;
			case Type::Bool:
				out.Kind = RobloxValueKind::Bool;
				out.Bool = std::get<bool>(source);
				return true;
			case Type::Int32:
				out.Kind = RobloxValueKind::Integer;
				out.Integer = std::get<int32_t>(source);
				return true;
			case Type::Int64:
				out.Kind = RobloxValueKind::Integer;
				out.Integer = std::get<int64_t>(source);
				return true;
			case Type::Float32:
				out.Kind = RobloxValueKind::Number;
				out.Number = std::get<float>(source);
				return true;
			case Type::Float64:
				out.Kind = RobloxValueKind::Number;
				out.Number = std::get<double>(source);
				return true;
			case Type::UDim: {
				const rbxl::UDim &value = std::get<rbxl::UDim>(source);
				out.Kind = RobloxValueKind::UDim;
				out.UDim = {value.scale, static_cast<float>(value.offset)};
				return true;
			}
			case Type::UDim2: {
				const rbxl::UDim2 &value = std::get<rbxl::UDim2>(source);
				out.Kind = RobloxValueKind::UDim2;
				out.UDim2 = {
					value.x.scale,
					static_cast<float>(value.x.offset),
					value.y.scale,
					static_cast<float>(value.y.offset),
				};
				return true;
			}
			case Type::Color3: {
				const rbxl::Color3 &value = std::get<rbxl::Color3>(source);
				out.Kind = RobloxValueKind::Color3;
				out.Color3 = {value.r, value.g, value.b};
				return true;
			}
			case Type::Color3uint8: {
				const rbxl::Color3uint8 &value = std::get<rbxl::Color3uint8>(source);
				out.Kind = RobloxValueKind::Color3;
				out.Color3 = engine::core::Color3::FromRGB(value.r, value.g, value.b);
				return true;
			}
			case Type::Vector2: {
				const rbxl::Vector2 &value = std::get<rbxl::Vector2>(source);
				out.Kind = RobloxValueKind::Vector2;
				out.Vector2 = {value.x, value.y};
				return true;
			}
			case Type::Vector2int16: {
				const rbxl::Vector2int16 &value = std::get<rbxl::Vector2int16>(source);
				out.Kind = RobloxValueKind::Vector2;
				out.Vector2 = {static_cast<float>(value.x), static_cast<float>(value.y)};
				return true;
			}
			case Type::Vector3: {
				const rbxl::Vector3 &value = std::get<rbxl::Vector3>(source);
				out.Kind = RobloxValueKind::Vector3;
				out.Vector3 = {value.x, value.y, value.z};
				return true;
			}
			case Type::Vector3int16: {
				const rbxl::Vector3int16 &value = std::get<rbxl::Vector3int16>(source);
				out.Kind = RobloxValueKind::Vector3;
				out.Vector3 = {
					static_cast<float>(value.x),
					static_cast<float>(value.y),
					static_cast<float>(value.z),
				};
				return true;
			}
			case Type::CFrame:
				out.Kind = RobloxValueKind::CFrame;
				out.CFrame = ToCFrame(std::get<rbxl::CFrame>(source));
				return true;
			case Type::OptionalCFrame: {
				const rbxl::OptionalCFrame &value = std::get<rbxl::OptionalCFrame>(source);
				if (!value.hasValue) {
					return false;
				}
				out.Kind = RobloxValueKind::CFrame;
				out.CFrame = ToCFrame(value.value);
				return true;
			}
			case Type::Rect: {
				const rbxl::Rect &value = std::get<rbxl::Rect>(source);
				out.Kind = RobloxValueKind::Rect;
				out.Rect = {value.min.x, value.min.y, value.max.x, value.max.y};
				return true;
			}
			case Type::NumberRange: {
				const rbxl::NumberRange &value = std::get<rbxl::NumberRange>(source);
				out.Kind = RobloxValueKind::NumberRange;
				out.NumberRange = {value.min, value.max};
				return true;
			}
			case Type::Content: {
				const rbxl::Content &value = std::get<rbxl::Content>(source);
				if (value.sourceType == rbxl::Content::SourceType::Object) {
					return false;
				}
				out.Kind = RobloxValueKind::Text;
				out.Text = value.uri;
				return true;
			}
			case Type::ContentId:
				out.Kind = RobloxValueKind::Text;
				out.Text = std::get<rbxl::ContentId>(source).url;
				return true;
			case Type::ProtectedString:
				out.Kind = RobloxValueKind::Text;
				out.Text = std::get<rbxl::ProtectedString>(source).value;
				return true;
			case Type::SharedString:
				out.Kind = RobloxValueKind::Text;
				out.Text = std::get<rbxl::SharedString>(source).value;
				return true;
			case Type::NetAssetRef:
				out.Kind = RobloxValueKind::Text;
				out.Text = std::get<rbxl::NetAssetRef>(source).value;
				return true;
			case Type::Nil:
			case Type::Ray:
			case Type::Faces:
			case Type::Axes:
			case Type::BrickColor:
			case Type::Region3:
			case Type::Region3int16:
			case Type::EnumValue:
			case Type::EnumItem:
			case Type::Ref:
			case Type::NumberSequence:
			case Type::ColorSequence:
			case Type::PhysicalProperties:
			case Type::Font:
			case Type::UniqueId:
			case Type::SecurityCapabilities:
			case Type::BinaryString:
			case Type::Bytecode:
				return false;
			}
			return false;
		}

		void AnalyzeText(
			RobloxModel &model,
			std::string_view text,
			std::string_view path,
			std::string_view className,
			std::string_view propertyName,
			bool explicitContent
		) {
			const RobloxAssetKind kind = AssetKindFor(className, propertyName);
			FindEmbeddedAssets(model, text, kind, path, className, propertyName);
			const std::string lowered = Lowercase(text);
			const bool hasEmbeddedNumeric = lowered.find("rbxassetid://") != std::string::npos ||
											lowered.find("id=") != std::string::npos;
			const bool assetScheme = lowered.starts_with("rbxasset://") || lowered.starts_with("http://") ||
									 lowered.starts_with("https://");
			const bool numericAsset = kind != RobloxAssetKind::Unknown && IsDigits(text);
			const bool otherContentScheme = explicitContent && lowered.find("://") != std::string::npos;
			if (!hasEmbeddedNumeric && (assetScheme || numericAsset || otherContentScheme)) {
				AddAsset(model, text, kind, path, className, propertyName);
			}
		}

		bool CheckTree(const rbxl::Dom &dom, std::string &failure) {
			if (dom.instanceCount() > MAXIMUM_ROBLOX_INSTANCES) {
				failure = "Roblox file declares more than " + std::to_string(MAXIMUM_ROBLOX_INSTANCES) +
						  " instances";
				return false;
			}

			std::vector<std::pair<rbxl::InstanceId, uint32_t>> pending;
			pending.reserve(dom.instanceCount());
			for (const rbxl::InstanceId root : dom.roots()) {
				pending.emplace_back(root, 1);
			}
			while (!pending.empty()) {
				const auto [id, depth] = pending.back();
				pending.pop_back();
				if (!dom.valid(id)) {
					failure = "Roblox file contains an invalid instance reference";
					return false;
				}
				if (depth > MAXIMUM_ROBLOX_DEPTH) {
					failure = "Roblox instance tree is deeper than " + std::to_string(MAXIMUM_ROBLOX_DEPTH);
					return false;
				}
				for (const rbxl::InstanceId child : dom.at(id).children) {
					pending.emplace_back(child, depth + 1);
				}
			}
			return true;
		}

		RobloxInstance
		BuildInstance(const rbxl::Dom &dom, rbxl::InstanceId id, std::string path, RobloxModel &model) {
			const rbxl::Instance &source = dom.at(id);
			RobloxInstance instance;
			instance.ClassName = source.className;
			instance.Name = dom.nameOf(id);
			if (instance.Name.empty()) {
				instance.Name = instance.ClassName;
			}
			path = path.empty() ? instance.Name : path + "/" + instance.Name;

			for (const auto &[nameId, sourceValue] : source.properties) {
				const std::string &propertyName = dom.names().name(nameId);
				if (propertyName == "Name") {
					continue;
				}

				const rbxl::VariantType sourceType = rbxl::variantTypeOf(sourceValue);
				bool explicitContent = sourceType == rbxl::VariantType::Content ||
									   sourceType == rbxl::VariantType::ContentId ||
									   sourceType == rbxl::VariantType::NetAssetRef;
				if (sourceType == rbxl::VariantType::Font) {
					const rbxl::Font &font = std::get<rbxl::Font>(sourceValue);
					AnalyzeText(model, font.family, path, source.className, propertyName, true);
				}

				RobloxValue value;
				if (!ConvertValue(sourceValue, value)) {
					model.LostProperties.push_back(
						RobloxLostProperty{
							path,
							source.className,
							propertyName,
							rbxl::variantTypeName(sourceType),
							"the engine-facing Roblox value vocabulary has no lossless representation",
						}
					);
					continue;
				}

				if (value.Kind == RobloxValueKind::Text) {
					AnalyzeText(model, value.Text, path, source.className, propertyName, explicitContent);
				}
				if (IsScript(source.className) && propertyName == "Source" &&
					value.Kind == RobloxValueKind::Text) {
					model.Scripts.push_back(RobloxScript{path, source.className, value.Text});
				}
				instance.Properties.push_back(RobloxProperty{propertyName, std::move(value)});
			}

			instance.Children.reserve(source.children.size());
			for (const rbxl::InstanceId child : source.children) {
				instance.Children.push_back(BuildInstance(dom, child, path, model));
			}
			return instance;
		}
	}

	bool ReadRobloxFile(std::span<const std::byte> bytes, RobloxModel &out, std::string &failure) {
		if (bytes.empty()) {
			failure = "Roblox file is empty";
			return false;
		}
		if (bytes.size() > MAXIMUM_ROBLOX_FILE_BYTES) {
			failure = "Roblox file is larger than the 256 MiB import limit";
			return false;
		}

		const auto *data = reinterpret_cast<const uint8_t *>(bytes.data());
		rbxl::Result<rbxl::Dom> decoded = rbxl::loadBuffer(data, bytes.size());
		if (!decoded) {
			failure = decoded.error().toString();
			return false;
		}

		const rbxl::Dom &dom = decoded.value();
		if (!CheckTree(dom, failure)) {
			return false;
		}

		RobloxModel candidate;
		candidate.Roots.reserve(dom.roots().size());
		std::vector<rbxl::InstanceId> roots = dom.roots();
		std::sort(roots.begin(), roots.end());
		for (const rbxl::InstanceId root : roots) {
			candidate.Roots.push_back(BuildInstance(dom, root, {}, candidate));
		}

		for (const rbxl::RawChunk &chunk : dom.unknownChunks()) {
			candidate.Notes.push_back(
				"preserved an unsupported " + std::string(chunk.name, chunk.name + 4) + " chunk" +
				(chunk.className.empty() ? std::string{} : " for class " + chunk.className)
			);
		}

		out = std::move(candidate);
		return true;
	}
}
