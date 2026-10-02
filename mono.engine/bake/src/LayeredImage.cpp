#include "Decoders.hpp"

#include <engine/bake/Image.hpp>
#include <engine/bake/LayeredImage.hpp>
#include <engine/core/Xml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cryptopp/crc.h>
#include <cryptopp/filters.h>
#include <cryptopp/zlib.h>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace engine::bake {
	namespace {
		constexpr uint64_t MAXIMUM_ARCHIVE_BYTES = 64ull * 1024 * 1024;
		constexpr uint64_t MAXIMUM_DECODED_BYTES = 128ull * 1024 * 1024;
		constexpr uint32_t MAXIMUM_DIMENSION = 4096;
		struct Member {
			std::string Name;
			uint32_t Crc = 0, Compressed = 0, Expanded = 0, Offset = 0;
			uint16_t Method = 0, Flags = 0;
		};
		uint32_t Little(std::span<const std::byte> bytes, size_t offset, size_t count) {
			uint32_t value = 0;
			for (size_t index = 0; index < count; index++)
				value |= uint32_t{std::to_integer<uint8_t>(bytes[offset + index])} << (index * 8);
			return value;
		}
		bool Refuse(std::string &failure, std::string message) {
			failure = "layered image: " + std::move(message);
			return false;
		}
		bool SafeName(std::string_view name) {
			if (name.empty() || name.size() > 1024 || name.front() == '/' ||
				name.find('\\') != std::string_view::npos || name.find(':') != std::string_view::npos ||
				name.find('\0') != std::string_view::npos)
				return false;
			while (!name.empty()) {
				const size_t slash = name.find('/');
				const auto part = name.substr(0, slash);
				if (part == ".." || part == ".") return false;
				if (slash == std::string_view::npos) break;
				name.remove_prefix(slash + 1);
			}
			return true;
		}
		bool Members(
			std::span<const std::byte> bytes,
			std::vector<Member> &members,
			std::string &failure,
			uint64_t maximumExpandedBytes
		) {
			if (bytes.size() < 22 || bytes.size() > MAXIMUM_ARCHIVE_BYTES)
				return Refuse(failure, "archive size is outside its bounds");
			size_t ending = bytes.size() - 22;
			const size_t minimum = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
			while (true) {
				if (Little(bytes, ending, 4) == 0x06054b50 &&
					ending + 22 + Little(bytes, ending + 20, 2) == bytes.size())
					break;
				if (ending == minimum) return Refuse(failure, "ZIP end record is missing");
				ending--;
			}
			const uint32_t count = Little(bytes, ending + 10, 2);
			const uint32_t centralBytes = Little(bytes, ending + 12, 4),
						   centralOffset = Little(bytes, ending + 16, 4);
			if (Little(bytes, ending + 4, 2) != 0 || Little(bytes, ending + 6, 2) != 0 ||
				count != Little(bytes, ending + 8, 2) || count > 4096 || centralOffset > ending ||
				centralBytes != ending - centralOffset)
				return Refuse(failure, "multi-disk, ZIP64 or malformed central directory");
			size_t cursor = centralOffset;
			uint64_t expandedTotal = 0;
			std::unordered_set<std::string> names;
			for (uint32_t index = 0; index < count; index++) {
				if (ending - cursor < 46 || Little(bytes, cursor, 4) != 0x02014b50)
					return Refuse(failure, "truncated central member");
				const size_t nameBytes = Little(bytes, cursor + 28, 2),
							 extraBytes = Little(bytes, cursor + 30, 2),
							 commentBytes = Little(bytes, cursor + 32, 2);
				const size_t recordBytes = 46 + nameBytes + extraBytes + commentBytes;
				if (recordBytes > ending - cursor)
					return Refuse(failure, "member metadata exceeds directory");
				Member member;
				member.Name.assign(reinterpret_cast<const char *>(bytes.data() + cursor + 46), nameBytes);
				member.Flags = static_cast<uint16_t>(Little(bytes, cursor + 8, 2));
				member.Method = static_cast<uint16_t>(Little(bytes, cursor + 10, 2));
				member.Crc = Little(bytes, cursor + 16, 4);
				member.Compressed = Little(bytes, cursor + 20, 4);
				member.Expanded = Little(bytes, cursor + 24, 4);
				member.Offset = Little(bytes, cursor + 42, 4);
				if (!SafeName(member.Name) || !names.insert(member.Name).second ||
					(member.Flags & ~0x0808u) != 0 || (member.Method != 0 && member.Method != 8) ||
					member.Expanded > maximumExpandedBytes - expandedTotal ||
					member.Offset >= centralOffset || Little(bytes, cursor + 34, 2) != 0)
					return Refuse(failure, "member name, flags, method or sizes are unsupported");
				expandedTotal += member.Expanded;
				if (centralOffset - member.Offset < 30 || Little(bytes, member.Offset, 4) != 0x04034b50)
					return Refuse(failure, "local member header is missing");
				const size_t localNameBytes = Little(bytes, member.Offset + 26, 2),
							 localExtraBytes = Little(bytes, member.Offset + 28, 2);
				const size_t localHeaderBytes = 30 + localNameBytes + localExtraBytes;
				if (localHeaderBytes > centralOffset - member.Offset ||
					member.Compressed > centralOffset - member.Offset - localHeaderBytes ||
					Little(bytes, member.Offset + 6, 2) != member.Flags ||
					Little(bytes, member.Offset + 8, 2) != member.Method ||
					localNameBytes != member.Name.size() ||
					std::memcmp(bytes.data() + member.Offset + 30, member.Name.data(), localNameBytes) != 0)
					return Refuse(failure, "local and central member records disagree");
				if ((member.Flags & 8) == 0 && (Little(bytes, member.Offset + 14, 4) != member.Crc ||
												Little(bytes, member.Offset + 18, 4) != member.Compressed ||
												Little(bytes, member.Offset + 22, 4) != member.Expanded))
					return Refuse(failure, "local member checksum or size disagrees");
				member.Offset += static_cast<uint32_t>(localHeaderBytes);
				members.push_back(std::move(member));
				cursor += recordBytes;
			}
			return cursor == ending || Refuse(failure, "unexpected data after central members");
		}
		struct InflateBounds {};
		class FixedSink final : public CryptoPP::Bufferless<CryptoPP::Sink> {
		  public:
			FixedSink(std::vector<std::byte> &bytes, size_t maximum) : Bytes(bytes), Maximum(maximum) {}
			void IsolatedInitialize(const CryptoPP::NameValuePairs &) override {}
			size_t Put2(const CryptoPP::byte *input, size_t size, int, bool) override {
				if (size > Maximum - Bytes.size()) throw InflateBounds{};
				for (size_t index = 0; index < size; index++)
					Bytes.push_back(static_cast<std::byte>(input[index]));
				return 0;
			}

		  private:
			std::vector<std::byte> &Bytes;
			size_t Maximum;
		};
		bool Extract(
			std::span<const std::byte> bytes,
			const std::vector<Member> &members,
			std::string_view name,
			std::vector<std::byte> &out,
			std::string &failure
		) {
			const auto found = std::find_if(members.begin(), members.end(), [&](const Member &member) {
				return member.Name == name;
			});
			if (found == members.end())
				return Refuse(failure, "required member is missing: " + std::string(name));
			std::vector<std::byte> decoded;
			decoded.reserve(found->Expanded);
			const auto compressed = bytes.subspan(found->Offset, found->Compressed);
			if (found->Method == 0) {
				if (found->Compressed != found->Expanded)
					return Refuse(failure, "stored member sizes disagree");
				decoded.assign(compressed.begin(), compressed.end());
			} else {
				try {
					CryptoPP::Inflator inflator(new FixedSink(decoded, found->Expanded), false);
					inflator.Put(
						reinterpret_cast<const CryptoPP::byte *>(compressed.data()), compressed.size()
					);
					inflator.MessageEnd();
				} catch (const InflateBounds &) {
					return Refuse(failure, "member inflated past its stated size");
				} catch (const CryptoPP::Exception &) {
					return Refuse(failure, "invalid deflate member");
				}
			}
			if (decoded.size() != found->Expanded)
				return Refuse(failure, "member inflated size does not match directory");
			CryptoPP::CRC32 crc;
			std::array<CryptoPP::byte, 4> digest{};
			crc.CalculateDigest(
				digest.data(), reinterpret_cast<const CryptoPP::byte *>(decoded.data()), decoded.size()
			);
			uint32_t checksum = 0;
			for (size_t index = 0; index < 4; index++)
				checksum |= uint32_t{digest[index]} << (8 * index);
			if (checksum != found->Crc) return Refuse(failure, "member checksum does not match directory");
			out = std::move(decoded);
			return true;
		}
		bool DecodeImage(
			std::span<const std::byte> bytes,
			assets::TextureData &out,
			uint64_t &used,
			std::string &failure,
			uint64_t maximumDecodedBytes
		) {
			if (bytes.size() < 24 || ImageFormatOfBytes(bytes) != ImageFormat::Png)
				return Refuse(failure, "layer image is not PNG");
			uint32_t width = 0, height = 0;
			for (size_t index = 0; index < 4; index++) {
				width = (width << 8) | std::to_integer<uint8_t>(bytes[16 + index]);
				height = (height << 8) | std::to_integer<uint8_t>(bytes[20 + index]);
			}
			const uint64_t decoded = uint64_t{width} * height * 4;
			if (width == 0 || height == 0 || width > MAXIMUM_DIMENSION || height > MAXIMUM_DIMENSION ||
				decoded > maximumDecodedBytes - used)
				return Refuse(failure, "layer image dimensions exceed the decode budget");
			if (!ReadPng(bytes, out, failure)) return false;
			used += out.Pixels.size();
			return true;
		}
		bool KritaTiles(
			std::span<const std::byte> bytes,
			uint32_t width,
			uint32_t height,
			std::span<const std::byte> defaultPixel,
			assets::TextureData &out,
			uint64_t &used,
			std::string &failure,
			uint64_t maximumDecodedBytes
		) {
			const uint64_t imageBytes = uint64_t{width} * height * 4;
			if (imageBytes > maximumDecodedBytes - used ||
				(!defaultPixel.empty() && defaultPixel.size() != 4))
				return Refuse(failure, "Krita layer exceeds its image budget or default pixel size");
			size_t cursor = 0;
			const auto line = [&](std::string_view &value) {
				const size_t begin = cursor;
				while (cursor < bytes.size() && bytes[cursor] != std::byte{'\n'}) {
					if (cursor - begin >= 128) return false;
					cursor++;
				}
				if (cursor == bytes.size()) return false;
				value = {reinterpret_cast<const char *>(bytes.data() + begin), cursor++ - begin};
				return true;
			};
			const auto number = [](std::string_view value, auto &result) {
				const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
				return !value.empty() && parsed.ec == std::errc{} &&
					   parsed.ptr == value.data() + value.size();
			};
			std::string_view value;
			for (const auto expected : {"VERSION 2", "TILEWIDTH 64", "TILEHEIGHT 64", "PIXELSIZE 4"})
				if (!line(value) || value != expected)
					return Refuse(failure, "unsupported Krita tile version, dimensions or pixel size");
			uint32_t count = 0;
			if (!line(value) || !value.starts_with("DATA ") || !number(value.substr(5), count) ||
				count > 8192)
				return Refuse(failure, "invalid Krita tile count");
			assets::TextureData decoded;
			decoded.Width = width;
			decoded.Height = height;
			decoded.Format = assets::TextureFormat::RGBA8;
			decoded.Pixels.resize(static_cast<size_t>(imageBytes));
			if (!defaultPixel.empty()) {
				for (size_t offset = 0; offset < decoded.Pixels.size(); offset += 4) {
					decoded.Pixels[offset] = defaultPixel[2];
					decoded.Pixels[offset + 1] = defaultPixel[1];
					decoded.Pixels[offset + 2] = defaultPixel[0];
					decoded.Pixels[offset + 3] = defaultPixel[3];
				}
			}
			std::unordered_set<uint64_t> positions;
			for (uint32_t tileIndex = 0; tileIndex < count; tileIndex++) {
				if (!line(value)) return Refuse(failure, "missing Krita tile header");
				std::array<std::string_view, 4> fields{};
				for (size_t index = 0; index < 3; index++) {
					const size_t comma = value.find(',');
					if (comma == std::string_view::npos)
						return Refuse(failure, "malformed Krita tile header");
					fields[index] = value.substr(0, comma);
					value.remove_prefix(comma + 1);
				}
				fields[3] = value;
				int32_t x = 0, y = 0;
				uint32_t length = 0;
				if (!number(fields[0], x) || !number(fields[1], y) || fields[2] != "LZF" ||
					!number(fields[3], length) || length == 0 || length > 16385 ||
					length > bytes.size() - cursor || x % 64 != 0 || y % 64 != 0 ||
					!positions.insert((uint64_t{static_cast<uint32_t>(x)} << 32) | static_cast<uint32_t>(y))
						 .second)
					return Refuse(failure, "invalid or duplicated Krita tile coordinates or data size");
				const auto block = bytes.subspan(cursor, length);
				cursor += length;
				std::array<uint8_t, 16384> pixels{};
				bool planar = false;
				const auto flag = std::to_integer<uint8_t>(block[0]);
				if (flag == 0) {
					if (length != 16385) return Refuse(failure, "raw Krita tile has wrong size");
					for (size_t index = 0; index < pixels.size(); index++)
						pixels[index] = std::to_integer<uint8_t>(block[index + 1]);
				} else if (flag == 1) {
					planar = true;
					size_t input = 1, output = 0;
					while (input < block.size()) {
						const unsigned control = std::to_integer<uint8_t>(block[input++]);
						if (control < 32) {
							const size_t n = control + 1;
							if (n > block.size() - input || n > pixels.size() - output)
								return Refuse(failure, "truncated Krita LZF literal");
							for (size_t i = 0; i < n; i++)
								pixels[output++] = std::to_integer<uint8_t>(block[input++]);
						} else {
							size_t n = control >> 5;
							if (n == 7) {
								if (input == block.size())
									return Refuse(failure, "truncated Krita LZF length");
								n += std::to_integer<uint8_t>(block[input++]);
							}
							if (input == block.size()) return Refuse(failure, "truncated Krita LZF distance");
							const size_t distance =
								((control & 31) << 8) + std::to_integer<uint8_t>(block[input++]) + 1;
							n += 2;
							if (distance > output || n > pixels.size() - output)
								return Refuse(failure, "Krita LZF back-reference exceeds tile");
							for (size_t i = 0; i < n; i++) {
								pixels[output] = pixels[output - distance];
								output++;
							}
						}
					}
					if (output != pixels.size())
						return Refuse(failure, "Krita LZF tile expanded size is wrong");
				} else
					return Refuse(failure, "unknown Krita tile compression flag");
				for (size_t row = 0; row < 64; row++)
					for (size_t col = 0; col < 64; col++) {
						const int64_t targetX = int64_t{x} + static_cast<int64_t>(col),
									  targetY = int64_t{y} + static_cast<int64_t>(row);
						if (targetX < 0 || targetY < 0 || targetX >= width || targetY >= height) continue;
						const size_t pixel = row * 64 + col, target = (static_cast<size_t>(targetY) * width +
																	   static_cast<size_t>(targetX)) *
																	  4;
						for (size_t channel = 0; channel < 4; channel++) {
							const size_t nativeChannel = channel == 0 ? 2 : channel == 2 ? 0 : channel;
							decoded.Pixels[target + channel] = static_cast<std::byte>(
								pixels[planar ? nativeChannel * 4096 + pixel : pixel * 4 + nativeChannel]
							);
						}
					}
			}
			if (cursor != bytes.size()) return Refuse(failure, "unexpected bytes after Krita tiles");
			used += imageBytes;
			out = std::move(decoded);
			return true;
		}

	}

	bool ReadLayeredImage(
		std::span<const std::byte> bytes,
		LayeredImageFormat format,
		LayeredImage &out,
		std::string &failure,
		uint64_t maximumDecodedBytes
	) {
		maximumDecodedBytes = std::min(maximumDecodedBytes, MAXIMUM_DECODED_BYTES);
		if (format != LayeredImageFormat::OpenRaster && format != LayeredImageFormat::Krita)
			return Refuse(failure, "unknown layered format");
		std::vector<Member> members;
		if (!Members(bytes, members, failure, maximumDecodedBytes)) return false;
		std::vector<std::byte> decoded;
		if (!Extract(bytes, members, "mimetype", decoded, failure)) return false;
		const std::string_view mime(reinterpret_cast<const char *>(decoded.data()), decoded.size());
		if ((format == LayeredImageFormat::OpenRaster && mime != "image/openraster") ||
			(format == LayeredImageFormat::Krita && mime != "application/x-krita" &&
			 mime != "application/x-kra"))
			return Refuse(failure, "archive MIME type does not match the requested format");
		LayeredImage parsed;
		parsed.Format = format;
		if (!Extract(
				bytes,
				members,
				format == LayeredImageFormat::OpenRaster ? "stack.xml" : "maindoc.xml",
				decoded,
				failure
			))
			return false;
		if (decoded.size() > 4 * 1024 * 1024) return Refuse(failure, "layer metadata exceeds its budget");
		parsed.Metadata.assign(reinterpret_cast<const char *>(decoded.data()), decoded.size());
		if (!Extract(bytes, members, "mergedimage.png", decoded, failure)) return false;
		uint64_t used = 0;
		if (!DecodeImage(decoded, parsed.Merged, used, failure, maximumDecodedBytes)) return false;
		parsed.Width = parsed.Merged.Width;
		parsed.Height = parsed.Merged.Height;
		namespace xml = core::xml;
		const xml::Options options{"layered image", 64, false};
		xml::Failure refusal;
		std::string_view text = parsed.Metadata;
		std::vector<std::string> stack;
		std::string imageName, imageColourSpace;
		if (format == LayeredImageFormat::Krita) {
			const size_t begin = text.find("<!DOCTYPE");
			if (begin != std::string_view::npos) {
				const size_t ending = text.find('>', begin);
				const std::string_view expected = "<!DOCTYPE DOC PUBLIC \"-//KDE//DTD krita 2.0//EN\" "
												  "\"http://www.calligra.org/DTD/krita-2.0.dtd\">";
				if (ending == std::string_view::npos || text.substr(begin, ending - begin + 1) != expected)
					return Refuse(failure, "unknown Krita document type");
				parsed.Metadata.erase(begin, ending - begin + 1);
				text = parsed.Metadata;
			}
		}
		bool sawRoot = false;
		while (true) {
			xml::Tag tag;
			const auto status = xml::NextTag(text, options, tag, refusal);
			if (status == xml::Scan::Error) {
				failure = refusal.Message;
				return false;
			}
			if (status == xml::Scan::End) break;
			if (tag.Closing) {
				if (stack.empty() || stack.back() != tag.Name)
					return Refuse(failure, "unbalanced metadata elements");
				stack.pop_back();
				continue;
			}
			if (stack.size() >= 64) return Refuse(failure, "metadata nesting exceeds its budget");
			std::vector<xml::Attribute> attributes;
			if (!xml::ReadAttributes(tag.Attributes, options, attributes, refusal)) {
				failure = refusal.Message;
				return false;
			}
			const auto read = [&](std::string_view name, std::string &value) {
				const auto *attribute = xml::Find(attributes, name);
				value.clear();
				return !attribute || xml::Unescape(attribute->Value, options, value, refusal);
			};
			if (!sawRoot) {
				if ((format == LayeredImageFormat::OpenRaster && tag.Name != "image") ||
					(format == LayeredImageFormat::Krita && tag.Name != "DOC"))
					return Refuse(failure, "unexpected metadata root");
				sawRoot = true;
				if (format == LayeredImageFormat::Krita) {
					std::string version;
					if (!read("syntaxVersion", version) || version != "2.0")
						return Refuse(failure, "unsupported Krita document syntax version");
				}
				if (format == LayeredImageFormat::OpenRaster) {
					for (const auto &[name, expected] :
						 {std::pair{"w", parsed.Width}, {"h", parsed.Height}}) {
						std::string number;
						uint32_t dimension = 0;
						if (!read(name, number)) {
							failure = refusal.Message;
							return false;
						}
						const auto result =
							std::from_chars(number.data(), number.data() + number.size(), dimension);
						if (number.empty() || result.ec != std::errc{} ||
							result.ptr != number.data() + number.size() || dimension != expected)
							return Refuse(failure, "metadata canvas dimensions do not match merged image");
					}
				}
			}
			if (format == LayeredImageFormat::Krita && tag.Name == "IMAGE") {
				if (!read("name", imageName) || !read("colorspacename", imageColourSpace) ||
					!SafeName(imageName))
					return Refuse(failure, "invalid Krita image name or colour space");
				for (const auto &[name, expected] :
					 {std::pair{"width", parsed.Width}, {"height", parsed.Height}}) {
					std::string number;
					uint32_t dimension = 0;
					if (!read(name, number)) return Refuse(failure, "invalid Krita canvas dimension");
					const auto result =
						std::from_chars(number.data(), number.data() + number.size(), dimension);
					if (result.ec != std::errc{} || result.ptr != number.data() + number.size() ||
						dimension != expected)
						return Refuse(failure, "Krita canvas dimensions disagree with merged image");
				}
			}
			if (format == LayeredImageFormat::Krita && tag.Name == "layer") {
				std::string type, colourSpace, file;
				LayeredImageLayer layer;
				if (!read("nodetype", type) || !read("name", layer.Name) || !read("filename", file) ||
					!read("colorspacename", colourSpace))
					return Refuse(failure, "invalid Krita layer metadata");
				if (type == "grouplayer") {
					if (!tag.SelfClosing) stack.emplace_back(tag.Name);
					continue;
				}
				if (type != "paintlayer") return Refuse(failure, "unsupported Krita layer type: " + type);
				if (colourSpace.empty()) colourSpace = imageColourSpace;
				if (colourSpace != "RGBA" && colourSpace != "RGB8")
					return Refuse(failure, "Krita paint layer is not BGRA8");
				if (parsed.Layers.size() >= 4096 || imageName.empty() || !SafeName(file))
					return Refuse(failure, "invalid Krita paint layer path or count");
				for (const auto &[name, value] : {std::pair{"x", &layer.X}, {"y", &layer.Y}}) {
					std::string number;
					if (!read(name, number)) return Refuse(failure, "invalid Krita layer offset");
					if (!number.empty()) {
						const auto result =
							std::from_chars(number.data(), number.data() + number.size(), *value);
						if (result.ec != std::errc{} || result.ptr != number.data() + number.size())
							return Refuse(failure, "invalid Krita layer offset");
					}
				}
				layer.Source = imageName + "/layers/" + file;
				std::vector<std::byte> tiles, defaultPixel;
				if (!Extract(bytes, members, layer.Source, tiles, failure)) return false;
				if (std::any_of(members.begin(), members.end(), [&](const Member &member) {
						return member.Name == layer.Source + ".defaultpixel";
					}))
					if (!Extract(bytes, members, layer.Source + ".defaultpixel", defaultPixel, failure))
						return false;
				if (!KritaTiles(
						tiles,
						parsed.Width,
						parsed.Height,
						defaultPixel,
						layer.Pixels,
						used,
						failure,
						maximumDecodedBytes
					))
					return false;
				parsed.Layers.push_back(std::move(layer));
			}
			if (format == LayeredImageFormat::OpenRaster && tag.Name == "layer") {
				if (parsed.Layers.size() >= 4096) return Refuse(failure, "layer count exceeds its budget");
				LayeredImageLayer layer;
				if (!read("name", layer.Name) || !read("src", layer.Source)) {
					failure = refusal.Message;
					return false;
				}
				for (const auto &[name, value] : {std::pair{"x", &layer.X}, {"y", &layer.Y}}) {
					std::string number;
					if (!read(name, number)) {
						failure = refusal.Message;
						return false;
					}
					if (!number.empty()) {
						const auto result =
							std::from_chars(number.data(), number.data() + number.size(), *value);
						if (result.ec != std::errc{} || result.ptr != number.data() + number.size())
							return Refuse(failure, "layer offset is invalid");
					}
				}
				if (!Extract(bytes, members, layer.Source, decoded, failure) ||
					!DecodeImage(decoded, layer.Pixels, used, failure, maximumDecodedBytes))
					return false;
				parsed.Layers.push_back(std::move(layer));
			}
			if (!tag.SelfClosing) stack.emplace_back(tag.Name);
		}
		if (!sawRoot || !stack.empty()) return Refuse(failure, "metadata is missing or incomplete");
		out = std::move(parsed);
		return true;
	}
}
