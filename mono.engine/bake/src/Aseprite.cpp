#include <engine/bake/Aseprite.hpp>

#include <algorithm>
#include <cmath>
#include <cryptopp/zlib.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace engine::bake {
	namespace {
		struct Invalid : std::runtime_error {
			using std::runtime_error::runtime_error;
		};
		struct Cursor {
			std::span<const std::byte> Bytes;
			size_t At = 0;
			std::span<const std::byte> Take(size_t n) {
				if (n > Bytes.size() - At) throw Invalid("truncated Aseprite record");
				auto r = Bytes.subspan(At, n);
				At += n;
				return r;
			}
			uint8_t U8() {
				return std::to_integer<uint8_t>(Take(1)[0]);
			}
			uint16_t U16() {
				auto a = U8();
				return a | uint16_t(U8()) << 8;
			}
			uint32_t U32() {
				auto a = U16();
				return a | uint32_t(U16()) << 16;
			}
			int16_t I16() {
				return static_cast<int16_t>(U16());
			}
			std::string Text() {
				auto data = Take(U16());
				return {reinterpret_cast<const char *>(data.data()), data.size()};
			}
		};
		struct Budget {
			uint64_t Remaining;
			void Take(uint64_t n) {
				if (n > Remaining) throw Invalid("Aseprite decoded payload exceeds byte budget");
				Remaining -= n;
			}
		};
		using Json = nlohmann::ordered_json;
		Json ByteArray(std::span<const std::byte> bytes) {
			Json out = Json::array();
			for (auto b : bytes)
				out.push_back(std::to_integer<uint8_t>(b));
			return out;
		}
		Json Binary(std::span<const std::byte> bytes, Budget &budget) {
			budget.Take(bytes.size() * 2 + 256);
			static constexpr char digits[] = "0123456789abcdef";
			std::string hex(bytes.size() * 2, '0');
			for (size_t i = 0; i < bytes.size(); ++i) {
				auto b = std::to_integer<uint8_t>(bytes[i]);
				hex[i * 2] = digits[b >> 4];
				hex[i * 2 + 1] = digits[b & 15];
			}
			return Json{{"$buffer_hex", std::move(hex)}};
		}
		uint64_t SerializedBound(const Json &value) {
			if (value.is_string()) return 2 + 6 * value.get_ref<const std::string &>().size();
			if (!value.is_structured()) return 32;
			uint64_t bytes = 2;
			for (auto it = value.begin(); it != value.end(); ++it) {
				bytes += 1 + SerializedBound(it.value());
				if (value.is_object()) bytes += 3 + 6 * it.key().size();
			}
			return bytes;
		}
		class Sink final : public CryptoPP::Bufferless<CryptoPP::Sink> {
		  public:
			Sink(std::vector<std::byte> &bytes, size_t maximum) : Bytes(bytes), Maximum(maximum) {}
			void IsolatedInitialize(const CryptoPP::NameValuePairs &) override {}
			size_t Put2(const CryptoPP::byte *input, size_t size, int, bool) override {
				if (size > Maximum - Bytes.size())
					throw Invalid("Aseprite inflated pixels exceed declared size");
				Bytes.insert(
					Bytes.end(),
					reinterpret_cast<const std::byte *>(input),
					reinterpret_cast<const std::byte *>(input) + size
				);
				return 0;
			}

		  private:
			std::vector<std::byte> &Bytes;
			size_t Maximum;
		};
		std::vector<std::byte> Inflate(std::span<const std::byte> data, size_t size) {
			std::vector<std::byte> result;
			result.reserve(size);
			CryptoPP::ZlibDecompressor decoder(new Sink(result, size), false);
			decoder.Put(reinterpret_cast<const CryptoPP::byte *>(data.data()), data.size());
			decoder.MessageEnd();
			if (result.size() != size) throw Invalid("Aseprite inflated pixels have wrong size");
			return result;
		}
		assets::TextureData Pixels(
			std::span<const std::byte> raw,
			uint32_t w,
			uint32_t h,
			uint16_t depth,
			const std::vector<std::array<uint8_t, 4>> &palette,
			uint8_t transparent,
			bool background,
			Budget &budget,
			bool tallSheet = false
		) {
			if (!w || !h || w > 8192 || (!tallSheet && h > 8192) || uint64_t(w) * h > 16777216)
				throw Invalid("Aseprite pixel dimensions exceed bounds");
			if (raw.size() != uint64_t(w) * h * (depth / 8))
				throw Invalid("Aseprite pixel byte count differs from dimensions");
			budget.Take(uint64_t(w) * h * 4);
			assets::TextureData out;
			out.Width = w;
			out.Height = h;
			out.Pixels.resize(uint64_t(w) * h * 4);
			for (size_t p = 0; p < uint64_t(w) * h; p++) {
				std::array<uint8_t, 4> rgba{};
				if (depth == 32)
					for (size_t c = 0; c < 4; c++)
						rgba[c] = std::to_integer<uint8_t>(raw[p * 4 + c]);
				else if (depth == 16) {
					rgba[0] = rgba[1] = rgba[2] = std::to_integer<uint8_t>(raw[p * 2]);
					rgba[3] = std::to_integer<uint8_t>(raw[p * 2 + 1]);
				} else {
					auto index = std::to_integer<uint8_t>(raw[p]);
					if (index >= palette.size()) throw Invalid("Aseprite palette index is missing");
					rgba = palette[index];
					if (index == transparent && !background) rgba[3] = 0;
				}
				for (size_t c = 0; c < 4; c++)
					out.Pixels[p * 4 + c] = std::byte(rgba[c]);
			}
			return out;
		}
		uint32_t Word(std::span<const std::byte> bytes) {
			Cursor c{bytes};
			return c.U32();
		}
	}
	bool ReadAseprite(
		std::span<const std::byte> bytes, AsepriteDocument &out, std::string &failure, uint64_t maximumBytes
	) try {
		failure.clear();
		if (bytes.size() < 128 || bytes.size() > 64ull * 1024 * 1024)
			throw Invalid("Aseprite file size exceeds bounds");
		Cursor file{bytes};
		if (file.U32() != bytes.size() || file.U16() != 0xa5e0) throw Invalid("invalid Aseprite file header");
		const auto count = file.U16();
		AsepriteDocument doc;
		doc.Width = file.U16();
		doc.Height = file.U16();
		const auto depth = file.U16();
		if (!count || count > 4096 || !doc.Width || !doc.Height || doc.Width > 8192 || doc.Height > 8192 ||
			(depth != 8 && depth != 16 && depth != 32))
			throw Invalid("Aseprite header dimensions, frames or depth unsupported");
		const auto flags = file.U32();
		const auto speed = file.U16();
		const auto reserved0 = file.U32(), reserved1 = file.U32();
		const auto transparent = file.U8();
		auto ignored = ByteArray(file.Take(3));
		auto colours = file.U16();
		const auto declaredColours = colours;
		if (!colours) colours = 256;
		Budget budget{maximumBytes};
		budget.Take(4096);
		Json inspection = {
			{"File size", bytes.size()},
			{"Magic number", 0xa5e0},
			{"Frame amount", count},
			{"Width", doc.Width},
			{"Height", doc.Height},
			{"Color depth", depth},
			{"Flags", flags},
			{"Speed", speed},
			{"0", reserved1},
			{"Palette entry", transparent},
			{"Ignore", std::move(ignored)},
			{"Number of colors", declaredColours}
		};
		(void)reserved0;
		inspection["Pixel width"] = file.U8();
		inspection["Pixel height"] = file.U8();
		inspection["Grid X"] = file.I16();
		inspection["Grid Y"] = file.I16();
		inspection["Grid width"] = file.U16();
		inspection["Grid height"] = file.U16();
		inspection["Unused"] = ByteArray(file.Take(84));
		inspection["Frames"] = Json::array();
		budget.Take(uint64_t(count) * sizeof(AsepriteFrame) + uint64_t(colours) * 4);
		doc.Palette.resize(colours, {0, 0, 0, 255});
		file.At = 128;
		doc.Frames.reserve(count);
		std::vector<uint16_t> layerFlags;
		uint64_t chunksTotal = 0;
		for (size_t f = 0; f < count; f++) {
			const auto start = file.At;
			const auto frameBytes = file.U32();
			if (frameBytes < 16 || frameBytes > bytes.size() - start)
				throw Invalid("invalid Aseprite frame size");
			if (file.U16() != 0xf1fa) throw Invalid("invalid Aseprite frame magic");
			auto chunks = file.U16();
			budget.Take(1024);
			Json rawFrame = {{"Length", frameBytes}, {"Magic number", 0xf1fa}, {"Chunk amount", chunks}};
			AsepriteFrame frame;
			frame.Duration = file.U16();
			rawFrame["Duration"] = frame.Duration;
			rawFrame["Unused"] = ByteArray(file.Take(2));
			const auto extended = file.U32();
			rawFrame["Chunk amount new"] = extended;
			rawFrame["Chunks"] = Json::array();
			const uint32_t n = extended ? extended : chunks;
			if (n > (frameBytes - 16) / 6 || (chunksTotal += n) > 65536)
				throw Invalid("Aseprite chunk count exceeds bounds");
			for (uint32_t ci = 0; ci < n; ci++) {
				const size_t frameEnd = start + frameBytes;
				if (file.At > frameEnd || frameEnd - file.At < 6)
					throw Invalid("Aseprite chunk header exceeds its frame");
				const auto chunkBytes = file.U32();
				const auto type = file.U16();
				if (chunkBytes < 6 || chunkBytes - 6 > frameEnd - file.At)
					throw Invalid("invalid Aseprite chunk size");
				Cursor chunk{file.Take(chunkBytes - 6)};
				budget.Take(2048);
				Json rawChunk = {{"Length", chunkBytes}, {"Type", type}};
				if (type == 0x2004) {
					if (f || doc.Layers.size() >= 4096) throw Invalid("Aseprite layer count/order invalid");
					AsepriteLayer layer;
					const auto lf = chunk.U16();
					rawChunk["Flag"] = lf;
					layer.Type = chunk.U16();
					rawChunk["Layer type"] = layer.Type;
					layer.Depth = chunk.U16();
					rawChunk["Child level"] = layer.Depth;
					rawChunk["Ignore"] = chunk.U16();
					rawChunk["Ignore"] = chunk.U16();
					layer.Blend = chunk.U16();
					rawChunk["Blend mode"] = layer.Blend;
					layer.Opacity = chunk.U8();
					rawChunk["Opacity"] = layer.Opacity;
					rawChunk["Unused"] = ByteArray(chunk.Take(3));
					layer.Name = chunk.Text();
					rawChunk["Name"] = layer.Name;
					layer.Visible = lf & 1;
					if (layer.Type > 2 || layer.Blend > 18 ||
						(layer.Depth && (doc.Layers.empty() || layer.Depth > doc.Layers.back().Depth + 1)))
						throw Invalid("Aseprite layer hierarchy or blend unsupported");
					if (!(flags & 1)) layer.Opacity = 255;
					if (layer.Type == 1 && !(flags & 2)) {
						layer.Opacity = 255;
						layer.Blend = 0;
					}
					if (layer.Type == 2) {
						layer.Tileset = chunk.U32();
						rawChunk["Tileset index"] = layer.Tileset;
					}
					if (flags & 4) chunk.Take(16);
					budget.Take(sizeof(layer) + layer.Name.size());
					doc.Layers.push_back(std::move(layer));
					layerFlags.push_back(lf);
				} else if (type == 0x2019) {
					const auto size = chunk.U32(), first = chunk.U32(), last = chunk.U32();
					rawChunk["Color amount"] = size;
					rawChunk["First index"] = first;
					rawChunk["Last index"] = last;
					rawChunk["Unused"] = ByteArray(chunk.Take(8));
					rawChunk["Palette"] = Json::array();
					if (!size || size > 65536 || first > last || last >= size)
						throw Invalid("Aseprite palette range invalid");
					if (size > doc.Palette.size()) {
						budget.Take((size - doc.Palette.size()) * 4);
						doc.Palette.resize(size);
					}
					for (uint32_t p = first; p <= last; p++) {
						budget.Take(1024);
						auto pf = chunk.U16();
						Json color = {{"Flag", pf}};
						for (auto &c : doc.Palette[p])
							c = chunk.U8();
						color["Red"] = doc.Palette[p][0];
						color["Green"] = doc.Palette[p][1];
						color["Blue"] = doc.Palette[p][2];
						color["Alpha"] = doc.Palette[p][3];
						if (pf & 1) color["Name"] = chunk.Text();
						rawChunk["Palette"].push_back(std::move(color));
					}
				} else if (type == 4 || type == 17) {
					uint32_t position = 0;
					auto packets = chunk.U16();
					rawChunk["Packet amount"] = packets;
					rawChunk["Packets"] = Json::array();
					for (uint32_t p = 0; p < packets; p++) {
						budget.Take(1024);
						auto skip = chunk.U8();
						position += skip;
						uint32_t ncol = chunk.U8();
						Json packet = {
							{"Entries skip index", skip}, {"Color amount", ncol}, {"Colors", Json::array()}
						};
						if (!ncol) ncol = 256;
						if (ncol > doc.Palette.size() - std::min(size_t(position), doc.Palette.size()))
							throw Invalid("Aseprite old palette range invalid");
						for (uint32_t i = 0; i < ncol; i++, position++) {
							uint32_t packed = 0;
							for (size_t c = 0; c < 3; c++) {
								auto v = chunk.U8();
								packed |= uint32_t(v) << (c * 8);
								doc.Palette[position][c] = type == 17 ? uint8_t(v * 255 / 63) : v;
							}
							doc.Palette[position][3] = 255;
							budget.Take(64);
							packet["Colors"].push_back(packed);
						}
						rawChunk["Packets"].push_back(std::move(packet));
					}
				} else if (type == 0x2018) {
					auto ntag = chunk.U16();
					rawChunk["Tag amount"] = ntag;
					rawChunk["Unused"] = ByteArray(chunk.Take(8));
					rawChunk["Tags"] = Json::array();
					if (ntag > 4096 - doc.Tags.size()) throw Invalid("Aseprite tag count exceeds bounds");
					for (uint32_t t = 0; t < ntag; t++) {
						budget.Take(2048);
						Json rawTag = Json::object();
						AsepriteTag tag;
						tag.First = chunk.U16();
						tag.Last = chunk.U16();
						tag.Direction = chunk.U8();
						tag.Repeat = chunk.U16();
						rawTag["Unused"] = ByteArray(chunk.Take(6));
						auto rgb = chunk.Take(3);
						rawTag["Color"] = uint32_t(std::to_integer<uint8_t>(rgb[0])) |
										  uint32_t(std::to_integer<uint8_t>(rgb[1])) << 8 |
										  uint32_t(std::to_integer<uint8_t>(rgb[2])) << 16;
						rawTag["Extra"] = chunk.U8();
						tag.Name = chunk.Text();
						rawTag["Frame start"] = tag.First;
						rawTag["Frame end"] = tag.Last;
						rawTag["Loop"] = tag.Direction;
						rawTag["Repeat amount"] = tag.Repeat;
						rawTag["Name"] = tag.Name;
						rawChunk["Tags"].push_back(std::move(rawTag));
						if (tag.First > tag.Last || tag.Last >= count || tag.Direction > 3)
							throw Invalid("Aseprite tag range invalid");
						budget.Take(sizeof(tag) + tag.Name.size());
						doc.Tags.push_back(std::move(tag));
					}
				} else if (type == 0x2023) {
					if (doc.Tilesets.size() >= 4096) throw Invalid("Aseprite tileset count exceeds bounds");
					AsepriteTileset set;
					set.Id = chunk.U32();
					set.Flags = chunk.U32();
					set.Count = chunk.U32();
					set.Width = chunk.U16();
					set.Height = chunk.U16();
					set.Base = chunk.I16();
					auto reserved = ByteArray(chunk.Take(14));
					set.Name = chunk.Text();
					rawChunk["ID"] = set.Id;
					rawChunk["Flag"] = set.Flags;
					rawChunk["Tile amount"] = set.Count;
					rawChunk["Tile width"] = set.Width;
					rawChunk["Tile height"] = set.Height;
					rawChunk["Base index"] = set.Base;
					rawChunk["Name"] = set.Name;
					rawChunk["Reserved"] = std::move(reserved);
					if ((set.Flags & 1) || !(set.Flags & 2))
						throw Invalid("Aseprite external tileset requires an explicit host asset");
					if (!set.Count || set.Count > 4096 || !set.Width || !set.Height || set.Height > 8192)
						throw Invalid("Aseprite tileset dimensions exceed bounds");
					if (set.Width > 8192 ||
						uint64_t(set.Width) * set.Height * set.Count * (depth / 8) > budget.Remaining)
						throw Invalid("Aseprite tileset pixels exceed budget");
					const auto compressed = chunk.U32();
					rawChunk["Data length"] = compressed;
					auto raw = Inflate(
						chunk.Take(compressed), uint64_t(set.Width) * set.Height * set.Count * (depth / 8)
					);
					rawChunk["Buffer"] = Binary(raw, budget);
					set.Pixels = Pixels(
						raw,
						set.Width,
						set.Height * set.Count,
						depth,
						doc.Palette,
						transparent,
						false,
						budget,
						true
					);
					budget.Take(sizeof(set) + set.Name.size());
					doc.Tilesets.push_back(std::move(set));
				} else if (type == 0x2005) {
					if (frame.Cels.size() >= doc.Layers.size())
						throw Invalid("Aseprite cel count exceeds layer count");
					AsepriteCel cel;
					cel.Layer = chunk.U16();
					cel.X = chunk.I16();
					cel.Y = chunk.I16();
					cel.Opacity = chunk.U8();
					const auto ct = chunk.U16();
					cel.Z = chunk.I16();
					rawChunk["Layer index"] = cel.Layer;
					rawChunk["X"] = cel.X;
					rawChunk["Y"] = cel.Y;
					rawChunk["Opacity"] = cel.Opacity;
					rawChunk["Cel type"] = ct;
					Json unused = Json::array({uint8_t(cel.Z & 255), uint8_t((uint16_t(cel.Z) >> 8) & 255)});
					for (auto b : chunk.Take(5))
						unused.push_back(std::to_integer<uint8_t>(b));
					rawChunk["Unused"] = std::move(unused);

					if (cel.Layer >= doc.Layers.size() ||
						std::any_of(frame.Cels.begin(), frame.Cels.end(), [&](const auto &other) {
							return other.Layer == cel.Layer;
						}))
						throw Invalid("Aseprite cel layer missing or duplicated");
					if (ct == 1) {
						auto previous = chunk.U16();
						rawChunk["Frame position"] = previous;
						if (previous >= f) throw Invalid("Aseprite linked cel is not an earlier frame");
						auto &cels = doc.Frames[previous].Cels;
						auto found = std::find_if(cels.begin(), cels.end(), [&](const auto &c) {
							return c.Layer == cel.Layer;
						});
						if (found == cels.end()) throw Invalid("Aseprite linked cel target absent");
						budget.Take(found->Pixels.Pixels.size());
						cel.Pixels = found->Pixels;
					} else if (ct == 0 || ct == 2) {
						auto w = chunk.U16(), h = chunk.U16();
						rawChunk["Width"] = w;
						rawChunk["Height"] = h;
						const uint64_t expected = uint64_t(w) * h * (depth / 8);
						if (expected > budget.Remaining)
							throw Invalid("Aseprite inflated cel exceeds budget");
						auto remaining = chunk.Take(chunk.Bytes.size() - chunk.At);
						auto raw = ct == 2 ? Inflate(remaining, expected)
										   : std::vector<std::byte>(remaining.begin(), remaining.end());
						if (raw.size() != expected)
							throw Invalid("Aseprite pixel byte count differs from dimensions");
						if (ct == 2) {
							rawChunk["Surface"] = nullptr;
							rawChunk["Buffer"] = Binary(raw, budget);
						} else {
							budget.Take(raw.size() * 16);
							rawChunk["Pixels"] = Json::array();
							for (size_t p = 0; p < uint64_t(w) * h; ++p) {
								auto pixel = std::span(raw).subspan(p * (depth / 8), depth / 8);
								rawChunk["Pixels"].push_back(ByteArray(pixel));
							}
						}
						cel.Pixels = Pixels(
							raw, w, h, depth, doc.Palette, transparent, layerFlags[cel.Layer] & 8, budget
						);
					} else if (ct == 3) {
						auto w = chunk.U16(), h = chunk.U16(), bits = chunk.U16();
						auto idmask = chunk.U32(), xmask = chunk.U32(), ymask = chunk.U32(),
							 dmask = chunk.U32();
						rawChunk["Width"] = w;
						rawChunk["Height"] = h;
						rawChunk["Bits per tile"] = bits;
						rawChunk["Bitmask for tile ID"] = idmask;
						rawChunk["X flip"] = xmask;
						rawChunk["Y flip"] = ymask;
						rawChunk["90CW rotation"] = dmask;
						rawChunk["Unused"] = ByteArray(chunk.Take(10));
						rawChunk["Surface"] = nullptr;
						if (bits != 32 || !idmask || uint64_t(w) * h * 4 > budget.Remaining)
							throw Invalid("Aseprite tilemap encoding exceeds bounds");
						const auto &layer = doc.Layers[cel.Layer];
						if (layer.Type != 2 || layer.Tileset >= doc.Tilesets.size())
							throw Invalid("Aseprite tilemap tileset missing");
						const auto &set = doc.Tilesets[layer.Tileset];
						auto raw = Inflate(chunk.Take(chunk.Bytes.size() - chunk.At), uint64_t(w) * h * 4);
						rawChunk["Tile Buffer"] = Binary(raw, budget);
						const uint64_t pw = uint64_t(w) * set.Width, ph = uint64_t(h) * set.Height;
						if (!pw || !ph || pw > 8192 || ph > 8192 || pw * ph * 4 > budget.Remaining)
							throw Invalid("Aseprite tilemap raster exceeds bounds");
						budget.Take(pw * ph * 4);
						cel.Pixels.Width = pw;
						cel.Pixels.Height = ph;
						cel.Pixels.Pixels.resize(pw * ph * 4);
						for (size_t ti = 0; ti < uint64_t(w) * h; ti++) {
							auto tile = Word(std::span(raw).subspan(ti * 4, 4));
							auto index = tile & idmask;
							if ((set.Flags & 4) && !index) continue;
							if (index >= set.Count) throw Invalid("Aseprite tile ID outside tileset");
							for (uint32_t y = 0; y < set.Height; y++)
								for (uint32_t x = 0; x < set.Width; x++) {
									uint32_t sx = x, sy = y;
									if (tile & dmask) {
										if (set.Width != set.Height)
											throw Invalid("Aseprite diagonal tile requires square tile");
										std::swap(sx, sy);
									}
									if (tile & xmask) sx = set.Width - 1 - sx;
									if (tile & ymask) sy = set.Height - 1 - sy;
									auto src =
										(uint64_t(index) * set.Width * set.Height + sy * set.Width + sx) * 4;
									auto dst = ((ti / w * set.Height + y) * pw + ti % w * set.Width + x) * 4;
									std::copy_n(
										set.Pixels.Pixels.begin() + src, 4, cel.Pixels.Pixels.begin() + dst
									);
								}
						}
					} else
						throw Invalid("Aseprite cel type unsupported");
					budget.Take(sizeof(cel));
					frame.Cels.push_back(std::move(cel));
				} else if (type == 0x2007) {
					auto profile = chunk.U16();
					rawChunk["Type"] = profile;
					rawChunk["Flag"] = chunk.U16();
					rawChunk["Fixed gamma"] = double(int32_t(chunk.U32())) / 65536;
					rawChunk["Unused"] = ByteArray(chunk.Take(8));
					if (profile == 2) {
						auto size = chunk.U32();
						budget.Take(uint64_t(size) * 64);
						rawChunk["ICC Data length"] = size;
						rawChunk["ICC Data"] = ByteArray(chunk.Take(size));
					}
				} else {
					rawChunk["Raw bytes"] = Binary(chunk.Bytes, budget);
					chunk.At = chunk.Bytes.size();
				}
				if (chunk.At < chunk.Bytes.size() && type != 0x2007)
					rawChunk["Trailing bytes"] = Binary(chunk.Bytes.subspan(chunk.At), budget);
				rawFrame["Chunks"].push_back(std::move(rawChunk));
			}
			if (file.At != start + frameBytes) throw Invalid("Aseprite frame has unaccounted bytes");
			inspection["Frames"].push_back(std::move(rawFrame));
			doc.Frames.push_back(std::move(frame));
		}
		if (file.At != bytes.size()) throw Invalid("Aseprite trailing bytes are not a frame");
		budget.Take(SerializedBound(inspection));
		doc.InspectionJson = inspection.dump();
		out = std::move(doc);
		return true;
	} catch (const std::exception &e) {
		failure = e.what();
		return false;
	}
}

namespace engine::bake {
	namespace {
		using RGB = std::array<double, 3>;
		double Lum(RGB c) {
			return .3 * c[0] + .59 * c[1] + .11 * c[2];
		}
		double Sat(RGB c) {
			return *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end());
		}
		RGB SetLum(RGB c, double l) {
			double d = l - Lum(c);
			for (auto &v : c)
				v += d;
			double n = *std::min_element(c.begin(), c.end()), x = *std::max_element(c.begin(), c.end());
			l = Lum(c);
			if (n < 0)
				for (auto &v : c)
					v = l + (v - l) * l / (l - n);
			if (x > 1)
				for (auto &v : c)
					v = l + (v - l) * (1 - l) / (x - l);
			return c;
		}
		RGB SetSat(RGB c, double s) {
			auto order = std::array<size_t, 3>{0, 1, 2};
			std::sort(order.begin(), order.end(), [&](auto a, auto b) { return c[a] < c[b]; });
			double range = c[order[2]] - c[order[0]];
			c[order[1]] = range > 0 ? (c[order[1]] - c[order[0]]) * s / range : 0;
			c[order[2]] = range > 0 ? s : 0;
			c[order[0]] = 0;
			return c;
		}
		RGB Blend(RGB b, RGB s, uint16_t mode) {
			if (mode == 12) return SetLum(SetSat(s, Sat(b)), Lum(b));
			if (mode == 13) return SetLum(SetSat(b, Sat(s)), Lum(b));
			if (mode == 14) return SetLum(s, Lum(b));
			if (mode == 15) return SetLum(b, Lum(s));
			RGB out{};
			for (size_t c = 0; c < 3; c++) {
				double x = b[c], y = s[c], v = y;
				switch (mode) {
				case 1:
					v = x * y;
					break;
				case 2:
					v = x + y - x * y;
					break;
				case 3:
					v = x <= .5 ? 2 * x * y : 1 - 2 * (1 - x) * (1 - y);
					break;
				case 4:
					v = std::min(x, y);
					break;
				case 5:
					v = std::max(x, y);
					break;
				case 6:
					v = y >= 1 ? 1 : std::min(1.0, x / (1 - y));
					break;
				case 7:
					v = y <= 0 ? 0 : 1 - std::min(1.0, (1 - x) / y);
					break;
				case 8:
					v = y <= .5 ? 2 * x * y : 1 - 2 * (1 - x) * (1 - y);
					break;
				case 9:
					v = y <= .5
							? x - (1 - 2 * y) * x * (1 - x)
							: x + (2 * y - 1) * ((x <= .25 ? ((16 * x - 12) * x + 4) * x : std::sqrt(x)) - x);
					break;
				case 10:
					v = std::abs(x - y);
					break;
				case 11:
					v = x + y - 2 * x * y;
					break;
				case 16:
					v = std::min(1.0, x + y);
					break;
				case 17:
					v = std::max(0.0, x - y);
					break;
				case 18:
					v = y <= 0 ? 1 : std::min(1.0, x / y);
					break;
				default:
					break;
				}
				out[c] = v;
			}
			return out;
		}
		void Composite(
			assets::TextureData &dst,
			const assets::TextureData &src,
			int x,
			int y,
			double opacity,
			uint16_t mode,
			uint64_t &work
		) {
			if (uint64_t(src.Width) * src.Height > work)
				throw Invalid("Aseprite composition work exceeds bounds");
			work -= uint64_t(src.Width) * src.Height;
			for (uint32_t sy = 0; sy < src.Height; sy++)
				for (uint32_t sx = 0; sx < src.Width; sx++) {
					int dx = x + int(sx), dy = y + int(sy);
					if (dx < 0 || dy < 0 || dx >= int(dst.Width) || dy >= int(dst.Height)) continue;
					size_t sp = (uint64_t(sy) * src.Width + sx) * 4, dp = (uint64_t(dy) * dst.Width + dx) * 4;
					double sa = std::to_integer<uint8_t>(src.Pixels[sp + 3]) / 255.0 * opacity,
						   ba = std::to_integer<uint8_t>(dst.Pixels[dp + 3]) / 255.0, a = sa + ba * (1 - sa);
					if (a == 0) continue;
					RGB b{}, s{};
					for (size_t c = 0; c < 3; c++) {
						b[c] = std::to_integer<uint8_t>(dst.Pixels[dp + c]) / 255.0;
						s[c] = std::to_integer<uint8_t>(src.Pixels[sp + c]) / 255.0;
					}
					auto mixed = Blend(b, s, mode);
					for (size_t c = 0; c < 3; c++)
						dst.Pixels[dp + c] = std::byte(uint8_t(
							std::clamp(
								std::round(
									((1 - sa) * ba * b[c] + (1 - ba) * sa * s[c] + ba * sa * mixed[c]) / a *
									255
								),
								0.0,
								255.0
							)
						));
					dst.Pixels[dp + 3] = std::byte(uint8_t(std::round(a * 255)));
				}
		}
	}
	bool RenderAseprite(
		const AsepriteDocument &doc,
		size_t frame,
		std::string_view name,
		bool crop,
		bool opacity,
		assets::TextureData &out,
		std::string &failure,
		uint64_t maximumBytes
	) try {
		failure.clear();
		if (frame >= doc.Frames.size()) throw Invalid("Aseprite frame outside animation");
		if (!doc.Width || !doc.Height || doc.Width > 8192 || doc.Height > 8192 || doc.Layers.size() > 4096 ||
			doc.Frames[frame].Cels.size() > 4096)
			throw Invalid("Aseprite render document exceeds structure bounds");
		std::vector<const AsepriteCel *> cels(doc.Layers.size(), nullptr);
		for (const auto &cel : doc.Frames[frame].Cels) {
			if (cel.Layer >= cels.size() || cels[cel.Layer] || !cel.Pixels.Width || !cel.Pixels.Height ||
				cel.Pixels.Width > 8192 || cel.Pixels.Height > 8192 ||
				uint64_t(cel.Pixels.Width) * cel.Pixels.Height * 4 != cel.Pixels.Pixels.size())
				throw Invalid("Aseprite render cel layout is invalid");
			cels[cel.Layer] = &cel;
		}
		for (const auto &layer : doc.Layers)
			if (layer.Type > 2 || layer.Blend > 18)
				throw Invalid("Aseprite render layer type or blend invalid");
		size_t first = 0, last = doc.Layers.size();
		if (!name.empty()) {
			auto found = std::find_if(doc.Layers.begin(), doc.Layers.end(), [&](const auto &layer) {
				return layer.Name == name;
			});
			if (found == doc.Layers.end()) throw Invalid("Aseprite layer name not found");
			first = found - doc.Layers.begin();
			last = first + 1;
			while (last < doc.Layers.size() && doc.Layers[last].Depth > found->Depth)
				last++;
		}
		assets::TextureData result;
		result.Width = doc.Width;
		result.Height = doc.Height;
		int originX = 0, originY = 0;
		if (crop && last == first + 1) {
			auto found = std::find_if(
				doc.Frames[frame].Cels.begin(), doc.Frames[frame].Cels.end(), [&](const auto &cel) {
					return cel.Layer == first;
				}
			);
			if (found != doc.Frames[frame].Cels.end()) {
				result.Width = found->Pixels.Width;
				result.Height = found->Pixels.Height;
				originX = found->X;
				originY = found->Y;
			}
		}
		const uint64_t canvas = uint64_t(result.Width) * result.Height * 4;
		if (canvas > maximumBytes) throw Invalid("Aseprite render surface exceeds byte budget");
		Budget budget{maximumBytes};
		budget.Take(canvas);
		result.Pixels.resize(canvas);
		uint64_t work = 64ull * 1024 * 1024;
		const auto render =
			[&](auto &&self, assets::TextureData &target, size_t begin, size_t end, size_t depth) -> void {
			if (depth > 128) throw Invalid("Aseprite group depth exceeds bounds");
			std::vector<size_t> order;
			for (size_t index = begin; index < end;) {
				order.push_back(index);
				size_t next = index + 1;
				while (next < end && doc.Layers[next].Depth > doc.Layers[index].Depth)
					++next;
				index = next;
			}
			const auto position = [&](size_t index) {
				return int64_t(index) + (cels[index] ? cels[index]->Z : 0);
			};
			std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
				return position(a) < position(b);
			});
			for (size_t index : order) {
				const auto &layer = doc.Layers[index];
				size_t next = index + 1;
				while (next < end && doc.Layers[next].Depth > layer.Depth)
					next++;
				if (!layer.Visible) {
					continue;
				}
				if (layer.Type == 1) {
					budget.Take(canvas);
					assets::TextureData group;
					group.Width = target.Width;
					group.Height = target.Height;
					group.Pixels.resize(canvas);
					self(self, group, index + 1, next, depth + 1);
					Composite(target, group, 0, 0, opacity ? layer.Opacity / 255.0 : 1, layer.Blend, work);
					budget.Remaining += canvas;
				} else {
					const auto *cel = cels[index];
					if (cel)
						Composite(
							target,
							cel->Pixels,
							cel->X - originX,
							cel->Y - originY,
							opacity ? layer.Opacity / 255.0 * cel->Opacity / 255.0 : 1,
							layer.Blend,
							work
						);
				}
			}
		};
		render(render, result, first, last, 0);
		out = std::move(result);
		return true;
	} catch (const std::exception &e) {
		failure = e.what();
		return false;
	}
}
