#include <engine/imagegraphexport/GraphInputs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
TEST_SUITE_ID("engine.imagegraphexport.graph_inputs")
TEST_DEPENDS("engine.bake.image")
TEST_CASE("Image grants reject dimensions before allocating decoder pixels", "[imagegraph][export]") {
	const auto file = std::filesystem::temp_directory_path() / "atomic-export-input-bounds.png";
	struct Cleanup {
		std::filesystem::path File;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove(File, error);
		}
	} cleanup{file};
	const std::array<unsigned char, 24> bytes{137, 80,	78,	 71,  13, 10, 26, 10, 0, 0, 0, 13,
											  'I', 'H', 'D', 'R', 0,  0,  16, 1,  0, 0, 0, 1};
	{
		std::ofstream stream(file, std::ios::binary);
		stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
	}
	engine::imagegraphexport::GraphExportSettings settings;
	settings.ImageInputs.push_back({"source", file});
	std::vector<engine::imagegraph::RequestImageSource> images;
	std::string failure;
	CHECK_FALSE(engine::imagegraphexport::LoadGraphImageInputs(settings, images, failure));
	CHECK(failure.find("decode budget") != std::string::npos);
	CHECK(images.empty());
}
