#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/PxcxThumbnail.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>

TEST_SUITE_ID("engine.imagegraphexport.pxcx_thumbnail")
TEST_DEPENDS("engine.imagegraphio.pxcxedit")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	engine::bake::PxcxArchive Source() {
		engine::bake::PxcxArchive source;
		source.HasThumbnailBlock = true;
		source.ThumbnailRgba.assign(engine::bake::PxcxLimits::ThumbnailRgbaBytes, 17);
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201 preserved text";
		source.GraphJson =
			R"({"future":{"keep":[1,"opaque"]},"nodes":[{"id":"number","type":"Node_Number_Simple","x":1,"y":2,"unknown":{"keep":17},"inputs":[{"r":{"d":2},"unknown_input":8}]}]})";
		source.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		return checked;
	}
	Image Preview(uint32_t width = 4, uint32_t height = 2) {
		Image image;
		image.Width = width;
		image.Height = height;
		image.Pixels.resize(size_t(width) * height * 4);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const SurfacePixel pixel{
					double(20 + x * 40) / 255, double(30 + y * 80) / 255, 70.0 / 255, 100.0 / 255
				};
				REQUIRE(StoreSurfacePixel(image, x, y, pixel));
			}
		image.Hash = SurfaceHash(image);
		return image;
	}
	void Pixel(const engine::bake::PxcxArchive &archive, uint32_t x, uint32_t y, uint8_t red, uint8_t green) {
		const size_t offset = (size_t(y) * 256 + x) * 4;
		CHECK(archive.ThumbnailRgba[offset] == red);
		CHECK(archive.ThumbnailRgba[offset + 1] == green);
		CHECK(archive.ThumbnailRgba[offset + 2] == 70);
		CHECK(archive.ThumbnailRgba[offset + 3] == 100);
	}
}
TEST_CASE(
	"PXC thumbnail uses source cover crop geometry with independent nearest pixel goldens",
	"[imagegraph][pxc-thumbnail]"
) {
	auto source = Source();
	Image image;
	SECTION("landscape center crop") {
		image = Preview();
	}
	SECTION("portrait center crop") {
		image = Preview(2, 4);
	}
	SECTION("odd centered offset") {
		image = Preview(5, 2);
	}
	Diagnostic diagnostic;
	std::vector<std::byte> written;
	REQUIRE(WritePxcxPreparedThumbnail(source, image, written, diagnostic));
	engine::bake::PxcxArchive result;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(written, result, failure));
	CHECK(result.GraphJson == source.GraphJson);
	CHECK(result.MetadataPayload == source.MetadataPayload);
	CHECK(result.Nodes == source.Nodes);
	CHECK(result.Links == source.Links);
	REQUIRE(result.ThumbnailRgba.size() == 256 * 256 * 4);
	if (image.Width == 4) {
		Pixel(result, 0, 0, 60, 30);
		Pixel(result, 127, 127, 60, 30);
		Pixel(result, 128, 128, 100, 110);
		Pixel(result, 255, 255, 100, 110);
	} else if (image.Height == 4) {
		Pixel(result, 0, 0, 20, 110);
		Pixel(result, 127, 127, 20, 110);
		Pixel(result, 128, 128, 60, 190);
		Pixel(result, 255, 255, 60, 190);
	} else {
		Pixel(result, 0, 0, 60, 30);
		Pixel(result, 63, 127, 60, 30);
		Pixel(result, 64, 128, 100, 110);
		Pixel(result, 191, 255, 100, 110);
		Pixel(result, 192, 255, 140, 110);
		Pixel(result, 255, 255, 140, 110);
	}
	CHECK(source.ThumbnailRgba.front() == 17);
}
TEST_CASE(
	"explicit PXC thumbnail update follows checked projection edit and reload adoption",
	"[imagegraph][pxc-thumbnail]"
) {
	auto source = Source();
	engine::imagegraphio::PxcxImport imported;
	std::string failure;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(source, imported, failure));
	Diagnostic diagnostic;
	std::vector<std::byte> candidate;
	REQUIRE(engine::imagegraphio::WritePxcxProjection(imported, imported.Graph, {}, candidate, diagnostic));
	CHECK(candidate == source.OriginalBytes);
	auto edited = imported.Graph;
	edited.Nodes[0].Position.X = 37;
	REQUIRE(engine::imagegraphio::WritePxcxProjection(imported, edited, {}, candidate, diagnostic));
	engine::bake::PxcxArchive checked;
	REQUIRE(engine::bake::ReadPxcx(candidate, checked, failure));
	std::vector<std::byte> published;
	REQUIRE(WritePxcxPreparedThumbnail(checked, Preview(), published, diagnostic));
	engine::bake::PxcxArchive reloaded;
	REQUIRE(engine::bake::ReadPxcx(published, reloaded, failure));
	CHECK(reloaded.GraphJson == checked.GraphJson);
	CHECK(reloaded.MetadataPayload == source.MetadataPayload);
	engine::imagegraphio::PxcxImport adopted;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(reloaded, adopted, failure));
	CHECK(adopted.Graph == edited);
	REQUIRE(adopted.ReferencePreview());
	CHECK(adopted.ReferencePreview()->Rgba.front() == 60);
	std::vector<std::byte> again;
	REQUIRE(engine::imagegraphio::WritePxcxProjection(adopted, adopted.Graph, {}, again, diagnostic));
	CHECK(again == published);
	REQUIRE(WritePxcxPreparedThumbnail(reloaded, Preview(), again, diagnostic));
	CHECK(again == published);
}
TEST_CASE(
	"PXC thumbnail conversion and aggregate budget refusals preserve prior output",
	"[imagegraph][pxc-thumbnail]"
) {
	auto source = Source();
	auto preview = Preview();
	std::vector<std::byte> written{std::byte{42}};
	const auto prior = written;
	Diagnostic diagnostic;
	SECTION("nonfinite input") {
		preview.Format = SurfaceFormat::RGBA32Float;
		preview.Width = preview.Height = 1;
		preview.Pixels.resize(16);
		const float invalid = std::numeric_limits<float>::infinity();
		std::memcpy(preview.Pixels.data(), &invalid, sizeof(invalid));
		CHECK_FALSE(WritePxcxPreparedThumbnail(source, preview, written, diagnostic));
		CHECK(diagnostic.Code == Status::InvalidValue);
	}
	SECTION("invalid layout") {
		preview.Width = 0;
		CHECK_FALSE(WritePxcxPreparedThumbnail(source, preview, written, diagnostic));
		CHECK(diagnostic.Code == Status::InvalidValue);
	}
	SECTION("tight operation") {
		CHECK_FALSE(WritePxcxPreparedThumbnail(source, preview, written, diagnostic, 1024));
		CHECK(diagnostic.Code == Status::LimitExceeded);
	}
	SECTION("retained prior capacity") {
		std::vector<std::byte> independent;
		REQUIRE(WritePxcxPreparedThumbnail(source, preview, independent, diagnostic, 107 * 1024 * 1024));
		written.reserve(8 * 1024 * 1024);
		CHECK_FALSE(WritePxcxPreparedThumbnail(source, preview, written, diagnostic, 107 * 1024 * 1024));
		CHECK(diagnostic.Code == Status::LimitExceeded);
	}
	SECTION("borrowed archive spare backing is charged") {
		source.GraphJson.reserve(8 * 1024 * 1024);
		CHECK_FALSE(WritePxcxPreparedThumbnail(source, preview, written, diagnostic));
		CHECK(diagnostic.Code == Status::LimitExceeded);
	}
	SECTION("borrowed preview spare backing is charged") {
		std::vector<std::byte> independent;
		REQUIRE(WritePxcxPreparedThumbnail(source, preview, independent, diagnostic, 107 * 1024 * 1024));
		preview.Pixels.reserve(8 * 1024 * 1024);
		CHECK_FALSE(WritePxcxPreparedThumbnail(source, preview, written, diagnostic, 107 * 1024 * 1024));
		CHECK(diagnostic.Code == Status::LimitExceeded);
	}
	SECTION("unserialized source") {
		source.OriginalBytes.clear();
		CHECK_FALSE(WritePxcxPreparedThumbnail(source, preview, written, diagnostic));
		CHECK(diagnostic.Code == Status::InvalidValue);
	}
	SECTION("stale checked META identity") {
		source.MetadataPayload[0] ^= std::byte{1};
		CHECK_FALSE(WritePxcxPreparedThumbnail(source, preview, written, diagnostic));
		CHECK(diagnostic.Code == Status::InvalidValue);
	}
	SECTION("HDR straight RGBA8 native conversion") {
		preview.Format = SurfaceFormat::RGBA32Float;
		preview.Width = preview.Height = 1;
		preview.Pixels.resize(16);
		REQUIRE(StoreSurfacePixel(preview, 0, 0, {2, -1, 1, .5}));
		REQUIRE(WritePxcxPreparedThumbnail(source, preview, written, diagnostic));
		engine::bake::PxcxArchive result;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(written, result, failure));
		CHECK(result.ThumbnailRgba[0] == 255);
		CHECK(result.ThumbnailRgba[1] == 0);
		CHECK(result.ThumbnailRgba[2] == 255);
		CHECK(result.ThumbnailRgba[3] == 128);
		return;
	}
	CHECK(written == prior);
}
