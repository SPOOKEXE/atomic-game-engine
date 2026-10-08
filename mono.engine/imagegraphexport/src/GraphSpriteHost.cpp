#include "GraphSpriteHost.hpp"

#include <engine/bake/Aseprite.hpp>
#include <engine/bake/GameMakerRoom.hpp>
#include <engine/imagegraphio/SourceArtworkEdit.hpp>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		std::optional<uint64_t> AsepriteRetainedBytes(const engine::bake::AsepriteDocument &doc) {
			uint64_t total = sizeof(doc);
			const auto add = [&](uint64_t count, uint64_t size = 1) {
				if (count > (UINT64_MAX - total) / size) return false;
				total += count * size;
				return true;
			};
			const auto texture = [&](const engine::assets::TextureData &image) {
				if (!add(image.Pixels.capacity()) ||
					!add(image.Mips.capacity(), sizeof(std::vector<std::byte>)))
					return false;
				for (const auto &level : image.Mips)
					if (!add(level.capacity())) return false;
				return true;
			};
			if (!add(doc.Layers.capacity(), sizeof(engine::bake::AsepriteLayer)) ||
				!add(doc.Frames.capacity(), sizeof(engine::bake::AsepriteFrame)) ||
				!add(doc.Tags.capacity(), sizeof(engine::bake::AsepriteTag)) ||
				!add(doc.Tilesets.capacity(), sizeof(engine::bake::AsepriteTileset)) ||
				!add(doc.Palette.capacity(), sizeof(std::array<uint8_t, 4>)) ||
				!add(doc.InspectionJson.capacity() + 1))
				return std::nullopt;
			for (const auto &layer : doc.Layers)
				if (!add(layer.Name.capacity() + 1)) return std::nullopt;
			for (const auto &tag : doc.Tags)
				if (!add(tag.Name.capacity() + 1)) return std::nullopt;
			for (const auto &frame : doc.Frames) {
				if (!add(frame.Cels.capacity(), sizeof(engine::bake::AsepriteCel))) return std::nullopt;
				for (const auto &cel : frame.Cels)
					if (!texture(cel.Pixels)) return std::nullopt;
			}
			for (const auto &set : doc.Tilesets)
				if (!add(set.Name.capacity() + 1) || !texture(set.Pixels)) return std::nullopt;
			return total;
		}
		const Value *Input(const HostNodeInvocation &in, std::string_view name) {
			auto it = std::find_if(in.Inputs.begin(), in.Inputs.end(), [&](const auto &v) {
				return v.Port == name;
			});
			return it == in.Inputs.end() ? nullptr : &it->Data;
		}
		std::string Text(const HostNodeInvocation &in, std::string_view name) {
			auto v = Input(in, name);
			auto p = v ? std::get_if<std::string>(v) : nullptr;
			return p ? *p : std::string{};
		}
		bool Bool(const HostNodeInvocation &in, std::string_view name, bool fallback) {
			auto v = Input(in, name);
			auto p = v ? std::get_if<bool>(v) : nullptr;
			return p ? *p : fallback;
		}
		Image Surface(engine::assets::TextureData data) {
			Image image;
			image.Width = data.Width;
			image.Height = data.Height;
			image.Pixels.reserve(data.Pixels.size());
			for (auto b : data.Pixels)
				image.Pixels.push_back(std::to_integer<uint8_t>(b));
			image.Hash = SurfaceHash(image);
			return image;
		}
		HostNodeCapture Base(const HostNodeInvocation &in) {
			HostNodeCapture out;
			out.Authored = in.Authored;
			out.Tick = in.Request.Tick;
			out.Subframe = in.Request.Subframe;
			out.NegativeFrame = in.Request.NegativeFrame;
			out.Inputs.assign(in.Inputs.begin(), in.Inputs.end());
			for (auto image : in.Images) {
				if (!image.Data) throw std::runtime_error("missing sprite host image");
				out.InputImages.push_back({std::string(image.Port), SurfaceHash(*image.Data)});
			}
			return out;
		}
		const Value *Field(const StructValue &value, std::string_view name) {
			if (!value.Data) return nullptr;
			auto it = std::find_if(value.Data->Fields.begin(), value.Data->Fields.end(), [&](const auto &v) {
				return v.first == name;
			});
			return it == value.Data->Fields.end() ? nullptr : &it->second;
		}
		ArrayValue Names(const auto &entries) {
			ArrayValue names;
			names.ElementType = ValueType::Text;
			for (const auto &entry : entries)
				names.Elements.emplace_back(entry.Name);
			return names;
		}
		std::vector<std::byte> File(const std::filesystem::path &path, uint64_t maximum) {
			std::error_code error;
			auto size = std::filesystem::file_size(path, error);
			if (error || size > maximum || size > 64ull * 1024 * 1024)
				throw std::runtime_error("sprite source exceeds file byte budget");
			std::vector<std::byte> bytes(size);
			std::ifstream stream(path, std::ios::binary);
			stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
			if (!stream || size_t(stream.gcount()) != bytes.size())
				throw std::runtime_error("sprite source read incomplete");
			return bytes;
		}
		using Json = nlohmann::ordered_json;
		void InspectionCharge(uint64_t size, uint64_t &remaining) {
			if (size > remaining) throw std::runtime_error("Aseprite inspection tree exceeds capture budget");
			remaining -= size;
		}
		Value InspectionValue(const Json &, uint64_t &, size_t);
		SourceArrayItem InspectionItem(const Json &json, uint64_t &remaining, size_t depth) {
			if (depth > 16) throw std::runtime_error("Aseprite inspection array nesting exceeds bound");
			if (json.is_array()) {
				InspectionCharge(json.size() * sizeof(SourceArrayItem), remaining);
				std::vector<SourceArrayItem> items;
				items.reserve(json.size());
				for (const auto &v : json)
					items.push_back(InspectionItem(v, remaining, depth + 1));
				return {std::move(items)};
			}
			auto value = InspectionValue(json, remaining, depth + 1);
			return std::visit(
				[](auto &&v) -> SourceArrayItem {
					using T = std::decay_t<decltype(v)>;
					if constexpr (std::is_constructible_v<ElementValue, T>)
						return {ElementValue(std::move(v))};
					else
						throw std::runtime_error("Aseprite inspection leaf unsupported");
				},
				std::move(value)
			);
		}
		Value InspectionValue(const Json &json, uint64_t &remaining, size_t depth) {
			if (depth > 16) throw std::runtime_error("Aseprite inspection tree nesting exceeds bound");
			InspectionCharge(sizeof(Value) + 128, remaining);
			if (json.is_null()) return UndefinedValue{};
			if (json.is_boolean()) return json.get<bool>();
			if (json.is_number_integer()) return json.get<int64_t>();
			if (json.is_number_float()) return json.get<double>();
			if (json.is_string()) {
				const auto &v = json.get_ref<const std::string &>();
				InspectionCharge(v.size(), remaining);
				return v;
			}
			if (json.is_array()) {
				ArrayValue array;
				array.ElementType = ValueType::Any;
				InspectionCharge(json.size() * sizeof(SourceArrayItem), remaining);
				array.Items.reserve(json.size());
				for (const auto &v : json)
					array.Items.push_back(InspectionItem(v, remaining, depth + 1));
				return array;
			}
			if (json.is_object() && json.size() == 1 && json.contains("$buffer_hex")) {
				const auto &hex = json["$buffer_hex"].get_ref<const std::string &>();
				if (hex.size() % 2) throw std::runtime_error("Aseprite buffer tag malformed");
				InspectionCharge(hex.size() / 2, remaining);
				BufferValue buffer;
				buffer.Bytes.resize(hex.size() / 2);
				const auto digit = [](char c) -> uint8_t {
					if (c >= '0' && c <= '9') return c - '0';
					if (c >= 'a' && c <= 'f') return c - 'a' + 10;
					throw std::runtime_error("Aseprite buffer tag invalid");
				};
				for (size_t i = 0; i < buffer.Bytes.size(); ++i)
					buffer.Bytes[i] = (digit(hex[i * 2]) << 4) | digit(hex[i * 2 + 1]);
				return buffer;
			}
			if (!json.is_object()) throw std::runtime_error("Aseprite inspection value unsupported");
			StructValue object;
			object.Data.emplace();
			InspectionCharge(json.size() * sizeof(AuthoredValue), remaining);
			object.Data->Fields.reserve(json.size());
			for (const auto &[key, v] : json.items()) {
				InspectionCharge(key.size(), remaining);
				object.Data->Fields.push_back({key, InspectionValue(v, remaining, depth + 1)});
			}
			return object;
		}
		StructValue Content(
			const engine::bake::AsepriteDocument &doc, std::span<const std::byte> bytes, uint64_t maximum
		) {
			uint64_t budget = maximum;
			InspectionCharge(doc.InspectionJson.size() * 2, budget);
			auto tree = Json::parse(doc.InspectionJson);
			StructValue value = std::get<StructValue>(InspectionValue(tree, budget, 0));
			InspectionCharge(bytes.size() + sizeof(BufferValue), budget);
			BufferValue buffer;
			buffer.Bytes.reserve(bytes.size());
			for (auto b : bytes)
				buffer.Bytes.push_back(std::to_integer<uint8_t>(b));
			value.Data->Fields.push_back({"source_bytes", std::move(buffer)});
			InspectionCharge(3 * sizeof(AuthoredValue), budget);
			for (const auto &layer : doc.Layers)
				InspectionCharge(sizeof(ElementValue) + layer.Name.size(), budget);
			for (const auto &tag : doc.Tags)
				InspectionCharge(sizeof(ElementValue) + tag.Name.size(), budget);
			for (const auto &set : doc.Tilesets)
				InspectionCharge(sizeof(ElementValue) + set.Name.size(), budget);
			value.Data->Fields.push_back({"layers", Names(doc.Layers)});
			value.Data->Fields.push_back({"tags", Names(doc.Tags)});
			value.Data->Fields.push_back({"tilesets", Names(doc.Tilesets)});
			return value;
		}

	}
	bool IsGraphSpriteHost(std::string_view type) {
		return type == "pc.ase_file_read" || type == "pc.ase_layer" || type == "pc.ase_tag" ||
			   type == "pc.ase_tileset" || type == "pc.gmroom";
	}
	bool CaptureGraphSprite(
		const HostNodeInvocation &in,
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		HostNodeCapture &out,
		std::string &failure
	) try {
		const auto &type = in.Authored.Type;
		if (type == "pc.gmroom") {
			std::vector<engine::bake::GameMakerTileOverride> overrides;
			uint64_t overlayBudget = in.MaximumOperationBytes / 4;
			for (const auto &socket : in.Authored.DynamicInputs) {
				auto value = Input(in, socket.Id);
				if (!value) continue;
				if (socket.SourceLayerName.empty())
					throw std::runtime_error("GameMaker dynamic room control has no source layer binding");
				auto object = std::get_if<StructValue>(value);
				if (!object || !object->Data)
					throw std::runtime_error("GameMaker tile control requires a struct");
				engine::bake::GameMakerTileOverride edit;
				edit.LayerName = socket.SourceLayerName;
				if (auto data = Field(*object, "data")) {
					auto array = std::get_if<ArrayValue>(data);
					if (!array || !array->Nested.empty() || !array->Items.empty() ||
						array->Elements.size() > 1048576 || array->Elements.size() * 4 > overlayBudget)
						throw std::runtime_error("GameMaker tile array shape exceeds budget");
					overlayBudget -= array->Elements.size() * 4;
					for (const auto &v : array->Elements) {
						auto integer = std::get_if<int64_t>(&v);
						if (!integer || *integer < 0 || *integer > 0xffffffff)
							throw std::runtime_error("GameMaker tile array contains invalid index");
						edit.Data.push_back(uint32_t(*integer));
					}
				}
				if (auto preview = Field(*object, "preview")) {
					if (auto surface = std::get_if<SurfaceValue>(preview)) {
						const auto &image = surface->Data;
						if (image.Pixels.size() > overlayBudget)
							throw std::runtime_error("GameMaker preview exceeds budget");
						overlayBudget -= image.Pixels.size();
						edit.Preview.emplace();
						edit.Preview->Width = image.Width;
						edit.Preview->Height = image.Height;
						edit.Preview->Pixels.reserve(image.Pixels.size());
						for (auto pixel : image.Pixels)
							edit.Preview->Pixels.push_back(std::byte(pixel));
					} else if (!std::holds_alternative<UndefinedValue>(*preview))
						throw std::runtime_error("GameMaker preview is not a surface");
				}
				if (auto tileset = Field(*object, "tileset"))
					if (auto set = std::get_if<StructValue>(tileset))
						if (auto gm = Field(*set, "gmTile"))
							if (auto metadata = std::get_if<StructValue>(gm))
								if (auto key = Field(*metadata, "key")) {
									auto text = std::get_if<std::string>(key);
									if (!text || text->size() > 4096)
										throw std::runtime_error("GameMaker tileset binding invalid");
									edit.TilesetKey = *text;
								}
				overrides.push_back(std::move(edit));
			}
			const GraphFileGrant *primary = nullptr;
			std::vector<const GraphFileGrant *> dependencies;
			for (const auto &grant : grants)
				if (grant.NodeId == in.Authored.Id) {
					if (grant.Write || !policy.AllowsName(grant.File.string()))
						throw std::runtime_error("GameMaker resource read capability violates policy");
					if (grant.Resource.empty()) {
						if (primary) throw std::runtime_error("GameMaker room grant duplicated");
						primary = &grant;
					} else
						dependencies.push_back(&grant);
				}
			if (!primary || dependencies.size() > 4096)
				throw std::runtime_error("GameMaker room exact read capability not granted");
			uint64_t remaining = in.MaximumOperationBytes / 4;
			auto room = File(primary->File, remaining);
			remaining -= room.size();
			std::vector<std::vector<std::byte>> payloads;
			payloads.reserve(dependencies.size());
			std::vector<engine::bake::GameMakerResource> resources;
			resources.reserve(dependencies.size());
			for (auto grant : dependencies) {
				payloads.push_back(File(grant->File, remaining));
				remaining -= payloads.back().size();
				resources.push_back({grant->Resource, payloads.back()});
			}
			engine::assets::TextureData image;
			if (!engine::bake::ReadGameMakerRoom(
					room, resources, image, failure, in.MaximumOperationBytes / 2, overrides
				))
				return false;
			auto capture = Base(in);
			capture.Images.push_back({"room_preview", Surface(std::move(image))});
			out = std::move(capture);
			return true;
		}
		std::vector<std::byte> owned;
		std::span<const std::byte> bytes;
		if (type == "pc.ase_file_read") {
			const GraphFileGrant *grant = nullptr;
			for (const auto &g : grants)
				if (g.NodeId == in.Authored.Id && g.Resource.empty()) {
					if (grant) throw std::runtime_error("Aseprite primary file grant duplicated");
					grant = &g;
				}
			const auto path = Text(in, "path");
			if (!grant || grant->Write || path != grant->File.string() || !policy.AllowsName(path))
				throw std::runtime_error("Aseprite exact read capability not granted");
			owned = File(grant->File, in.MaximumOperationBytes / 4);
			bytes = owned;
		} else {
			const auto *v = Input(in, "ase_data");
			const auto *content = v ? std::get_if<StructValue>(v) : nullptr;
			const auto *raw = content ? Field(*content, "source_bytes") : nullptr;
			const auto *buffer = raw ? std::get_if<BufferValue>(raw) : nullptr;
			if (!buffer) throw std::runtime_error("Aseprite content recording lacks source bytes");
			bytes = {reinterpret_cast<const std::byte *>(buffer->Bytes.data()), buffer->Bytes.size()};
		}
		engine::bake::AsepriteDocument doc;
		if (!engine::bake::ReadAseprite(bytes, doc, failure, in.MaximumOperationBytes / 4)) return false;
		std::optional<size_t> sourceLayerIndex;
		if (type == "pc.ase_layer") {
			const auto name = Text(in, "layer_name");
			for (size_t i = 0; i < doc.Layers.size(); ++i)
				if (doc.Layers[i].Name == name) sourceLayerIndex = i;
			if (!sourceLayerIndex) {
				failure = "ASE layer name is absent from the pinned source raw-name map";
				return false;
			}
		}
		const auto decodedHeld = AsepriteRetainedBytes(doc);
		const auto previousHeld = HostCaptureRetainedPayloadBytes(out);
		if (!decodedHeld || !previousHeld || *decodedHeld > in.MaximumOperationBytes ||
			*previousHeld > in.MaximumOperationBytes - *decodedHeld ||
			owned.capacity() > in.MaximumOperationBytes - *decodedHeld - *previousHeld) {
			failure = "ASE metadata decoder and prior observation overlap exceeds operation bounds";
			return false;
		}
		const uint64_t profileMaximum =
			in.MaximumOperationBytes - *decodedHeld - *previousHeld - owned.capacity();
		Value sourceContent;
		const Value *profileContent = nullptr;
		if (type == "pc.ase_file_read") {
			sourceContent = Content(doc, bytes, std::min(in.MaximumOperationBytes / 4, profileMaximum));
			profileContent = &sourceContent;
		} else
			profileContent = Input(in, "ase_data");

		engine::imagegraphio::SourceArtworkMetadata metadata;
		Diagnostic profileDiagnostic;
		if (!profileContent || engine::imagegraphio::ReadSourceAsepriteMetadata(
								   in.Authored, *profileContent, metadata, profileDiagnostic, profileMaximum
							   ) != Status::Ok) {
			failure = profileDiagnostic.Message.empty() ? "ASE source inspection profile is absent"
														: profileDiagnostic.Message;
			return false;
		}
		if (metadata.Layers.size() != doc.Layers.size() || metadata.Tags.size() > doc.Tags.size()) {
			failure = "ASE source layer or tag profile differs from owned binary data";
			return false;
		}
		for (size_t i = 0; i < metadata.Layers.size(); ++i)
			doc.Layers[i].Name = std::move(metadata.Layers[i].Name);
		doc.Tags.clear();
		// The selected first-frame tag chunk fits the already decoded cumulative table.
		for (auto &tag : metadata.Tags)
			doc.Tags.push_back({std::move(tag.Name), uint16_t(tag.First), uint16_t(tag.Last)});
		metadata = {}; // The profile scratch is retired before rendering and receipt cloning.
		const auto emptyImage = [&](engine::assets::TextureData &image, bool crop) {
			image.Width = crop ? 1 : doc.Width;
			image.Height = crop ? 1 : doc.Height;
			const uint64_t size = uint64_t(image.Width) * image.Height * 4;
			if (size > in.MaximumOperationBytes / 4)
				throw std::runtime_error("Aseprite empty frame exceeds capture budget");
			image.Pixels.resize(size, std::byte{});
		};
		auto capture = Base(in);
		size_t frame = in.Request.Tick;
		bool emptyFrame = frame >= doc.Frames.size();
		if (type == "pc.ase_file_read") {
			auto tag = Text(in, "current_tag");
			if (!tag.empty()) {
				auto t = std::find_if(doc.Tags.begin(), doc.Tags.end(), [&](const auto &t) {
					return t.Name == tag;
				});
				if (t == doc.Tags.end()) throw std::runtime_error("Aseprite current tag not found");
				emptyFrame = frame < t->First || frame > t->Last;
			}
			auto &content = std::get<StructValue>(sourceContent);
			for (auto &[key, value] : content.Data->Fields) {
				if (key == "layers") value = Names(doc.Layers);
				if (key == "tags") value = Names(doc.Tags);
			}
			capture.Outputs.push_back({"content", sourceContent});
			capture.Outputs.push_back({"raw_data", std::move(sourceContent)});
			capture.Outputs.push_back({"frame_amount", int64_t(doc.Frames.size())});
			capture.Outputs.push_back({"path", Text(in, "path")});
			capture.Outputs.push_back({"layers", Names(doc.Layers)});
			capture.Outputs.push_back({"tags", Names(doc.Tags)});
			ArrayValue palette;
			palette.ElementType = ValueType::Colour;
			for (auto colour : doc.Palette)
				palette.Elements.emplace_back(Colour{colour[0], colour[1], colour[2], colour[3]});
			capture.Outputs.push_back({"palette", std::move(palette)});
			ArrayValue sets;
			sets.ElementType = ValueType::Struct;
			for (const auto &set : doc.Tilesets) {
				StructValue value;
				value.Data.emplace();
				value.Data->Fields = {
					{"name", set.Name},
					{"tileAmount", int64_t(set.Count)},
					{"tileWidth", int64_t(set.Width)},
					{"tileHeight", int64_t(set.Height)}
				};
				sets.Elements.emplace_back(std::move(value));
			}
			capture.Outputs.push_back({"tilesets", std::move(sets)});
			const auto flag = [&](const Value *value, size_t index, bool fallback) {
				if (!value) return fallback;
				const auto *array = std::get_if<ArrayValue>(value);
				if (array && array->Elements.empty() && array->Items.empty() && array->Nested.empty())
					return fallback;
				if (!array || array->ElementType != ValueType::Boolean || !array->Items.empty() ||
					!array->Nested.empty())
					throw std::runtime_error("Aseprite layer controls require a boolean array");
				if (index >= array->Elements.size()) return fallback;
				const auto *result = std::get_if<bool>(&array->Elements[index]);
				if (!result) throw std::runtime_error("Aseprite layer control is not boolean");
				return *result;
			};
			const Value *visibility = nullptr;
			for (const auto &property : in.Authored.SourceProperties)
				if (property.Port == "layer_visible") visibility = &property.Data;
			const Value *loop = Input(in, "attribute_layer_loop");
			std::vector<engine::bake::AsepriteCel> selected;
			if (doc.Layers.size() * (sizeof(engine::bake::AsepriteCel) + sizeof(size_t)) >
				in.MaximumOperationBytes / 4)
				throw std::runtime_error("Aseprite layer selection exceeds capture budget");
			selected.reserve(doc.Layers.size());
			std::vector<size_t> layerFrameCounts(doc.Layers.size(), 0);
			for (size_t sourceFrame = 0; sourceFrame < doc.Frames.size(); ++sourceFrame)
				for (const auto &cel : doc.Frames[sourceFrame].Cels)
					layerFrameCounts[cel.Layer] = sourceFrame + 1;

			for (size_t index = 0; index < doc.Layers.size(); ++index) {
				if (!flag(visibility, index, true)) {
					doc.Layers[index].Visible = false;
					continue;
				}
				uint64_t selectedFrame = frame;
				const size_t layerFrames = layerFrameCounts[index];
				doc.Layers[index].Visible = true;
				if (!layerFrames) continue;
				if (flag(loop, index, layerFrames == 1)) {
					if (tag.empty())
						selectedFrame %= layerFrames;
					else {
						const auto region =
							std::find_if(doc.Tags.begin(), doc.Tags.end(), [&](const auto &value) {
								return value.Name == tag;
							});
						selectedFrame = region->First + selectedFrame % (region->Last - region->First + 1);
					}
				} else if (emptyFrame)
					continue;
				if (selectedFrame >= doc.Frames.size()) continue;
				auto &cels = doc.Frames[selectedFrame].Cels;
				const auto cel = std::find_if(cels.begin(), cels.end(), [&](const auto &value) {
					return value.Layer == index;
				});
				if (cel != cels.end()) selected.push_back(std::move(*cel));
			}
			doc.Frames.resize(1);
			doc.Frames[0].Cels = std::move(selected);
			frame = 0;
			emptyFrame = false;

			engine::assets::TextureData image;
			if (emptyFrame)
				emptyImage(image, false);
			else if (!engine::bake::RenderAseprite(
						 doc,
						 frame,
						 {},
						 Bool(in, "use_cel_dimension", false),
						 true,
						 image,
						 failure,
						 in.MaximumOperationBytes / 4
					 ))
				return false;
			capture.Images.push_back({"output", Surface(std::move(image))});
		} else if (type == "pc.ase_layer" || type == "pc.ase_tag") {
			std::string layer;
			if (type == "pc.ase_layer") {
				layer = Text(in, "layer_name");
				if (layer.empty()) throw std::runtime_error("Aseprite layer name is empty");
				auto l = doc.Layers.begin() + *sourceLayerIndex;
				// setFrameCel extends the selected layer array through the authored frame index.
				bool animated = false;
				for (size_t f = 1; f < doc.Frames.size(); ++f)
					for (const auto &cel : doc.Frames[f].Cels)
						if (cel.Layer == *sourceLayerIndex) animated = true;
				capture.SourceUpdateOnFrame = animated;
				const auto requestedLayer = layer;
				// The source map selects an index, while the codec selects a name. Use one bounded
				// private unique selector so suffix collisions do not change the selected subtree.
				for (size_t serial = 0;; ++serial) {
					layer = "native-artwork-selection/" + std::to_string(serial);
					if (std::none_of(doc.Layers.begin(), doc.Layers.end(), [&](const auto &item) {
							return item.Name == layer;
						}))
						break;
				}
				l->Name = layer;
				frame = Bool(in, "loop", false) ? in.Request.Tick % doc.Frames.size() : in.Request.Tick;
				emptyFrame = frame >= doc.Frames.size();
				capture.Outputs.push_back({"layer_name", requestedLayer});
				capture.Outputs.push_back({"opacity", l->Opacity / 255.0});
			} else {
				auto name = Text(in, "tag");
				auto tag = std::find_if(doc.Tags.begin(), doc.Tags.end(), [&](const auto &t) {
					return t.Name == name;
				});
				if (tag == doc.Tags.end()) throw std::runtime_error("Aseprite tag name not found");
				if (tag->First > tag->Last || tag->Last >= doc.Frames.size())
					throw std::runtime_error("ASE tag frame interval is outside the native render profile");
				frame = tag->First + in.Request.Tick % (tag->Last - tag->First + 1);
				emptyFrame = false;
				capture.Outputs.push_back({"frame_range", Vector2{double(tag->First), double(tag->Last)}});
			}
			engine::assets::TextureData image;
			if (emptyFrame)
				emptyImage(image, Bool(in, "crop_output", false));
			else if (!engine::bake::RenderAseprite(
						 doc,
						 frame,
						 layer,
						 Bool(in, "crop_output", false),
						 Bool(in, "apply_opacity", true),
						 image,
						 failure,
						 in.MaximumOperationBytes / 4
					 ))
				return false;
			capture.Images.push_back({"surface_out", Surface(std::move(image))});
		} else if (type == "pc.ase_tileset") {
			const auto name = Text(in, "tileset_name");
			auto set = std::find_if(doc.Tilesets.begin(), doc.Tilesets.end(), [&](const auto &t) {
				return t.Name == name;
			});
			if (set == doc.Tilesets.end()) throw std::runtime_error("Aseprite tileset name not found");
			const uint64_t total = uint64_t(set->Width) * set->Height * set->Count * 4;
			if (total > in.MaximumOperationBytes / 4)
				throw std::runtime_error("Aseprite tile array exceeds capture budget");
			HostCapturedImageArray tiles;
			tiles.Port = "tiles";
			for (size_t t = 0; t < set->Count; t++) {
				engine::assets::TextureData tile;
				tile.Width = set->Width;
				tile.Height = set->Height;
				size_t length = size_t(set->Width) * set->Height * 4;
				tile.Pixels.assign(
					set->Pixels.Pixels.begin() + t * length, set->Pixels.Pixels.begin() + (t + 1) * length
				);
				tiles.Frames.push_back(Surface(std::move(tile)));
			}
			capture.ImageArrays.push_back(std::move(tiles));
			capture.Outputs.push_back({"tile_size", Vector2{double(set->Width), double(set->Height)}});
			capture.Outputs.push_back({"tile_amount", int64_t(set->Count)});
			capture.Outputs.push_back({"tileset_name", name});
		}
		out = std::move(capture);
		return true;
	} catch (const std::exception &e) {
		failure = e.what();
		return false;
	}
}
