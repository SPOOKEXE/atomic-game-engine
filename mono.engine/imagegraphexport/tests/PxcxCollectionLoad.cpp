#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/PxcxCollectionFiles.hpp>
#include <engine/imagegraphexport/PxcxCollectionLoad.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.imagegraphexport.pxcx_collection_load")
TEST_DEPENDS("engine.imagegraphio.pxcx_collection_load")
TEST_DEPENDS("engine.imagegraphexport.pxcx_collection_files")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	using engine::imagegraphio::PxcxCollectionSave;

	struct Temporary {
		std::filesystem::path Root =
			std::filesystem::temp_directory_path() /
			("atomic-pxcx-load-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Temporary() {
			std::filesystem::create_directory(Root);
		}
		~Temporary() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	};

	constexpr std::string_view Graph =
		R"JSON({"version":121092,"versionStr":"1.21.10.203","animator":{"frames_total":99,"framerate":24,"playback":1},"nodes":[{"id":"root","type":"Node_Collection","x":0,"y":0,"inputs":[],"attri":{"custom_input_list":[],"custom_output_list":[]}}]})JSON";
	constexpr std::string_view Metadata = R"JSON({"keep":true})JSON";
	const TimelineSettings Timeline{.Frames = 3, .Last = 2, .Playback = "loop", .FramesPerSecond = 24.0};
	void Write(const std::filesystem::path &path, std::span<const std::byte> bytes) {
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
	}

	void Write(const std::filesystem::path &path, std::string_view text) {
		Write(path, std::as_bytes(std::span(text.data(), text.size())));
	}

	std::vector<std::byte>
	Zip(const std::vector<std::pair<std::string, std::vector<std::byte>>> &entries,
		int compression = MZ_DEFAULT_COMPRESSION) {
		mz_zip_archive zip{};
		REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
		for (const auto &[name, bytes] : entries)
			REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(), compression));
		void *output = nullptr;
		size_t size = 0;
		REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &output, &size));
		std::vector<std::byte> result(
			static_cast<std::byte *>(output), static_cast<std::byte *>(output) + size
		);
		mz_free(output);
		REQUIRE(mz_zip_writer_end(&zip));
		return result;
	}

	std::vector<std::byte> Bytes(std::string_view text) {
		const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
		return {bytes.begin(), bytes.end()};
	}

	bool Read(
		const std::filesystem::path &path,
		PxcxSourceRead &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes = Limits::MaximumEvaluationBytes,
		bool readPreview = true
	) {
		return ReadPxcxSourceFile(path, Timeline, result, diagnostic, maximumBytes, readPreview);
	}
}

TEST_CASE("Raw Collection files load graph metadata and full preview", "[imagegraphexport][pxcc]") {
	Temporary temporary;
	const auto graphPath = temporary.Root / "scene.pxcc";
	Write(graphPath, Graph);
	Write(temporary.Root / "scene.meta", Metadata);
	Image preview{1, 1, {255, 0, 0, 255}, 0, SurfaceFormat::RGBA8Unorm};
	std::vector<std::byte> png;
	Diagnostic diagnostic;
	REQUIRE(WritePxcxCollectionPreview(preview, png, diagnostic));
	Write(temporary.Root / "scene.png", png);
	PxcxSourceRead result;
	const bool loaded = Read(graphPath, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(loaded);
	REQUIRE(result.Collection.has_value());
	CHECK(result.Collection->GraphJson == Graph);
	CHECK(result.Collection->MetadataJson == Metadata);
	CHECK(result.Archive.GraphJson.find("\"frames_total\":3") != std::string::npos);
	CHECK(result.Preview.Width == 1);
	CHECK(result.Preview.Height == 1);
}

TEST_CASE(
	"Collection package reader accepts basename entries and both ZIP methods", "[imagegraphexport][pxcc]"
) {
	Temporary temporary;
	const auto packagePath = temporary.Root / "bundle.pxz";
	PxcxCollectionSave collection{std::string(Graph), std::string(Metadata)};
	std::vector<std::byte> png;
	Diagnostic diagnostic;
	Image preview{1, 1, {255, 0, 0, 255}, 0, SurfaceFormat::RGBA8Unorm};
	REQUIRE(WritePxcxCollectionPreview(preview, png, diagnostic));
	std::vector<std::byte> package;
	REQUIRE(WritePxcxCollectionPackage("bundle", collection, png, package, diagnostic));
	Write(packagePath, package);
	PxcxSourceRead result;
	const bool loaded = Read(packagePath, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(loaded);
	REQUIRE(result.Collection.has_value());
	CHECK(result.Collection->GraphJson == Graph);
	CHECK(result.Collection->MetadataJson == Metadata);
	CHECK(result.Preview.Width == 1);
	CHECK(result.Preview.Height == 1);
}

TEST_CASE("Native PXCX bytes are preserved by source loading", "[imagegraphexport][pxcx]") {
	Temporary temporary;
	engine::bake::PxcxArchive archive;
	archive.MetadataNumber = 121092;
	archive.MetadataText = "1.21.10.203";
	archive.GraphJson = R"JSON({"nodes":[{"id":"native","type":"image.solid","x":0,"y":0,"inputs":[]}]})JSON";
	archive.GraphJson.push_back('\0');
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
	const auto path = temporary.Root / "native.pxcx";
	Write(path, bytes);
	PxcxSourceRead result;
	Diagnostic diagnostic;
	const bool loaded = Read(path, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(loaded);
	CHECK(result.Archive.OriginalBytes == bytes);
	CHECK_FALSE(result.Collection.has_value());
}

TEST_CASE("Malformed collection sidecars and ZIP names fail atomically", "[imagegraphexport][pxcc]") {
	Temporary temporary;
	const auto path = temporary.Root / "asset.pxcc";
	PxcxSourceRead result;
	result.Collection.emplace(PxcxCollectionSave{"prior", std::string("prior metadata")});
	Diagnostic diagnostic;
	Write(path, Graph);
	Write(temporary.Root / "asset.meta", "");
	Write(temporary.Root / "asset.png", "bad");
	const bool emptySidecarsLoaded = Read(path, result, diagnostic);
	INFO(diagnostic.Message);
	CHECK_FALSE(emptySidecarsLoaded);
	CHECK(result.Collection->GraphJson == "prior");
	Write(temporary.Root / "asset.meta", Metadata);
	const bool badPreviewLoaded = Read(path, result, diagnostic);
	INFO(diagnostic.Message);
	CHECK_FALSE(badPreviewLoaded);
	CHECK(result.Collection->GraphJson == "prior");

	const auto zipPath = temporary.Root / "asset.pxz";
	const auto graph = Bytes(Graph);
	const auto metadata = Bytes(Metadata);
	for (const auto &entries :
		 {std::vector<std::pair<std::string, std::vector<std::byte>>>{{"other.pxcc", graph}},
		  {{"asset.pxcc", graph}, {"asset.pxcc", graph}},
		  {{"asset.pxcc", graph}, {"folder/asset.meta", metadata}},
		  {{"asset.meta", metadata}},
		  {{"asset.pxcc", graph}, {"extra", metadata}}}) {
		Write(zipPath, Zip(entries));
		const bool loaded = Read(zipPath, result, diagnostic);
		INFO(diagnostic.Message);
		CHECK_FALSE(loaded);
		CHECK(result.Collection->GraphJson == "prior");
	}
}

TEST_CASE("Collection ZIP accepts stored and deflated basename entries", "[imagegraphexport][pxcc]") {
	Temporary temporary;
	const auto path = temporary.Root / "bundle.pxz";
	const std::vector<std::pair<std::string, std::vector<std::byte>>> entries{
		{"bundle.pxcc", Bytes(Graph)}, {"bundle.meta", Bytes(Metadata)}
	};
	for (const int compression : {MZ_NO_COMPRESSION, MZ_DEFAULT_COMPRESSION}) {
		Write(path, Zip(entries, compression));
		PxcxSourceRead result;
		Diagnostic diagnostic;
		const bool loaded = Read(path, result, diagnostic, Limits::MaximumEvaluationBytes, false);
		INFO(diagnostic.Message);
		REQUIRE(loaded);
		REQUIRE(result.Collection.has_value());
		CHECK(result.Collection->GraphJson == Graph);
		CHECK(result.Collection->MetadataJson == Metadata);
	}
}

TEST_CASE("Collection source load bounds are atomic and preview can be skipped", "[imagegraphexport][pxcc]") {
	Temporary temporary;
	const auto path = temporary.Root / "asset.pxcc";
	Write(path, Graph);
	Write(temporary.Root / "asset.png", "bad");
	PxcxSourceRead result;
	result.Collection.emplace(PxcxCollectionSave{"prior", std::nullopt});
	Diagnostic diagnostic;
	const bool withinTinyAllowance = Read(path, result, diagnostic, 1);
	INFO(diagnostic.Message);
	CHECK_FALSE(withinTinyAllowance);
	CHECK(result.Collection->GraphJson == "prior");
	const bool loaded = Read(path, result, diagnostic, Limits::MaximumEvaluationBytes, false);
	INFO(diagnostic.Message);
	REQUIRE(loaded);
	REQUIRE(result.Collection.has_value());
	CHECK(result.Collection->GraphJson == Graph);
	CHECK(result.Preview.Pixels.empty());
}

TEST_CASE("Collection ZIP verifies graph checksum before publication", "[imagegraphexport][pxcc]") {
	Temporary temporary;
	const auto path = temporary.Root / "asset.pxz";
	auto bytes = Zip({{"asset.pxcc", Bytes(Graph)}}, MZ_NO_COMPRESSION);
	const size_t payloadOffset = 30 + std::string_view("asset.pxcc").size();
	REQUIRE(bytes.size() > payloadOffset);
	bytes[payloadOffset] ^= std::byte{1};
	Write(path, bytes);
	PxcxSourceRead result;
	result.Collection.emplace(PxcxCollectionSave{"prior", std::nullopt});
	Diagnostic diagnostic;
	CHECK_FALSE(Read(path, result, diagnostic));
	CHECK(result.Collection->GraphJson == "prior");
	CHECK_FALSE(diagnostic.Message.empty());
}
