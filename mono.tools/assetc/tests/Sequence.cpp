#include <engine/assets/TextureSequence.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <assetc/Bake.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

TEST_SUITE_ID("tools.assetc.sequence")
TEST_DEPENDS("engine.assets.texturesequence")

namespace {
	namespace fs = std::filesystem;

	std::vector<uint8_t> AnimatedGif(size_t count) {
		std::vector<uint8_t> bytes{
			'G',
			'I',
			'F',
			'8',
			'9',
			'a',
			0x02,
			0x00,
			0x01,
			0x00,
			0x80,
			0x00,
			0x00,
			0xFF,
			0x00,
			0x00,
			0x00,
			0x00,
			0xFF
		};
		for (size_t frame = 0; frame < count; ++frame) {
			const uint16_t delay = (frame & 1) == 0 ? 4 : 11;
			bytes.insert(
				bytes.end(), {0x21, 0xF9, 0x04, 0x00, static_cast<uint8_t>(delay), 0x00, 0x00, 0x00}
			);
			bytes.insert(
				bytes.end(),
				{0x2C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x0A, 0x00}
			);
		}
		bytes.push_back(0x3B);
		return bytes;
	}

	class Scratch {
	  public:
		explicit Scratch(const char *label) {
			Root = fs::temp_directory_path() / (std::string("assetc-sequence-") + label);
			std::error_code error;
			fs::remove_all(Root, error);
			fs::create_directories(Input(), error);
		}
		~Scratch() {
			std::error_code error;
			fs::remove_all(Root, error);
		}
		fs::path Input() const {
			return Root / "in";
		}
		fs::path Output() const {
			return Root / "out";
		}

	  private:
		fs::path Root;
	};

	assetc::Report Bake(const Scratch &scratch) {
		assetc::Settings settings;
		settings.Input = scratch.Input();
		settings.Output = scratch.Output();
		std::string failure;
		const auto report = assetc::Bake(settings, failure);
		CHECK(failure.empty());
		return report;
	}
}

TEST_CASE("assetc bakes a 257-frame GIF with original frame delays", "[assetc][sequence]") {
	const Scratch scratch("gif257");
	const auto source = AnimatedGif(257);
	{
		std::ofstream file(scratch.Input() / "spark.gif", std::ios::binary);
		file.write(reinterpret_cast<const char *>(source.data()), source.size());
	}
	const auto report = Bake(scratch);
	REQUIRE(report.Failures == 0);
	REQUIRE(report.Assets.size() == 1);
	CHECK(report.Assets[0].Output == "spark.aseq");
	CHECK(report.Assets[0].Kind == engine::assets::AssetKind::Animation);
	CHECK_FALSE(fs::exists(scratch.Output() / "spark.atex"));
	std::ifstream file(scratch.Output() / "spark.aseq", std::ios::binary);
	const std::vector<char> raw(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
	engine::core::ByteReader reader({reinterpret_cast<const std::byte *>(raw.data()), raw.size()});
	engine::assets::TextureSequenceData sequence;
	REQUIRE(engine::assets::TextureSequence::Read(reader, sequence));
	CHECK(reader.AtEnd());
	REQUIRE(sequence.FrameDurations.size() == 257);
	CHECK(sequence.FrameDurations[0] == 0.04f);
	CHECK(sequence.FrameDurations[1] == 0.11f);
	CHECK(sequence.FrameDurations[256] == 0.04f);
	CHECK(sequence.FramePixels(256).size() == 8);
}

TEST_CASE("a material cannot name a sequence as an atlas texture", "[assetc][sequence]") {
	const Scratch scratch("material-reference");
	const auto source = AnimatedGif(257);
	{
		std::ofstream file(scratch.Input() / "spark.gif", std::ios::binary);
		file.write(reinterpret_cast<const char *>(source.data()), source.size());
	}
	{
		std::ofstream file(scratch.Input() / "spark.mat");
		file << "colour = spark.gif\n";
	}
	const auto report = Bake(scratch);
	REQUIRE(report.Assets.size() == 2);
	CHECK(report.Failures == 1);
	CHECK(report.Assets[0].Failure.find("cannot be a material texture map") != std::string::npos);
	CHECK_FALSE(fs::exists(scratch.Output() / "spark.aseq"));
}
