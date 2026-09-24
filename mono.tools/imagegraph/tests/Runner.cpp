#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <imagegraph_runner/Runner.hpp>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_ID("tools.imagegraph.runner")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraph.audio_capture")

namespace {
	struct Scratch {
		std::filesystem::path Root = std::filesystem::temp_directory_path() / "atomic-imagegraph-runner-test";
		std::filesystem::path Input = Root / "solid.graph";
		std::filesystem::path Output = Root / "solid.png";
		std::filesystem::path ArrayInput = Root / "array.graph";
		std::filesystem::path AudioInput = Root / "audio.graph";
		std::filesystem::path AudioCapture = Root / "audio.capture";

		Scratch() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
			std::filesystem::create_directories(Root, error);
			std::ofstream file(Input, std::ios::binary);
			file << "imagegraph 1\n"
					"node \"solid\" \"image.solid\" \"\" 0 0\n"
					"value 0 \"solid\" \"width\" i 2\n"
					"value 0 \"solid\" \"height\" i 1\n"
					"value 0 \"solid\" \"colour\" c 12 34 56 255\n"
					"output \"final\" \"solid\" \"image\"\n";
		}
		void AddKeyframe() {
			std::ofstream file(Input, std::ios::binary | std::ios::app);
			file << "keyframe \"solid\" \"colour\" 0 \"step\" c 1 2 3 255\n"
					"keyframe \"solid\" \"colour\" 2 \"step\" c 4 5 6 255\n";
		}
		void AddInvalidWidthKeyframe() {
			std::ofstream file(Input, std::ios::binary | std::ios::app);
			file << "keyframe \"solid\" \"width\" 0 \"step\" i 2\n"
					"keyframe \"solid\" \"width\" 1 \"step\" i 0\n";
		}
		void WriteNestedArrayGraph() {
			std::ofstream file(ArrayInput, std::ios::binary);
			file << "imagegraph 2\n"
					"node \"red\" \"image.solid\" \"\" 0 0\n"
					"value 0 \"red\" \"width\" i 1\n"
					"value 0 \"red\" \"height\" i 1\n"
					"value 0 \"red\" \"colour\" c 255 0 0 255\n"
					"node \"inner\" \"value.array\" \"\" 0 0\n"
					"dynamic 1 \"inner\" \"leaf\" image 0\n"
					"node \"outer\" \"value.array\" \"\" 0 0\n"
					"dynamic 2 \"outer\" \"nested\" array 0\n"
					"link \"red\" \"image\" \"inner\" \"leaf\"\n"
					"link \"inner\" \"array\" \"outer\" \"nested\"\n"
					"output \"preview\" \"red\" \"image\"\n"
					"output \"all\" \"outer\" \"array\"\n";
		}
		void WriteAudioFixture() {
			engine::imagegraph::Document document;
			document.Nodes.push_back(
				{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}}
			);
			document.Nodes.push_back({"volume", "image.audio_volume", "", {}, {}, {}});
			document.Links.push_back({"capture", "samples", "volume", "samples"});
			document.Outputs.push_back({"loudness", "volume", "loudness"});
			{
				std::ofstream graph(AudioInput, std::ios::binary);
				graph << engine::imagegraph::Write(document);
			}
			const std::vector<engine::imagegraph::AudioCaptureFrame> frames{
				{"mono", 0, {1.0, -1.0}},
				{"mono", 1, {0.5, -0.5}},
				{"mono", 2, {0.25, -0.25}},
				{"mono", 3, {}},
			};
			std::string text;
			engine::imagegraph::Diagnostic diagnostic;
			if (engine::imagegraph::WriteAudioCapture(frames, text, diagnostic) !=
				engine::imagegraph::Status::Ok)
				throw std::runtime_error("could not create audio capture fixture");
			std::ofstream capture(AudioCapture, std::ios::binary);
			capture << text;
		}
		void WriteOverLimitArrayGraph() {
			std::ofstream file(ArrayInput, std::ios::binary);
			file << "imagegraph 2\n"
					"node \"pixel\" \"image.solid\" \"\" 0 0\n"
					"value 0 \"pixel\" \"width\" i 1\n"
					"value 0 \"pixel\" \"height\" i 1\n"
					"value 0 \"pixel\" \"colour\" c 255 0 0 255\n";
			for (size_t arrayIndex = 0; arrayIndex < 64; arrayIndex++) {
				const size_t nodeIndex = arrayIndex + 1;
				file << "node \"leaf-" << arrayIndex << "\" \"value.array\" \"\" 0 0\n";
				for (size_t inputIndex = 0; inputIndex < 64; inputIndex++) {
					file << "dynamic " << nodeIndex << " \"leaf-" << arrayIndex << "\" \"item-" << inputIndex
						 << "\" image 0\n"
						 << "link \"pixel\" \"image\" \"leaf-" << arrayIndex << "\" \"item-" << inputIndex
						 << "\"\n";
				}
			}
			file << "node \"upper-a\" \"value.array\" \"\" 0 0\n"
					"node \"upper-b\" \"value.array\" \"\" 0 0\n";
			for (size_t upperIndex = 0; upperIndex < 2; upperIndex++) {
				const char *upperId = upperIndex == 0 ? "upper-a" : "upper-b";
				const size_t nodeIndex = 65 + upperIndex;
				for (size_t inputIndex = 0; inputIndex < 64; inputIndex++) {
					file << "dynamic " << nodeIndex << " \"" << upperId << "\" \"child-" << inputIndex
						 << "\" array 0\n"
						 << "link \"leaf-" << inputIndex << "\" \"array\" \"" << upperId << "\" \"child-"
						 << inputIndex << "\"\n";
				}
			}
			file << "node \"outer\" \"value.array\" \"\" 0 0\n"
					"dynamic 67 \"outer\" \"left\" array 0\n"
					"dynamic 67 \"outer\" \"right\" array 0\n"
					"link \"upper-a\" \"array\" \"outer\" \"left\"\n"
					"link \"upper-b\" \"array\" \"outer\" \"right\"\n"
					"output \"all\" \"outer\" \"array\"\n";
		}
		~Scratch() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	};

	int RunArgs(std::vector<std::string> values, std::ostringstream &out, std::ostringstream &err) {
		std::vector<char *> argv;
		argv.reserve(values.size());
		for (std::string &value : values)
			argv.push_back(value.data());
		return imagegraph_runner::Run(static_cast<int>(argv.size()), argv.data(), out, err);
	}

	std::string ReadText(const std::filesystem::path &path) {
		std::ifstream input(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}
}

TEST_CASE("runner writes a PNG and reports the deterministic image hash", "[imagegraph][runner]") {
	Scratch scratch;
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string(),
	};

	REQUIRE(RunArgs(args, out, err) == 0);
	REQUIRE(err.str().empty());
	REQUIRE(out.str().find("hash=0x4c71c14abed3596d") != std::string::npos);
	std::ifstream png(scratch.Output, std::ios::binary);
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(png)), std::istreambuf_iterator<char>());
	constexpr std::array<uint8_t, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
	REQUIRE(bytes.size() > 26);
	REQUIRE(std::equal(signature.begin(), signature.end(), bytes.begin()));
	CHECK(bytes[24] == 8);
	CHECK(bytes[25] == 6); // PNG32 is RGBA8.
}

TEST_CASE("runner publishes a complete still image or preserves the destination", "[imagegraph][runner]") {
	Scratch scratch;
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string()
	};
	{
		std::ofstream old(scratch.Output, std::ios::binary);
		old << "previous";
	}
	REQUIRE(RunArgs(args, out, err) == 0);
	std::ifstream published(scratch.Output, std::ios::binary);
	const std::vector<uint8_t> bytes(
		(std::istreambuf_iterator<char>(published)), std::istreambuf_iterator<char>()
	);
	REQUIRE(bytes.size() > 26);
	CHECK(bytes[25] == 6);

	std::filesystem::remove(scratch.Output);
	std::filesystem::create_directory(scratch.Output);
	{
		std::ofstream prior(scratch.Output / "prior.txt", std::ios::binary);
		prior << "keep";
	}
	out.str("");
	err.str("");
	CHECK(RunArgs(args, out, err) == 1);
	CHECK(err.str().find("status=OutputError") != std::string::npos);
	CHECK(std::filesystem::is_directory(scratch.Output));
	std::ifstream prior(scratch.Output / "prior.txt", std::ios::binary);
	CHECK(std::string(std::istreambuf_iterator<char>(prior), std::istreambuf_iterator<char>()) == "keep");
	for (const auto &entry : std::filesystem::directory_iterator(scratch.Root))
		CHECK(entry.path().filename().string().find("solid.png.partial-") != 0);
}

TEST_CASE("runner applies typed property overrides before compiling", "[imagegraph][runner]") {
	Scratch scratch;
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string(),
		"--set",
		"solid.colour=colour:1,2,3,255",
	};

	REQUIRE(RunArgs(args, out, err) == 0);
	REQUIRE(err.str().empty());
	REQUIRE(out.str().find("hash=0x77096ba2893f9e11") != std::string::npos);
}

TEST_CASE("runner accepts repeated typed property overrides", "[imagegraph][runner]") {
	Scratch scratch;
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string(),
		"--set",
		"solid.colour=colour:1,2,3,255",
		"--set",
		"solid.width=int:3"
	};

	REQUIRE(RunArgs(args, out, err) == 0);
	REQUIRE(err.str().empty());
	REQUIRE(out.str().find("width=3") != std::string::npos);
}

TEST_CASE("runner validates override node and property schemas", "[imagegraph][runner]") {
	Scratch scratch;
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string(),
		"--set",
		"solid.width=colour:1,2,3,255",
	};

	REQUIRE(RunArgs(args, out, err) == 1);
	REQUIRE(err.str().find("status=TypeMismatch") != std::string::npos);
	REQUIRE(!std::filesystem::exists(scratch.Output));
}

TEST_CASE("runner refuses override names absent from the node schema", "[imagegraph][runner]") {
	Scratch scratch;
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string(),
		"--set",
		"solid.missing=int:1",
	};

	REQUIRE(RunArgs(args, out, err) == 1);
	REQUIRE(err.str().find("status=UnknownPort") != std::string::npos);
	REQUIRE(!std::filesystem::exists(scratch.Output));
}

TEST_CASE("runner evaluates a requested tick and authored keyframes", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.AddKeyframe();
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string(),
		"--tick",
		"3",
	};

	REQUIRE(RunArgs(args, out, err) == 0);
	REQUIRE(err.str().empty());
	REQUIRE(out.str().find("tick=3") != std::string::npos);
	REQUIRE(std::filesystem::exists(scratch.Output));
}

TEST_CASE("runner rejects input files above its byte limit before reading them", "[imagegraph][runner]") {
	Scratch scratch;
	std::filesystem::resize_file(scratch.Input, 16ull * 1024 * 1024 + 1);
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string(),
	};

	REQUIRE(RunArgs(args, out, err) == 1);
	REQUIRE(err.str().find("status=LimitExceeded") != std::string::npos);
	REQUIRE(err.str().find(std::to_string(16ull * 1024 * 1024) + " byte limit") != std::string::npos);
	REQUIRE(!std::filesystem::exists(scratch.Output));
}

TEST_CASE("runner streams inclusive frame ranges to stable per-tick filenames", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.AddKeyframe();
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string(),
		"--frames",
		"0:2",
	};

	REQUIRE(RunArgs(args, out, err) == 0);
	REQUIRE(err.str().empty());
	for (uint64_t tick = 0; tick <= 2; tick++) {
		std::ostringstream suffix;
		suffix << "solid.tick-" << std::string(20 - std::to_string(tick).size(), '0') << tick << ".png";
		REQUIRE(std::filesystem::exists(scratch.Root / suffix.str()));
		REQUIRE(out.str().find("tick=" + std::to_string(tick)) != std::string::npos);
	}
	REQUIRE(!std::filesystem::exists(scratch.Output));
}

TEST_CASE("runner publishes a deterministic complete PNG frame bundle", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.AddKeyframe();
	const auto first = scratch.Root / "first-bundle";
	const auto second = scratch.Root / "second-bundle";
	const auto run = [&](const std::filesystem::path &destination) {
		std::ostringstream out;
		std::ostringstream err;
		const std::vector<std::string> args = {
			"imagegraph",
			"--input",
			scratch.Input.string(),
			"--output-id",
			"final",
			"--bundle",
			destination.string(),
			"--frames",
			"0:2"
		};
		REQUIRE(RunArgs(args, out, err) == 0);
		REQUIRE(err.str().empty());
		CHECK(out.str().find("format=png-sequence frames=3") != std::string::npos);
	};
	run(first);
	run(second);
	const std::string expected =
		"{\"format\":\"atomic.imagegraph.sequence.v1\",\"output_id\":\"final\",\"frames\":["
		"{\"tick\":0,\"file\":\"frame.tick-00000000000000000000.png\",\"width\":2,\"height\":1,\"hash\":"
		"\"0x77096ba2893f9e11\"},"
		"{\"tick\":1,\"file\":\"frame.tick-00000000000000000001.png\",\"width\":2,\"height\":1,\"hash\":"
		"\"0x77096ba2893f9e11\"},"
		"{\"tick\":2,\"file\":\"frame.tick-00000000000000000002.png\",\"width\":2,\"height\":1,\"hash\":"
		"\"0xca498c2fd5a8c2fd\"}]}\n";
	CHECK(ReadText(first / "manifest.json") == expected);
	CHECK(ReadText(second / "manifest.json") == expected);
	for (uint64_t tick = 0; tick <= 2; tick++) {
		std::ostringstream name;
		name << "frame.tick-" << std::string(20 - std::to_string(tick).size(), '0') << tick << ".png";
		const std::string bytes = ReadText(first / name.str());
		REQUIRE(bytes.size() > 26);
		CHECK(bytes == ReadText(second / name.str()));
	}
}

TEST_CASE("runner preserves an existing bundle destination and validates its range", "[imagegraph][runner]") {
	Scratch scratch;
	const auto destination = scratch.Root / "bundle";
	std::filesystem::create_directory(destination);
	{
		std::ofstream existing(destination / "keep.txt");
		existing << "original";
	}
	std::ostringstream out;
	std::ostringstream err;
	std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--bundle",
		destination.string(),
		"--frames",
		"0:2"
	};
	CHECK(RunArgs(args, out, err) == 1);
	CHECK(err.str().find("status=OutputError") != std::string::npos);
	CHECK(ReadText(destination / "keep.txt") == "original");
	std::filesystem::remove_all(destination);
	out.str("");
	err.str("");
	args.back() = "0:10000000";
	CHECK(RunArgs(args, out, err) == 1);
	CHECK(err.str().find("status=LimitExceeded") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(destination));
	for (const auto &entry : std::filesystem::directory_iterator(scratch.Root))
		CHECK(entry.path().filename().string().find("bundle.partial-") != 0);
}

TEST_CASE("runner rolls back a bundle when a later tick cannot evaluate", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.AddInvalidWidthKeyframe();
	const auto destination = scratch.Root / "bundle";
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--bundle",
		destination.string(),
		"--frames",
		"0:1"
	};
	CHECK(RunArgs(args, out, err) == 1);
	CHECK(out.str().empty());
	CHECK(err.str().find("status=LimitExceeded node=\"solid\"") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(destination));
	for (const auto &entry : std::filesystem::directory_iterator(scratch.Root))
		CHECK(entry.path().filename().string().find("bundle.partial-") != 0);
}

TEST_CASE("runner exports nested image arrays with a deterministic JSON manifest", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.WriteNestedArrayGraph();
	const std::filesystem::path manifestPath = scratch.Root / "nested.json";
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.ArrayInput.string(),
		"--output-id",
		"all",
		"--output",
		manifestPath.string(),
		"--tick",
		"7"
	};

	REQUIRE(RunArgs(args, out, err) == 0);
	REQUIRE(err.str().empty());
	REQUIRE(out.str().find("format=image-array images=1 tick=7") != std::string::npos);
	std::ifstream manifestFile(manifestPath, std::ios::binary);
	const std::string manifest(
		(std::istreambuf_iterator<char>(manifestFile)), std::istreambuf_iterator<char>()
	);
	CHECK(manifest.find("\"format\":\"atomic.imagegraph.array.v1\"") != std::string::npos);
	CHECK(manifest.find("\"items\":[{\"items\":[{\"image\":0}]}]") != std::string::npos);
	CHECK(manifest.find("\"file\":\"nested.image-000000.png\"") != std::string::npos);
	CHECK(manifest.find("\"hash\":\"0x") != std::string::npos);
	const std::filesystem::path imagePath = scratch.Root / "nested.image-000000.png";
	REQUIRE(std::filesystem::exists(imagePath));
	std::ifstream imageFile(imagePath, std::ios::binary);
	const std::vector<uint8_t> firstImage(
		(std::istreambuf_iterator<char>(imageFile)), std::istreambuf_iterator<char>()
	);

	std::ostringstream repeatedOutput;
	std::ostringstream repeatedError;
	REQUIRE(RunArgs(args, repeatedOutput, repeatedError) == 0);
	REQUIRE(repeatedError.str().empty());
	CHECK(repeatedOutput.str() == out.str());
	std::ifstream repeatedManifestFile(manifestPath, std::ios::binary);
	const std::string repeatedManifest(
		(std::istreambuf_iterator<char>(repeatedManifestFile)), std::istreambuf_iterator<char>()
	);
	CHECK(repeatedManifest == manifest);
	std::ifstream repeatedImageFile(imagePath, std::ios::binary);
	const std::vector<uint8_t> repeatedImage(
		(std::istreambuf_iterator<char>(repeatedImageFile)), std::istreambuf_iterator<char>()
	);
	CHECK(repeatedImage == firstImage);
}

TEST_CASE("runner streams array outputs as one manifest per requested tick", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.WriteNestedArrayGraph();
	const std::filesystem::path manifestPath = scratch.Root / "frames.json";
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.ArrayInput.string(),
		"--output-id",
		"all",
		"--output",
		manifestPath.string(),
		"--frames",
		"2:3"
	};

	REQUIRE(RunArgs(args, out, err) == 0);
	REQUIRE(err.str().empty());
	REQUIRE(out.str().find("images=1 tick=2") != std::string::npos);
	REQUIRE(out.str().find("images=1 tick=3") != std::string::npos);
	REQUIRE(std::filesystem::exists(scratch.Root / "frames.tick-00000000000000000002.json"));
	REQUIRE(std::filesystem::exists(scratch.Root / "frames.tick-00000000000000000003.json"));
	REQUIRE(std::filesystem::exists(scratch.Root / "frames.tick-00000000000000000002.image-000000.png"));
	REQUIRE(std::filesystem::exists(scratch.Root / "frames.tick-00000000000000000003.image-000000.png"));
}

TEST_CASE("runner requires JSON for image arrays and PNG for a single image", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.WriteNestedArrayGraph();
	std::ostringstream arrayOut;
	std::ostringstream arrayErr;
	const std::vector<std::string> arrayAsPng = {
		"imagegraph",
		"--input",
		scratch.ArrayInput.string(),
		"--output-id",
		"all",
		"--output",
		(scratch.Root / "wrong.png").string()
	};
	REQUIRE(RunArgs(arrayAsPng, arrayOut, arrayErr) == 1);
	CHECK(arrayErr.str().find("status=InvalidOutput") != std::string::npos);
	CHECK(!std::filesystem::exists(scratch.Root / "wrong.png"));

	std::ostringstream imageOut;
	std::ostringstream imageErr;
	const std::vector<std::string> imageAsJson = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		(scratch.Root / "wrong.json").string()
	};
	REQUIRE(RunArgs(imageAsJson, imageOut, imageErr) == 1);
	CHECK(imageErr.str().find("status=InvalidOutput") != std::string::npos);
	CHECK(!std::filesystem::exists(scratch.Root / "wrong.json"));
}

TEST_CASE("runner refuses image arrays beyond the core leaf bound before exporting", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.WriteOverLimitArrayGraph();
	const std::filesystem::path manifestPath = scratch.Root / "over-limit-array.json";
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.ArrayInput.string(),
		"--output-id",
		"all",
		"--output",
		manifestPath.string()
	};

	REQUIRE(RunArgs(args, out, err) == 1);
	CHECK(err.str().find("status=LimitExceeded") != std::string::npos);
	CHECK(!std::filesystem::exists(manifestPath));
	CHECK(!std::filesystem::exists(scratch.Root / "over-limit-array.image-000000.png"));
}

TEST_CASE("runner returns core tick range diagnostics", "[imagegraph][runner]") {
	Scratch scratch;
	std::ostringstream out;
	std::ostringstream err;
	const std::vector<std::string> args = {
		"imagegraph",
		"--input",
		scratch.Input.string(),
		"--output-id",
		"final",
		"--output",
		scratch.Output.string(),
		"--frames",
		"0:2:0"
	};

	REQUIRE(RunArgs(args, out, err) == 1);
	REQUIRE(err.str().find("status=InvalidValue") != std::string::npos);
	REQUIRE(!std::filesystem::exists(scratch.Output));
}

TEST_CASE("runner replays recorded mono frames into a scalar Audio Volume output", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.WriteAudioFixture();
	std::ostringstream firstOut;
	std::ostringstream firstErr;
	const std::vector<std::string> args{
		"imagegraph",
		"--input",
		scratch.AudioInput.string(),
		"--output-id",
		"loudness",
		"--audio-capture",
		scratch.AudioCapture.string(),
		"--value",
		"--frames",
		"0:3",
	};
	REQUIRE(RunArgs(args, firstOut, firstErr) == 0);
	CHECK(firstErr.str().empty());
	const std::string records = firstOut.str();
	CHECK(records.find("tick=0 value=0") != std::string::npos);
	const std::string_view halfMarker = "tick=1 value=";
	const size_t halfPosition = records.find(halfMarker);
	REQUIRE(halfPosition != std::string::npos);
	const double halfValue = std::stod(records.substr(halfPosition + halfMarker.size()));
	CHECK(std::abs(halfValue - 10.0 * std::log10(0.5)) < 1.0e-12);
	const std::string_view quarterMarker = "tick=2 value=";
	const size_t quarterPosition = records.find(quarterMarker);
	REQUIRE(quarterPosition != std::string::npos);
	const double quarterValue = std::stod(records.substr(quarterPosition + quarterMarker.size()));
	CHECK(std::abs(quarterValue - 10.0 * std::log10(0.25)) < 1.0e-12);
	CHECK(records.find("tick=3 value=0") != std::string::npos);

	std::ostringstream replayOut;
	std::ostringstream replayErr;
	REQUIRE(RunArgs(args, replayOut, replayErr) == 0);
	CHECK(replayOut.str() == firstOut.str());
	CHECK(replayErr.str().empty());
}

TEST_CASE("runner reports missing and nonempty-silent recorded audio explicitly", "[imagegraph][runner]") {
	Scratch scratch;
	scratch.WriteAudioFixture();
	const std::vector<std::string> missingArgs{
		"imagegraph",
		"--input",
		scratch.AudioInput.string(),
		"--output-id",
		"loudness",
		"--value",
		"--tick",
		"4"
	};
	std::ostringstream missingOut;
	std::ostringstream missingErr;
	CHECK(RunArgs(missingArgs, missingOut, missingErr) == 1);
	CHECK(missingErr.str().find("exact tick") != std::string::npos);

	const std::vector<engine::imagegraph::AudioCaptureFrame> silentFrames{{"mono", 0, {0.0, 0.0}}};
	std::string silentText;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		engine::imagegraph::WriteAudioCapture(silentFrames, silentText, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	{
		std::ofstream capture(scratch.AudioCapture, std::ios::binary | std::ios::trunc);
		capture << silentText;
	}
	std::vector<std::string> silentArgs{
		"imagegraph",
		"--input",
		scratch.AudioInput.string(),
		"--output-id",
		"loudness",
		"--audio-capture",
		scratch.AudioCapture.string(),
		"--value",
	};
	std::ostringstream silentOut;
	std::ostringstream silentErr;
	CHECK(RunArgs(silentArgs, silentOut, silentErr) == 1);
	CHECK(silentErr.str().find("nonempty silent audio") != std::string::npos);
}
