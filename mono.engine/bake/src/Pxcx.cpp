#include <engine/bake/Pxcx.hpp>

#include <cmath>
#include <cryptopp/filters.h>
#include <cryptopp/zlib.h>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace engine::bake {
	namespace {
		using Json = nlohmann::json;

		constexpr size_t HEADER_BYTES = 16;
		constexpr size_t PREFIX_BYTES = 8;
		constexpr size_t META_HEADER_BYTES = 8;
		constexpr size_t META_NUMBER_BYTES = 4;

		struct InflateLimitExceeded {};
		struct InflateTrailingData {};

		class BoundedVectorSink final : public CryptoPP::Bufferless<CryptoPP::Sink> {
		  public:
			BoundedVectorSink(std::vector<uint8_t> &bytes, size_t maximumBytes)
				: Bytes(bytes), MaximumBytes(maximumBytes) {}

			void IsolatedInitialize(const CryptoPP::NameValuePairs &parameters) override {
				CRYPTOPP_UNUSED(parameters);
			}

			size_t Put2(const CryptoPP::byte *source, size_t length, int messageEnd, bool blocking) override {
				CRYPTOPP_UNUSED(blocking);
				if (StreamEnded && length > 0) throw InflateTrailingData{};
				if (length > MaximumBytes - Bytes.size()) throw InflateLimitExceeded{};
				if (length > 0) Bytes.insert(Bytes.end(), source, source + length);
				StreamEnded |= messageEnd != 0;
				return 0;
			}

		  private:
			std::vector<uint8_t> &Bytes;
			size_t MaximumBytes;
			bool StreamEnded = false;
		};

		bool Fail(std::string &failure, std::string message) {
			failure = std::move(message);
			return false;
		}

		bool HasTag(std::span<const std::byte> bytes, size_t offset, std::string_view tag) {
			if (offset > bytes.size() || tag.size() > bytes.size() - offset) return false;
			return std::memcmp(bytes.data() + offset, tag.data(), tag.size()) == 0;
		}

		uint32_t ReadUInt32(std::span<const std::byte> bytes, size_t offset) {
			return static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset])) |
				   (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 1])) << 8) |
				   (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 2])) << 16) |
				   (static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 3])) << 24);
		}

		void AppendUInt32(std::vector<std::byte> &bytes, uint32_t value) {
			for (uint32_t shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::byte>(value >> shift));
		}

		void PatchUInt32(std::vector<std::byte> &bytes, size_t offset, uint32_t value) {
			for (uint32_t shift = 0; shift < 32; shift += 8)
				bytes[offset + shift / 8] = static_cast<std::byte>(value >> shift);
		}

		void AppendText(std::vector<std::byte> &bytes, std::string_view value) {
			for (const char character : value)
				bytes.push_back(static_cast<std::byte>(static_cast<uint8_t>(character)));
		}

		bool Compress(std::span<const uint8_t> input, std::vector<std::byte> &out, std::string &failure) {
			std::string compressed;
			try {
				CryptoPP::ZlibCompressor compressor(new CryptoPP::StringSink(compressed));
				compressor.Put(input.data(), input.size());
				compressor.MessageEnd();
			} catch (const CryptoPP::Exception &error) {
				return Fail(failure, "pxcx: zlib compression failed: " + std::string(error.what()));
			}
			out.reserve(out.size() + compressed.size());
			AppendText(out, compressed);
			return true;
		}

		bool ValidUtf8(std::string_view text) {
			size_t offset = 0;
			while (offset < text.size()) {
				const uint8_t lead = static_cast<uint8_t>(text[offset]);
				if (lead <= 0x7F) {
					offset++;
					continue;
				}

				uint32_t codePoint = 0;
				size_t continuationCount = 0;
				uint32_t minimum = 0;
				if (lead >= 0xC2 && lead <= 0xDF) {
					codePoint = lead & 0x1F;
					continuationCount = 1;
					minimum = 0x80;
				} else if (lead >= 0xE0 && lead <= 0xEF) {
					codePoint = lead & 0x0F;
					continuationCount = 2;
					minimum = 0x800;
				} else if (lead >= 0xF0 && lead <= 0xF4) {
					codePoint = lead & 0x07;
					continuationCount = 3;
					minimum = 0x10000;
				} else {
					return false;
				}

				if (continuationCount > text.size() - offset - 1) return false;
				for (size_t index = 1; index <= continuationCount; index++) {
					const uint8_t continuation = static_cast<uint8_t>(text[offset + index]);
					if ((continuation & 0xC0) != 0x80) return false;
					codePoint = (codePoint << 6) | (continuation & 0x3F);
				}
				if (codePoint < minimum || codePoint > 0x10FFFF ||
					(codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
					return false;
				}
				offset += continuationCount + 1;
			}
			return true;
		}

		bool Inflate(
			std::span<const std::byte> compressed,
			size_t maximumBytes,
			std::vector<uint8_t> &out,
			std::string &failure,
			std::string_view label
		) {
			out.clear();
			try {
				CryptoPP::ZlibDecompressor decompressor(new BoundedVectorSink(out, maximumBytes));
				decompressor.Put(
					reinterpret_cast<const CryptoPP::byte *>(compressed.data()), compressed.size()
				);
				decompressor.MessageEnd();
			} catch (const InflateLimitExceeded &) {
				return Fail(failure, "pxcx: " + std::string(label) + " expands past its byte limit");
			} catch (const InflateTrailingData &) {
				return Fail(failure, "pxcx: " + std::string(label) + " zlib stream has trailing bytes");
			} catch (const CryptoPP::Exception &error) {
				return Fail(failure, "pxcx: invalid " + std::string(label) + " zlib stream: " + error.what());
			}
			return true;
		}

		bool HasNoNul(std::string_view text) {
			return text.find('\0') == std::string_view::npos;
		}

		bool ReadTextField(const Json &object, std::string_view key, std::string &out) {
			const auto found = object.find(key);
			if (found == object.end() || !found->is_string()) return false;
			out = found->get<std::string>();
			return !out.empty() && out.size() <= PxcxLimits::MaximumNodeTextBytes && HasNoNul(out) &&
				   ValidUtf8(out);
		}

		bool ReadCoordinate(const Json &value, double &out) {
			constexpr int64_t MAXIMUM_EXACT_INTEGER = 9007199254740992ll;
			if (!value.is_number()) return false;
			if (value.is_number_unsigned()) {
				const uint64_t integer = value.get<uint64_t>();
				if (integer > static_cast<uint64_t>(MAXIMUM_EXACT_INTEGER)) return false;
			} else if (value.is_number_integer()) {
				const int64_t integer = value.get<int64_t>();
				if (integer < -MAXIMUM_EXACT_INTEGER || integer > MAXIMUM_EXACT_INTEGER) return false;
			}
			out = value.get<double>();
			return std::isfinite(out);
		}

		bool ReadIndex(const Json &value, uint32_t &out) {
			uint64_t index = 0;
			if (value.is_number_unsigned()) {
				index = value.get<uint64_t>();
			} else if (value.is_number_integer()) {
				const int64_t signedIndex = value.get<int64_t>();
				if (signedIndex < 0) return false;
				index = static_cast<uint64_t>(signedIndex);
			} else {
				return false;
			}
			if (index > std::numeric_limits<uint32_t>::max()) return false;
			out = static_cast<uint32_t>(index);
			return true;
		}

		bool ReadGraphFacts(std::string_view graphJson, PxcxArchive &out, std::string &failure) {
			if (graphJson.empty() || graphJson.back() != '\0') {
				return Fail(failure, "pxcx: graph JSON does not end with its required NUL byte");
			}
			graphJson.remove_suffix(1);
			if (graphJson.empty() || !ValidUtf8(graphJson) ||
				graphJson.find('\0') != std::string_view::npos) {
				return Fail(failure, "pxcx: graph JSON is empty or not valid UTF-8");
			}

			bool tooDeep = false;
			bool duplicateKey = false;
			std::vector<std::unordered_set<std::string>> keysByObjectDepth;
			const Json::parser_callback_t callback = [&](int depth, Json::parse_event_t event, Json &parsed) {
				if (depth > static_cast<int>(PxcxLimits::MaximumJsonDepth)) {
					tooDeep = true;
					return false;
				}
				if (event == Json::parse_event_t::object_start) {
					if (keysByObjectDepth.size() <= static_cast<size_t>(depth)) {
						keysByObjectDepth.resize(static_cast<size_t>(depth) + 1);
					}
					keysByObjectDepth[static_cast<size_t>(depth)].clear();
				}
				if (event == Json::parse_event_t::key && depth > 0 && parsed.is_string()) {
					const size_t objectDepth = static_cast<size_t>(depth - 1);
					if (objectDepth >= keysByObjectDepth.size() ||
						!keysByObjectDepth[objectDepth].insert(parsed.get<std::string>()).second) {
						duplicateKey = true;
						return false;
					}
				}
				return true;
			};

			Json root;
			try {
				root = Json::parse(graphJson.begin(), graphJson.end(), callback, true, false);
			} catch (const Json::exception &error) {
				return Fail(failure, "pxcx: malformed graph JSON: " + std::string(error.what()));
			}
			if (tooDeep) return Fail(failure, "pxcx: graph JSON exceeds its nesting limit");
			if (duplicateKey) return Fail(failure, "pxcx: graph JSON contains a duplicate object key");
			if (!root.is_object()) return Fail(failure, "pxcx: graph JSON root is not an object");

			const auto nodeField = root.find("nodes");
			if (nodeField == root.end() || !nodeField->is_array()) {
				return Fail(failure, "pxcx: graph JSON has no nodes array");
			}
			if (nodeField->size() > PxcxLimits::MaximumNodes) {
				return Fail(failure, "pxcx: graph node count exceeds its limit");
			}

			struct PendingLink {
				PxcxLinkFact Fact;
			};
			std::vector<PendingLink> pendingLinks;
			std::unordered_set<std::string> nodeIds;
			out.Nodes.reserve(nodeField->size());
			for (const Json &node : *nodeField) {
				if (!node.is_object()) return Fail(failure, "pxcx: graph node is not an object");

				PxcxNodeFact fact;
				if (!ReadTextField(node, "id", fact.Id) || !ReadTextField(node, "type", fact.Type)) {
					return Fail(failure, "pxcx: graph node has an invalid id or type");
				}
				if (!nodeIds.insert(fact.Id).second)
					return Fail(failure, "pxcx: graph node id is repeated: " + fact.Id);

				const auto x = node.find("x");
				const auto y = node.find("y");
				const auto inputs = node.find("inputs");
				if (x == node.end() || y == node.end() || !ReadCoordinate(*x, fact.X) ||
					!ReadCoordinate(*y, fact.Y)) {
					return Fail(failure, "pxcx: graph node has an invalid canvas position: " + fact.Id);
				}
				if (inputs == node.end() || !inputs->is_array()) {
					return Fail(failure, "pxcx: graph node has no positional inputs array: " + fact.Id);
				}
				if (inputs->size() > PxcxLimits::MaximumInputsPerNode) {
					return Fail(failure, "pxcx: graph node input count exceeds its limit: " + fact.Id);
				}

				for (size_t inputIndex = 0; inputIndex < inputs->size(); inputIndex++) {
					const Json &input = (*inputs)[inputIndex];
					if (!input.is_object())
						return Fail(failure, "pxcx: graph input is not an object: " + fact.Id);
					const auto fromNode = input.find("from_node");
					const auto fromIndex = input.find("from_index");
					if ((fromNode == input.end()) != (fromIndex == input.end())) {
						return Fail(
							failure, "pxcx: graph connection has only one endpoint field: " + fact.Id
						);
					}
					if (fromNode == input.end()) continue;

					if (!fromNode->is_string()) {
						return Fail(failure, "pxcx: graph connection source id is not text: " + fact.Id);
					}
					std::string sourceId = fromNode->get<std::string>();
					uint32_t sourceIndex = 0;
					if (sourceId.empty() || sourceId.size() > PxcxLimits::MaximumNodeTextBytes ||
						!HasNoNul(sourceId) || !ValidUtf8(sourceId) || !ReadIndex(*fromIndex, sourceIndex)) {
						return Fail(failure, "pxcx: graph connection endpoint is invalid: " + fact.Id);
					}
					if (pendingLinks.size() >= PxcxLimits::MaximumLinks) {
						return Fail(failure, "pxcx: graph link count exceeds its limit");
					}
					pendingLinks.push_back(
						{{std::move(sourceId), sourceIndex, fact.Id, static_cast<uint32_t>(inputIndex)}}
					);
				}
				out.Nodes.push_back(std::move(fact));
			}

			out.Links.reserve(pendingLinks.size());
			for (PendingLink &pending : pendingLinks) {
				if (nodeIds.find(pending.Fact.FromNode) == nodeIds.end()) {
					return Fail(
						failure,
						"pxcx: graph connection names a missing source node: " + pending.Fact.FromNode
					);
				}
				out.Links.push_back(std::move(pending.Fact));
			}
			return true;
		}
	}

	bool ReadPxcx(std::span<const std::byte> bytes, PxcxArchive &out, std::string &failure) {
		failure.clear();
		if (bytes.size() > PxcxLimits::MaximumArchiveBytes) {
			return Fail(failure, "pxcx: archive exceeds its byte limit");
		}
		if (bytes.size() < PREFIX_BYTES || !HasTag(bytes, 0, "PXCX")) {
			return Fail(failure, "pxcx: missing PXCX header");
		}

		const size_t graphOffset = ReadUInt32(bytes, 4);
		const bool hasThumbnailBlock = HasTag(bytes, PREFIX_BYTES, "THMB");
		size_t thumbnailCompressedBytes = 0;
		size_t metadataOffset = PREFIX_BYTES;
		if (hasThumbnailBlock) {
			if (bytes.size() < HEADER_BYTES) return Fail(failure, "pxcx: truncated THMB header");
			thumbnailCompressedBytes = ReadUInt32(bytes, 12);
			if (thumbnailCompressedBytes > PxcxLimits::MaximumThumbnailCompressedBytes)
				return Fail(failure, "pxcx: thumbnail compressed size is invalid");
			if (thumbnailCompressedBytes > bytes.size() - HEADER_BYTES)
				return Fail(failure, "pxcx: thumbnail compressed span is truncated");
			metadataOffset = HEADER_BYTES + thumbnailCompressedBytes;
		}
		if (metadataOffset > bytes.size() || META_HEADER_BYTES > bytes.size() - metadataOffset ||
			!HasTag(bytes, metadataOffset, "META")) {
			return Fail(failure, "pxcx: missing META block after thumbnail");
		}

		const size_t metadataBytes = ReadUInt32(bytes, metadataOffset + 4);
		if (metadataBytes > PxcxLimits::MaximumMetadataBytes) {
			return Fail(failure, "pxcx: META payload exceeds its byte limit");
		}
		const size_t metadataPayloadOffset = metadataOffset + META_HEADER_BYTES;
		if (metadataBytes > bytes.size() - metadataPayloadOffset ||
			metadataPayloadOffset + metadataBytes != graphOffset) {
			return Fail(failure, "pxcx: META span does not end at the graph offset");
		}
		if (graphOffset > bytes.size() || graphOffset == bytes.size()) {
			return Fail(failure, "pxcx: graph offset is outside the archive");
		}
		if (metadataBytes < META_NUMBER_BYTES + 1) {
			return Fail(failure, "pxcx: META payload has no number and terminated text");
		}

		const std::span<const std::byte> metadataPayload =
			bytes.subspan(metadataPayloadOffset, metadataBytes);
		if (std::to_integer<uint8_t>(metadataPayload.back()) != 0) {
			return Fail(failure, "pxcx: META text has no terminal NUL byte");
		}
		const auto metadataTextBytes =
			metadataPayload.subspan(META_NUMBER_BYTES, metadataPayload.size() - META_NUMBER_BYTES - 1);
		for (const std::byte value : metadataTextBytes) {
			if (std::to_integer<uint8_t>(value) == 0)
				return Fail(failure, "pxcx: META text contains an embedded NUL byte");
		}
		const std::string_view metadataText(
			reinterpret_cast<const char *>(metadataTextBytes.data()), metadataTextBytes.size()
		);
		if (metadataText.empty() || !ValidUtf8(metadataText))
			return Fail(failure, "pxcx: META text is empty or invalid UTF-8");

		const size_t graphCompressedBytes = bytes.size() - graphOffset;
		if (graphCompressedBytes > PxcxLimits::MaximumGraphCompressedBytes) {
			return Fail(failure, "pxcx: graph compressed span exceeds its byte limit");
		}
		if (graphCompressedBytes == 0) return Fail(failure, "pxcx: graph compressed span is empty");

		PxcxArchive parsed;
		parsed.OriginalBytes.assign(bytes.begin(), bytes.end());
		parsed.HasThumbnailBlock = hasThumbnailBlock;
		parsed.MetadataPayload.assign(metadataPayload.begin(), metadataPayload.end());
		parsed.MetadataNumber = ReadUInt32(metadataPayload, 0);
		parsed.MetadataText.assign(metadataText);

		if (thumbnailCompressedBytes != 0) {
			std::vector<uint8_t> thumbnail;
			const auto thumbnailStream = bytes.subspan(HEADER_BYTES, thumbnailCompressedBytes);
			if (!Inflate(thumbnailStream, PxcxLimits::ThumbnailRgbaBytes, thumbnail, failure, "thumbnail"))
				return false;
			if (thumbnail.size() != PxcxLimits::ThumbnailRgbaBytes)
				return Fail(failure, "pxcx: thumbnail does not decode to 256 by 256 RGBA8 bytes");
			parsed.ThumbnailRgba = std::move(thumbnail);
		}

		std::vector<uint8_t> graph;
		const auto graphStream = bytes.subspan(graphOffset, graphCompressedBytes);
		if (!Inflate(graphStream, PxcxLimits::MaximumGraphJsonBytes, graph, failure, "graph")) return false;
		if (graph.empty()) return Fail(failure, "pxcx: graph zlib stream decoded no JSON bytes");
		parsed.GraphJson.assign(reinterpret_cast<const char *>(graph.data()), graph.size());
		if (!ReadGraphFacts(parsed.GraphJson, parsed, failure)) return false;

		out = std::move(parsed);
		return true;
	}

	bool WritePxcx(const PxcxArchive &archive, std::vector<std::byte> &out, std::string &failure) {
		failure.clear();
		if (!archive.HasThumbnailBlock && !archive.ThumbnailRgba.empty())
			return Fail(failure, "pxcx: thumbnail bytes require a THMB block");
		if (!archive.ThumbnailRgba.empty() && archive.ThumbnailRgba.size() != PxcxLimits::ThumbnailRgbaBytes)
			return Fail(failure, "pxcx: thumbnail must contain 256 by 256 RGBA8 bytes");
		if (archive.GraphJson.empty() || archive.GraphJson.size() > PxcxLimits::MaximumGraphJsonBytes)
			return Fail(failure, "pxcx: graph JSON byte count is invalid");
		if (archive.MetadataText.empty() || !HasNoNul(archive.MetadataText) ||
			!ValidUtf8(archive.MetadataText) ||
			archive.MetadataText.size() + META_NUMBER_BYTES + 1 > PxcxLimits::MaximumMetadataBytes)
			return Fail(failure, "pxcx: META version text is invalid");

		PxcxArchive validated;
		if (!ReadGraphFacts(archive.GraphJson, validated, failure)) return false;
		if ((!archive.Nodes.empty() && archive.Nodes != validated.Nodes) ||
			(!archive.Links.empty() && archive.Links != validated.Links))
			return Fail(failure, "pxcx: graph facts differ from the JSON stream");

		if (!archive.OriginalBytes.empty()) {
			PxcxArchive checked;
			if (!ReadPxcx(archive.OriginalBytes, checked, failure)) return false;
			if (checked.MetadataPayload != archive.MetadataPayload)
				return Fail(failure, "pxcx: edit META through its version number and text fields");
			if (checked.HasThumbnailBlock == archive.HasThumbnailBlock &&
				checked.ThumbnailRgba == archive.ThumbnailRgba &&
				checked.MetadataNumber == archive.MetadataNumber &&
				checked.MetadataText == archive.MetadataText && checked.GraphJson == archive.GraphJson) {
				out = archive.OriginalBytes;
				return true;
			}
		}

		std::vector<std::byte> written;
		AppendText(written, "PXCX");
		AppendUInt32(written, 0);
		if (archive.HasThumbnailBlock) {
			AppendText(written, "THMB");
			const size_t thumbnailLengthOffset = written.size();
			AppendUInt32(written, 0);
			if (!archive.ThumbnailRgba.empty()) {
				const size_t thumbnailStart = written.size();
				if (!Compress(archive.ThumbnailRgba, written, failure)) return false;
				const size_t compressedBytes = written.size() - thumbnailStart;
				if (compressedBytes > PxcxLimits::MaximumThumbnailCompressedBytes)
					return Fail(failure, "pxcx: thumbnail compressed size exceeds its limit");
				PatchUInt32(written, thumbnailLengthOffset, static_cast<uint32_t>(compressedBytes));
			}
		}
		AppendText(written, "META");
		AppendUInt32(written, static_cast<uint32_t>(META_NUMBER_BYTES + archive.MetadataText.size() + 1));
		AppendUInt32(written, archive.MetadataNumber);
		AppendText(written, archive.MetadataText);
		written.push_back(std::byte{0});
		PatchUInt32(written, 4, static_cast<uint32_t>(written.size()));

		const auto graphBytes = std::span<const uint8_t>(
			reinterpret_cast<const uint8_t *>(archive.GraphJson.data()), archive.GraphJson.size()
		);
		const size_t graphStart = written.size();
		if (!Compress(graphBytes, written, failure)) return false;
		if (written.size() - graphStart > PxcxLimits::MaximumGraphCompressedBytes ||
			written.size() > PxcxLimits::MaximumArchiveBytes)
			return Fail(failure, "pxcx: written archive exceeds its byte limit");

		PxcxArchive checked;
		if (!ReadPxcx(written, checked, failure)) return false;
		out = std::move(written);
		return true;
	}
}
