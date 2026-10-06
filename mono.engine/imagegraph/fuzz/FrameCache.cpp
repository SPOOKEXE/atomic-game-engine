#include "NativeFrameCacheReceipt.hpp"

#include <engine/imagegraph/FrameCacheReplay.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	using namespace engine::imagegraph;
	constexpr size_t HEADER_BYTES = 10, MAXIMUM_INPUT_BYTES = 128 * 1024;
	constexpr uint64_t DECODE_BYTES = 8 * 1024 * 1024;
	void Require(bool condition) {
		if (!condition) std::abort();
	}
	DataReplayEntry Sentinel() {
		DataReplayEntry row;
		row.NodeId = "sentinel";
		row.Initialized = true;
		row.PreviousValue = 9;
		row.LoadedCacheData = "old";
		row.Values = {{7, int64_t{23}}};
		return row;
	}
	Node PacketNode(ArrayValue chunks, uint8_t controls) {
		Node node{"cache", controls & 1 ? "pc.cache_array" : "pc.cache", "", {}, {}};
		node.SourceProperties = {
			{"cache", std::string{"saved"}},
			{"serialize", true},
			{std::string(SOURCE_FRAME_CACHE_NATIVE_TEXT), std::string{"saved"}},
			{std::string(SOURCE_FRAME_CACHE_NATIVE_DATA), std::move(chunks)}
		};
		if (controls & 4) node.SourceProperties[2].Data = std::string{"stale"};
		if (controls & 8) node.SourceProperties.push_back(node.SourceProperties.front());
		if (controls & 16) node.SourceProperties[1].Data = int64_t{1};
		if (controls & 32)
			std::get<ArrayValue>(node.SourceProperties[3].Data).ElementType = ValueType::Integer;
		return node;
	}
	void WriteSeeds(const std::filesystem::path &directory) {
		std::filesystem::create_directories(directory);
		for (const bool array : {false, true})
			for (const bool wide : {false, true}) {
				DataReplayEntry row;
				row.NodeId = "cache";
				row.Initialized = true;
				row.PreviousValue = 1;
				row.LoadedCacheData = "saved";
				Image image = wide ? Image{32, 256, std::vector<uint8_t>(32 * 256 * 4, 91)}
								   : Image{2, 1, {0, 10, 13, 34, 92, 128, 255, 9}};
				image.Hash = SurfaceHash(image);
				ArrayValue nested{ValueType::Any, {}};
				nested.Items = {SourceArrayItem{image}, SourceArrayItem{ElementValue{int64_t{-4}}}};
				row.Values = {
					{0, std::string{array ? "pc.cache_array" : "pc.cache"}},
					{1, array ? Value{ArrayValue{ValueType::Any, {}}} : Value{int64_t{-4}}},
					{2, SurfaceValue{image}},
					{4, array ? Value{nested} : Value{SurfaceValue{image}}},
					{9, int64_t{-4}}
				};
				ArrayValue chunks;
				Diagnostic diagnostic;
				Require(EncodeSourceFrameCacheReceipt(row, chunks, diagnostic, DECODE_BYTES) == Status::Ok);
				std::array<uint8_t, HEADER_BYTES> header{};
				header[0] = (array ? 1 : 0) | (wide ? 2 : 0);
				header[1] = wide ? 128 : 0;
				header[2] = 2;
				std::ofstream stream(
					directory /
						(std::string{array ? "nested-array" : "sparse-surface"} + (wide ? "-split" : "")),
					std::ios::binary
				);
				stream.write(reinterpret_cast<const char *>(header.data()), header.size());
				for (const auto &element : chunks.Elements) {
					const auto &text = std::get<std::string>(element);
					stream.write(text.data(), text.size());
				}
				stream.close();
				Require(stream.good());
			}
	}
}

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	WriteSeeds((*arguments)[2]);
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	if (size < HEADER_BYTES || size > MAXIMUM_INPUT_BYTES) return 0;
	uint64_t tick = 0;
	for (size_t index = 0; index < 8; ++index)
		tick |= uint64_t{input[index + 2]} << (index * 8);
	const auto *packet = reinterpret_cast<const char *>(input + HEADER_BYTES);
	const size_t packetBytes = size - HEADER_BYTES;
	ArrayValue chunks{ValueType::Text, {}};
	constexpr size_t CHUNK_BYTES = 64 * 1024;
	const size_t minimumSplit = packetBytes > CHUNK_BYTES ? packetBytes - CHUNK_BYTES : 0;
	const size_t maximumSplit = std::min(packetBytes, CHUNK_BYTES);
	const size_t split =
		input[0] & 2 ? minimumSplit + (maximumSplit - minimumSplit) * input[1] / 255 : packetBytes;
	if (split) chunks.Elements.emplace_back(std::string(packet, split));
	if (split < packetBytes) chunks.Elements.emplace_back(std::string(packet + split, packetBytes - split));
	const auto node = PacketNode(std::move(chunks), input[0]);
	uint64_t measured = 0x1234;
	Diagnostic diagnostic;
	const auto measure = MeasureSourceFrameCacheReceipt(node, measured, diagnostic);
	detail::NativeFrameCacheReceiptInspection inspection{0x5678, true};
	const auto inspect = detail::InspectNativeFrameCacheReceipt(node, tick, inspection);
	Require(measure == inspect);
	if (measure != Status::Ok) {
		Require(measured == 0x1234);
		Require(inspection == detail::NativeFrameCacheReceiptInspection{0x5678, true});
	} else
		Require(inspection.DecodedBytes == measured);
	auto output = Sentinel();
	const auto before = output;
	const auto decoded = DecodeSourceFrameCacheReceipt(node, output, diagnostic, DECODE_BYTES);
	if (decoded != Status::Ok) {
		Require(output == before);
		Require(!diagnostic.Message.empty());
		return 0;
	}
	Require(measure == Status::Ok && diagnostic.Code == Status::Ok);
	Require(output.NodeId == node.Id && output.LoadedCacheData == "saved");
	Require(SourceFrameCacheRowType(output) == node.Type);
	ArrayValue encoded;
	Require(EncodeSourceFrameCacheReceipt(output, encoded, diagnostic, DECODE_BYTES) == Status::Ok);
	const auto roundTripNode = PacketNode(std::move(encoded), input[0] & 1);
	DataReplayEntry roundTrip;
	Require(DecodeSourceFrameCacheReceipt(roundTripNode, roundTrip, diagnostic, DECODE_BYTES) == Status::Ok);
	Require(roundTrip == output);
	return 0;
}
