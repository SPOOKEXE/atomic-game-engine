#include <engine/core/Arguments.hpp>
#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <imagegraph_runner/Runner.hpp>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace imagegraph_runner {
	namespace {
		using engine::imagegraph::AudioCaptureFrame;
		using engine::imagegraph::Diagnostic;
		using engine::imagegraph::Image;
		using engine::imagegraph::ImageArray;
		using engine::imagegraph::ImageArrayItem;
		using engine::imagegraph::Limits;
		using engine::imagegraph::Status;

		constexpr uint64_t MAXIMUM_INPUT_BYTES = 16ull * 1024 * 1024;
		constexpr size_t MAXIMUM_OVERRIDE_BYTES = 4096;
		constexpr uint64_t MAXIMUM_RANGE_OUTPUT_BYTES = 512ull * 1024 * 1024;
		constexpr size_t MAXIMUM_ARRAY_TREE_ITEMS = 65536;
		constexpr size_t MAXIMUM_ARRAY_MANIFEST_BYTES = 4 * 1024 * 1024;
		constexpr size_t MAXIMUM_BUNDLE_MANIFEST_BYTES = 4 * 1024 * 1024;
		constexpr std::array<uint8_t, 8> PNG_SIGNATURE = {137, 80, 78, 71, 13, 10, 26, 10};

		constexpr std::array<uint32_t, 256> MakeCrcTable() {
			std::array<uint32_t, 256> table{};
			for (uint32_t index = 0; index < 256u; index++) {
				uint32_t value = index;
				for (unsigned bit = 0; bit < 8; bit++) {
					value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
				}
				table[index] = value;
			}
			return table;
		}

		constexpr std::array<uint32_t, 256> CRC_TABLE = MakeCrcTable();

		const char *StatusName(Status status) {
			switch (status) {
			case Status::Ok:
				return "Ok";
			case Status::Malformed:
				return "Malformed";
			case Status::UnsupportedVersion:
				return "UnsupportedVersion";
			case Status::LimitExceeded:
				return "LimitExceeded";
			case Status::DuplicateId:
				return "DuplicateId";
			case Status::UnknownNode:
				return "UnknownNode";
			case Status::UnknownPort:
				return "UnknownPort";
			case Status::TypeMismatch:
				return "TypeMismatch";
			case Status::InvalidValue:
				return "InvalidValue";
			case Status::InvalidGroup:
				return "InvalidGroup";
			case Status::InvalidOutput:
				return "InvalidOutput";
			case Status::DuplicateLink:
				return "DuplicateLink";
			case Status::Cycle:
				return "Cycle";
			case Status::UnsupportedExecution:
				return "UnsupportedExecution";
			}
			return "UnknownStatus";
		}

		void PrintDiagnostic(std::ostream &errors, const Diagnostic &diagnostic) {
			errors << "error status=" << StatusName(diagnostic.Code);
			if (!diagnostic.NodeId.empty()) errors << " node=" << std::quoted(diagnostic.NodeId);
			if (!diagnostic.Port.empty()) errors << " port=" << std::quoted(diagnostic.Port);
			errors << " message=" << std::quoted(diagnostic.Message) << '\n';
		}

		bool ParseInteger(std::string_view text, int64_t &value) {
			if (text.empty()) return false;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
		}

		bool ParseScalar(std::string_view text, double &value) {
			if (text.empty()) return false;
			const auto parsed =
				std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
				   std::isfinite(value);
		}

		bool ParseTick(std::string_view text, uint64_t &tick) {
			if (text.empty()) return false;
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), tick);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
		}

		bool ParseTickRange(std::string_view text, engine::imagegraph::TickRange &range) {
			std::array<std::string_view, 3> fields{};
			size_t count = 0;
			while (count < fields.size()) {
				const size_t separator = text.find(':');
				fields[count++] = text.substr(0, separator);
				if (separator == std::string_view::npos) {
					text = {};
					break;
				}
				text.remove_prefix(separator + 1);
			}
			if (!text.empty() || count < 2 || count > 3) return false;
			if (!ParseTick(fields[0], range.First) || !ParseTick(fields[1], range.Last)) return false;
			range.Step = 1;
			return count == 2 || ParseTick(fields[2], range.Step);
		}

		bool ParseTuple(std::string_view text, std::span<std::string_view> fields) {
			for (size_t index = 0; index < fields.size(); index++) {
				const size_t comma = text.find(',');
				if (index + 1 == fields.size()) {
					if (comma != std::string_view::npos) return false;
					fields[index] = text;
				} else {
					if (comma == std::string_view::npos) return false;
					fields[index] = text.substr(0, comma);
					text.remove_prefix(comma + 1);
				}
			}
			return true;
		}

		bool ParseTypedValue(
			std::string_view type,
			std::string_view text,
			engine::imagegraph::Value &value,
			engine::imagegraph::ValueType &valueType
		) {
			using engine::imagegraph::Colour;
			using engine::imagegraph::ValueType;
			using engine::imagegraph::Vector2;
			if (type == "bool") {
				if (text == "true")
					value = true;
				else if (text == "false")
					value = false;
				else
					return false;
				valueType = ValueType::Boolean;
				return true;
			}
			if (type == "int") {
				int64_t parsed = 0;
				if (!ParseInteger(text, parsed)) return false;
				value = parsed;
				valueType = ValueType::Integer;
				return true;
			}
			if (type == "scalar") {
				double parsed = 0.0;
				if (!ParseScalar(text, parsed)) return false;
				value = parsed;
				valueType = ValueType::Scalar;
				return true;
			}
			if (type == "text") {
				value = std::string(text);
				valueType = ValueType::Text;
				return true;
			}
			std::array<std::string_view, 4> components{};
			if (type == "colour") {
				if (!ParseTuple(text, components)) return false;
				std::array<uint8_t, 4> channels{};
				for (size_t index = 0; index < channels.size(); index++) {
					int64_t channel = 0;
					if (!ParseInteger(components[index], channel) || channel < 0 || channel > 255)
						return false;
					channels[index] = static_cast<uint8_t>(channel);
				}
				value = Colour{channels[0], channels[1], channels[2], channels[3]};
				valueType = ValueType::Colour;
				return true;
			}
			if (type == "vector2") {
				if (!ParseTuple(text, std::span<std::string_view>(components.data(), 2))) return false;
				Vector2 vector;
				if (!ParseScalar(components[0], vector.X) || !ParseScalar(components[1], vector.Y))
					return false;
				value = vector;
				valueType = ValueType::Vector2;
				return true;
			}
			return false;
		}

		Status ApplyOverride(
			engine::imagegraph::Document &document, std::string_view assignment, Diagnostic &diagnostic
		) {
			using engine::imagegraph::ValueType;
			if (assignment.size() > MAXIMUM_OVERRIDE_BYTES) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "parameter override exceeds the 4096-byte limit"
				};
				return diagnostic.Code;
			}
			const size_t equals = assignment.find('=');
			const size_t separator = assignment.rfind('.', equals);
			if (equals == std::string_view::npos || separator == std::string_view::npos || separator == 0 ||
				separator + 1 == equals) {
				diagnostic = {
					Status::Malformed, {}, {}, "parameter override must be NODE_ID.PROPERTY=TYPE:VALUE"
				};
				return diagnostic.Code;
			}
			const std::string_view nodeId = assignment.substr(0, separator);
			const std::string_view propertyId = assignment.substr(separator + 1, equals - separator - 1);
			const std::string_view encodedValue = assignment.substr(equals + 1);
			const size_t colon = encodedValue.find(':');
			if (colon == std::string_view::npos || colon == 0) {
				diagnostic = {
					Status::Malformed,
					std::string(nodeId),
					std::string(propertyId),
					"typed value is missing its type prefix"
				};
				return diagnostic.Code;
			}
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
					return candidate.Id == nodeId;
				});
			if (node == document.Nodes.end()) {
				diagnostic = {
					Status::UnknownNode,
					std::string(nodeId),
					std::string(propertyId),
					"parameter node does not exist"
				};
				return diagnostic.Code;
			}
			const auto *schema = engine::imagegraph::FindSchema(node->Type);
			if (!schema) {
				diagnostic = {
					Status::UnknownNode, node->Id, node->Type, "parameter node type has no registered schema"
				};
				return diagnostic.Code;
			}
			const auto property = std::find_if(
				schema->Properties.begin(), schema->Properties.end(), [&](const auto &candidate) {
					return candidate.Id == propertyId;
				}
			);
			if (property == schema->Properties.end()) {
				diagnostic = {
					Status::UnknownPort,
					node->Id,
					std::string(propertyId),
					"parameter property is not declared"
				};
				return diagnostic.Code;
			}
			const std::string_view type = encodedValue.substr(0, colon);
			const std::string_view text = encodedValue.substr(colon + 1);
			engine::imagegraph::Value value;
			ValueType valueType = ValueType::Boolean;
			if (!ParseTypedValue(type, text, value, valueType)) {
				diagnostic = {
					Status::InvalidValue,
					node->Id,
					std::string(propertyId),
					"parameter value is malformed or out of range"
				};
				return diagnostic.Code;
			}
			if (valueType != property->Type) {
				diagnostic = {
					Status::TypeMismatch,
					node->Id,
					std::string(propertyId),
					"parameter type does not match the registered property"
				};
				return diagnostic.Code;
			}
			const auto authored =
				std::find_if(node->Values.begin(), node->Values.end(), [&](const auto &candidate) {
					return candidate.Port == propertyId;
				});
			if (authored == node->Values.end())
				node->Values.push_back({std::string(propertyId), std::move(value)});
			else
				authored->Data = std::move(value);
			diagnostic = {};
			return Status::Ok;
		}

		bool ReadBounded(
			const std::filesystem::path &path,
			std::string &contents,
			bool &limitExceeded,
			std::string &failure,
			uint64_t maximumBytes = MAXIMUM_INPUT_BYTES
		) {
			limitExceeded = false;
			std::ifstream input(path, std::ios::binary);
			if (!input) {
				failure = "cannot open input graph";
				return false;
			}
			input.seekg(0, std::ios::end);
			const std::streamoff size = input.tellg();
			if (size < 0) {
				failure = "cannot determine input graph size";
				return false;
			}
			if (static_cast<uint64_t>(size) > maximumBytes) {
				limitExceeded = true;
				failure = "input file exceeds its " + std::to_string(maximumBytes) + " byte limit";
				return false;
			}
			contents.resize(static_cast<size_t>(size));
			input.seekg(0, std::ios::beg);
			if (!contents.empty()) {
				input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
				if (input.gcount() != static_cast<std::streamsize>(contents.size())) {
					failure = "could not read the complete input graph";
					return false;
				}
			}
			return true;
		}

		void AppendU32(std::vector<uint8_t> &bytes, uint32_t value) {
			bytes.push_back(static_cast<uint8_t>(value >> 24));
			bytes.push_back(static_cast<uint8_t>(value >> 16));
			bytes.push_back(static_cast<uint8_t>(value >> 8));
			bytes.push_back(static_cast<uint8_t>(value));
		}

		void UpdateCrc(uint32_t &crc, uint8_t byte) {
			crc = (crc >> 8) ^ CRC_TABLE[(crc ^ byte) & 0xffu];
		}

		uint8_t FilteredByte(const Image &image, uint64_t offset) {
			const uint64_t rowBytes = static_cast<uint64_t>(image.Width) * 4 + 1;
			const uint64_t column = offset % rowBytes;
			if (column == 0) return 0;
			const uint64_t row = offset / rowBytes;
			return image.Pixels[static_cast<size_t>(row * image.Width * 4 + column - 1)];
		}

		bool CompressStoredZlib(const Image &image, std::vector<uint8_t> &compressed) {
			const uint64_t rowBytes = static_cast<uint64_t>(image.Width) * 4 + 1;
			if (image.Height != 0 && rowBytes > std::numeric_limits<uint64_t>::max() / image.Height)
				return false;
			const uint64_t inputSize = rowBytes * image.Height;
			const uint64_t blockCount = (inputSize + 65534) / 65535;
			const uint64_t maximumSize = inputSize + blockCount * 5 + 6;
			if (maximumSize > std::numeric_limits<size_t>::max()) return false;
			compressed.clear();
			compressed.reserve(static_cast<size_t>(maximumSize));
			compressed.push_back(0x78);
			compressed.push_back(0x01);

			uint64_t adlerA = 1;
			uint64_t adlerB = 0;
			constexpr uint64_t ADLER_MODULUS = 65521;
			uint64_t moduloCount = 0;
			for (uint64_t offset = 0; offset < inputSize;) {
				const uint16_t count = static_cast<uint16_t>(std::min<uint64_t>(65535, inputSize - offset));
				const bool finalBlock = offset + count == inputSize;
				compressed.push_back(finalBlock ? 1 : 0);
				compressed.push_back(static_cast<uint8_t>(count));
				compressed.push_back(static_cast<uint8_t>(count >> 8));
				const uint16_t inverse = static_cast<uint16_t>(~count);
				compressed.push_back(static_cast<uint8_t>(inverse));
				compressed.push_back(static_cast<uint8_t>(inverse >> 8));
				for (uint64_t index = 0; index < count; index++) {
					const uint8_t byte = FilteredByte(image, offset + index);
					compressed.push_back(byte);
					adlerA += byte;
					adlerB += adlerA;
					if (++moduloCount == 5552) {
						adlerA %= ADLER_MODULUS;
						adlerB %= ADLER_MODULUS;
						moduloCount = 0;
					}
				}
				offset += count;
			}
			adlerA %= ADLER_MODULUS;
			adlerB %= ADLER_MODULUS;
			AppendU32(compressed, static_cast<uint32_t>((adlerB << 16) | adlerA));
			return compressed.size() == maximumSize;
		}

		bool WriteChunk(std::ofstream &output, const char (&type)[5], std::span<const uint8_t> payload) {
			if (payload.size() > std::numeric_limits<uint32_t>::max()) return false;
			uint32_t crc = 0xffffffffu;
			for (size_t index = 0; index < 4; index++)
				UpdateCrc(crc, static_cast<uint8_t>(type[index]));
			for (const uint8_t byte : payload)
				UpdateCrc(crc, byte);
			crc = ~crc;
			std::vector<uint8_t> header;
			AppendU32(header, static_cast<uint32_t>(payload.size()));
			output.write(
				reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size())
			);
			output.write(type, 4);
			if (!payload.empty()) {
				output.write(
					reinterpret_cast<const char *>(payload.data()),
					static_cast<std::streamsize>(payload.size())
				);
			}
			header.clear();
			AppendU32(header, crc);
			output.write(
				reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size())
			);
			return output.good();
		}

		bool WritePng(const std::filesystem::path &path, const Image &image, std::string &failure) {
			const uint64_t pixelBytes = static_cast<uint64_t>(image.Width) * image.Height * 4;
			if (image.Width == 0 || image.Height == 0 || image.Width > Limits::MaximumDimension ||
				image.Height > Limits::MaximumDimension || pixelBytes > Limits::MaximumOutputBytes ||
				image.Pixels.size() != pixelBytes) {
				failure = "evaluated image violates the RGBA8 export bounds";
				return false;
			}
			std::vector<uint8_t> compressed;
			if (!CompressStoredZlib(image, compressed)) {
				failure = "could not encode bounded PNG image data";
				return false;
			}
			if (compressed.size() > std::numeric_limits<uint32_t>::max()) {
				failure = "PNG image data exceeds the chunk size limit";
				return false;
			}

			// Publish only complete still images. Keep staging beside the target so rename stays on one
			// filesystem.
			static std::atomic<uint64_t> stagingSequence{0};
			const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
			std::filesystem::path staging = path;
			staging += ".partial-" + std::to_string(nonce) + "-" +
					   std::to_string(stagingSequence.fetch_add(1, std::memory_order_relaxed));
			std::ofstream output(staging, std::ios::binary | std::ios::trunc);
			if (!output) {
				failure = "cannot open PNG output path";
				return false;
			}
			const auto discard = [&] {
				output.close();
				std::error_code ignored;
				std::filesystem::remove(staging, ignored);
			};
			output.write(reinterpret_cast<const char *>(PNG_SIGNATURE.data()), PNG_SIGNATURE.size());
			std::vector<uint8_t> header;
			AppendU32(header, image.Width);
			AppendU32(header, image.Height);
			header.insert(header.end(), {8, 6, 0, 0, 0});
			const std::array<uint8_t, 0> empty{};
			if (!WriteChunk(output, "IHDR", header) || !WriteChunk(output, "IDAT", compressed) ||
				!WriteChunk(output, "IEND", empty)) {
				failure = "could not write complete PNG output";
				discard();
				return false;
			}
			output.flush();
			if (!output) {
				failure = "could not flush PNG output";
				discard();
				return false;
			}
			output.close();
			if (!output) {
				failure = "could not close complete PNG output";
				discard();
				return false;
			}
			std::error_code error;
			std::filesystem::rename(staging, path, error);
			if (error) {
				failure = "could not publish complete PNG output";
				discard();
				return false;
			}
			return true;
		}

		uint64_t EncodedPngBytes(const Image &image) {
			const uint64_t scanlineBytes =
				static_cast<uint64_t>(image.Width) * image.Height * 4 + image.Height;
			const uint64_t blockCount = (scanlineBytes + 65534) / 65535;
			return 63 + scanlineBytes + blockCount * 5;
		}

		std::filesystem::path FramePath(const std::filesystem::path &base, uint64_t tick) {
			std::ostringstream suffix;
			suffix.imbue(std::locale::classic());
			suffix << base.stem().string() << ".tick-" << std::setw(20) << std::setfill('0') << tick
				   << base.extension().string();
			return base.parent_path() / suffix.str();
		}

		bool SamePath(const std::filesystem::path &left, const std::filesystem::path &right) {
			std::error_code error;
			const auto absoluteLeft = std::filesystem::absolute(left, error).lexically_normal();
			if (error) return false;
			const auto absoluteRight = std::filesystem::absolute(right, error).lexically_normal();
			if (error) return false;
			if (absoluteLeft == absoluteRight) return true;
			error.clear();
			return std::filesystem::equivalent(absoluteLeft, absoluteRight, error) && !error;
		}

		std::filesystem::path ArrayImagePath(const std::filesystem::path &manifest, size_t index) {
			std::ostringstream suffix;
			suffix.imbue(std::locale::classic());
			suffix << manifest.stem().string() << ".image-" << std::setw(6) << std::setfill('0') << index
				   << ".png";
			return manifest.parent_path() / suffix.str();
		}

		void AppendJsonString(std::string &json, std::string_view text) {
			constexpr char HEX[] = "0123456789abcdef";
			json.push_back('"');
			for (const unsigned char character : text) {
				switch (character) {
				case '"':
					json += "\\\"";
					break;
				case '\\':
					json += "\\\\";
					break;
				case '\b':
					json += "\\b";
					break;
				case '\f':
					json += "\\f";
					break;
				case '\n':
					json += "\\n";
					break;
				case '\r':
					json += "\\r";
					break;
				case '\t':
					json += "\\t";
					break;
				default:
					if (character < 0x20) {
						json += "\\u00";
						json.push_back(HEX[character >> 4]);
						json.push_back(HEX[character & 0x0f]);
					} else {
						json.push_back(static_cast<char>(character));
					}
					break;
				}
			}
			json.push_back('"');
		}

		void AppendHash(std::string &json, uint64_t hash) {
			std::ostringstream encoded;
			encoded.imbue(std::locale::classic());
			encoded << "0x" << std::hex << std::setfill('0') << std::setw(16) << hash;
			AppendJsonString(json, encoded.str());
		}

		struct BundleFrame {
			uint64_t Tick = 0;
			uint32_t Width = 0;
			uint32_t Height = 0;
			uint64_t Hash = 0;
			std::string File;
		};

		struct BundleStage {
			std::filesystem::path Directory;
			~BundleStage() {
				if (Directory.empty()) return;
				std::error_code ignored;
				std::filesystem::remove_all(Directory, ignored);
			}
		};

		bool Occupied(const std::filesystem::path &path) {
			std::error_code error;
			const auto status = std::filesystem::symlink_status(path, error);
			if (error == std::errc::no_such_file_or_directory) return false;
			return error || status.type() != std::filesystem::file_type::not_found;
		}

		bool BeginBundle(const std::filesystem::path &destination, BundleStage &stage, std::string &failure) {
			if (Occupied(destination)) {
				failure = "bundle destination already exists or cannot be inspected";
				return false;
			}
			const std::filesystem::path parent =
				destination.parent_path().empty() ? std::filesystem::path(".") : destination.parent_path();
			std::error_code error;
			std::filesystem::create_directories(parent, error);
			if (error) {
				failure = "cannot create bundle parent directory";
				return false;
			}
			static std::atomic<uint64_t> sequence{0};
			const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
			std::filesystem::path staging = destination;
			staging += ".partial-" + std::to_string(nonce) + "-" +
					   std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
			if (!std::filesystem::create_directory(staging, error) || error) {
				failure = "cannot create bundle staging directory";
				return false;
			}
			stage.Directory = std::move(staging);
			return true;
		}

		bool WriteBundleManifest(
			const std::filesystem::path &path,
			std::string_view outputId,
			std::span<const BundleFrame> frames,
			uint64_t imageBytes,
			std::string &failure
		) {
			std::string manifest = "{\"format\":\"atomic.imagegraph.sequence.v1\",\"output_id\":";
			AppendJsonString(manifest, outputId);
			manifest += ",\"frames\":[";
			for (size_t index = 0; index < frames.size(); index++) {
				const BundleFrame &frame = frames[index];
				if (index != 0) manifest.push_back(',');
				manifest += "{\"tick\":" + std::to_string(frame.Tick) + ",\"file\":";
				AppendJsonString(manifest, frame.File);
				manifest += ",\"width\":" + std::to_string(frame.Width) +
							",\"height\":" + std::to_string(frame.Height) + ",\"hash\":";
				AppendHash(manifest, frame.Hash);
				manifest.push_back('}');
			}
			manifest += "]}\n";
			if (manifest.size() > MAXIMUM_BUNDLE_MANIFEST_BYTES ||
				manifest.size() > MAXIMUM_RANGE_OUTPUT_BYTES - imageBytes) {
				failure = "bundle manifest exceeds the bounded frame range output";
				return false;
			}
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if (!output) {
				failure = "cannot open bundle manifest";
				return false;
			}
			output.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
			output.flush();
			if (!output) {
				failure = "cannot write complete bundle manifest";
				return false;
			}
			return true;
		}

		struct ArrayFrame {
			std::vector<std::filesystem::path> ImagePaths;
			std::string Manifest;
			uint64_t EncodedBytes = 0;
		};

		Status BuildArrayFrame(
			const ImageArray &images,
			const std::string &outputId,
			uint64_t tick,
			const std::filesystem::path &manifestPath,
			const std::filesystem::path &inputPath,
			ArrayFrame &frame,
			Diagnostic &diagnostic
		) {
			if (images.Images.size() > Limits::MaximumArrayElements) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "image array exceeds the 4096-image export limit"
				};
				return diagnostic.Code;
			}
			frame = {};
			frame.ImagePaths.reserve(images.Images.size());
			for (size_t index = 0; index < images.Images.size(); index++) {
				const Image &image = images.Images[index];
				const uint64_t pixelBytes = static_cast<uint64_t>(image.Width) * image.Height * 4;
				if (image.Width == 0 || image.Height == 0 || image.Width > Limits::MaximumDimension ||
					image.Height > Limits::MaximumDimension || pixelBytes > Limits::MaximumOutputBytes ||
					image.Pixels.size() != pixelBytes) {
					diagnostic = {
						Status::InvalidOutput,
						{},
						{},
						"image array contains an image outside RGBA8 export bounds"
					};
					return diagnostic.Code;
				}
				const std::filesystem::path imagePath = ArrayImagePath(manifestPath, index);
				if (SamePath(inputPath, imagePath)) {
					diagnostic = {Status::InvalidValue, {}, {}, "array image path aliases the input graph"};
					return diagnostic.Code;
				}
				const uint64_t bytes = EncodedPngBytes(image);
				if (bytes > MAXIMUM_RANGE_OUTPUT_BYTES ||
					frame.EncodedBytes > MAXIMUM_RANGE_OUTPUT_BYTES - bytes) {
					diagnostic = {
						Status::LimitExceeded, {}, {}, "image array exceeds the 512 MiB PNG output limit"
					};
					return diagnostic.Code;
				}
				frame.EncodedBytes += bytes;
				frame.ImagePaths.push_back(imagePath);
			}

			std::vector<uint8_t> referenced(images.Images.size(), 0);
			struct ShapeCursor {
				const std::vector<ImageArrayItem> *Items = nullptr;
				size_t Next = 0;
				bool CloseObject = false;
			};
			std::vector<ShapeCursor> stack;
			stack.push_back({&images.Items, 0, false});
			size_t shapeItems = 0;
			frame.Manifest = "{\"format\":\"atomic.imagegraph.array.v1\",\"output_id\":";
			AppendJsonString(frame.Manifest, outputId);
			frame.Manifest += ",\"tick\":" + std::to_string(tick) + ",\"items\":[";
			while (!stack.empty()) {
				ShapeCursor &cursor = stack.back();
				if (cursor.Next == cursor.Items->size()) {
					frame.Manifest.push_back(']');
					const bool closeObject = cursor.CloseObject;
					stack.pop_back();
					if (closeObject) frame.Manifest.push_back('}');
					continue;
				}
				if (cursor.Next != 0) frame.Manifest.push_back(',');
				if (++shapeItems > MAXIMUM_ARRAY_TREE_ITEMS || stack.size() > Limits::MaximumNodes) {
					diagnostic = {
						Status::LimitExceeded, {}, {}, "image array shape exceeds the manifest item limit"
					};
					return diagnostic.Code;
				}
				const ImageArrayItem &item = (*cursor.Items)[cursor.Next++];
				if (const auto *imageIndex = std::get_if<size_t>(&item.Data)) {
					if (*imageIndex >= images.Images.size()) {
						diagnostic = {
							Status::InvalidOutput, {}, {}, "image array shape references a missing image"
						};
						return diagnostic.Code;
					}
					referenced[*imageIndex] = 1;
					frame.Manifest += "{\"image\":" + std::to_string(*imageIndex) + "}";
				} else {
					const auto &children = std::get<std::vector<ImageArrayItem>>(item.Data);
					if (stack.size() >= Limits::MaximumNodes) {
						diagnostic = {
							Status::LimitExceeded,
							{},
							{},
							"image array nesting exceeds the manifest depth limit"
						};
						return diagnostic.Code;
					}
					frame.Manifest += "{\"items\":[";
					stack.push_back({&children, 0, true});
				}
			}
			if (std::any_of(referenced.begin(), referenced.end(), [](uint8_t used) { return used == 0; })) {
				diagnostic = {Status::InvalidOutput, {}, {}, "image array contains an unreferenced image"};
				return diagnostic.Code;
			}
			frame.Manifest += ",\"images\":[";
			for (size_t index = 0; index < images.Images.size(); index++) {
				if (index != 0) frame.Manifest.push_back(',');
				const Image &image = images.Images[index];
				frame.Manifest += "{\"index\":" + std::to_string(index) + ",\"file\":";
				AppendJsonString(frame.Manifest, frame.ImagePaths[index].filename().generic_string());
				frame.Manifest += ",\"width\":" + std::to_string(image.Width) +
								  ",\"height\":" + std::to_string(image.Height) + ",\"hash\":";
				AppendHash(frame.Manifest, image.Hash);
				frame.Manifest.push_back('}');
			}
			frame.Manifest += "]}\n";
			if (frame.Manifest.size() > MAXIMUM_ARRAY_MANIFEST_BYTES) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "image array manifest exceeds the 4 MiB output limit"
				};
				return diagnostic.Code;
			}
			if (frame.Manifest.size() > MAXIMUM_RANGE_OUTPUT_BYTES - frame.EncodedBytes) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "image array and manifest exceed the 512 MiB output limit"
				};
				return diagnostic.Code;
			}
			frame.EncodedBytes += frame.Manifest.size();
			diagnostic = {};
			return Status::Ok;
		}

		bool WriteArrayFrame(
			const ImageArray &images,
			const ArrayFrame &frame,
			const std::filesystem::path &manifestPath,
			std::string &failure
		) {
			for (size_t index = 0; index < images.Images.size(); index++)
				if (!WritePng(frame.ImagePaths[index], images.Images[index], failure)) return false;
			std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
			if (!output) {
				failure = "cannot open image array manifest path";
				return false;
			}
			output.write(frame.Manifest.data(), static_cast<std::streamsize>(frame.Manifest.size()));
			output.flush();
			if (!output) {
				failure = "could not write complete image array manifest";
				return false;
			}
			return true;
		}

	}

	int Run(int argc, char **argv, std::ostream &output, std::ostream &errors) {
		engine::core::Arguments arguments(
			"imagegraph",
			"Evaluates an imagegraph output at one tick or a streamed tick range, writing deterministic "
			"RGBA8 PNGs or typed scalar previews."
		);
		arguments.Value("input", "PATH", "Authored imagegraph text document");
		arguments.Value("output-id", "ID", "Durable output ID to evaluate");
		arguments.Value("output", "PATH", "PNG path, or JSON manifest path for an image-array output");
		arguments.Flag("value", "Evaluate a typed scalar output and print its deterministic value record");
		arguments.Value("audio-capture", "PATH", "Bounded recorded mono audio input frames");
		arguments.Value(
			"bundle", "DIR", "Publish a complete PNG frame range and manifest into a new directory"
		);
		arguments.Value(
			"set", "NODE_ID.PROPERTY=TYPE:VALUE", "Override a typed node property; may be repeated"
		);
		arguments.Value("tick", "N", "Timeline tick to evaluate; defaults to 0");
		arguments.Value(
			"frames", "FIRST:LAST[:STEP]", "Inclusive timeline tick range streamed to numbered PNG files"
		);

		const auto parsed = arguments.Parse(argc, argv);
		if (parsed.VersionRequested) {
			output << arguments.VersionLine();
			return 0;
		}
		if (parsed.HelpRequested) {
			output << arguments.Help();
			return 0;
		}
		if (parsed.DescribeRequested) {
			output << arguments.Describe();
			return 0;
		}
		if (!parsed.Ok) {
			errors << "error status=Arguments message=" << std::quoted(parsed.Error) << '\n';
			return 2;
		}
		if (!arguments.Positional().empty()) {
			errors << "error status=Arguments message=\"unexpected positional argument\"\n";
			return 2;
		}
		const auto inputPath = arguments.Get("input");
		const auto outputId = arguments.Get("output-id");
		const auto outputPath = arguments.Get("output");
		const auto bundlePath = arguments.Get("bundle");
		const auto audioCapturePath = arguments.Get("audio-capture");
		const bool bundleOutput = bundlePath.has_value();
		const bool scalarOutput = arguments.Has("value");
		const bool pngOutput = outputPath.has_value();
		const unsigned outputModes = static_cast<unsigned>(bundleOutput) +
									 static_cast<unsigned>(scalarOutput) + static_cast<unsigned>(pngOutput);
		if (!inputPath || inputPath->empty() || !outputId || outputId->empty() || outputModes != 1 ||
			(bundleOutput && bundlePath->empty()) || (pngOutput && outputPath->empty())) {
			errors << "error status=Arguments message=\"--input, --output-id, and exactly one of --output, "
					  "--bundle, or --value are required\"\n";
			return 2;
		}
		if (bundleOutput && !arguments.Has("frames")) {
			errors << "error status=Arguments message=\"--bundle requires --frames\"\n";
			return 2;
		}
		if (arguments.Has("frames") && arguments.Has("tick")) {
			PrintDiagnostic(errors, {Status::InvalidValue, {}, {}, "--tick and --frames cannot be combined"});
			return 1;
		}
		const bool renderRange = arguments.Has("frames");
		engine::imagegraph::TickRange tickRange;
		size_t frameCount = 1;
		if (renderRange) {
			const auto range = arguments.Get("frames");
			if (!range || !ParseTickRange(*range, tickRange)) {
				PrintDiagnostic(
					errors,
					{Status::InvalidValue, {}, {}, "--frames must be FIRST:LAST[:STEP] using unsigned ticks"}
				);
				return 1;
			}
		} else if (const auto tick = arguments.Get("tick")) {
			if (!ParseTick(*tick, tickRange.First)) {
				PrintDiagnostic(errors, {Status::InvalidValue, {}, {}, "--tick must be an unsigned integer"});
				return 1;
			}
			tickRange.Last = tickRange.First;
		}
		Diagnostic diagnostic;
		Status status = engine::imagegraph::ValidateTickRange(tickRange, frameCount, diagnostic);
		if (status != Status::Ok) {
			PrintDiagnostic(errors, diagnostic);
			return 1;
		}
		const std::filesystem::path inputFile(*inputPath);
		const std::filesystem::path bundleDirectory =
			bundleOutput ? std::filesystem::path(*bundlePath) : std::filesystem::path{};
		const std::filesystem::path outputFile =
			bundleOutput ? bundleDirectory / "frame.png"
						 : (outputPath ? std::filesystem::path(*outputPath) : std::filesystem::path{});
		const bool arrayOutput = pngOutput && outputFile.extension() == ".json";
		if (audioCapturePath &&
			((pngOutput && SamePath(std::filesystem::path(*audioCapturePath), outputFile)) ||
			 (bundleOutput && SamePath(std::filesystem::path(*audioCapturePath), bundleDirectory)))) {
			errors << "error status=Arguments message=\"audio capture and output paths must differ\"\n";
			return 2;
		}
		if ((pngOutput && SamePath(inputFile, outputFile)) ||
			(bundleOutput && SamePath(inputFile, bundleDirectory))) {
			errors << "error status=Arguments message=\"input and output paths must differ\"\n";
			return 2;
		}
		if (pngOutput && outputFile.extension() != ".png" && !arrayOutput) {
			errors << "error status=Arguments message=\"output path must use the .png or .json extension\"\n";
			return 2;
		}

		std::string text;
		std::string fileFailure;
		bool inputLimitExceeded = false;
		if (!ReadBounded(inputFile, text, inputLimitExceeded, fileFailure)) {
			if (inputLimitExceeded) {
				PrintDiagnostic(errors, {Status::LimitExceeded, {}, {}, fileFailure});
				return 1;
			}
			errors << "error status=InputError message=" << std::quoted(fileFailure) << '\n';
			return 1;
		}
		engine::imagegraph::Document document;
		status = engine::imagegraph::Read(text, document, diagnostic);
		if (status != Status::Ok) {
			PrintDiagnostic(errors, diagnostic);
			return 1;
		}
		std::vector<AudioCaptureFrame> audioFrames;
		if (audioCapturePath) {
			const std::filesystem::path captureFile(*audioCapturePath);
			if (SamePath(inputFile, captureFile)) {
				errors << "error status=Arguments message=\"graph and audio capture paths must differ\"\n";
				return 2;
			}
			std::string captureText;
			if (!ReadBounded(
					captureFile,
					captureText,
					inputLimitExceeded,
					fileFailure,
					Limits::MaximumAudioCaptureDocumentBytes
				)) {
				if (inputLimitExceeded) {
					PrintDiagnostic(errors, {Status::LimitExceeded, {}, {}, fileFailure});
				} else {
					errors << "error status=InputError message=" << std::quoted(fileFailure) << '\n';
				}
				return 1;
			}
			status = engine::imagegraph::ReadAudioCapture(captureText, audioFrames, diagnostic);
			if (status != Status::Ok) {
				PrintDiagnostic(errors, diagnostic);
				return 1;
			}
		}
		for (const std::string_view assignment : arguments.GetAll("set")) {
			status = ApplyOverride(document, assignment, diagnostic);
			if (status != Status::Ok) {
				PrintDiagnostic(errors, diagnostic);
				return 1;
			}
		}
		engine::imagegraph::Plan plan;
		status = engine::imagegraph::Compile(document, plan, diagnostic);
		if (status != Status::Ok) {
			PrintDiagnostic(errors, diagnostic);
			return 1;
		}
		BundleStage bundleStage;
		if (bundleOutput && !BeginBundle(bundleDirectory, bundleStage, fileFailure)) {
			errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
			return 1;
		}
		std::vector<BundleFrame> bundleFrames;
		if (bundleOutput) bundleFrames.reserve(frameCount);
		uint64_t rangeOutputBytes = 0;
		uint64_t tick = tickRange.First;
		for (size_t frameIndex = 0; frameIndex < frameCount; frameIndex++) {
			const std::filesystem::path framePath =
				bundleOutput ? FramePath(bundleStage.Directory / "frame.png", tick)
							 : (renderRange ? FramePath(outputFile, tick) : outputFile);
			engine::imagegraph::EvaluationRequest request;
			request.Tick = tick;
			request.AudioFrames = std::span<const AudioCaptureFrame>(audioFrames);
			if (scalarOutput) {
				engine::imagegraph::EvaluatedValue value;
				status = engine::imagegraph::EvaluateValue(
					document, plan, std::string(*outputId), request, value, diagnostic
				);
				if (status != Status::Ok) {
					PrintDiagnostic(errors, diagnostic);
					return 1;
				}
				const auto *scalar = std::get_if<double>(&value.Data);
				if (scalar == nullptr) {
					PrintDiagnostic(
						errors,
						{Status::UnsupportedExecution,
						 {},
						 value.Port,
						 "--value currently previews scalar outputs only"}
					);
					return 1;
				}
				output.imbue(std::locale::classic());
				output << "ok output_id=" << std::quoted(std::string(*outputId))
					   << " format=scalar tick=" << tick
					   << " value=" << std::setprecision(std::numeric_limits<double>::max_digits10) << *scalar
					   << '\n';
			} else if (arrayOutput) {
				ImageArray images;
				status = engine::imagegraph::EvaluateArray(
					document, plan, std::string(*outputId), request, images, diagnostic
				);
				if (status != Status::Ok) {
					PrintDiagnostic(errors, diagnostic);
					return 1;
				}
				ArrayFrame arrayFrame;
				status = BuildArrayFrame(
					images, std::string(*outputId), tick, framePath, inputFile, arrayFrame, diagnostic
				);
				if (status != Status::Ok) {
					PrintDiagnostic(errors, diagnostic);
					return 1;
				}
				if (arrayFrame.EncodedBytes > MAXIMUM_RANGE_OUTPUT_BYTES ||
					rangeOutputBytes > MAXIMUM_RANGE_OUTPUT_BYTES - arrayFrame.EncodedBytes) {
					PrintDiagnostic(
						errors,
						{Status::LimitExceeded, {}, {}, "frame range exceeds the 512 MiB total output limit"}
					);
					return 1;
				}
				if (!WriteArrayFrame(images, arrayFrame, framePath, fileFailure)) {
					errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
					return 1;
				}
				rangeOutputBytes += arrayFrame.EncodedBytes;
				output << "ok output_id=" << std::quoted(std::string(*outputId))
					   << " format=image-array images=" << images.Images.size() << " tick=" << tick
					   << " manifest=" << std::quoted(framePath.generic_string()) << '\n';
			} else {
				Image image;
				status = engine::imagegraph::Evaluate(
					document, plan, std::string(*outputId), tick, image, diagnostic
				);
				if (status != Status::Ok) {
					PrintDiagnostic(errors, diagnostic);
					return 1;
				}
				const uint64_t encodedBytes = EncodedPngBytes(image);
				if (encodedBytes > MAXIMUM_RANGE_OUTPUT_BYTES ||
					rangeOutputBytes > MAXIMUM_RANGE_OUTPUT_BYTES - encodedBytes) {
					PrintDiagnostic(
						errors,
						{Status::LimitExceeded,
						 {},
						 {},
						 "frame range exceeds the 512 MiB total PNG output limit"}
					);
					return 1;
				}
				if (!WritePng(framePath, image, fileFailure)) {
					errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
					return 1;
				}
				rangeOutputBytes += encodedBytes;
				if (bundleOutput) {
					bundleFrames.push_back(
						{tick, image.Width, image.Height, image.Hash, framePath.filename().generic_string()}
					);
				} else {
					output << "ok output_id=" << std::quoted(std::string(*outputId))
						   << " width=" << image.Width << " height=" << image.Height
						   << " format=rgba8 hash=0x" << std::hex << std::setfill('0') << std::setw(16)
						   << image.Hash << std::dec << " tick=" << tick
						   << " file=" << std::quoted(framePath.generic_string()) << '\n';
				}
			}
			if (frameIndex + 1 < frameCount) tick += tickRange.Step;
		}
		if (bundleOutput) {
			if (!WriteBundleManifest(
					bundleStage.Directory / "manifest.json",
					*outputId,
					bundleFrames,
					rangeOutputBytes,
					fileFailure
				)) {
				errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
				return 1;
			}
			if (Occupied(bundleDirectory)) {
				errors << "error status=OutputError message=\"bundle destination already exists or cannot be "
						  "inspected\"\n";
				return 1;
			}
			std::error_code error;
			std::filesystem::rename(bundleStage.Directory, bundleDirectory, error);
			if (error) {
				errors << "error status=OutputError message=\"cannot publish complete bundle\"\n";
				return 1;
			}
			bundleStage.Directory.clear();
			output << "ok output_id=" << std::quoted(std::string(*outputId))
				   << " format=png-sequence frames=" << bundleFrames.size()
				   << " manifest=" << std::quoted((bundleDirectory / "manifest.json").generic_string())
				   << '\n';
		}
		return 0;
	}
}
