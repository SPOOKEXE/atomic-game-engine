#include "StillExport.hpp"

#include <engine/core/Arguments.hpp>
#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/WavClip.hpp>
#include <engine/imagegraphexport/BuiltinRandomFile.hpp>
#include <engine/imagegraphexport/Runner.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

namespace engine::imagegraphexport::runner {
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

		std::string FrameText(engine::imagegraph::FrameTime frame) {
			std::ostringstream text;
			text.imbue(std::locale::classic());
			text << std::setprecision(std::numeric_limits<long double>::max_digits10)
				 << engine::imagegraph::FrameTimeToReal(frame);
			return text.str();
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

		bool ScaleExport(Image &image, double scale, bool linear, std::string &failure) {
			using namespace engine::imagegraph;
			if (scale == 1) return true;
			const double rawWidth = std::floor(image.Width * scale),
						 rawHeight = std::floor(image.Height * scale);
			if (rawWidth < 1 || rawHeight < 1 || rawWidth > Limits::MaximumDimension ||
				rawHeight > Limits::MaximumDimension) {
				failure = "export scale produces dimensions outside the image budget";
				return false;
			}
			const auto layout = CheckedSurfaceLayout(
				static_cast<uint32_t>(rawWidth),
				static_cast<uint32_t>(rawHeight),
				image.Format,
				Limits::MaximumOutputBytes
			);
			if (!layout || !ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
				!FiniteSurfaceSamples(image)) {
				failure = "export scale needs a bounded finite surface";
				return false;
			}
			Image output;
			output.Width = static_cast<uint32_t>(rawWidth);
			output.Height = static_cast<uint32_t>(rawHeight);
			output.Format = image.Format;
			output.Pixels.resize(static_cast<size_t>(layout->Bytes));
			for (uint32_t y = 0; y < output.Height; y++)
				for (uint32_t x = 0; x < output.Width; x++) {
					const double sx = (x + 0.5) / scale - 0.5, sy = (y + 0.5) / scale - 0.5;
					const auto sample = [&](int64_t xx, int64_t yy, SurfacePixel &pixel) {
						return LoadSurfacePixel(
							image,
							static_cast<uint32_t>(std::clamp<int64_t>(xx, 0, image.Width - 1)),
							static_cast<uint32_t>(std::clamp<int64_t>(yy, 0, image.Height - 1)),
							pixel
						);
					};
					SurfacePixel pixel{};
					if (linear) {
						const auto ix = static_cast<int64_t>(std::floor(sx)),
								   iy = static_cast<int64_t>(std::floor(sy));
						const double fx = sx - std::floor(sx), fy = sy - std::floor(sy);
						SurfacePixel a{}, b{}, c{}, d{};
						if (!sample(ix, iy, a) || !sample(ix + 1, iy, b) || !sample(ix, iy + 1, c) ||
							!sample(ix + 1, iy + 1, d)) {
							failure = "cannot sample export surface";
							return false;
						}
						for (size_t channel = 0; channel < 4; channel++)
							pixel[channel] = (a[channel] * (1 - fx) + b[channel] * fx) * (1 - fy) +
											 (c[channel] * (1 - fx) + d[channel] * fx) * fy;
					} else if (!sample(
								   static_cast<int64_t>(std::floor(sx + 0.5)),
								   static_cast<int64_t>(std::floor(sy + 0.5)),
								   pixel
							   )) {
						failure = "cannot sample export surface";
						return false;
					}
					if (!StoreSurfacePixel(output, x, y, pixel)) {
						failure = "cannot store scaled export surface";
						return false;
					}
				}
			output.Hash = SurfaceHash(output);
			image = std::move(output);
			return true;
		}

		bool NormalizePng(const Image &image, Image &normalized, std::string &failure) {
			using namespace engine::imagegraph;
			if (!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
				!FiniteSurfaceSamples(image)) {
				failure = "PNG input has invalid numeric surface storage";
				return false;
			}
			const auto layout = CheckedSurfaceLayout(
				image.Width, image.Height, SurfaceFormat::RGBA8Unorm, Limits::MaximumOutputBytes
			);
			if (!layout) {
				failure = "PNG normalized image exceeds its byte budget";
				return false;
			}
			normalized.Width = image.Width;
			normalized.Height = image.Height;
			normalized.Pixels.resize(static_cast<size_t>(layout->Bytes));
			for (uint32_t y = 0; y < image.Height; y++)
				for (uint32_t x = 0; x < image.Width; x++) {
					SurfacePixel pixel{};
					if (!LoadSurfacePixel(image, x, y, pixel) ||
						!StoreSurfacePixel(normalized, x, y, pixel)) {
						failure = "PNG numeric sample is not representable";
						return false;
					}
				}
			normalized.Hash = SurfaceHash(normalized);
			return true;
		}

		bool WritePng(const std::filesystem::path &path, const Image &image, std::string &failure) {
			using namespace engine::imagegraph;
			if (image.Format != SurfaceFormat::RGBA8Unorm) {
				Image normalized;
				if (!NormalizePng(image, normalized, failure)) return false;
				return WritePng(path, normalized, failure);
			}
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

		bool WriteNativeStill(
			const std::filesystem::path &path, std::span<const uint8_t> bytes, std::string &failure
		) {
			static std::atomic<uint64_t> sequence{0};
			std::filesystem::path staging = path;
			staging += ".partial-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
					   std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
			std::ofstream stream(staging, std::ios::binary | std::ios::trunc);
			if (stream)
				stream.write(
					reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
				);
			stream.flush();
			const bool written = stream.good();
			stream.close();
			std::error_code error;
			if (written && stream)
				std::filesystem::rename(staging, path, error);
			else
				error = std::make_error_code(std::errc::io_error);
			if (!error) return true;
			std::filesystem::remove(staging, error);
			failure = "could not publish complete native still export";
			return false;
		}

		struct AnimationStage {
			std::filesystem::path Staging;
			std::ofstream Stream;
			uint32_t Width = 0, Height = 0, Sequence = 0;
			uint64_t Bytes = 0;
			~AnimationStage() {
				Stream.close();
				if (Staging.empty()) return;
				std::error_code error;
				std::filesystem::remove(Staging, error);
			}
		};

		bool AppendApngFrame(
			AnimationStage &stage,
			const std::filesystem::path &path,
			const Image &image,
			size_t index,
			size_t count,
			uint16_t delayMilliseconds,
			uint16_t delayDenominator,
			uint32_t plays,
			std::string &failure
		) {
			if (image.Format != engine::imagegraph::SurfaceFormat::RGBA8Unorm) {
				Image normalized;
				if (!NormalizePng(image, normalized, failure)) return false;
				return AppendApngFrame(
					stage, path, normalized, index, count, delayMilliseconds, delayDenominator, plays, failure
				);
			}

			if (image.Format != engine::imagegraph::SurfaceFormat::RGBA8Unorm ||
				!engine::imagegraph::ValidSurfaceLayout(
					image, Limits::MaximumDimension, Limits::MaximumOutputBytes
				)) {
				failure = "APNG requires a bounded RGBA8 surface";
				return false;
			}
			std::vector<uint8_t> compressed;
			if (!CompressStoredZlib(image, compressed)) {
				failure = "could not encode APNG frame";
				return false;
			}
			const uint64_t frameBytes = compressed.size() + 38 + 12 + (index == 0 ? 53 : 4);
			if (frameBytes > MAXIMUM_RANGE_OUTPUT_BYTES - 12 ||
				stage.Bytes > MAXIMUM_RANGE_OUTPUT_BYTES - frameBytes - 12) {
				failure = "APNG exceeds the 512 MiB output limit";
				return false;
			}
			if (index == 0) {
				static std::atomic<uint64_t> sequence{0};
				stage.Staging = path;
				stage.Staging += ".partial-" +
								 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
								 "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
				stage.Stream.open(stage.Staging, std::ios::binary | std::ios::trunc);
				if (!stage.Stream) {
					failure = "cannot open APNG staging file";
					return false;
				}
				stage.Width = image.Width;
				stage.Height = image.Height;
				stage.Stream.write(
					reinterpret_cast<const char *>(PNG_SIGNATURE.data()), PNG_SIGNATURE.size()
				);
				std::vector<uint8_t> header;
				AppendU32(header, image.Width);
				AppendU32(header, image.Height);
				header.insert(header.end(), {8, 6, 0, 0, 0});
				if (!WriteChunk(stage.Stream, "IHDR", header)) return false;
				header.clear();
				AppendU32(header, static_cast<uint32_t>(count));
				AppendU32(header, plays);
				if (!WriteChunk(stage.Stream, "acTL", header)) return false;
			} else if (image.Width != stage.Width || image.Height != stage.Height) {
				failure = "APNG frame dimensions must match the first frame";
				return false;
			}
			std::vector<uint8_t> control;
			AppendU32(control, stage.Sequence++);
			AppendU32(control, image.Width);
			AppendU32(control, image.Height);
			AppendU32(control, 0);
			AppendU32(control, 0);
			control.push_back(static_cast<uint8_t>(delayMilliseconds >> 8));
			control.push_back(static_cast<uint8_t>(delayMilliseconds));
			control.push_back(static_cast<uint8_t>(delayDenominator >> 8));
			control.push_back(static_cast<uint8_t>(delayDenominator));
			control.insert(control.end(), {0, 0});
			if (!WriteChunk(stage.Stream, "fcTL", control)) {
				failure = "could not write APNG frame control";
				return false;
			}
			if (index == 0) {
				if (!WriteChunk(stage.Stream, "IDAT", compressed)) {
					failure = "could not write APNG frame";
					return false;
				}
			} else {
				std::vector<uint8_t> sequence;
				AppendU32(sequence, stage.Sequence++);
				compressed.insert(compressed.begin(), sequence.begin(), sequence.end());
				if (!WriteChunk(stage.Stream, "fdAT", compressed)) {
					failure = "could not write APNG frame";
					return false;
				}
			}
			stage.Bytes += frameBytes;
			return true;
		}

		bool PublishApng(AnimationStage &stage, const std::filesystem::path &path, std::string &failure) {
			if (!WriteChunk(stage.Stream, "IEND", {})) {
				failure = "could not finalize APNG";
				return false;
			}
			stage.Stream.flush();
			const bool flushed = stage.Stream.good();
			stage.Stream.close();
			if (!flushed || !stage.Stream) {
				failure = "could not flush APNG";
				return false;
			}
			std::error_code error;
			std::filesystem::rename(stage.Staging, path, error);
			if (error) {
				failure = "could not publish APNG";
				return false;
			}
			stage.Staging.clear();
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
			Diagnostic &diagnostic,
			const engine::imagegraph::FrameTime *authoredFrame = nullptr
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
			frame.Manifest += authoredFrame ? ",\"frame\":" + FrameText(*authoredFrame)
											: ",\"tick\":" + std::to_string(tick);
			frame.Manifest += ",\"items\":[";
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

	bool WriteStillImage(
		const std::filesystem::path &path, const engine::imagegraph::Image &image, std::string &failure
	) {
		const auto extension = path.extension().string();
		if (extension == ".png") return WritePng(path, image, failure);
		if (extension == ".bmp" || extension == ".exr") {
			std::vector<uint8_t> bytes;
			if (!EncodeStill(extension, image, bytes, failure)) return false;
			std::ofstream stream(path, std::ios::binary);
			stream.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
			stream.close();
			if (!stream) {
				failure = "cannot write complete encoded still image";
				return false;
			}
			return true;
		}
		failure = "host still encoder needs PNG, BMP or EXR destination";
		return false;
	}

	static int RunImpl(
		int argc,
		char **argv,
		std::ostream &output,
		std::ostream &errors,
		std::span<const engine::imagegraph::RequestImageSource> imageSources,
		std::span<const engine::imagegraph::HostNodeCapture> captures,
		engine::imagegraph::HostNodeProvider *provider,
		const engine::imagegraph::Document *liveDocument,
		const engine::imagegraph::Plan *livePlan,
		const engine::imagegraph::EvaluationRequest *liveRequest
	);

	int Run(int argc, char **argv, std::ostream &output, std::ostream &errors) {
		return RunWithImages(argc, argv, output, errors, {});
	}

	int RunWithImages(
		int argc,
		char **argv,
		std::ostream &output,
		std::ostream &errors,
		std::span<const engine::imagegraph::RequestImageSource> imageSources
	) {
		return RunWithHostInputs(argc, argv, output, errors, imageSources, {}, nullptr);
	}

	int RunWithHostInputs(
		int argc,
		char **argv,
		std::ostream &output,
		std::ostream &errors,
		std::span<const engine::imagegraph::RequestImageSource> images,
		std::span<const engine::imagegraph::HostNodeCapture> captures,
		engine::imagegraph::HostNodeProvider *provider
	) {
		return RunImpl(argc, argv, output, errors, images, captures, provider, nullptr, nullptr, nullptr);
	}
	int RunWithDocument(
		int argc,
		char **argv,
		std::ostream &output,
		std::ostream &errors,
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		const engine::imagegraph::EvaluationRequest &request
	) {
		return RunImpl(
			argc,
			argv,
			output,
			errors,
			request.ImageSources,
			request.HostCaptures,
			request.HostProvider,
			&document,
			&plan,
			&request
		);
	}

	static int RunImpl(
		int argc,
		char **argv,
		std::ostream &output,
		std::ostream &errors,
		std::span<const engine::imagegraph::RequestImageSource> imageSources,
		std::span<const engine::imagegraph::HostNodeCapture> captures,
		engine::imagegraph::HostNodeProvider *provider,
		const engine::imagegraph::Document *liveDocument,
		const engine::imagegraph::Plan *livePlan,
		const engine::imagegraph::EvaluationRequest *liveRequest
	) {
		engine::core::Arguments arguments(
			"imagegraph",
			"Evaluates an imagegraph output at one authored frame or a streamed tick range, writing "
			"deterministic "
			"PNG, BMP, EXR and APNG images or numeric scalar and array previews."
		);
		arguments.Value("input", "PATH", "Authored imagegraph text document");
		arguments.Value("output-id", "ID", "Durable output ID to evaluate");
		arguments.Value("output", "PATH", "PNG, BMP or EXR path, or JSON image-array manifest path");
		arguments.Flag("rigid-playing", "Capture source rigid playback as active");
		arguments.Flag("rigid-frame-progress", "Capture source rigid frame advancement");
		arguments.Flag("value", "Evaluate a numeric scalar or array output and print its value record");
		arguments.Value("audio-capture", "PATH", "Bounded recorded audio input frames");
		arguments.Value(
			"builtin-random-capture", "PATH", "Exact file containing recorded builtin random observations"
		);
		arguments.Value("audio-source", "SOURCE=PATH", "Explicit source PCM WAV whole clip; may be repeated");
		arguments.Value(
			"bundle", "DIR", "Publish a complete PNG frame range and manifest into a new directory"
		);
		arguments.Value(
			"set", "NODE_ID.PROPERTY=TYPE:VALUE", "Override a typed node property; may be repeated"
		);
		arguments.Value(
			"frame-duration-ms", "N", "APNG frame duration in milliseconds, 1..65535; defaults to 33"
		);
		arguments.Flag("require-rgba8", "Refuse exported surfaces outside RGBA8Unorm");
		arguments.Value("debug-frame-directory", "PATH", "Retain PNG inputs beside an APNG export");
		arguments.Value("array-index", "N", "Encode one top-level image-array member");
		arguments.Value("export-scale", "REAL", "Scale encoded output, 0..64 exclusive of zero");
		arguments.Value("export-filter", "point|linear", "Sampling used by encoded output scaling");
		arguments.Value("frame-delay", "N:D", "Exact APNG frame duration as a bounded seconds fraction");
		arguments.Value("plays", "N", "APNG play count; zero loops forever; defaults to zero");
		arguments.Value("tick", "N", "Timeline tick to evaluate; defaults to 0");
		arguments.Value(
			"frame", "REAL", "Single signed or fractional authored frame; exclusive with --tick and --frames"
		);
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
		const auto builtinRandomPath = arguments.Get("builtin-random-capture");
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
		const unsigned clockModes = static_cast<unsigned>(arguments.Has("tick")) +
									static_cast<unsigned>(arguments.Has("frame")) +
									static_cast<unsigned>(arguments.Has("frames"));
		if (clockModes > 1) {
			PrintDiagnostic(
				errors, {Status::InvalidValue, {}, {}, "--tick, --frame and --frames are mutually exclusive"}
			);
			return 1;
		}
		const bool renderRange = arguments.Has("frames");
		engine::imagegraph::TickRange tickRange;
		std::optional<engine::imagegraph::FrameTime> authoredFrame;
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
		} else if (const auto frame = arguments.Get("frame")) {
			double real = 0;
			engine::imagegraph::FrameTime time;
			if (!ParseScalar(*frame, real) || !engine::imagegraph::SplitFrameTime(real, time)) {
				PrintDiagnostic(
					errors,
					{Status::InvalidValue,
					 {},
					 {},
					 "--frame must be a finite authored time within the timeline limit"}
				);
				return 1;
			}
			authoredFrame = time;
			tickRange.First = tickRange.Last = time.Tick;
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
		const bool animationOutput = pngOutput && outputFile.extension() == ".apng";
		uint64_t delayMilliseconds = 33, delayDenominator = 1000, plays = 0;
		if ((arguments.Has("frame-duration-ms") || arguments.Has("plays")) && !animationOutput) {
			errors << "error status=Arguments message=\"animation timing options require .apng output\"\n";
			return 2;
		}
		if (const auto duration = arguments.Get("frame-duration-ms")) {
			if (!ParseTick(*duration, delayMilliseconds) || delayMilliseconds == 0 ||
				delayMilliseconds > 65535) {
				errors << "error status=Arguments message=\"APNG frame duration must be 1..65535 "
						  "milliseconds\"\n";
				return 2;
			}
		}
		if (const auto count = arguments.Get("plays")) {
			if (!ParseTick(*count, plays) || plays > std::numeric_limits<uint32_t>::max()) {
				errors << "error status=Arguments message=\"APNG play count must fit uint32\"\n";
				return 2;
			}
		}
		if (const auto fraction = arguments.Get("frame-delay")) {
			const auto separator = fraction->find(':');
			if (!animationOutput || arguments.Has("frame-duration-ms") ||
				separator == std::string_view::npos ||
				!ParseTick(fraction->substr(0, separator), delayMilliseconds) ||
				!ParseTick(fraction->substr(separator + 1), delayDenominator) || delayMilliseconds == 0 ||
				delayMilliseconds > 65535 || delayDenominator == 0 || delayDenominator > 65535) {
				errors << "error status=Arguments message=\"APNG frame delay needs N:D bounded positive "
						  "integers\"\n";
				return 2;
			}
		}

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
		if (pngOutput && outputFile.extension() != ".png" && outputFile.extension() != ".bmp" &&
			outputFile.extension() != ".exr" && !arrayOutput && !animationOutput) {
			errors << "error status=Arguments message=\"output path must use the .png, .bmp, .exr, .apng or "
					  ".json extension\"\n";
			return 2;
		}

		std::optional<size_t> arrayIndex;
		if (const auto textIndex = arguments.Get("array-index")) {
			uint64_t parsedIndex = 0;
			const auto result =
				std::from_chars(textIndex->data(), textIndex->data() + textIndex->size(), parsedIndex);
			if (result.ec != std::errc{} || result.ptr != textIndex->data() + textIndex->size() ||
				parsedIndex >= Limits::MaximumArrayElements || scalarOutput || arrayOutput) {
				errors
					<< "error status=Arguments message=\"array index must select a bounded image member\"\n";
				return 2;
			}
			arrayIndex = static_cast<size_t>(parsedIndex);
		}
		double exportScale = 1;
		bool exportLinear = false;
		if (const auto value = arguments.Get("export-scale")) {
			const auto parsedScale =
				std::from_chars(value->data(), value->data() + value->size(), exportScale);
			if (parsedScale.ec != std::errc{} || parsedScale.ptr != value->data() + value->size() ||
				!std::isfinite(exportScale) || exportScale <= 0 || exportScale > 64) {
				errors << "error status=Arguments message=\"export scale must be finite and in (0,64]\"\n";
				return 2;
			}
		}
		if (const auto filter = arguments.Get("export-filter")) {
			if (*filter != "point" && *filter != "linear") {
				errors << "error status=Arguments message=\"export filter must be point or linear\"\n";
				return 2;
			}
			exportLinear = *filter == "linear";
		}

		std::string text;
		std::string fileFailure;
		bool inputLimitExceeded = false;
		if (!liveDocument && !ReadBounded(inputFile, text, inputLimitExceeded, fileFailure)) {
			if (inputLimitExceeded) {
				PrintDiagnostic(errors, {Status::LimitExceeded, {}, {}, fileFailure});
				return 1;
			}
			errors << "error status=InputError message=" << std::quoted(fileFailure) << '\n';
			return 1;
		}
		engine::imagegraph::Document parsedDocument;
		const auto &document = liveDocument ? *liveDocument : parsedDocument;
		status = liveDocument ? Status::Ok : engine::imagegraph::Read(text, parsedDocument, diagnostic);
		if (status != Status::Ok) {
			PrintDiagnostic(errors, diagnostic);
			return 1;
		}
		std::vector<engine::imagegraph::SourceBuiltinRandomCapture> builtinRandomCaptures;
		if (builtinRandomPath) {
			const std::filesystem::path captureFile(*builtinRandomPath);
			if (SamePath(inputFile, captureFile) || (pngOutput && SamePath(outputFile, captureFile)) ||
				(bundleOutput && SamePath(bundleDirectory, captureFile))) {
				errors << "error status=Arguments message=\"graph, output and builtin "
						  "random capture paths must differ\"\n";
				return 2;
			}
			if (!LoadBuiltinRandomCaptureFile(
					captureFile,
					engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
					builtinRandomCaptures,
					fileFailure
				)) {
				errors << "error status=InputError message=" << std::quoted(fileFailure) << '\n';
				return 1;
			}
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
		std::vector<engine::imagegraph::AudioClipSource> audioClips;
		uint64_t clipBytes = 0;
		for (const std::string_view assignment : arguments.GetAll("audio-source")) {
			const size_t separator = assignment.find('=');
			if (separator == std::string_view::npos || separator == 0 || separator + 1 == assignment.size() ||
				assignment.size() > MAXIMUM_OVERRIDE_BYTES || audioClips.size() >= Limits::MaximumNodes) {
				errors << "error status=Arguments message=\"--audio-source must be bounded SOURCE=PATH\"\n";
				return 2;
			}
			std::string source(assignment.substr(0, separator));
			if (std::any_of(audioClips.begin(), audioClips.end(), [&](const auto &clip) {
					return clip.SourceId == source;
				})) {
				PrintDiagnostic(errors, {Status::DuplicateId, {}, "path", "duplicate WAV source name"});
				return 1;
			}
			const std::filesystem::path sourceFile(assignment.substr(separator + 1));
			if (SamePath(inputFile, sourceFile) || (pngOutput && SamePath(outputFile, sourceFile)) ||
				(bundleOutput && SamePath(bundleDirectory, sourceFile))) {
				errors
					<< "error status=Arguments message=\"WAV source, graph and output paths must differ\"\n";
				return 2;
			}
			std::string waveBytes;
			if (!ReadBounded(
					sourceFile,
					waveBytes,
					inputLimitExceeded,
					fileFailure,
					std::min(MAXIMUM_INPUT_BYTES, Limits::MaximumEvaluationBytes - clipBytes)
				)) {
				if (inputLimitExceeded)
					PrintDiagnostic(errors, {Status::LimitExceeded, {}, {}, fileFailure});
				else
					errors << "error status=InputError message=" << std::quoted(fileFailure) << '\n';
				return 1;
			}
			const uint64_t assetOverhead = sizeof(engine::imagegraph::AudioClipSource) + source.size();
			if (assetOverhead > Limits::MaximumEvaluationBytes - clipBytes - waveBytes.size()) {
				PrintDiagnostic(
					errors, {Status::LimitExceeded, {}, {}, "WAV asset metadata exceeds its byte budget"}
				);
				return 1;
			}
			engine::imagegraph::AudioBit data;
			status = engine::imagegraph::DecodeWavClip(
				std::as_bytes(std::span(waveBytes.data(), waveBytes.size())),
				engine::imagegraph::WavClipPolicy::PixelComposer,
				Limits::MaximumEvaluationBytes - clipBytes - waveBytes.size() - assetOverhead,
				data,
				diagnostic
			);
			if (status != Status::Ok) {
				PrintDiagnostic(errors, diagnostic);
				return 1;
			}
			uint64_t bytes = sizeof(engine::imagegraph::AudioClipSource) + source.size() +
							 data.Channels.size() * sizeof(std::vector<double>);
			for (const auto &channel : data.Channels)
				bytes += channel.size() * sizeof(double);
			if (bytes > Limits::MaximumEvaluationBytes - clipBytes) {
				PrintDiagnostic(
					errors, {Status::LimitExceeded, {}, {}, "WAV assets exceed their aggregate byte budget"}
				);
				return 1;
			}
			clipBytes += bytes;
			audioClips.push_back({std::move(source), std::move(data)});
		}
		for (const std::string_view assignment : arguments.GetAll("set")) {
			if (liveDocument) {
				errors << "error status=Arguments message="
					   << std::quoted("Live exports do not mutate authored overrides") << '\n';
				return 2;
			}
			status = ApplyOverride(parsedDocument, assignment, diagnostic);
			if (status != Status::Ok) {
				PrintDiagnostic(errors, diagnostic);
				return 1;
			}
		}
		engine::imagegraph::Plan compiledPlan;
		const auto &plan = livePlan ? *livePlan : compiledPlan;
		status = livePlan ? Status::Ok : engine::imagegraph::Compile(document, compiledPlan, diagnostic);
		if (status != Status::Ok) {
			PrintDiagnostic(errors, diagnostic);
			return 1;
		}
		AnimationStage animationStage;
		BundleStage bundleStage;
		if (bundleOutput && !BeginBundle(bundleDirectory, bundleStage, fileFailure)) {
			errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
			return 1;
		}
		std::vector<BundleFrame> bundleFrames;
		if (bundleOutput) bundleFrames.reserve(frameCount);
		uint64_t rangeOutputBytes = 0;
		uint64_t tick = tickRange.First;
		engine::imagegraphphysics::RigidProvider rigidProvider;
		engine::imagegraph::CapturedFeedbackHost replayHost;
		for (size_t frameIndex = 0; frameIndex < frameCount; frameIndex++) {
			const std::filesystem::path framePath =
				bundleOutput ? FramePath(bundleStage.Directory / "frame.png", tick)
							 : (renderRange ? FramePath(outputFile, tick) : outputFile);
			auto request = liveRequest ? *liveRequest : engine::imagegraph::EvaluationRequest{};
			if (!request.RigidProvider) request.RigidProvider = &rigidProvider;
			if (!request.SourceCachePlayback)
				request.SourceCachePlayback = engine::imagegraph::SourceCachePlaybackObservation{
					true, engine::imagegraph::SourceCacheSampling::NativePlayedPrefix, true
				};
			if (!liveRequest) {
				request.RigidPlaying = arguments.Has("rigid-playing");
				request.RigidFrameProgress = arguments.Has("rigid-frame-progress");
			}
			if (!liveRequest || renderRange)
				(void)engine::imagegraph::SetFrameTime(
					request, authoredFrame.value_or(engine::imagegraph::FrameTime{tick})
				);
			const std::string clockRecord =
				liveRequest && !renderRange
					? " frame=" + FrameText({request.Tick, request.Subframe, request.NegativeFrame})
				: authoredFrame ? " frame=" + FrameText(*authoredFrame)
								: " tick=" + std::to_string(tick);
			if (builtinRandomPath) request.BuiltinRandomCaptures = builtinRandomCaptures;
			if (!liveRequest || audioCapturePath)
				request.AudioFrames = std::span<const AudioCaptureFrame>(audioFrames);
			if (!liveRequest || !audioClips.empty()) request.AudioClips = audioClips;
			request.ImageSources = imageSources;
			request.HostCaptures = captures;
			request.HostProvider = provider;
			if (!replayHost.Prepare(
					document,
					plan,
					liveRequest ? liveRequest->RigidAuthoringRevision : 1,
					1,
					request,
					diagnostic,
					Limits::MaximumEvaluationBytes,
					*outputId
				)) {
				PrintDiagnostic(errors, diagnostic);
				return 1;
			}
			const auto *replayed = replayHost.Active() ? replayHost.Value(*outputId) : nullptr;
			const auto replayTypeError = [&]() {
				diagnostic = {Status::InvalidOutput, {}, {}, "selected replay output has incompatible type"};
				return diagnostic.Code;
			};
			const auto evaluateArray = [&](ImageArray &images) {
				if (!replayed)
					return engine::imagegraph::EvaluateArray(
						document, plan, std::string(*outputId), request, images, diagnostic
					);
				if (const auto *array = std::get_if<ImageArray>(&replayed->Output)) {
					images = *array;
					return Status::Ok;
				}
				if (const auto *image = std::get_if<Image>(&replayed->Output)) {
					images.Images = {*image};
					images.Items = {{size_t{0}}};
					return Status::Ok;
				}
				return replayTypeError();
			};
			if (scalarOutput) {
				engine::imagegraph::EvaluatedValue value;
				if (replayed) {
					if (const auto *typed =
							std::get_if<engine::imagegraph::EvaluatedValue>(&replayed->Output)) {
						value = *typed;
						status = Status::Ok;
					} else
						status = replayTypeError();
				} else
					status = engine::imagegraph::EvaluateValue(
						document, plan, std::string(*outputId), request, value, diagnostic
					);
				if (status != Status::Ok) {
					PrintDiagnostic(errors, diagnostic);
					return 1;
				}
				std::ostringstream preview;
				preview.imbue(std::locale::classic());
				preview << std::setprecision(std::numeric_limits<double>::max_digits10);
				std::string_view format = "scalar";
				bool supported = true;
				if (const auto *scalar = std::get_if<double>(&value.Data))
					preview << *scalar;
				else if (const auto *array = std::get_if<engine::imagegraph::ArrayValue>(&value.Data)) {
					format = "array";
					const auto row = [&](const auto &elements) {
						preview << '[';
						for (size_t index = 0; index < elements.size(); index++) {
							const auto *number = std::get_if<double>(&elements[index]);
							if (!number || !std::isfinite(*number)) {
								supported = false;
								return;
							}
							if (index) preview << ',';
							preview << *number;
						}
						preview << ']';
					};
					if (array->ElementType != engine::imagegraph::ValueType::Scalar)
						supported = false;
					else if (array->Nested.empty())
						row(array->Elements);
					else {
						preview << '[';
						for (size_t index = 0; index < array->Nested.size(); index++) {
							if (index) preview << ',';
							row(array->Nested[index]);
						}
						preview << ']';
					}
				} else
					supported = false;
				if (!supported) {
					PrintDiagnostic(
						errors,
						{Status::UnsupportedExecution,
						 {},
						 value.Port,
						 "--value previews scalar and finite numeric array outputs"}
					);
					return 1;
				}
				output << "ok output_id=" << std::quoted(std::string(*outputId)) << " format=" << format
					   << clockRecord << " value=" << preview.str() << '\n';

			} else if (arrayOutput) {
				ImageArray images;
				status = evaluateArray(images);
				if (status != Status::Ok) {
					PrintDiagnostic(errors, diagnostic);
					return 1;
				}
				ArrayFrame arrayFrame;
				status = BuildArrayFrame(
					images,
					std::string(*outputId),
					tick,
					framePath,
					inputFile,
					arrayFrame,
					diagnostic,
					authoredFrame ? &*authoredFrame : nullptr
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
					   << " format=image-array images=" << images.Images.size() << clockRecord
					   << " manifest=" << std::quoted(framePath.generic_string()) << '\n';
			} else {
				Image image;
				if (arrayIndex) {
					ImageArray members;
					status = evaluateArray(members);
					if (status == Status::Ok) {
						const auto *selected = *arrayIndex < members.Items.size()
												   ? std::get_if<size_t>(&members.Items[*arrayIndex].Data)
												   : nullptr;
						if (!selected || *selected >= members.Images.size()) {
							diagnostic = {
								Status::InvalidOutput, {}, {}, "selected array member is absent or nested"
							};
							status = diagnostic.Code;
						} else
							image = std::move(members.Images[*selected]);
					}
				} else {
					if (replayed) {
						if (const auto *captured = replayHost.Output(*outputId)) {
							image = *captured;
							status = Status::Ok;
						} else
							status = replayTypeError();
					} else
						status = engine::imagegraph::Evaluate(
							document, plan, std::string(*outputId), request, image, diagnostic
						);
				}
				if (status != Status::Ok) {
					PrintDiagnostic(errors, diagnostic);
					return 1;
				}
				if (arguments.Has("require-rgba8") &&
					image.Format != engine::imagegraph::SurfaceFormat::RGBA8Unorm) {
					errors << "error status=OutputError message="
						   << std::quoted("Export profile requires RGBA8Unorm surfaces") << '\n';
					return 1;
				}
				if (!ScaleExport(image, exportScale, exportLinear, fileFailure)) {
					errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
					return 1;
				}
				uint64_t encodedBytes = EncodedPngBytes(image);
				std::vector<uint8_t> encoded;
				const bool nativeStill = !animationOutput && framePath.extension() != ".png";
				if (nativeStill) {
					if (!EncodeStill(framePath.extension().string(), image, encoded, fileFailure)) {
						errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
						return 1;
					}
					encodedBytes = encoded.size();
				}
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
				if (animationOutput) {
					if (const auto debug = arguments.Get("debug-frame-directory")) {
						std::filesystem::path directory(*debug);
						std::error_code error;
						std::filesystem::create_directories(directory, error);
						std::ostringstream name;
						name << "frame" << std::setw(8) << std::setfill('0') << frameIndex << ".png";
						if (error || encodedBytes > MAXIMUM_RANGE_OUTPUT_BYTES - rangeOutputBytes ||
							!WritePng(directory / name.str(), image, fileFailure)) {
							errors << "error status=OutputError message="
								   << std::quoted(error ? error.message() : fileFailure) << '\n';
							return 1;
						}
						rangeOutputBytes += encodedBytes;
					}
					if (!AppendApngFrame(
							animationStage,
							outputFile,
							image,
							frameIndex,
							frameCount,
							static_cast<uint16_t>(delayMilliseconds),
							static_cast<uint16_t>(delayDenominator),
							static_cast<uint32_t>(plays),
							fileFailure
						)) {
						errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
						return 1;
					}
				} else if (nativeStill ? !WriteNativeStill(framePath, encoded, fileFailure)
									   : !WritePng(framePath, image, fileFailure)) {
					errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
					return 1;
				}
				rangeOutputBytes += encodedBytes;
				if (bundleOutput) {
					bundleFrames.push_back(
						{tick, image.Width, image.Height, image.Hash, framePath.filename().generic_string()}
					);
				} else if (!animationOutput) {
					output << "ok output_id=" << std::quoted(std::string(*outputId))
						   << " width=" << image.Width << " height=" << image.Height
						   << " format=" << (nativeStill ? framePath.extension().string().substr(1) : "rgba8")
						   << " hash=0x" << std::hex << std::setfill('0') << std::setw(16) << image.Hash
						   << std::dec << clockRecord << " file=" << std::quoted(framePath.generic_string())
						   << '\n';
				}
			}
			if (frameIndex + 1 < frameCount) tick += tickRange.Step;
		}
		if (animationOutput) {
			if (!PublishApng(animationStage, outputFile, fileFailure)) {
				errors << "error status=OutputError message=" << std::quoted(fileFailure) << '\n';
				return 1;
			}
			output << "ok output_id=" << std::quoted(std::string(*outputId))
				   << " format=apng frames=" << frameCount << " frame_duration_ms="
				   << (static_cast<double>(delayMilliseconds) * 1000 / static_cast<double>(delayDenominator))
				   << " plays=" << plays << " file=" << std::quoted(outputFile.generic_string()) << '\n';
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
