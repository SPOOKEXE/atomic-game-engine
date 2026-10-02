#include "AtlasPayload.hpp"
#include "SourceAtlasCodec.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <sstream>

TEST_SUITE_ID("engine.imagegraph.source-atlas")
using namespace engine::imagegraph;
namespace {
	AtlasValue SampleAtlas() {
		AtlasValue value;
		auto &data = value.Data.emplace();
		data.Surface.Data = {1, 1, {12, 34, 56, 78}, 991};
		data.Position = {-2.5, 10};
		data.Scale = {-1, 3};
		data.Dimension = {1, 1};
		data.RotationDegrees = 37;
		data.Alpha = .75;
		data.Blend = {10, 20, 30, 40};
		data.OriginalSurface = SurfaceValue{Image{2, 1, {1, 2, 3, 4, 5, 6, 7, 8}, 992}};
		data.OriginalDimension = {2, 1};
		return value;
	}
}
TEST_CASE("Source Atlas keeps transform and original pixels in an independent owned copy", "[atlas]") {
	const auto original = SampleAtlas();
	REQUIRE(detail::ValidAtlasPayload(original));
	auto copy = original;
	copy.Data->Surface.Data.Pixels[0] = 200;
	copy.Data->OriginalSurface->Data.Pixels[0] = 201;
	copy.Data->Position.X = 25;
	CHECK(original.Data->Surface.Data.Pixels[0] == 12);
	CHECK(original.Data->OriginalSurface->Data.Pixels[0] == 1);
	CHECK(original.Data->Position.X == -2.5);
	CHECK(detail::AtlasStorageBytes(original, false) == sizeof(AtlasData) + 12);
	CHECK(detail::AtlasStorageBytes(original, true) >= detail::AtlasStorageBytes(original, false));
}
TEST_CASE(
	"Source Atlas codec preserves every draw field and admits both images before allocation", "[atlas]"
) {
	const auto original = SampleAtlas();
	std::ostringstream encoded;
	detail::WriteAtlasValue(encoded, original);
	AtlasValue decoded;
	uint64_t bytes = 0;
	const auto admit = [&](uint64_t size) {
		bytes += size;
		return true;
	};
	std::istringstream stream(encoded.str());
	REQUIRE(detail::ReadAtlasValue(stream, decoded, admit));
	CHECK(decoded == original);
	CHECK(bytes == sizeof(AtlasData) + 12);
	stream >> std::ws;
	CHECK(stream.eof());
	AtlasValue retained = original;
	std::istringstream refused(encoded.str());
	uint64_t budget = sizeof(AtlasData) + 4;
	CHECK_FALSE(detail::ReadAtlasValue(refused, retained, [&](uint64_t size) {
		if (size > budget) return false;
		budget -= size;
		return true;
	}));
	CHECK(retained == original);
}
TEST_CASE("Source Atlas rejects invalid payloads and malformed images atomically", "[atlas]") {
	auto value = SampleAtlas();
	value.Data->Position.X = std::numeric_limits<double>::infinity();
	CHECK_FALSE(detail::ValidAtlasPayload(value));
	value = SampleAtlas();
	value.Data->Surface.Data.Pixels.pop_back();
	CHECK_FALSE(detail::ValidAtlasPayload(value));
	const auto original = SampleAtlas();
	std::ostringstream encoded;
	detail::WriteAtlasValue(encoded, original);
	auto text = encoded.str();
	const auto pixel = text.find("0c22384e");
	REQUIRE(pixel != std::string::npos);
	text[pixel] = 'z';
	std::istringstream malformed(text);
	AtlasValue retained = original;
	CHECK_FALSE(detail::ReadAtlasValue(malformed, retained, [](uint64_t) { return true; }));
	CHECK(retained == original);
	AtlasValue empty;
	empty.Data.emplace();
	REQUIRE(detail::ValidAtlasPayload(empty));
	std::ostringstream emptyText;
	detail::WriteAtlasValue(emptyText, empty);
	std::istringstream emptyStream(emptyText.str());
	CHECK(detail::ReadAtlasValue(emptyStream, retained, [](uint64_t) { return true; }));
	CHECK(retained == empty);
}
