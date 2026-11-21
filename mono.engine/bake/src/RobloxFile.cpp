#include "RobloxEnums.hpp"
#include "RobloxProperties.hpp"

#include <engine/bake/RobloxModel.hpp>
#include <engine/parallel/Jobs.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <rbxl/blob.hpp>
#include <rbxl/rbxl.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
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
			std::string_view lowered,
			RobloxAssetKind kind,
			std::string_view path,
			std::string_view className,
			std::string_view propertyName
		) {
			static constexpr std::string_view PREFIXES[] = {
				"rbxassetid://",
				"https://www.roblox.com/asset/?id=",
				"http://www.roblox.com/asset/?id=",
				"https://assetdelivery.roblox.com/v1/asset/?id=",
				"http://assetdelivery.roblox.com/v1/asset/?id=",
			};

			for (const std::string_view prefix : PREFIXES) {
				size_t at = 0;
				while ((at = lowered.find(prefix, at)) != std::string_view::npos) {
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

		bool ConvertValue(rbxl::Variant &source, RobloxValue &out) {
			using Type = rbxl::VariantType;
			switch (rbxl::variantTypeOf(source)) {
			case Type::String:
				out.Set(std::move(std::get<std::string>(source)));
				return true;
			case Type::Bool:
				out.Set(std::get<bool>(source));
				return true;
			case Type::Int32:
				out.Set(static_cast<int64_t>(std::get<int32_t>(source)));
				return true;
			case Type::Int64:
				out.Set(std::get<int64_t>(source));
				return true;
			case Type::Float32:
				out.Set(static_cast<double>(std::get<float>(source)));
				return true;
			case Type::Float64:
				out.Set(std::get<double>(source));
				return true;
			case Type::UDim: {
				const rbxl::UDim &value = std::get<rbxl::UDim>(source);
				out.Set(engine::core::UDim{value.scale, static_cast<float>(value.offset)});
				return true;
			}
			case Type::UDim2: {
				const rbxl::UDim2 &value = std::get<rbxl::UDim2>(source);
				out.Set(
					engine::core::UDim2{
						value.x.scale,
						static_cast<float>(value.x.offset),
						value.y.scale,
						static_cast<float>(value.y.offset),
					}
				);
				return true;
			}
			case Type::Color3: {
				const rbxl::Color3 &value = std::get<rbxl::Color3>(source);
				out.Set(engine::core::Color3{value.r, value.g, value.b});
				return true;
			}
			case Type::Color3uint8: {
				const rbxl::Color3uint8 &value = std::get<rbxl::Color3uint8>(source);
				out.Set(engine::core::Color3::FromRGB(value.r, value.g, value.b));
				return true;
			}
			case Type::Vector2: {
				const rbxl::Vector2 &value = std::get<rbxl::Vector2>(source);
				out.Set(engine::core::Vector2{value.x, value.y});
				return true;
			}
			case Type::Vector2int16: {
				const rbxl::Vector2int16 &value = std::get<rbxl::Vector2int16>(source);
				out.Set(engine::core::Vector2{static_cast<float>(value.x), static_cast<float>(value.y)});
				return true;
			}
			case Type::Vector3: {
				const rbxl::Vector3 &value = std::get<rbxl::Vector3>(source);
				out.Set(engine::core::Vector3{value.x, value.y, value.z});
				return true;
			}
			case Type::Vector3int16: {
				const rbxl::Vector3int16 &value = std::get<rbxl::Vector3int16>(source);
				out.Set(
					engine::core::Vector3{
						static_cast<float>(value.x),
						static_cast<float>(value.y),
						static_cast<float>(value.z),
					}
				);
				return true;
			}
			case Type::CFrame:
				out.Set(ToCFrame(std::get<rbxl::CFrame>(source)));
				return true;
			case Type::OptionalCFrame: {
				const rbxl::OptionalCFrame &value = std::get<rbxl::OptionalCFrame>(source);
				if (!value.hasValue) {
					return false;
				}
				out.Set(ToCFrame(value.value));
				return true;
			}
			case Type::Rect: {
				const rbxl::Rect &value = std::get<rbxl::Rect>(source);
				out.Set(engine::core::Rect{value.min.x, value.min.y, value.max.x, value.max.y});
				return true;
			}
			case Type::NumberRange: {
				const rbxl::NumberRange &value = std::get<rbxl::NumberRange>(source);
				out.Set(engine::core::NumberRange{value.min, value.max});
				return true;
			}
			case Type::NumberSequence: {
				const auto &points = std::get<rbxl::NumberSequence>(source).keypoints;
				if (points.size() > core::SEQUENCE_CAPACITY) {
					return false;
				}
				RobloxNumberSequence sequence;
				sequence.reserve(points.size());
				for (const auto &point : points) {
					sequence.emplace_back(point.time, point.value, point.envelope);
				}
				out.Set(std::move(sequence));
				return true;
			}
			case Type::ColorSequence: {
				const auto &points = std::get<rbxl::ColorSequence>(source).keypoints;
				if (points.size() > core::SEQUENCE_CAPACITY) {
					return false;
				}
				RobloxColorSequence sequence;
				sequence.reserve(points.size());
				for (const auto &point : points) {
					if (point.envelope != 0.0f) {
						return false;
					}
					sequence.emplace_back(
						point.time, core::Color3{point.color.r, point.color.g, point.color.b}
					);
				}
				out.Set(std::move(sequence));
				return true;
			}
			case Type::Content: {
				rbxl::Content &value = std::get<rbxl::Content>(source);
				if (value.sourceType == rbxl::Content::SourceType::Object) {
					return false;
				}
				out.Set(std::move(value.uri));
				return true;
			}
			case Type::ContentId:
				out.Set(std::move(std::get<rbxl::ContentId>(source).url));
				return true;
			case Type::ProtectedString:
				out.Set(std::move(std::get<rbxl::ProtectedString>(source).value));
				return true;
			case Type::SharedString:
				out.Set(std::move(std::get<rbxl::SharedString>(source).value));
				return true;
			case Type::NetAssetRef:
				out.Set(std::move(std::get<rbxl::NetAssetRef>(source).value));
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
			// Every asset spelling has a scheme (`:`), a query (`=`) or is a bare
			// number. Most text is names and labels with none of them, and
			// skipping it here keeps a large place from lowercasing every string.
			if (text.find(':') == std::string_view::npos && text.find('=') == std::string_view::npos &&
				!IsDigits(text)) {
				return;
			}
			const RobloxAssetKind kind = AssetKindFor(className, propertyName);
			const std::string lowered = Lowercase(text);
			FindEmbeddedAssets(model, text, lowered, kind, path, className, propertyName);
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

		// Where one worker's conversion goes. Lost values are grouped as they
		// arrive, keyed by class, property, type and reason, so a place that
		// loses millions of values holds one row per group.
		constexpr std::string_view NO_REPRESENTATION =
			"the engine-facing Roblox value vocabulary has no lossless representation";

		struct Output {
			RobloxModel Model;
			// Preorder index of every DOM instance, for turning a `Ref` into a
			// `RobloxReference`. Shared and read-only across shards.
			const std::vector<uint32_t> *Preorder = nullptr;
			std::unordered_map<std::string, size_t> LostIndex;

			void Lose(
				std::string_view path,
				std::string_view className,
				std::string_view propertyName,
				std::string_view robloxType,
				std::string_view reason,
				size_t count = 1
			) {
				// Type and reason are static texts, so their addresses name them.
				std::string key;
				key.reserve(className.size() + propertyName.size() + 2 + 2 * sizeof(uintptr_t));
				key.append(className).append(1, '\0').append(propertyName).append(1, '\0');
				for (const uintptr_t text :
					 {reinterpret_cast<uintptr_t>(robloxType.data()),
					  reinterpret_cast<uintptr_t>(reason.data())}) {
					key.append(reinterpret_cast<const char *>(&text), sizeof(text));
				}
				const auto [found, inserted] =
					LostIndex.try_emplace(std::move(key), Model.LostProperties.size());
				if (inserted) {
					Model.LostProperties.push_back(
						RobloxLostProperty{
							std::string(path),
							std::string(className),
							std::string(propertyName),
							robloxType,
							reason,
							count
						}
					);
				} else {
					Model.LostProperties[found->second].Count += count;
				}
			}
		};

		// Decodes the `Tags` or `AttributesSerialize` blob onto `instance`.
		// Returns false when the value is not a blob of the expected shape, so
		// the caller keeps it as an ordinary property.
		bool DecodeBlob(
			const rbxl::Variant &source,
			std::string_view propertyName,
			std::string_view path,
			RobloxInstance &instance,
			Output &output
		) {
			std::vector<uint8_t> bytes;
			if (const auto *text = std::get_if<std::string>(&source)) {
				bytes.assign(text->begin(), text->end());
			} else if (const auto *binary = std::get_if<rbxl::BinaryString>(&source)) {
				bytes = binary->data;
			} else {
				return false;
			}

			if (propertyName == "Tags") {
				auto tags = rbxl::blob::parseTags(bytes);
				if (!tags) {
					return false;
				}
				for (std::string &tag : tags.value()) {
					if (!tag.empty()) {
						instance.Tags.push_back(std::move(tag));
					}
				}
				return true;
			}

			auto attributes = rbxl::blob::parseAttributes(bytes);
			if (!attributes) {
				return false;
			}
			for (auto &[name, attribute] : attributes.value()) {
				RobloxValue value;
				if (!ConvertValue(attribute, value)) {
					output.Lose(
						path,
						instance.ClassName,
						"Attributes." + name,
						rbxl::variantTypeName(rbxl::variantTypeOf(attribute)),
						NO_REPRESENTATION
					);
					continue;
				}
				instance.Attributes.push_back(RobloxProperty{name, std::move(value)});
			}
			return true;
		}

		// Converts one instance's own properties and leaves its children to the
		// caller. `path` arrives as the parent's path and leaves as this one's.
		RobloxInstance BuildNode(rbxl::Dom &dom, rbxl::InstanceId id, std::string &path, Output &output) {
			RobloxModel &model = output.Model;
			rbxl::Instance &source = dom.at(id);
			RobloxInstance instance;
			instance.ClassName = std::move(source.className);
			instance.Name = dom.nameOf(id);
			if (instance.Name.empty()) {
				instance.Name = instance.ClassName;
			}
			path = path.empty() ? instance.Name : path + "/" + instance.Name;
			instance.Properties.reserve(source.properties.size());

			for (auto &[nameId, sourceValue] : source.properties) {
				const std::string propertyName(PublicRobloxPropertyName(dom.names().name(nameId)));
				if (propertyName == "Name") {
					continue;
				}

				const rbxl::VariantType sourceType = rbxl::variantTypeOf(sourceValue);
				bool explicitContent = sourceType == rbxl::VariantType::Content ||
									   sourceType == rbxl::VariantType::ContentId ||
									   sourceType == rbxl::VariantType::NetAssetRef;
				if (sourceType == rbxl::VariantType::Font) {
					const rbxl::Font &font = std::get<rbxl::Font>(sourceValue);
					AnalyzeText(model, font.family, path, instance.ClassName, propertyName, true);
				}

				if (propertyName == "Tags" || propertyName == "AttributesSerialize") {
					if (DecodeBlob(sourceValue, propertyName, path, instance, output)) {
						continue;
					}
				}

				RobloxValue value;
				if (sourceType == rbxl::VariantType::Ref) {
					// An unset reference is Roblox's nil, which is what a property
					// left out already means.
					const rbxl::InstanceId target = std::get<rbxl::Ref>(sourceValue).target;
					if (target == rbxl::kNoInstance) {
						continue;
					}
					if (output.Preorder == nullptr || target >= output.Preorder->size()) {
						output.Lose(
							path, instance.ClassName, propertyName, "Ref", "names no instance in this file"
						);
						continue;
					}
					value.Set(RobloxReference{(*output.Preorder)[target]});
					instance.Properties.push_back(RobloxProperty{propertyName, std::move(value)});
					continue;
				}
				if (sourceType == rbxl::VariantType::EnumValue) {
					// Named through Roblox's own table, then checked by the
					// engine's enum table on the way in.
					const uint32_t token = std::get<rbxl::EnumValue>(sourceValue).value;
					const std::string_view member = RobloxEnumName(instance.ClassName, propertyName, token);
					if (member.empty()) {
						output.Lose(
							path,
							instance.ClassName,
							propertyName,
							"EnumValue",
							"no Roblox enum name is known for this value"
						);
						continue;
					}
					value.Set(std::string(member));
					instance.Properties.push_back(RobloxProperty{propertyName, std::move(value)});
					continue;
				}
				if (!ConvertValue(sourceValue, value)) {
					output.Lose(
						path,
						instance.ClassName,
						propertyName,
						rbxl::variantTypeName(sourceType),
						NO_REPRESENTATION
					);
					continue;
				}

				if (value.Kind() == RobloxValueKind::Text) {
					AnalyzeText(
						model,
						value.As<std::string>(),
						path,
						instance.ClassName,
						propertyName,
						explicitContent
					);
				}
				if (IsScript(instance.ClassName) && propertyName == "Source" &&
					value.Kind() == RobloxValueKind::Text) {
					model.Scripts.push_back(RobloxScript{path, instance.ClassName, value.As<std::string>()});
				}
				instance.Properties.push_back(RobloxProperty{propertyName, std::move(value)});
			}
			// This reader owns the decoded DOM. Release each large row after its
			// engine value exists so a place does not retain both full models.
			// The reserve above assumed every property survives; many do not.
			decltype(source.properties){}.swap(source.properties);
			instance.Properties.shrink_to_fit();
			return instance;
		}

		RobloxInstance BuildInstance(rbxl::Dom &dom, rbxl::InstanceId id, std::string path, Output &output) {
			RobloxInstance instance = BuildNode(dom, id, path, output);
			rbxl::Instance &source = dom.at(id);
			instance.Children.reserve(source.children.size());
			for (const rbxl::InstanceId child : source.children) {
				instance.Children.push_back(BuildInstance(dom, child, path, output));
			}
			decltype(source.children){}.swap(source.children);
			return instance;
		}

		template <typename T> void Append(std::vector<T> &into, std::vector<T> &from) {
			into.insert(
				into.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end())
			);
		}

		// One unit of tree conversion: either a node whose own properties were
		// converted while planning, or a whole subtree left for the job pool.
		// Each writes its own shard; shards join in plan order, which is the
		// tree's preorder, so the result matches a serial conversion.
		struct Piece {
			rbxl::InstanceId Id = 0;
			std::string ParentPath;
			RobloxInstance *Slot = nullptr;
			bool Subtree = false;
			Output Shard;
		};

		// Number of instances under each instance, itself included.
		std::vector<size_t> SubtreeSizes(const rbxl::Dom &dom) {
			std::vector<size_t> sizes(dom.instanceCount(), 1);
			for (const rbxl::InstanceId id : dom.postOrder()) {
				for (const rbxl::InstanceId child : dom.at(id).children) {
					sizes[id] += sizes[child];
				}
			}
			return sizes;
		}

		// Splits the tree under `id` until every subtree left whole is at most
		// `threshold` instances, converting the nodes above them in order.
		void Plan(
			rbxl::Dom &dom,
			rbxl::InstanceId id,
			const std::string &parentPath,
			RobloxInstance *slot,
			const std::vector<size_t> &sizes,
			size_t threshold,
			const std::vector<uint32_t> &preorder,
			std::deque<Piece> &pieces
		) {
			Piece &piece = pieces.emplace_back();
			piece.Shard.Preorder = &preorder;
			piece.Id = id;
			piece.ParentPath = parentPath;
			piece.Slot = slot;
			rbxl::Instance &source = dom.at(id);
			if (sizes[id] <= threshold || source.children.empty()) {
				piece.Subtree = true;
				return;
			}
			std::string path = parentPath;
			*slot = BuildNode(dom, id, path, piece.Shard);
			slot->Children.resize(source.children.size());
			for (size_t index = 0; index < source.children.size(); index++) {
				Plan(
					dom,
					source.children[index],
					path,
					&slot->Children[index],
					sizes,
					threshold,
					preorder,
					pieces
				);
			}
			decltype(source.children){}.swap(source.children);
		}

		// Converts every root, with the subtrees under them on the job pool.
		void BuildRoots(rbxl::Dom &dom, const std::vector<rbxl::InstanceId> &roots, RobloxModel &model) {
			const std::vector<size_t> sizes = SubtreeSizes(dom);
			const size_t workers = std::max<size_t>(engine::parallel::Jobs::WorkerCount(), 1);
			const size_t threshold = std::max<size_t>(dom.instanceCount() / (workers * 8), 256);

			// Numbered before conversion, which consumes each node's child list.
			std::vector<uint32_t> preorder(dom.instanceCount(), 0);
			{
				uint32_t next = 0;
				std::vector<rbxl::InstanceId> stack(roots.rbegin(), roots.rend());
				while (!stack.empty()) {
					const rbxl::InstanceId id = stack.back();
					stack.pop_back();
					preorder[id] = next++;
					const auto &children = dom.at(id).children;
					stack.insert(stack.end(), children.rbegin(), children.rend());
				}
			}

			model.Roots.resize(roots.size());
			std::deque<Piece> pieces;
			for (size_t index = 0; index < roots.size(); index++) {
				Plan(dom, roots[index], {}, &model.Roots[index], sizes, threshold, preorder, pieces);
			}
			engine::parallel::Jobs::For(
				pieces.size(),
				1,
				[&](size_t begin, size_t end) {
					for (size_t index = begin; index < end; index++) {
						Piece &piece = pieces[index];
						if (piece.Subtree) {
							*piece.Slot = BuildInstance(dom, piece.Id, piece.ParentPath, piece.Shard);
						}
					}
				},
				2
			);
			Output joined;
			for (Piece &piece : pieces) {
				Append(model.Assets, piece.Shard.Model.Assets);
				Append(model.Scripts, piece.Shard.Model.Scripts);
				for (const RobloxLostProperty &lost : piece.Shard.Model.LostProperties) {
					joined.Lose(
						lost.InstancePath,
						lost.ClassName,
						lost.PropertyName,
						lost.RobloxType,
						lost.Reason,
						lost.Count
					);
				}
			}
			model.LostProperties = std::move(joined.Model.LostProperties);
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

		rbxl::Dom &dom = decoded.value();
		if (!CheckTree(dom, failure)) {
			return false;
		}

		RobloxModel candidate;
		std::vector<rbxl::InstanceId> roots = dom.roots();
		std::sort(roots.begin(), roots.end());
		BuildRoots(dom, roots, candidate);

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
