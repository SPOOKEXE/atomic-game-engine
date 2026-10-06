#include <engine/bake/Image.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/PxcxCollectionFiles.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <miniz.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphexport.pxcx_collection_files")
TEST_DEPENDS("engine.bake.image")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	using engine::imagegraphio::PxcxCollectionSave;

	Image Preview() {
		Image image;
		image.Width = 3;
		image.Height = 2;
		image.Format = SurfaceFormat::RGBA8Unorm;
		image.Pixels = {1,	2,	3,	4,	11, 12, 13, 14, 21, 22, 23, 24,
						31, 32, 33, 34, 41, 42, 43, 44, 51, 52, 53, 54};
		image.Hash = SurfaceHash(image);
		return image;
	}

	std::map<std::string, std::vector<std::byte>> Entries(std::span<const std::byte> package) {
		mz_zip_archive zip{};
		REQUIRE(mz_zip_reader_init_mem(&zip, package.data(), package.size(), 0));
		std::map<std::string, std::vector<std::byte>> result;
		for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); ++index) {
			const mz_uint filenameLength = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
			REQUIRE(filenameLength > 1);
			std::vector<char> filename(filenameLength);
			REQUIRE(
				mz_zip_reader_get_filename(&zip, index, filename.data(), filenameLength) == filenameLength
			);
			size_t byteCount = 0;
			auto *bytes = static_cast<std::byte *>(mz_zip_reader_extract_to_heap(&zip, index, &byteCount, 0));
			REQUIRE(bytes != nullptr);
			result.emplace(
				std::string(filename.data(), filenameLength - 1),
				std::vector<std::byte>(bytes, bytes + byteCount)
			);
			mz_free(bytes);
		}
		REQUIRE(mz_zip_reader_end(&zip));
		return result;
	}

	std::vector<std::byte> Package(
		std::string_view baseName,
		std::span<const std::byte> preview,
		std::optional<std::string> metadata = std::string{"{\"keep\":true}"}
	) {
		PxcxCollectionSave save;
		save.GraphJson = "{\"nodes\":[{\"id\":\"kept\"}]}";
		save.MetadataJson = std::move(metadata);
		std::vector<std::byte> package;
		Diagnostic diagnostic;
		const bool written = WritePxcxCollectionPackage(baseName, save, preview, package, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		return package;
	}
}

TEST_CASE("Collection preview PNG keeps full RGBA8 dimensions and pixels", "[imagegraphexport][pxcc]") {
	const auto source = Preview();
	std::vector<std::byte> png;
	Diagnostic diagnostic;
	REQUIRE(WritePxcxCollectionPreview(source, png, diagnostic));
	engine::assets::TextureData decoded;
	std::string failure;
	REQUIRE(engine::bake::ReadImage(png, decoded, failure));
	CHECK(decoded.Width == source.Width);
	CHECK(decoded.Height == source.Height);
	REQUIRE(decoded.Pixels.size() == source.Pixels.size());
	for (size_t index = 0; index < source.Pixels.size(); ++index)
		CHECK(std::to_integer<uint8_t>(decoded.Pixels[index]) == source.Pixels[index]);
}

TEST_CASE(
	"Collection package includes optional preview and metadata beside its graph", "[imagegraphexport][pxcc]"
) {
	const auto source = Preview();
	std::vector<std::byte> png;
	Diagnostic diagnostic;
	REQUIRE(WritePxcxCollectionPreview(source, png, diagnostic));
	const auto entries = Entries(Package("雪", png));
	REQUIRE(entries.size() == 3);
	CHECK(
		std::string(
			reinterpret_cast<const char *>(entries.at("雪.pxcc").data()), entries.at("雪.pxcc").size()
		) == "{\"nodes\":[{\"id\":\"kept\"}]}"
	);
	CHECK(entries.contains("雪.png"));
	CHECK(entries.contains("雪.meta"));
	CHECK(
		std::string(
			reinterpret_cast<const char *>(entries.at("雪.meta").data()), entries.at("雪.meta").size()
		) == "{\"keep\":true}"
	);
	const auto minimal = Entries(Package("bare", {}, std::nullopt));
	REQUIRE(minimal.size() == 1);
	CHECK(minimal.contains("bare.pxcc"));
}

TEST_CASE("Collection PNG converts finite float samples without cropping", "[imagegraphexport][pxcc]") {
	Image image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	const std::array<float, 4> samples{-.2f, .5f, 1.2f, .25f};
	std::memcpy(image.Pixels.data(), samples.data(), image.Pixels.size());
	std::vector<std::byte> png;
	Diagnostic diagnostic;
	REQUIRE(WritePxcxCollectionPreview(image, png, diagnostic));
	engine::assets::TextureData decoded;
	std::string failure;
	REQUIRE(engine::bake::ReadImage(png, decoded, failure));
	CHECK(decoded.Width == 1);
	CHECK(decoded.Height == 1);
	CHECK(
		decoded.Pixels == std::vector<std::byte>{std::byte{0}, std::byte{128}, std::byte{255}, std::byte{64}}
	);
}

TEST_CASE(
	"Collection file writers refuse invalid data without replacing output", "[imagegraphexport][pxcc]"
) {
	const auto image = Preview();
	const auto baseline = std::vector<std::byte>{std::byte{7}, std::byte{9}};
	Diagnostic diagnostic;
	SECTION("invalid dimensions") {
		auto invalid = image;
		invalid.Width = 0;
		std::vector<std::byte> output = baseline;
		CHECK_FALSE(WritePxcxCollectionPreview(invalid, output, diagnostic));
		CHECK(output == baseline);
	}
	SECTION("nonfinite samples") {
		auto invalid = image;
		invalid.Format = SurfaceFormat::RGBA32Float;
		invalid.Width = invalid.Height = 1;
		invalid.Pixels.resize(4 * sizeof(float));
		const float sample = std::numeric_limits<float>::infinity();
		std::memcpy(invalid.Pixels.data(), &sample, sizeof(sample));
		std::vector<std::byte> output = baseline;
		CHECK_FALSE(WritePxcxCollectionPreview(invalid, output, diagnostic));
		CHECK(output == baseline);
	}
	SECTION("preview operation limit") {
		std::vector<std::byte> output = baseline;
		CHECK_FALSE(WritePxcxCollectionPreview(image, output, diagnostic, 1));
		CHECK(output == baseline);
	}
	SECTION("empty graph") {
		PxcxCollectionSave save;
		std::vector<std::byte> output = baseline;
		CHECK_FALSE(WritePxcxCollectionPackage("asset", save, {}, output, diagnostic));
		CHECK(output == baseline);
	}
	SECTION("bad preview PNG") {
		PxcxCollectionSave save;
		save.GraphJson = "{}";
		const std::array<std::byte, 3> invalid{std::byte{1}, std::byte{2}, std::byte{3}};
		std::vector<std::byte> output = baseline;
		CHECK_FALSE(WritePxcxCollectionPackage("asset", save, invalid, output, diagnostic));
		CHECK(output == baseline);
	}
	SECTION("invalid name and package limit") {
		PxcxCollectionSave save;
		save.GraphJson = "{}";
		std::vector<std::byte> output = baseline;
		for (const auto name : {"", ".", "..", "a/b", "a\\b", "bad\nname"}) {
			CHECK_FALSE(WritePxcxCollectionPackage(name, save, {}, output, diagnostic));
			CHECK(output == baseline);
		}
		const std::string longName(1025, 'x');
		CHECK_FALSE(WritePxcxCollectionPackage(longName, save, {}, output, diagnostic));
		CHECK(output == baseline);
		CHECK_FALSE(WritePxcxCollectionPackage("asset", save, {}, output, diagnostic, 1));
		CHECK(output == baseline);
	}
}
