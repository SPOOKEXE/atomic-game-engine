#include "NativeFrameCacheReceipt.hpp"

#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.native_frame_cache_loading_receipt")
namespace {
	using namespace engine::imagegraph;
	Image Pixels() {
		Image image{2, 1, {0, 10, 13, 34, 92, 128, 255, 9}};
		image.Hash = SurfaceHash(image);
		return image;
	}
	DataReplayEntry Inventory(bool array = false) {
		DataReplayEntry row;
		row.NodeId = "cache";
		row.Initialized = true;
		row.PreviousValue = 1;
		row.LoadedCacheData = "[exact source cache with trailing holes]";
		row.SourceFrameCacheSerializedSlots = 7;
		row.Values = {
			{0, std::string(array ? "pc.cache_array" : "pc.cache")},
			{1, array ? Value{ArrayValue{ValueType::Any, {}}} : Value{int64_t{-4}}},
			{2, SurfaceValue{Pixels()}},
			{4, SurfaceValue{Pixels()}}
		};
		return row;
	}
	Node Receipt(const DataReplayEntry &row) {
		Node node{row.NodeId, std::string(SourceFrameCacheRowType(row)), "", {}, {}};
		ArrayValue chunks;
		Diagnostic error;
		REQUIRE(EncodeSourceFrameCacheReceipt(row, chunks, error) == Status::Ok);
		node.SourceProperties = {
			{"cache", row.LoadedCacheData},
			{"serialize", true},
			{std::string(SOURCE_FRAME_CACHE_NATIVE_TEXT), row.LoadedCacheData},
			{std::string(SOURCE_FRAME_CACHE_NATIVE_DATA), std::move(chunks)}
		};
		return node;
	}
	ArrayValue &Chunks(Node &node) {
		return std::get<ArrayValue>(node.SourceProperties.back().Data);
	}
	std::string Bytes(Node node) {
		std::string bytes;
		for (const auto &chunk : Chunks(node).Elements)
			bytes += std::get<std::string>(chunk);
		return bytes;
	}
	void UInt(std::string &bytes, uint32_t value) {
		for (unsigned shift = 0; shift < 32; shift += 8)
			bytes.push_back(char((value >> shift) & 255));
	}
	void ReplaceUInt(std::string &bytes, size_t offset, uint32_t value) {
		REQUIRE(offset + 4 <= bytes.size());
		for (unsigned shift = 0; shift < 32; shift += 8)
			bytes[offset + shift / 8] = char((value >> shift) & 255);
	}
	std::string Expected(bool array, std::optional<uint64_t> slots) {
		std::string bytes;
		const std::string type = array ? "pc.cache_array" : "pc.cache";
		UInt(bytes, slots ? 2 : 1);
		UInt(bytes, uint32_t(type.size()));
		bytes += type;
		if (slots) UInt(bytes, uint32_t(*slots));
		UInt(bytes, 2);
		for (uint32_t frame : {2u, 4u}) {
			UInt(bytes, frame);
			bytes.push_back(1);
			UInt(bytes, 2);
			UInt(bytes, 1);
			for (auto pixel : Pixels().Pixels)
				bytes.push_back(char(pixel));
		}
		return bytes;
	}
	Node Roundtrip(Node node) {
		Document document;
		document.FormatVersion = 11;
		document.Nodes.push_back(std::move(node));
		Document reopened;
		Diagnostic error;
		const auto status = Read(Write(document), reopened, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		CHECK(reopened == document);
		return std::move(reopened.Nodes.front());
	}
	DataReplayEntry Sentinel() {
		DataReplayEntry row;
		row.NodeId = "untouched";
		row.Values = {{9, int64_t{42}}};
		row.SourceFrameCacheSerializedSlots = 23;
		return row;
	}
	void Reject(const Node &node) {
		Diagnostic error;
		uint64_t measured = 123;
		CHECK(MeasureSourceFrameCacheReceipt(node, measured, error) != Status::Ok);
		CHECK(measured == 123);
		detail::NativeFrameCacheReceiptInspection inspection{123, true, uint64_t{23}};
		const auto previous = inspection;
		CHECK(detail::InspectNativeFrameCacheReceipt(node, 0, inspection) != Status::Ok);
		CHECK(inspection == previous);
		auto row = Sentinel();
		const auto before = row;
		CHECK(DecodeSourceFrameCacheReceipt(node, row, error) != Status::Ok);
		CHECK(row == before);
	}
}

TEST_CASE(
	"Native v2 receipts preserve original holes and trailing slots through authored saves",
	"[imagegraph][source_frame_cache][native_frame_cache_loading_receipt]"
) {
	for (bool array : {false, true}) {
		const auto inventory = Inventory(array);
		const auto node = Roundtrip(Receipt(inventory));
		CHECK(Bytes(node) == Expected(array, 7));
		DataReplayEntry decoded;
		Diagnostic error;
		uint64_t measured = 0;
		REQUIRE(MeasureSourceFrameCacheReceipt(node, measured, error) == Status::Ok);
		REQUIRE(DecodeSourceFrameCacheReceipt(node, decoded, error) == Status::Ok);
		CHECK(decoded == inventory);
		CHECK(decoded.Values.size() == 4);
		CHECK(decoded.SourceFrameCacheSerializedSlots == 7);
		CHECK_FALSE(decoded.SourceFrameCacheLoading);
		CHECK(RetainedDataReplayEntryBytes(decoded) <= measured);
		for (uint64_t tick = 0; tick < 8; ++tick) {
			detail::NativeFrameCacheReceiptInspection inspection;
			REQUIRE(detail::InspectNativeFrameCacheReceipt(node, tick, inspection) == Status::Ok);
			CHECK(inspection.SerializedSlots == 7);
			CHECK(inspection.HasFrame == (tick == 0 || tick == 2));
		}
		const auto before = decoded;
		CHECK(DecodeSourceFrameCacheReceipt(node, decoded, error, measured - 1) == Status::LimitExceeded);
		CHECK(decoded == before);
	}
}

TEST_CASE(
	"Native v2 zero and maximum slot inventories retain exact count without guessed frames",
	"[imagegraph][source_frame_cache][native_frame_cache_loading_receipt]"
) {
	for (uint64_t count : {uint64_t{0}, uint64_t{3}, uint64_t{7}, uint64_t{Limits::MaximumArrayElements}}) {
		auto inventory = Inventory(true);
		inventory.SourceFrameCacheSerializedSlots = count;
		inventory.Values.resize(2);
		if (count) inventory.Values.push_back({count + 1, SurfaceValue{Pixels()}});
		const auto node = Roundtrip(Receipt(inventory));
		DataReplayEntry decoded;
		Diagnostic error;
		REQUIRE(DecodeSourceFrameCacheReceipt(node, decoded, error) == Status::Ok);
		CHECK(decoded == inventory);
		detail::NativeFrameCacheReceiptInspection inspection;
		REQUIRE(
			detail::InspectNativeFrameCacheReceipt(node, count ? count - 1 : 0, inspection) == Status::Ok
		);
		CHECK(inspection.SerializedSlots == count);
		CHECK(inspection.HasFrame == bool(count));
	}
	auto shorter = Inventory(true), longer = shorter;
	shorter.SourceFrameCacheSerializedSlots = 3;
	CHECK(Bytes(Receipt(shorter)) != Bytes(Receipt(longer)));
}

TEST_CASE(
	"Native v2 packet truncation counts ordering and metadata failures preserve prior output",
	"[imagegraph][source_frame_cache][native_frame_cache_loading_receipt]"
) {
	const auto good = Receipt(Inventory());
	const auto packet = Bytes(good);
	for (size_t length = 0; length < packet.size(); ++length) {
		CAPTURE(length);
		auto node = good;
		Chunks(node).Elements = {packet.substr(0, length)};
		Reject(node);
	}
	for (uint32_t count : {0u, 2u, uint32_t(Limits::MaximumArrayElements + 1)}) {
		CAPTURE(count);
		auto node = good;
		auto bytes = packet;
		ReplaceUInt(bytes, 16, count);
		Chunks(node).Elements = {bytes};
		Reject(node);
	}
	SECTION("unknown version") {
		auto node = good;
		auto bytes = packet;
		ReplaceUInt(bytes, 0, 3);
		Chunks(node).Elements = {bytes};
		Reject(node);
	}
	SECTION("frame outside original slots") {
		auto node = good;
		auto bytes = packet;
		ReplaceUInt(bytes, 24, 9);
		Chunks(node).Elements = {bytes};
		Reject(node);
	}
	SECTION("repeated frame") {
		auto node = good;
		auto bytes = packet;
		ReplaceUInt(bytes, 45, 2);
		Chunks(node).Elements = {bytes};
		Reject(node);
	}
	SECTION("exact source identity changed") {
		auto node = good;
		node.SourceProperties.front().Data = std::string{"different saved source"};
		Reject(node);
	}
	SECTION("source node type changed") {
		auto node = good;
		node.Type = "pc.cache_array";
		Reject(node);
	}
}

TEST_CASE(
	"Native v2 header counts can cross chunk boundaries without changing ownership",
	"[imagegraph][source_frame_cache][native_frame_cache_loading_receipt]"
) {
	auto node = Receipt(Inventory());
	const auto bytes = Bytes(node);
	Chunks(node).Elements.clear();
	for (size_t offset = 0; offset < bytes.size(); offset += 3)
		Chunks(node).Elements.emplace_back(bytes.substr(offset, 3));
	DataReplayEntry decoded;
	Diagnostic error;
	REQUIRE(DecodeSourceFrameCacheReceipt(node, decoded, error) == Status::Ok);
	CHECK(decoded == Inventory());
}

TEST_CASE(
	"Native frame cache encoding refuses original count violations and any loading state atomically",
	"[imagegraph][source_frame_cache][native_frame_cache_loading_receipt]"
) {
	auto inventory = Inventory();
	SECTION("count beyond source limit") {
		inventory.SourceFrameCacheSerializedSlots = Limits::MaximumArrayElements + 1;
	}
	SECTION("zero count cannot own populated slots") {
		inventory.SourceFrameCacheSerializedSlots = 0;
	}
	SECTION("sparse frame is outside original count") {
		inventory.SourceFrameCacheSerializedSlots = 2;
	}
	SECTION("active prefix") {
		inventory.SourceFrameCacheLoading.emplace();
		inventory.SourceFrameCacheLoading->NextSlot = 1;
		inventory.SourceFrameCacheLoading->PendingSlots.push_back(inventory.Values.back());
		inventory.Values.pop_back();
	}
	SECTION("ended loader still owns unconsumed inventory") {
		inventory.SourceFrameCacheLoading.emplace();
		inventory.SourceFrameCacheLoading->Loading = false;
		inventory.SourceFrameCacheLoading->PendingSlots.push_back(inventory.Values.back());
		inventory.Values.pop_back();
	}
	SECTION("ended loader has no explicit complete inventory profile") {
		inventory.SourceFrameCacheLoading.emplace();
		inventory.SourceFrameCacheLoading->Loading = false;
	}
	ArrayValue chunks{ValueType::Text, {std::string{"previous complete receipt"}}};
	const auto before = chunks;
	Diagnostic error;
	CHECK(EncodeSourceFrameCacheReceipt(inventory, chunks, error) != Status::Ok);
	CHECK(chunks == before);
}

TEST_CASE(
	"Legacy v1 receipts remain exact and support complete native playback without slot inference",
	"[imagegraph][source_frame_cache][native_frame_cache_loading_receipt]"
) {
	auto inventory = Inventory();
	inventory.SourceFrameCacheSerializedSlots.reset();
	const auto node = Roundtrip(Receipt(inventory));
	CHECK(Bytes(node) == Expected(false, std::nullopt));
	DataReplayEntry decoded;
	Diagnostic error;
	REQUIRE(DecodeSourceFrameCacheReceipt(node, decoded, error) == Status::Ok);
	CHECK(decoded == inventory);
	CHECK_FALSE(decoded.SourceFrameCacheSerializedSlots);
	detail::NativeFrameCacheReceiptInspection inspection;
	REQUIRE(detail::InspectNativeFrameCacheReceipt(node, 2, inspection) == Status::Ok);
	CHECK(inspection.HasFrame);
	CHECK_FALSE(inspection.SerializedSlots);
	Document document;
	document.FormatVersion = 11;
	document.Timeline = TimelineSettings{6, 0, 5, "loop", 24};
	document.Nodes = {
		{"input",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{99, 88, 77, 255}}}},
		node
	};
	document.Nodes.back().Values = {{"animated", false}};
	document.Links = {{"input", "image", "cache", "surface_in"}};
	document.Outputs = {{"out", "cache", "cache_surface"}};
	Plan plan;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 2;
	request.SourceCachePlayback =
		SourceCachePlaybackObservation{true, SourceCacheSampling::ObservedFrame, true};
	StatefulEvaluationResult result;
	const auto status = EvaluateStateful(document, plan, "out", request, result, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	const auto *image = std::get_if<Image>(&result.Output);
	REQUIRE(image);
	CHECK(*image == Pixels());
}

TEST_CASE(
	"Native progressive slot decode validates future packets but allocates only the selected slot",
	"[imagegraph][source_frame_cache][native_frame_cache_loading_receipt]"
) {
	auto inventory = Inventory();
	Image future{256, 256, std::vector<uint8_t>(256 * 256 * 4, 91)};
	future.Hash = SurfaceHash(future);
	inventory.Values.back().Data = SurfaceValue{future};
	const auto node = Roundtrip(Receipt(inventory));
	Diagnostic error;
	DataReplayEntry complete;
	CHECK(DecodeSourceFrameCacheReceipt(node, complete, error, 4096) == Status::LimitExceeded);
	Value selected = int64_t{23};
	REQUIRE(detail::DecodeSourceFrameCacheReceiptSlot(node, 0, selected, error, 4096) == Status::Ok);
	CHECK(std::get<SurfaceValue>(selected).Data == Pixels());
	REQUIRE(detail::DecodeSourceFrameCacheReceiptSlot(node, 1, selected, error, 4096) == Status::Ok);
	CHECK(selected == Value{int64_t{-4}});
	const auto previous = selected;
	CHECK(detail::DecodeSourceFrameCacheReceiptSlot(node, 2, selected, error, 4096) == Status::LimitExceeded);
	CHECK(selected == previous);
	CHECK(detail::DecodeSourceFrameCacheReceiptSlot(node, 7, selected, error, 4096) == Status::InvalidValue);
	CHECK(selected == previous);
	REQUIRE(detail::DecodeSourceFrameCacheReceiptSlot(node, 6, selected, error, 4096) == Status::Ok);
	CHECK(selected == Value{int64_t{-4}});
	auto damaged = node;
	std::get<std::string>(Chunks(damaged).Elements.back()).pop_back();
	CHECK(detail::DecodeSourceFrameCacheReceiptSlot(damaged, 0, selected, error, 4096) == Status::Malformed);
	CHECK(selected == previous);
	auto legacyInventory = Inventory();
	legacyInventory.SourceFrameCacheSerializedSlots.reset();
	const auto legacy = Receipt(legacyInventory);
	CHECK(
		detail::DecodeSourceFrameCacheReceiptSlot(legacy, 0, selected, error, 4096) ==
		Status::UnsupportedExecution
	);
	CHECK(selected == previous);
}

TEST_CASE(
	"Native progressive array slot decode owns nested images without decoding future arrays",
	"[imagegraph][source_frame_cache][native_frame_cache_loading_receipt]"
) {
	auto inventory = Inventory(true);
	ArrayValue array{ValueType::Any, {}};
	array.Items = {
		SourceArrayItem{Pixels()},
		SourceArrayItem{std::vector<SourceArrayItem>{
			SourceArrayItem{ElementValue{int64_t{-4}}}, SourceArrayItem{Pixels()}
		}}
	};
	inventory.Values[2].Data = array;
	const auto node = Receipt(inventory);
	Value selected = int64_t{-4};
	Diagnostic error;
	REQUIRE(detail::DecodeSourceFrameCacheReceiptSlot(node, 0, selected, error, 4096) == Status::Ok);
	CHECK(selected == Value{array});
	CHECK(detail::DecodeSourceFrameCacheReceiptSlot(node, 0, selected, error, 1) == Status::LimitExceeded);
	CHECK(selected == Value{array});
}
