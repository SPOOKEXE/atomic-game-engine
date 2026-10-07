#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_SUITE_ID("engine.imagegraphexport.icon_integration")
TEST_DEPENDS("engine.imagegraphexport.runner")
TEST_DEPENDS("engine.imagegraphexport.icon_export")

TEST_CASE("native ICO graph export needs no external codec and preserves output on dimension refusal") {
	const auto directory = std::filesystem::temp_directory_path() / "atomic-native-icon-export-test";
	std::error_code error;
	std::filesystem::remove_all(directory, error);
	std::filesystem::create_directories(directory);
	struct Cleanup {
		std::filesystem::path Directory;
		~Cleanup() {
			std::error_code ignored;
			std::filesystem::remove_all(Directory, ignored);
		}
	} cleanup{directory};
	engine::imagegraphexport::GraphExportSettings settings;
	settings.Input = directory / "source.graph";
	settings.Output = directory / "result.ico";
	settings.OutputId = "final";
	const auto writeGraph = [&](unsigned width) {
		std::ofstream graph(settings.Input);
		graph << "imagegraph 1\nnode \"solid\" \"image.solid\" \"\" 0 0\n"
			  << "value 0 \"solid\" \"width\" i " << width << '\n'
			  << "value 0 \"solid\" \"height\" i 1\n"
			  << "value 0 \"solid\" \"colour\" c 255 0 0 255\n"
			  << "output \"final\" \"solid\" \"image\"\n";
		graph.close();
		REQUIRE(graph.good());
	};
	const auto readIcon = [&] {
		std::ifstream icon(settings.Output, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(icon), std::istreambuf_iterator<char>());
	};
	writeGraph(1);
	std::string failure;
	REQUIRE(engine::imagegraphexport::ExportGraph(settings, failure));
	const auto bytes = readIcon();
	REQUIRE(bytes.size() == 70);
	CHECK(bytes.substr(0, 6) == std::string("\0\0\1\0\1\0", 6));
	CHECK(bytes.substr(62, 4) == std::string("\0\0\xff\xff", 4));
	writeGraph(257);
	CHECK_FALSE(engine::imagegraphexport::ExportGraph(settings, failure));
	CHECK(failure.find("ICO") != std::string::npos);
	CHECK(readIcon() == bytes);
	for (const auto &entry : std::filesystem::directory_iterator(directory))
		CHECK(entry.path().filename().string().find(".graph-export-") == std::string::npos);
}
