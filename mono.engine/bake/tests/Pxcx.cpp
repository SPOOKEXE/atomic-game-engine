// The fixture is written here so each damaged length, tag and reference is a
// deliberate mutation of one valid container rather than a second parser's file.

#include <engine/bake/Pxcx.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.bake.pxcx")

using engine::bake::PxcxArchive;
using engine::bake::PxcxLimits;
using engine::bake::PxcxLinkFact;
using engine::bake::PxcxNodeFact;
using engine::bake::ReadPxcx;
using engine::bake::WritePxcx;

namespace {
	constexpr std::string_view GRAPH =
		R"JSON({"nodes":[{"id":"source","type":"image.solid","x":1,"y":-2,"inputs":[]},{"id":"foreign-node","type":"vendor.unimplemented","x":3.5,"y":4,"inputs":[{"from_node":"source","from_index":1016}]}]})JSON";

	std::span<const std::byte> Bytes(const std::vector<std::byte> &bytes) {
		return bytes;
	}

	void AppendLittle16(std::vector<uint8_t> &bytes, uint16_t value) {
		bytes.push_back(static_cast<uint8_t>(value));
		bytes.push_back(static_cast<uint8_t>(value >> 8));
	}

	void AppendBig32(std::vector<uint8_t> &bytes, uint32_t value) {
		bytes.push_back(static_cast<uint8_t>(value >> 24));
		bytes.push_back(static_cast<uint8_t>(value >> 16));
		bytes.push_back(static_cast<uint8_t>(value >> 8));
		bytes.push_back(static_cast<uint8_t>(value));
	}

	std::vector<uint8_t> Compress(std::span<const uint8_t> bytes) {
		std::vector<uint8_t> compressed{0x78, 0x01};
		size_t offset = 0;
		do {
			const size_t blockBytes = std::min<size_t>(bytes.size() - offset, UINT16_MAX);
			const bool finalBlock = offset + blockBytes == bytes.size();
			compressed.push_back(finalBlock ? 0x01 : 0x00);
			const uint16_t length = static_cast<uint16_t>(blockBytes);
			AppendLittle16(compressed, length);
			AppendLittle16(compressed, static_cast<uint16_t>(~length));
			compressed.insert(compressed.end(), bytes.begin() + offset, bytes.begin() + offset + blockBytes);
			offset += blockBytes;
		} while (offset < bytes.size());

		uint32_t first = 1;
		uint32_t second = 0;
		for (const uint8_t value : bytes) {
			first = (first + value) % 65521;
			second = (second + first) % 65521;
		}
		AppendBig32(compressed, (second << 16) | first);
		return compressed;
	}

	std::vector<uint8_t> Compress(std::string_view text, bool terminalNul = false) {
		std::string input(text);
		if (terminalNul) input.push_back('\0');
		return Compress({reinterpret_cast<const uint8_t *>(input.data()), input.size()});
	}

	struct PxcxFixture {
		std::vector<std::byte> Bytes;
		size_t MetadataOffset = 0;
		size_t MetadataPayloadOffset = 0;
		size_t GraphOffset = 0;
	};

	void AppendByte(std::vector<std::byte> &bytes, uint8_t value) {
		bytes.push_back(static_cast<std::byte>(value));
	}

	void AppendUInt32(std::vector<std::byte> &bytes, uint32_t value) {
		for (uint32_t shift = 0; shift < 32; shift += 8) {
			AppendByte(bytes, static_cast<uint8_t>(value >> shift));
		}
	}

	void AppendText(std::vector<std::byte> &bytes, std::string_view text) {
		for (const char value : text)
			AppendByte(bytes, static_cast<uint8_t>(value));
	}

	void AppendBytes(std::vector<std::byte> &bytes, std::span<const uint8_t> values) {
		for (const uint8_t value : values)
			AppendByte(bytes, value);
	}

	PxcxFixture BuildFixture(
		std::string_view graph = GRAPH,
		bool terminalNul = true,
		std::vector<uint8_t> thumbnail = std::vector<uint8_t>(PxcxLimits::ThumbnailRgbaBytes, 23),
		bool hasThumbnailBlock = true,
		uint32_t metadataNumber = 121092,
		std::string_view metadataText = "1.22.10.201"
	) {
		const std::vector<uint8_t> compressedThumbnail =
			thumbnail.empty() ? std::vector<uint8_t>{} : Compress(thumbnail);
		const std::vector<uint8_t> compressedGraph = Compress(graph, terminalNul);

		PxcxFixture fixture;
		AppendText(fixture.Bytes, "PXCX");
		AppendUInt32(fixture.Bytes, 0);
		if (hasThumbnailBlock) {
			AppendText(fixture.Bytes, "THMB");
			AppendUInt32(fixture.Bytes, static_cast<uint32_t>(compressedThumbnail.size()));
			AppendBytes(fixture.Bytes, compressedThumbnail);
		}
		fixture.MetadataOffset = fixture.Bytes.size();
		AppendText(fixture.Bytes, "META");
		AppendUInt32(fixture.Bytes, static_cast<uint32_t>(sizeof(metadataNumber) + metadataText.size() + 1));
		fixture.MetadataPayloadOffset = fixture.Bytes.size();
		AppendUInt32(fixture.Bytes, metadataNumber);
		AppendText(fixture.Bytes, metadataText);
		AppendByte(fixture.Bytes, 0);
		fixture.GraphOffset = fixture.Bytes.size();
		for (uint32_t shift = 0; shift < 32; shift += 8) {
			fixture.Bytes[4 + shift / 8] = static_cast<std::byte>(fixture.GraphOffset >> shift);
		}
		AppendBytes(fixture.Bytes, compressedGraph);
		return fixture;
	}

	void PatchUInt32(std::vector<std::byte> &bytes, size_t offset, uint32_t value) {
		for (uint32_t shift = 0; shift < 32; shift += 8) {
			bytes[offset + shift / 8] = static_cast<std::byte>(value >> shift);
		}
	}

	bool Refuses(const std::vector<std::byte> &bytes, std::string *failureOut = nullptr) {
		PxcxArchive out;
		out.OriginalBytes = {std::byte{42}};
		std::string failure;
		const bool read = ReadPxcx(Bytes(bytes), out, failure);
		if (failureOut != nullptr) *failureOut = failure;
		CHECK_FALSE(read);
		CHECK_FALSE(failure.empty());
		CHECK(out.OriginalBytes == std::vector<std::byte>{std::byte{42}});
		return !read;
	}
}

TEST_CASE("PXCX reader preserves container data and validates node links", "[bake][pxcx]") {
	const PxcxFixture fixture = BuildFixture();
	PxcxArchive archive;
	std::string failure;
	REQUIRE(ReadPxcx(Bytes(fixture.Bytes), archive, failure));
	CHECK(failure.empty());
	CHECK(archive.OriginalBytes == fixture.Bytes);
	CHECK(archive.MetadataNumber == 121092);
	CHECK(archive.MetadataText == "1.22.10.201");
	CHECK(archive.MetadataPayload.size() == 16);
	CHECK(archive.ThumbnailRgba.size() == PxcxLimits::ThumbnailRgbaBytes);
	CHECK(archive.ThumbnailRgba.front() == 23);
	CHECK(archive.ThumbnailRgba.back() == 23);
	CHECK(archive.GraphJson == std::string(GRAPH) + '\0');
	REQUIRE(archive.Nodes.size() == 2);
	CHECK((archive.Nodes[0] == PxcxNodeFact{"source", "image.solid", 1.0, -2.0}));
	CHECK((archive.Nodes[1] == PxcxNodeFact{"foreign-node", "vendor.unimplemented", 3.5, 4.0}));
	REQUIRE(archive.Links.size() == 1);
	CHECK((archive.Links[0] == PxcxLinkFact{"source", 1016, "foreign-node", 0}));
}

TEST_CASE("PXCX writer retains unchanged bytes and validates edited content", "[bake][pxcx]") {
	const PxcxFixture fixture = BuildFixture();
	PxcxArchive archive;
	std::string failure;
	REQUIRE(ReadPxcx(Bytes(fixture.Bytes), archive, failure));
	std::vector<std::byte> written;
	REQUIRE(WritePxcx(archive, written, failure));
	CHECK(written == fixture.Bytes);

	archive.MetadataNumber = 121093;
	archive.MetadataText = "1.21.10.203";
	REQUIRE(WritePxcx(archive, written, failure));
	PxcxArchive reread;
	REQUIRE(ReadPxcx(written, reread, failure));
	CHECK(reread.MetadataNumber == 121093);
	CHECK(reread.MetadataText == "1.21.10.203");
	CHECK(reread.GraphJson == archive.GraphJson);
	CHECK(reread.ThumbnailRgba == archive.ThumbnailRgba);

	archive.GraphJson =
		R"JSON({"nodes":[{"id":"only","type":"vendor.unknown","x":9,"y":-4,"inputs":[]}]})JSON";
	archive.GraphJson.push_back('\0');
	archive.Nodes = {PxcxNodeFact{"only", "vendor.unknown", 9, -4}};
	archive.Links.clear();
	REQUIRE(WritePxcx(archive, written, failure));
	REQUIRE(ReadPxcx(written, reread, failure));
	CHECK(reread.GraphJson == archive.GraphJson);
	CHECK(reread.Nodes == archive.Nodes);
	CHECK(reread.Links.empty());

	const std::vector<std::byte> beforeFailure = written;
	archive.GraphJson = R"JSON({"nodes":[]})JSON";
	archive.GraphJson.push_back('\0');
	CHECK_FALSE(WritePxcx(archive, written, failure));
	CHECK_FALSE(failure.empty());
	CHECK(written == beforeFailure);
}

TEST_CASE("PXCX optional thumbnail forms follow upstream save framing", "[bake][pxcx]") {
	for (const bool hasThumbnailBlock : {false, true}) {
		const PxcxFixture fixture = BuildFixture(GRAPH, true, {}, hasThumbnailBlock);
		PxcxArchive archive;
		std::string failure;
		REQUIRE(ReadPxcx(Bytes(fixture.Bytes), archive, failure));
		CHECK(archive.HasThumbnailBlock == hasThumbnailBlock);
		CHECK(archive.ThumbnailRgba.empty());
		std::vector<std::byte> written;
		REQUIRE(WritePxcx(archive, written, failure));
		CHECK(written == fixture.Bytes);
		archive.MetadataNumber++;
		REQUIRE(WritePxcx(archive, written, failure));
		PxcxArchive reread;
		REQUIRE(ReadPxcx(written, reread, failure));
		CHECK(reread.HasThumbnailBlock == hasThumbnailBlock);
		CHECK(reread.ThumbnailRgba.empty());
	}
}

TEST_CASE("PXCX reader preserves unknown version metadata", "[bake][pxcx]") {
	constexpr uint32_t unknownVersionNumber = UINT32_MAX;
	constexpr std::string_view unknownVersionText = "999.888.777.666";
	const PxcxFixture fixture =
		BuildFixture(GRAPH, true, {}, false, unknownVersionNumber, unknownVersionText);
	PxcxArchive archive;
	std::string failure;
	REQUIRE(ReadPxcx(Bytes(fixture.Bytes), archive, failure));
	CHECK(failure.empty());
	CHECK(archive.MetadataNumber == unknownVersionNumber);
	CHECK(archive.MetadataText == unknownVersionText);
	CHECK(archive.MetadataPayload.size() == sizeof(unknownVersionNumber) + unknownVersionText.size() + 1);

	std::vector<std::byte> written;
	REQUIRE(WritePxcx(archive, written, failure));
	CHECK(written == fixture.Bytes);
}

TEST_CASE("PXCX reader keeps unsupported node types opaque", "[bake][pxcx]") {
	constexpr std::string_view graph =
		R"JSON({"nodes":[{"id":"future-source","type":"vendor.texture-source-v99","x":0,"y":1,"inputs":[]},{"id":"future-effect","type":"vendor.color-grade-v9","x":2,"y":3,"inputs":[{"from_node":"future-source","from_index":999}]}]})JSON";
	const PxcxFixture fixture = BuildFixture(graph, true, {}, false);
	PxcxArchive archive;
	std::string failure;
	REQUIRE(ReadPxcx(Bytes(fixture.Bytes), archive, failure));
	REQUIRE(archive.Nodes.size() == 2);
	CHECK(archive.Nodes[0].Type == "vendor.texture-source-v99");
	CHECK(archive.Nodes[1].Type == "vendor.color-grade-v9");
	REQUIRE(archive.Links.size() == 1);
	CHECK((archive.Links[0] == PxcxLinkFact{"future-source", 999, "future-effect", 0}));

	std::vector<std::byte> written;
	REQUIRE(WritePxcx(archive, written, failure));
	CHECK(written == fixture.Bytes);
}

TEST_CASE("PXCX external projects preserve bytes through the native reader and writer", "[bake][pxcx]") {
	const char *directory = std::getenv("PXCX_EXTERNAL_FIXTURE_DIR");
	if (directory == nullptr || *directory == '\0') {
		SUCCEED("set PXCX_EXTERNAL_FIXTURE_DIR to run external project acceptance");
		return;
	}

	struct ExpectedProject {
		std::string_view Name;
		size_t Nodes;
		size_t Links;
	};
	constexpr std::array<ExpectedProject, 5> projects{{
		{"Black-Hole_121092.pxc", 67, 80},
		{"Fire-Tornado_121092.pxc", 32, 31},
		{"Glass-Block-Refraction_121092.pxc", 40, 44},
		{"Ornate-Trim_121092.pxc", 27, 28},
		{"Spark-Bolt_121092.pxc", 44, 51},
	}};
	for (const ExpectedProject &project : projects) {
		INFO(project.Name);
		const std::filesystem::path path = std::filesystem::path(directory) / project.Name;
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		REQUIRE(input.is_open());
		const std::streamsize byteCount = input.tellg();
		REQUIRE(byteCount > 0);
		REQUIRE(static_cast<uint64_t>(byteCount) <= PxcxLimits::MaximumArchiveBytes);
		std::vector<std::byte> bytes(static_cast<size_t>(byteCount));
		input.seekg(0);
		input.read(reinterpret_cast<char *>(bytes.data()), byteCount);
		REQUIRE(input.gcount() == byteCount);

		PxcxArchive archive;
		std::string failure;
		REQUIRE(ReadPxcx(bytes, archive, failure));
		CHECK(archive.Nodes.size() == project.Nodes);
		CHECK(archive.Links.size() == project.Links);
		CHECK(archive.MetadataNumber == 121092);
		CHECK(archive.MetadataText == "1.22.10.201");
		std::vector<std::byte> written;
		REQUIRE(WritePxcx(archive, written, failure));
		CHECK(written == bytes);
	}
}

TEST_CASE("PXCX reader rejects malformed framing and hostile lengths", "[bake][pxcx]") {
	const PxcxFixture fixture = BuildFixture();
	std::vector<std::byte> damaged = fixture.Bytes;
	damaged.resize(7);
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	damaged[0] = std::byte{'X'};
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	PatchUInt32(damaged, 4, 15);
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	damaged[8] = std::byte{'X'};
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	PatchUInt32(damaged, 12, UINT32_MAX);
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	damaged[fixture.MetadataOffset] = std::byte{'X'};
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	PatchUInt32(damaged, fixture.MetadataOffset + 4, UINT32_MAX);
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	damaged[fixture.MetadataPayloadOffset + 15] = std::byte{'X'};
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	damaged[16] ^= std::byte{0x40};
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	damaged.pop_back();
	CHECK(Refuses(damaged));

	damaged = fixture.Bytes;
	AppendByte(damaged, 0);
	CHECK(Refuses(damaged));
}

TEST_CASE("PXCX reader refuses malformed JSON and unvalidated topology", "[bake][pxcx]") {
	PxcxArchive archive;
	std::string failure;

	CHECK(Refuses(BuildFixture(R"JSON({"nodes":[])JSON").Bytes));
	CHECK(Refuses(BuildFixture(R"JSON({"nodes":[]} )JSON", false).Bytes));
	CHECK(Refuses(BuildFixture(R"JSON({"nodes":[],"nodes":[]})JSON").Bytes));
	CHECK(Refuses(
		BuildFixture(
			R"JSON({"nodes":[{"id":"same","type":"a","x":0,"y":0,"inputs":[]},{"id":"same","type":"b","x":0,"y":0,"inputs":[]}]})JSON"
		)
			.Bytes
	));
	CHECK(Refuses(
		BuildFixture(
			R"JSON({"nodes":[{"id":"sink","type":"vendor.unknown","x":0,"y":0,"inputs":[{"from_node":"gone","from_index":0}]}]})JSON"
		)
			.Bytes
	));
	CHECK(Refuses(
		BuildFixture(
			R"JSON({"nodes":[{"id":"sink","type":"vendor.unknown","x":0,"y":0,"inputs":[{"from_node":"source"}]}]})JSON"
		)
			.Bytes
	));
	CHECK(
		Refuses(BuildFixture(
					R"JSON({"nodes":[{"id":"bad","type":"vendor.unknown","x":1e9999,"y":0,"inputs":[]}]})JSON"
		)
					.Bytes)
	);

	std::string deep = R"JSON({"nested":)JSON";
	deep.append(PxcxLimits::MaximumJsonDepth + 1, '[');
	deep.push_back('0');
	deep.append(PxcxLimits::MaximumJsonDepth + 1, ']');
	deep += R"JSON(,"nodes":[]})JSON";
	CHECK(Refuses(BuildFixture(deep).Bytes));

	std::vector<uint8_t> oversizedThumbnail(PxcxLimits::ThumbnailRgbaBytes + 1, 23);
	CHECK(Refuses(BuildFixture(GRAPH, true, std::move(oversizedThumbnail)).Bytes));
}

TEST_CASE("PXCX common source selectors retain exact tags and trigger sentinel", "[bake][pxcx]") {
	const std::string graph =
		R"JSON({"nodes":[{"id":"source","type":"vendor.unknown","x":0,"y":0,"inputs":[]},{"id":"sink","type":"vendor.unknown","x":1,"y":2,"inputs":[{"from_node":"source","from_index":-1,"from_tag":-2},{"from_node":"source","from_index":-1,"from_tag":-3},{"from_node":"source","from_index":1,"from_tag":-4},{"from_node":"source","from_index":4294967295},{"from_node":"source","from_index":0,"from_tag":0},{"from_node":"source","from_index":7,"from_tag":9223372036854775807},{"from_node":"source","from_index":5,"from_tag":-3},{"from_node":"source","from_index":-1,"from_tag":-3.0},{"from_node":"source","from_index":0,"from_tag":0.0},{"from_node":"source","from_index":2,"from_tag":9000000000000000.0},{"from_node":"source","from_index":0,"from_tag":9007199254740992.0},{"from_node":"source","from_index":0,"from_tag":9000000000000001.0},{"from_node":"source","from_index":0,"from_tag":-9223372036854775808.0}]}]})JSON";
	const PxcxFixture fixture = BuildFixture(graph);
	PxcxArchive archive;
	std::string failure;
	REQUIRE(ReadPxcx(fixture.Bytes, archive, failure));
	REQUIRE(archive.Links.size() == 13);
	CHECK((archive.Links[0] == PxcxLinkFact{"source", 0, "sink", 0, -2, true}));
	CHECK((archive.Links[1] == PxcxLinkFact{"source", 0, "sink", 1, -3, true}));
	CHECK((archive.Links[2] == PxcxLinkFact{"source", 1, "sink", 2, -4, false}));
	CHECK((archive.Links[3] == PxcxLinkFact{"source", UINT32_MAX, "sink", 3}));
	CHECK((archive.Links[4] == PxcxLinkFact{"source", 0, "sink", 4, 0, false}));
	CHECK((archive.Links[5] == PxcxLinkFact{"source", 7, "sink", 5, INT64_MAX, false}));
	CHECK((archive.Links[6] == PxcxLinkFact{"source", 5, "sink", 6, -3, false}));
	CHECK((archive.Links[7] == PxcxLinkFact{"source", 0, "sink", 7, -3, true}));
	CHECK((archive.Links[8] == PxcxLinkFact{"source", 0, "sink", 8, 0, false}));
	CHECK((archive.Links[9] == PxcxLinkFact{"source", 2, "sink", 9, 9000000000000000ll, false}));
	CHECK((archive.Links[10] == PxcxLinkFact{"source", 0, "sink", 10, 9007199254740992ll, false}));
	CHECK((archive.Links[11] == PxcxLinkFact{"source", 0, "sink", 11, 9000000000000001ll, false}));
	CHECK((archive.Links[12] == PxcxLinkFact{"source", 0, "sink", 12, INT64_MIN, false}));
	std::vector<std::byte> written;
	REQUIRE(WritePxcx(archive, written, failure));
	CHECK(written == fixture.Bytes);
	archive.MetadataNumber++;
	REQUIRE(WritePxcx(archive, written, failure));
	PxcxArchive reread;
	REQUIRE(ReadPxcx(written, reread, failure));
	CHECK(reread.Links == archive.Links);
	CHECK(reread.GraphJson == archive.GraphJson);
	const auto before = written;
	archive.Links[0].SourceTriggerIndexMinusOne = false;
	CHECK_FALSE(WritePxcx(archive, written, failure));
	CHECK(written == before);
}

TEST_CASE("PXCX source selector rejects malformed tags and invalid negative indices", "[bake][pxcx]") {
	for (const std::string_view fields :
		 {"\"from_index\":-1",
		  "\"from_index\":-1,\"from_tag\":0",
		  "\"from_index\":-1,\"from_tag\":-4",
		  "\"from_index\":-2,\"from_tag\":-3",
		  "\"from_index\":4294967296,\"from_tag\":-3",
		  "\"from_index\":-1.0,\"from_tag\":-3",
		  "\"from_index\":0,\"from_tag\":null",
		  "\"from_index\":0,\"from_tag\":true",
		  "\"from_index\":0,\"from_tag\":\"-3\"",
		  "\"from_index\":0,\"from_tag\":-3.5",
		  "\"from_index\":0,\"from_tag\":9223372036854775808",
		  "\"from_index\":0,\"from_tag\":9223372036854775808.0",
		  "\"from_index\":0,\"from_tag\":-9223372036854777856.0"}) {
		INFO(fields);
		std::string graph =
			R"JSON({"nodes":[{"id":"source","type":"vendor.unknown","x":0,"y":0,"inputs":[]},{"id":"sink","type":"vendor.unknown","x":0,"y":0,"inputs":[{"from_node":"source",)JSON";
		graph += fields;
		graph += "}]}]}";
		CHECK(Refuses(BuildFixture(graph).Bytes));
	}
}

TEST_CASE("PXCX common Update destination stays distinct from positional input two", "[bake][pxcx]") {
	const auto fixture = BuildFixture(
		R"JSON({"nodes":[{"id":"source","type":"vendor.unknown","x":0,"y":0,"inputs":[]},{"id":"sink","type":"vendor.unknown","x":0,"y":0,"inputs":[{},{},{"from_node":"source","from_index":0}],"inspectInputs":[{},{},{"from_node":"source","from_index":-1,"from_tag":-3},{},{}]}]})JSON"
	);
	PxcxArchive archive;
	std::string failure;
	REQUIRE(ReadPxcx(fixture.Bytes, archive, failure));
	REQUIRE(archive.Links.size() == 2);
	CHECK((archive.Links[0] == PxcxLinkFact{"source", 0, "sink", 2}));
	CHECK((archive.Links[1] == PxcxLinkFact{"source", 0, "sink", 2, -3, true, true}));
	std::vector<std::byte> written;
	REQUIRE(WritePxcx(archive, written, failure));
	CHECK(written == fixture.Bytes);
	archive.MetadataNumber++;
	REQUIRE(WritePxcx(archive, written, failure));
	PxcxArchive reopened;
	REQUIRE(ReadPxcx(written, reopened, failure));
	CHECK(reopened.Links == archive.Links);
	const auto before = written;
	archive.Links[1].DestinationUpdateTrigger = false;
	CHECK_FALSE(WritePxcx(archive, written, failure));
	CHECK(written == before);
}

TEST_CASE(
	"PXCX common Update records validate linked endpoints without reading other inspector sockets",
	"[bake][pxcx]"
) {
	for (const std::string_view inspector : {"[]", "[{},{}]", "[{},{},{}]", "[{},{},{},null]"}) {
		INFO(inspector);
		std::string graph =
			R"JSON({"nodes":[{"id":"sink","type":"vendor.unknown","x":0,"y":0,"inputs":[],"inspectInputs":)JSON";
		graph += inspector;
		graph += "}]}";
		PxcxArchive archive;
		std::string failure;
		REQUIRE(ReadPxcx(BuildFixture(graph).Bytes, archive, failure));
		CHECK(archive.Links.empty());
	}
	for (const std::string_view inspector :
		 {"null",
		  "{}",
		  "[{},{},null]",
		  "[{},{},{\"from_node\":\"source\"}]",
		  "[{},{},{\"from_index\":0}]",
		  "[{},{},{\"from_node\":\"missing\",\"from_index\":0}]",
		  "[{},{},{\"from_node\":\"source\",\"from_index\":-1}]"}) {
		INFO(inspector);
		std::string graph =
			R"JSON({"nodes":[{"id":"source","type":"vendor.unknown","x":0,"y":0,"inputs":[]},{"id":"sink","type":"vendor.unknown","x":0,"y":0,"inputs":[],"inspectInputs":)JSON";
		graph += inspector;
		graph += "}]}";
		CHECK(Refuses(BuildFixture(graph).Bytes));
	}
}
