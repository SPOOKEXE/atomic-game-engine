#include "GraphTileHost.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		using Json = nlohmann::ordered_json;
		bool Fail(std::string &failure, const char *message) {
			failure = message;
			return false;
		}
		template <class T> const T *Get(const HostNodeInvocation &in, std::string_view port) {
			for (const auto &input : in.Inputs)
				if (input.Port == port) return std::get_if<T>(&input.Data);
			return nullptr;
		}
		bool Charge(uint64_t size, uint64_t &remaining) {
			if (size > remaining) return false;
			remaining -= size;
			return true;
		}
		// Every finite binary16 sample times 100 is exact in binary64. Match the
		// verified HTML5 string profile.
		std::string Number(double value) {
			const bool negative = value < 0;
			const double magnitude = std::abs(value);
			const auto integer = static_cast<uint64_t>(magnitude);
			if (magnitude == static_cast<double>(integer))
				return (negative ? "-" : "") + std::to_string(integer);
			const auto cents = static_cast<uint64_t>(std::floor(magnitude * 100 + .5));
			const auto remainder = cents % 100;
			return (negative ? "-" : "") + std::to_string(cents / 100) + "." + (remainder < 10 ? "0" : "") +
				   std::to_string(remainder);
		}
		bool Template(
			const HostNodeInvocation &in,
			std::span<const GraphFileGrant> grants,
			const engine::assets::ContentPolicy &policy,
			uint64_t cap,
			Json &output,
			std::string &failure
		) {
			const GraphFileGrant *grant = nullptr;
			for (const auto &candidate : grants)
				if (candidate.NodeId == in.Authored.Id &&
					candidate.Resource == "tileset_gamemaker2_room.yy") {
					if (grant) return Fail(failure, "tile room template grant is duplicated");
					grant = &candidate;
				}
			if (!grant || grant->Write || !grant->File.is_absolute() ||
				grant->File.lexically_normal() != grant->File || !policy.AllowsName(grant->File.string()))
				return Fail(failure, "tile room export requires an exact template read grant");
			std::error_code error;
			const auto size = std::filesystem::file_size(grant->File, error);
			if (error || !size || size > std::min<uint64_t>(65536, cap / 128))
				return Fail(failure, "tile room template exceeds its working-set budget");
			std::string bytes(static_cast<size_t>(size), '\0');
			std::ifstream stream(grant->File, std::ios::binary);
			stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			engine::core::Metrics::Count("image composer tile template read operations", 1);
			engine::core::Metrics::Count(
				"image composer tile template bytes read",
				static_cast<double>(std::max<std::streamsize>(0, stream.gcount()))
			);
			if (!stream) return Fail(failure, "cannot read complete tile room template");
			size_t nodes = 0, stringBytes = 0;
			const auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
				if (depth > 32 || ++nodes > 2048)
					throw std::runtime_error("tile room template structure exceeds its bounds");
				if ((event == Json::parse_event_t::key || event == Json::parse_event_t::value) &&
					value.is_string()) {
					stringBytes += value.get_ref<const std::string &>().size();
					if (stringBytes > 65536)
						throw std::runtime_error("tile room template strings exceed their bounds");
				}
				return true;
			};
			output = Json::parse(bytes, callback);
			if (!output.is_object() || !output.contains("parent") || !output["parent"].is_object() ||
				!output.contains("layers") || !output["layers"].is_array() || output["layers"].empty() ||
				output["layers"].size() > 64)
				return Fail(failure, "tile room template lacks source parent or first layer");
			auto &layer = output["layers"][0];
			if (!layer.is_object() || !layer.contains("tilesetId") || !layer["tilesetId"].is_object() ||
				!layer.contains("tiles") || !layer["tiles"].is_object())
				return Fail(failure, "tile room template lacks source tileset and tile records");
			return true;
		}
		bool Capture(
			const HostNodeInvocation &in,
			std::span<const GraphFileGrant> grants,
			const engine::assets::ContentPolicy &policy,
			HostNodeCapture &output,
			std::string &failure
		) {
			ENGINE_PROFILE_CAT("image composer tile file export", engine::core::ProfileCategory::Engine);
			if (in.Authored.Type != "pc.tile_tilemap_export")
				return Fail(failure, "not a source tilemap export node");
			const auto *tileset = Get<TilesetValue>(in, "input_7");
			const auto *path = Get<std::string>(in, "path");
			const auto *mode = Get<EnumValue>(in, "format");
			if (!tileset || !tileset->Data || !path || path->empty() || path->size() > 4096 || !mode ||
				mode->Value < 0 || mode->Value > 1)
				return Fail(failure, "tile export requires owned tileset, path and format controls");
			if (in.Images.size() != 1)
				return Fail(failure, "tile export requires exactly one resolved tilemap surface");
			const Image *map = nullptr;
			for (const auto &image : in.Images)
				if (image.Port == "tilemap") {
					if (map) return Fail(failure, "tile export surface input is duplicated");
					map = image.Data;
				}
			const auto cap = std::min<uint64_t>(in.MaximumOperationBytes, Limits::MaximumEvaluationBytes);
			if (!cap || !map || map->Format != SurfaceFormat::RGBA16Float ||
				!ValidSurfaceLayout(*map, in.Request.MaximumImageDimension, cap) ||
				!FiniteSurfaceSamples(*map))
				return Fail(failure, "tilemap export requires bounded finite RGBA16Float storage");
			const auto &set = *tileset->Data;
			if (!ValidSurfaceLayout(set.Texture, in.Request.MaximumImageDimension, cap) ||
				!FiniteSurfaceSamples(set.Texture) || !std::isfinite(set.TileSize.X) ||
				!std::isfinite(set.TileSize.Y) || set.TileSize.X <= 0 || set.TileSize.Y <= 0 ||
				set.DisplayName.empty() || set.DisplayName.size() > 4096)
				return Fail(failure, "tile export tileset texture, size or name is invalid");
			const uint64_t count = uint64_t(map->Width) * map->Height;
			if (count > 65536 || count > cap / 256)
				return Fail(failure, "tile export exceeds the tile working-set budget");
			std::filesystem::path destination(*path);
			destination.replace_extension(mode->Value == 0 ? ".csv" : ".yy");
			if (!destination.is_absolute() || destination.lexically_normal() != destination)
				return Fail(failure, "tile export destination must be an absolute normalized path");
			const GraphFileGrant *grant = nullptr;
			for (const auto &candidate : grants)
				if (candidate.NodeId == in.Authored.Id && candidate.Resource.empty()) {
					if (grant) return Fail(failure, "tile export primary grant is duplicated");
					grant = &candidate;
				}
			if (!grant || !grant->Write || grant->File != destination ||
				!policy.AllowsName(destination.string()))
				return Fail(failure, "tile export requires an exact changed-extension write grant");
			uint64_t remaining = cap / 4;
			const auto authoredBytes = NodeClonePayloadBytes(in.Authored);
			if (!authoredBytes || !Charge(*authoredBytes, remaining) ||
				!Charge(in.Inputs.size() * sizeof(AuthoredValue), remaining))
				return Fail(failure, "tile export recording exceeds its working-set budget");
			for (const auto &input : in.Inputs) {
				const auto bytes = ValueClonePayloadBytes(input.Data);
				if (!bytes || !Charge(*bytes + input.Port.size() + 1, remaining))
					return Fail(failure, "tile export resolved controls exceed their recording budget");
			}
			if (!Charge(sizeof(HostImageBinding) + sizeof("tilemap"), remaining))
				return Fail(failure, "tile export image binding exceeds its recording budget");
			// Stage the recording before publication so allocation failures cannot report
			// a failed completed write.
			HostNodeCapture candidate;
			candidate.Authored = in.Authored;
			candidate.Inputs.assign(in.Inputs.begin(), in.Inputs.end());
			candidate.InputImages.push_back({"tilemap", SurfaceHash(*map)});
			candidate.Tick = in.Request.Tick;
			candidate.Subframe = in.Request.Subframe;
			candidate.NegativeFrame = in.Request.NegativeFrame;
			std::string bytes;
			const uint64_t textCap = cap / 4;
			if (mode->Value == 0) {
				bytes.reserve(static_cast<size_t>(std::min<uint64_t>(count * 10 + map->Height, textCap)));
				for (uint32_t y = 0; y < map->Height; ++y) {
					for (uint32_t x = 0; x < map->Width; ++x) {
						SurfacePixel pixel{};
						if (!LoadSurfacePixel(*map, x, y, pixel))
							return Fail(failure, "cannot decode tilemap half sample");
						const auto cell = Number(pixel[0]);
						if (cell.size() + 1 > textCap - bytes.size())
							return Fail(failure, "tile CSV output exceeds its byte budget");
						if (x) bytes += ',';
						bytes += cell;
					}
					if (bytes.size() == textCap)
						return Fail(failure, "tile CSV output exceeds its byte budget");
					bytes += '\n';
				}
			} else {
				const auto *gmType = Get<EnumValue>(in, "gm_export_type");
				const auto *room = Get<std::string>(in, "gm_room_name");
				const auto *layerName = Get<std::string>(in, "gm_layer_name");
				if (!gmType || gmType->Value != 0 || !room || !layerName || room->empty() ||
					layerName->empty() || room->size() > 4096 || layerName->size() > 4096)
					return Fail(failure, "tile room export requires bounded Room format and names");
				// Source indexes H rows using the W loop. Refuse its undefined nonsquare
				// boundary.
				if (map->Width != map->Height)
					return Fail(
						failure,
						"source GameMaker tile export requires square "
						"maps; nonsquare indexing is undefined"
					);
				Json tree;
				if (!Template(in, grants, policy, cap / 2, tree, failure)) return false;
				auto &parent = tree["parent"];
				parent["name"] = *room;
				parent["path"] = "folders/" + *room + ".yy";
				auto &layer = tree["layers"][0];
				layer["name"] = *layerName;
				layer["gridX"] = set.TileSize.X / 2;
				layer["gridY"] = set.TileSize.Y / 2;
				layer["tilesetId"]["name"] = set.DisplayName;
				layer["tilesetId"]["path"] = "tilesets/" + set.DisplayName + "/" + set.DisplayName + ".yy";
				auto &data = layer["tiles"];
				data["SerialiseWidth"] = map->Width;
				data["SerialiseHeight"] = map->Height;
				auto values = Json::array();
				values.get_ref<Json::array_t &>().reserve(static_cast<size_t>(count));
				for (uint32_t i = 0; i < map->Width; ++i)
					for (uint32_t j = 0; j < map->Height; ++j) {
						SurfacePixel pixel{};
						if (!LoadSurfacePixel(*map, j, i, pixel))
							return Fail(failure, "cannot decode tilemap half sample");
						values.push_back(pixel[0]);
					}
				// The source's i-then-j iteration actually reads row i, column j, retaining
				// row-major square order.
				data["TileSerialiseData"] = std::move(values);
				bytes = tree.dump(2);
				if (bytes.size() > textCap) return Fail(failure, "tile room output exceeds its byte budget");
			}
			engine::core::Metrics::Count(
				"image composer tile file bytes prepared", static_cast<double>(bytes.size())
			);
			engine::core::Metrics::Count("image composer tile file publication attempts", 1);
			if (!PublishGraphHostFile(
					*grant,
					policy,
					{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()},
					cap,
					failure
				))
				return false;
			output = std::move(candidate);
			failure.clear();
			return true;
		}
	} // namespace
	bool CaptureGraphTileFile(
		const engine::imagegraph::HostNodeInvocation &in,
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::HostNodeCapture &output,
		std::string &failure
	) {
		try {
			return Capture(in, grants, policy, output, failure);
		} catch (const std::bad_alloc &) {
			return Fail(failure, "tile export allocation exceeds its budget");
		} catch (const std::filesystem::filesystem_error &) {
			return Fail(failure, "tile export filesystem operation failed");
		} catch (const nlohmann::json::exception &) {
			return Fail(failure, "tile room template is malformed or unsupported");
		} catch (const std::runtime_error &e) {
			failure = e.what();
			return false;
		}
	}
} // namespace engine::imagegraphexport
