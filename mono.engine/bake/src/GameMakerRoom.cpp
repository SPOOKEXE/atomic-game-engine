#include <engine/bake/GameMakerRoom.hpp>
#include <engine/bake/Image.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace engine::bake {
	namespace {
		using Json = nlohmann::json;
		struct Invalid : std::runtime_error {
			using std::runtime_error::runtime_error;
		};
		Json Parse(std::span<const std::byte> bytes) {
			if (bytes.empty() || bytes.size() > 4 * 1024 * 1024)
				throw Invalid("GameMaker JSON exceeds byte bounds");
			std::vector<std::set<std::string>> keys;
			uint64_t count = 0;
			auto text = std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
			return Json::parse(text, [&](int depth, Json::parse_event_t event, Json &value) {
				if (depth > 64 || ++count > 262144) throw Invalid("GameMaker JSON exceeds structure bounds");
				if (event == Json::parse_event_t::object_start) keys.emplace_back();
				if (event == Json::parse_event_t::object_end) keys.pop_back();
				if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
					throw Invalid("GameMaker JSON has duplicate keys");
				return true;
			});
		}
		double Number(const Json &j, std::string_view key, double fallback = 0) {
			auto i = j.find(key);
			if (i == j.end()) return fallback;
			if (!i->is_number()) throw Invalid("GameMaker numeric control has wrong type");
			auto v = i->get<double>();
			if (!std::isfinite(v) || std::abs(v) > 1000000)
				throw Invalid("GameMaker numeric control exceeds bounds");
			return v;
		}
		uint32_t Count(const Json &j, std::string_view key, uint32_t max) {
			auto v = Number(j, key);
			if (v < 0 || v > max || std::trunc(v) != v) throw Invalid("GameMaker count exceeds bounds");
			return uint32_t(v);
		}
		uint32_t Colour(const Json &j) {
			auto i = j.find("colour");
			if (i == j.end()) return 0xffffffff;
			if (!i->is_number_integer()) throw Invalid("GameMaker colour is not integral");
			auto n = i->get<int64_t>();
			if (n < 0 || uint64_t(n) > 0xffffffff) throw Invalid("GameMaker colour exceeds word range");
			return uint32_t(n);
		}
		std::string Ref(const Json &j, std::string_view key) {
			auto i = j.find(key);
			if (i == j.end() || i->is_null()) return {};
			if (!i->is_object() || !i->contains("path") || !(*i)["path"].is_string())
				throw Invalid("GameMaker resource reference is malformed");
			return (*i)["path"].get<std::string>();
		}
		struct Reader {
			std::span<const GameMakerResource> Resources;
			uint64_t Remaining, Work = 64ull * 1024 * 1024;
			std::map<std::string, Json> Jsons{};
			std::map<std::string, assets::TextureData> Images{};
			std::span<const GameMakerTileOverride> Overrides{};
			std::vector<bool> Used{};
			void Charge(uint64_t n) {
				if (n > Remaining) throw Invalid("GameMaker room exceeds operation byte budget");
				Remaining -= n;
			}
			std::span<const std::byte> Bytes(std::string_view key) {
				const GameMakerResource *found = nullptr;
				for (auto &r : Resources)
					if (r.Key == key) {
						if (found) throw Invalid("GameMaker dependency grant duplicated");
						found = &r;
					}
				if (!found) throw Invalid("GameMaker dependency not explicitly granted: " + std::string(key));
				return found->Bytes;
			}
			const Json &Resource(std::string_view key) {
				auto found = Jsons.find(std::string(key));
				if (found != Jsons.end()) return found->second;
				auto bytes = Bytes(key);
				Charge(bytes.size() * 4);
				return Jsons.emplace(std::string(key), Parse(bytes)).first->second;
			}
			const assets::TextureData &Sprite(std::string_view key) {
				auto found = Images.find(std::string(key));
				if (found != Images.end()) return found->second;
				const auto &sprite = Resource(key);
				if (sprite.value("resourceType", "") != "GMSprite" || !sprite.contains("frames") ||
					sprite["frames"].empty() || !sprite.contains("layers") || sprite["layers"].empty())
					throw Invalid("GameMaker sprite has no first frame/layer thumbnail");
				auto path = std::string(key);
				auto slash = path.find_last_of('/');
				if (slash == std::string::npos) throw Invalid("GameMaker sprite resource has no directory");
				path.resize(slash);
				path += "/layers/" + sprite["frames"][0].at("name").get<std::string>() + "/" +
						sprite["layers"][0].at("name").get<std::string>() + ".png";
				assets::TextureData image;
				std::string failure;
				auto bytes = Bytes(path);
				if (bytes.size() > Remaining) throw Invalid("GameMaker sprite bytes exceed budget");
				if (bytes.size() < 24 || bytes[0] != std::byte{137} || bytes[1] != std::byte{'P'} ||
					bytes[2] != std::byte{'N'} || bytes[3] != std::byte{'G'})
					throw Invalid("GameMaker thumbnail is not a PNG");
				const auto word = [&](size_t at) {
					uint32_t value = 0;
					for (size_t i = 0; i < 4; ++i)
						value = (value << 8) | std::to_integer<uint8_t>(bytes[at + i]);
					return value;
				};
				const auto width = word(16), height = word(20);
				if (!width || !height || width > 8192 || height > 8192 ||
					uint64_t(width) * height * 8 > Remaining)
					throw Invalid("GameMaker thumbnail dimensions exceed operation bounds");
				Charge(uint64_t(width) * height * 4);
				if (!ReadImage(bytes, image, failure)) throw Invalid(failure);
				Charge(image.Pixels.size());
				return Images.emplace(std::string(key), std::move(image)).first->second;
			}
			void Pixel(assets::TextureData &canvas, int x, int y, const std::byte *src, uint32_t tint) {
				if (x < 0 || y < 0 || x >= int(canvas.Width) || y >= int(canvas.Height)) return;
				auto p = (uint64_t(y) * canvas.Width + x) * 4;
				double a = std::to_integer<uint8_t>(src[3]) / 255.0,
					   b = std::to_integer<uint8_t>(canvas.Pixels[p + 3]) / 255.0, o = a + b * (1 - a);
				if (!o) return;
				for (size_t c = 0; c < 3; c++) {
					double s = std::to_integer<uint8_t>(src[c]) * ((tint >> (c * 8)) & 255) / 255.0;
					canvas.Pixels[p + c] = std::byte(uint8_t(
						std::clamp(
							std::round(
								(s * a + std::to_integer<uint8_t>(canvas.Pixels[p + c]) * b * (1 - a)) / o
							),
							0.0,
							255.0
						)
					));
				}
				canvas.Pixels[p + 3] = std::byte(uint8_t(std::round(o * 255)));
			}
			void Draw(
				assets::TextureData &canvas,
				const assets::TextureData &image,
				double x,
				double y,
				double sx,
				double sy,
				double rotation,
				uint32_t tint,
				uint32_t rx = 0,
				uint32_t ry = 0,
				uint32_t w = 0,
				uint32_t h = 0
			) {
				if (!w) w = image.Width;
				if (!h) h = image.Height;
				if (rx > image.Width || w > image.Width - rx || ry > image.Height || h > image.Height - ry)
					throw Invalid("GameMaker sprite part exceeds image");
				if (sx == 0 || sy == 0) return;
				double angle = rotation * 3.14159265358979323846 / 180, cs = std::cos(angle),
					   sn = std::sin(angle);
				double loX = x, hiX = x, loY = y, hiY = y;
				for (auto corner : std::array<std::array<double, 2>, 4>{
						 {{0, 0}, {double(w) * sx, 0}, {0, double(h) * sy}, {double(w) * sx, double(h) * sy}}
					 }) {
					double px = x + cs * corner[0] + sn * corner[1], py = y - sn * corner[0] + cs * corner[1];
					loX = std::min(loX, px);
					hiX = std::max(hiX, px);
					loY = std::min(loY, py);
					hiY = std::max(hiY, py);
				}
				int left = std::max(0, int(std::floor(loX))),
					right = std::min(int(canvas.Width), int(std::ceil(hiX))),
					top = std::max(0, int(std::floor(loY))),
					bottom = std::min(int(canvas.Height), int(std::ceil(hiY)));
				auto work = uint64_t(std::max(0, right - left)) * std::max(0, bottom - top);
				if (work > Work) throw Invalid("GameMaker sprite render exceeds work budget");
				Work -= work;
				for (int py = top; py < bottom; py++)
					for (int px = left; px < right; px++) {
						double dx = px + .5 - x, dy = py + .5 - y;
						int ix = int(std::floor((cs * dx - sn * dy) / sx)),
							iy = int(std::floor((sn * dx + cs * dy) / sy));
						if (ix >= 0 && iy >= 0 && ix < int(w) && iy < int(h))
							Pixel(
								canvas,
								px,
								py,
								image.Pixels.data() + ((uint64_t(iy + ry) * image.Width + ix + rx) * 4),
								tint
							);
					}
			}
			void Item(assets::TextureData &canvas, const Json &item, bool object) {
				auto key = Ref(item, object ? "objectId" : "spriteId");
				if (key.empty()) return;
				if (object) key = Ref(Resource(key), "spriteId");
				if (key.empty()) return;
				const auto &sprite = Resource(key);
				const auto &image = Sprite(key);
				double sx = Number(item, "scaleX", 1), sy = Number(item, "scaleY", 1),
					   ox = Number(sprite.at("sequence"), "xorigin"),
					   oy = Number(sprite.at("sequence"), "yorigin");
				Draw(
					canvas,
					image,
					Number(item, "x") - ox * sx,
					Number(item, "y") - oy * sy,
					sx,
					sy,
					Number(item, "rotation"),
					Colour(item)
				);
			}
			void Bind(const Json &layer, size_t depth) {
				if (depth > 64 || !layer.is_object())
					throw Invalid("GameMaker layer binding hierarchy invalid");
				if (layer.value("resourceType", "") == "GMRTileLayer")
					for (size_t i = 0; i < Overrides.size(); ++i)
						if (Overrides[i].LayerName == layer.value("name", "")) {
							if (Used[i]) throw Invalid("GameMaker layer binding is ambiguous");
							Used[i] = true;
						}
				auto children = layer.find("layers");
				if (children != layer.end()) {
					if (!children->is_array() || children->size() > 4096)
						throw Invalid("GameMaker layer binding count exceeds bounds");
					for (const auto &child : *children)
						Bind(child, depth + 1);
				}
			}
			void Layer(assets::TextureData &canvas, const Json &layer, size_t depth) {
				if (depth > 64) throw Invalid("GameMaker layer nesting exceeds bounds");
				if (!layer.is_object()) throw Invalid("GameMaker layer is not an object");
				if (!layer.value("visible", true)) return;
				auto children = layer.find("layers");
				if (children != layer.end()) {
					if (!children->is_array() || children->size() > 4096)
						throw Invalid("GameMaker nested layers exceed bounds");
					for (auto i = children->rbegin(); i != children->rend(); ++i)
						Layer(canvas, *i, depth + 1);
				}
				auto kind = layer.value("resourceType", "");
				if (kind == "GMRBackgroundLayer") {
					auto key = Ref(layer, "spriteId");
					auto tint = Colour(layer);
					if (key.empty()) {
						if (uint64_t(canvas.Width) * canvas.Height > Work)
							throw Invalid("GameMaker background exceeds fill work budget");
						Work -= uint64_t(canvas.Width) * canvas.Height;
						for (size_t p = 0; p < canvas.Pixels.size(); p += 4)
							for (size_t c = 0; c < 4; c++)
								canvas.Pixels[p + c] = std::byte(c == 3 ? 255 : (tint >> (c * 8)) & 255);
					} else {
						const auto &image = Sprite(key);
						bool tiled = layer.value("htiled", false) || layer.value("vtiled", false);
						for (uint32_t y = 0; y < (tiled ? canvas.Height : 1); y += image.Height)
							for (uint32_t x = 0; x < (tiled ? canvas.Width : 1); x += image.Width)
								Draw(canvas, image, x, y, 1, 1, 0, tint);
					}
				} else if (kind == "GMRInstanceLayer" || kind == "GMRAssetLayer") {
					auto key = kind == "GMRInstanceLayer" ? "instances" : "assets";
					auto items = layer.find(key);
					if (items != layer.end()) {
						if (!items->is_array() || items->size() > 32768)
							throw Invalid("GameMaker layer item count exceeds bounds");
						for (const auto &item : *items)
							Item(canvas, item, kind == "GMRInstanceLayer");
					}
				} else if (kind == "GMRTileLayer") {
					const GameMakerTileOverride *replacement = nullptr;
					for (size_t i = 0; i < Overrides.size(); ++i)
						if (Overrides[i].LayerName == layer.value("name", "")) {
							replacement = &Overrides[i];
						}
					if (replacement && replacement->Preview) {
						const auto &p = *replacement->Preview;
						if (!p.Width || !p.Height || p.Width > 8192 || p.Height > 8192 ||
							uint64_t(p.Width) * p.Height * 4 != p.Pixels.size())
							throw Invalid("GameMaker preview shape invalid");
						Charge(p.Pixels.size());
						Draw(canvas, p, 0, 0, 1, 1, 0, 0xffffffff);
						return;
					}
					auto key = replacement && !replacement->TilesetKey.empty() ? replacement->TilesetKey
																			   : Ref(layer, "tilesetId");
					if (key.empty()) return;
					const auto &set = Resource(key);
					auto sprite = Ref(set, "spriteId");
					if (sprite.empty()) return;
					const auto &image = Sprite(sprite);
					auto tw = Count(set, "tileWidth", 8192), th = Count(set, "tileHeight", 8192);
					if (!tw || !th) throw Invalid("GameMaker tile dimensions are empty");
					const auto &tiles = layer.at("tiles");
					auto w = Count(tiles, "SerialiseWidth", 8192), h = Count(tiles, "SerialiseHeight", 8192);
					if (uint64_t(w) * h > 1048576) throw Invalid("GameMaker tile grid exceeds bounds");
					Charge(uint64_t(w) * h * 4);
					std::vector<uint32_t> data;
					auto format = Count(tiles, "TileDataFormat", 1);
					if (replacement) {
						if (replacement->Data.size() != uint64_t(w) * h)
							throw Invalid("GameMaker replacement tile count differs");
						data = replacement->Data;
					} else if (!format) {
						const auto &raw = tiles.at("TileSerialiseData");
						if (!raw.is_array() || raw.size() != uint64_t(w) * h)
							throw Invalid("GameMaker tile serialisation count differs");
						for (const auto &v : raw) {
							if (!v.is_number_integer() || v.get<int64_t>() < 0 ||
								v.get<int64_t>() > 0xffffffff)
								throw Invalid("GameMaker tile index invalid");
							data.push_back(v.get<uint32_t>());
						}
					} else {
						const auto &raw = tiles.at("TileCompressedData");
						if (!raw.is_array() || raw.size() % 2 || raw.size() > 2 * 1048576)
							throw Invalid("GameMaker compressed tile runs invalid");
						for (size_t i = 0; i < raw.size(); i += 2) {
							if (!raw[i].is_number_integer() || !raw[i + 1].is_number_integer() ||
								raw[i].get<int64_t>() >= 0 || raw[i].get<int64_t>() < -int64_t(w) * h)
								throw Invalid("GameMaker compressed tile run is invalid");
							auto n = -raw[i].get<int64_t>(), v = raw[i + 1].get<int64_t>();
							if (n <= 0 || uint64_t(n) > uint64_t(w) * h - data.size() || v > 0xfffffffe)
								throw Invalid("GameMaker compressed tile run exceeds grid");
							auto tile = v < 0 ? 0 : uint32_t(v + (v != 0));
							data.insert(data.end(), n, tile);
						}
						if (data.size() != uint64_t(w) * h)
							throw Invalid("GameMaker compressed tiles leave incomplete grid");
					}
					const uint32_t cols = (image.Width + tw - 1) / tw;
					if (!cols) throw Invalid("GameMaker tile sprite columns empty");
					for (size_t i = 0; i < data.size(); i++) {
						auto tile = data[i];
						if (!tile) continue;
						uint64_t tx = uint64_t(tw) * tile % cols, ty = uint64_t(th) * (tile / cols);
						if (tx > UINT32_MAX || ty > UINT32_MAX)
							throw Invalid("GameMaker tile index exceeds atlas");
						Draw(canvas, image, (i % w) * tw, (i / w) * th, 1, 1, 0, 0xffffffff, tx, ty, tw, th);
					}
				} else if (kind != "GMRPathLayer" && kind != "GMREffectLayer" && kind != "GMRLayer" &&
						   kind != "GMRFolderLayer")
					throw Invalid("GameMaker room layer type is not represented: " + kind);
			}
		};
	}
	bool ReadGameMakerRoom(
		std::span<const std::byte> room,
		std::span<const GameMakerResource> resources,
		assets::TextureData &out,
		std::string &failure,
		uint64_t maximumBytes,
		std::span<const GameMakerTileOverride> overrides
	) try {
		failure.clear();
		if (resources.size() > 4096) throw Invalid("GameMaker resource count exceeds bounds");
		Reader reader{resources, maximumBytes};
		if (overrides.size() > 4096) throw Invalid("GameMaker replacement count exceeds bounds");
		reader.Overrides = overrides;
		reader.Used.resize(overrides.size());
		for (size_t i = 0; i < overrides.size(); ++i) {
			if (overrides[i].LayerName.empty() || overrides[i].LayerName.size() > 4096 ||
				overrides[i].Data.size() > 1048576)
				throw Invalid("GameMaker replacement binding exceeds bounds");
			for (size_t j = 0; j < i; ++j)
				if (overrides[j].LayerName == overrides[i].LayerName)
					throw Invalid("GameMaker replacement binding duplicated");
		}
		reader.Charge(room.size() * 4);
		auto root = Parse(room);
		if (root.value("resourceType", "") != "GMRoom") throw Invalid("GameMaker resource is not a room");
		auto w = Count(root.at("roomSettings"), "Width", 8192),
			 h = Count(root.at("roomSettings"), "Height", 8192);
		if (!w || !h || uint64_t(w) * h > 16777216) throw Invalid("GameMaker room dimensions exceed bounds");
		reader.Charge(uint64_t(w) * h * 4);
		assets::TextureData image;
		image.Width = w;
		image.Height = h;
		image.Pixels.resize(uint64_t(w) * h * 4);
		const auto &layers = root.at("layers");
		if (!layers.is_array() || layers.size() > 4096)
			throw Invalid("GameMaker room layer count exceeds bounds");
		for (const auto &layer : layers)
			reader.Bind(layer, 0);
		for (auto i = layers.rbegin(); i != layers.rend(); ++i)
			reader.Layer(image, *i, 0);
		if (std::find(reader.Used.begin(), reader.Used.end(), false) != reader.Used.end())
			throw Invalid("GameMaker replacement layer not found");
		out = std::move(image);
		return true;
	} catch (const std::exception &e) {
		failure = e.what();
		return false;
	}
}
