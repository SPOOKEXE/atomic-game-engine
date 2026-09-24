#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <pxcimport/Runner.hpp>
#include <sstream>
#include <string>
#include <vector>

TEST_SUITE_ID("tools.pxcimport.runner")
TEST_DEPENDS("engine.imagegraphio.pxcximport")

namespace {
	struct Scratch {
		std::filesystem::path Root = std::filesystem::temp_directory_path() / "atomic-pxcimport-runner-test";
		std::filesystem::path Input = Root / "fixture.pxc";
		std::filesystem::path Output = Root / "fixture.graph";
		std::filesystem::path Reference = Root / "source-reference.rgba";

		Scratch() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
			std::filesystem::create_directories(Root, error);
		}
		~Scratch() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
		void WriteFixture(bool withPreview = false, bool withOpaqueDependency = false) const {
			engine::bake::PxcxArchive archive;
			if (withPreview) {
				archive.HasThumbnailBlock = true;
				archive.ThumbnailRgba.assign(engine::bake::PxcxLimits::ThumbnailRgbaBytes, 0);
			}
			archive.MetadataNumber = 121092;
			archive.MetadataText = "1.22.10.201";
			archive.GraphJson =
				R"JSON({"attributes":{"surface_dimension":[2,1]},"nodes":[{"id":"solid","type":"Node_Solid","x":1,"y":2,"inputs":[{"r":{"d":[1,1]},"attri":{"use_project_dimension":1}},{"r":{"d":4278850590}},{"r":{"d":false}},{"r":{"d":-4},"attri":{"mask_alpha_only":false}},{"r":{"d":true}},{"r":{"d":-4}}]},{"id":"unknown","type":"Node_Custom","x":3,"y":4,"inputs":[]},{"id":"sink-a","type":"Node_Project_Output","x":5,"y":6,"inputs":[{"from_node":"solid","from_index":0}]},{"id":"sink-b","type":"Node_Project_Output","x":7,"y":8,"inputs":[{"from_node":"unknown","from_index":0}]}]})JSON";
			if (withOpaqueDependency) {
				archive.GraphJson.insert(
					archive.GraphJson.size() - 2,
					R"JSON(,{"id":"blend","type":"Node_Blend","x":9,"y":10,"inputs":[{"from_node":"solid","from_index":0},{"from_node":"unknown","from_index":0},{"r":{"d":3}},{"r":{"d":1}},{"r":{"d":-4},"attri":{"mask_alpha_only":false}},{"r":{"d":0}},{"r":{"d":0}},{"r":{"d":[2,1]}},{"r":{"d":true}},{"r":{"d":false}},{"r":{"d":0}},{"r":{"d":0}},{"r":{"d":false}},{"r":{"d":1}},{"r":{"d":[0.5,0.5]}},{"r":{"d":false}}]})JSON"
				);
			}
			archive.GraphJson.push_back('\0');
			std::vector<std::byte> bytes;
			std::string failure;
			const bool written = engine::bake::WritePxcx(archive, bytes, failure);
			INFO(failure);
			REQUIRE(written);
			std::ofstream file(Input, std::ios::binary);
			file.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
			REQUIRE(file.good());
		}
	};

	int RunArgs(std::vector<std::string> values, std::ostringstream &out, std::ostringstream &err) {
		std::vector<char *> argv;
		for (std::string &value : values)
			argv.push_back(value.data());
		return pxcimport::Run(static_cast<int>(argv.size()), argv.data(), out, err);
	}
}

TEST_CASE("PXC import CLI writes a selected native output and diagnoses opaque nodes", "[pxcimport]") {
	Scratch scratch;
	scratch.WriteFixture();
	std::ostringstream out;
	std::ostringstream err;
	REQUIRE(
		RunArgs(
			{"pxcimport",
			 "--input",
			 scratch.Input.string(),
			 "--output",
			 scratch.Output.string(),
			 "--output-id",
			 "sink-a"},
			out,
			err
		) == 0
	);
	CHECK(out.str().find("nodes=4 native=1 opaque=3 outputs=1") != std::string::npos);
	CHECK(out.str().find("opaque nodes block native graph evaluation") != std::string::npos);
	CHECK(err.str().find("diagnostic node=\"unknown\"") != std::string::npos);
	CHECK(err.str().find("diagnostic node=\"sink-a\"") != std::string::npos);
	std::ifstream input(scratch.Output, std::ios::binary);
	REQUIRE(input.is_open());
	std::string native((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	engine::imagegraph::Document graph;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(engine::imagegraph::Read(native, graph, diagnostic) == engine::imagegraph::Status::Ok);
	REQUIRE(graph.Outputs.size() == 1);
	CHECK(graph.Outputs[0].Id == "sink-a");
	CHECK(graph.Outputs[0].NodeId == "solid");
}

TEST_CASE("PXC import CLI rejects an unknown output before writing", "[pxcimport]") {
	Scratch scratch;
	scratch.WriteFixture();
	std::ostringstream out;
	std::ostringstream err;
	CHECK(
		RunArgs(
			{"pxcimport",
			 "--input",
			 scratch.Input.string(),
			 "--output",
			 scratch.Output.string(),
			 "--output-id",
			 "absent"},
			out,
			err
		) == 1
	);
	CHECK(err.str().find("unknown output id: absent") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(scratch.Output));
}

TEST_CASE("PXC import CLI enforces the archive file cap", "[pxcimport]") {
	Scratch scratch;
	std::ofstream(scratch.Input, std::ios::binary).close();
	std::filesystem::resize_file(scratch.Input, engine::bake::PxcxLimits::MaximumArchiveBytes + 1);
	std::ostringstream out;
	std::ostringstream err;
	CHECK(
		RunArgs(
			{"pxcimport", "--input", scratch.Input.string(), "--output", scratch.Output.string()}, out, err
		) == 1
	);
	CHECK(err.str().find("64 MiB file limit") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(scratch.Output));
}

TEST_CASE("PXC import CLI exports an explicit source-reference RGBA thumbnail", "[pxcimport]") {
	Scratch scratch;
	scratch.WriteFixture(true);
	std::ostringstream out;
	std::ostringstream err;
	REQUIRE(
		RunArgs(
			{"pxcimport",
			 "--input",
			 scratch.Input.string(),
			 "--output",
			 scratch.Output.string(),
			 "--reference-rgba",
			 scratch.Reference.string()},
			out,
			err
		) == 0
	);
	CHECK(std::filesystem::file_size(scratch.Reference) == engine::bake::PxcxLimits::ThumbnailRgbaBytes);
	CHECK(
		out.str().find(
			"source_reference width=256 height=256 format=rgba8 bytes=262144 "
			"hash=0x9c735bed0a722325"
		) != std::string::npos
	);
	CHECK(out.str().find("native graph is a projection") != std::string::npos);
}

TEST_CASE("PXC import CLI rejects a thumbnail request when the source has none", "[pxcimport]") {
	Scratch scratch;
	scratch.WriteFixture();
	std::ostringstream out;
	std::ostringstream err;
	CHECK(
		RunArgs(
			{"pxcimport",
			 "--input",
			 scratch.Input.string(),
			 "--output",
			 scratch.Output.string(),
			 "--reference-rgba",
			 scratch.Reference.string()},
			out,
			err
		) == 1
	);
	CHECK(err.str().find("no saved source-reference thumbnail") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(scratch.Reference));
	CHECK_FALSE(std::filesystem::exists(scratch.Output));
}

TEST_CASE("PXC import CLI writes a compilable selected source subgraph", "[pxcimport]") {
	Scratch scratch;
	scratch.WriteFixture();
	std::ostringstream out;
	std::ostringstream err;
	REQUIRE(
		RunArgs(
			{"pxcimport",
			 "--input",
			 scratch.Input.string(),
			 "--output",
			 scratch.Output.string(),
			 "--extract-node",
			 "solid"},
			out,
			err
		) == 0
	);
	CHECK(out.str().find("partial_subgraph selected=\"solid\" cuts=0") != std::string::npos);
	CHECK(out.str().find("nodes=1 native=1 opaque=0 outputs=1") != std::string::npos);
	std::ifstream input(scratch.Output, std::ios::binary);
	REQUIRE(input.is_open());
	const std::string native((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	engine::imagegraph::Document graph;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(engine::imagegraph::Read(native, graph, diagnostic) == engine::imagegraph::Status::Ok);
	engine::imagegraph::Plan plan;
	CHECK(engine::imagegraph::Compile(graph, plan, diagnostic) == engine::imagegraph::Status::Ok);
}

TEST_CASE("PXC import CLI help identifies selected extraction as partial", "[pxcimport]") {
	std::ostringstream out;
	std::ostringstream err;
	CHECK(RunArgs({"pxcimport", "--help"}, out, err) == 0);
	CHECK(out.str().find("--extract-node") != std::string::npos);
	CHECK(out.str().find("not the Pixel Composer project output") != std::string::npos);
	CHECK(err.str().empty());
}

TEST_CASE("PXC import CLI rejects an opaque selected node", "[pxcimport]") {
	Scratch scratch;
	scratch.WriteFixture();
	std::ostringstream out;
	std::ostringstream err;
	CHECK(
		RunArgs(
			{"pxcimport",
			 "--input",
			 scratch.Input.string(),
			 "--output",
			 scratch.Output.string(),
			 "--extract-node",
			 "unknown"},
			out,
			err
		) == 1
	);
	CHECK(err.str().find("selected PXCX node has no native image mapping") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(scratch.Output));
}

TEST_CASE("PXC import CLI names an opaque dependency cut in partial extraction", "[pxcimport]") {
	Scratch scratch;
	scratch.WriteFixture(false, true);
	std::ostringstream out;
	std::ostringstream err;
	const int status = RunArgs(
		{"pxcimport",
		 "--input",
		 scratch.Input.string(),
		 "--output",
		 scratch.Output.string(),
		 "--extract-node",
		 "blend"},
		out,
		err
	);
	INFO(err.str());
	CHECK(status == 0);
	CHECK(err.str().find("cut opaque dependency unknown.output-0 -> blend.foreground") != std::string::npos);
	CHECK(out.str().find("partial_subgraph selected=\"blend\" cuts=1") != std::string::npos);
	CHECK(std::filesystem::exists(scratch.Output));
}
