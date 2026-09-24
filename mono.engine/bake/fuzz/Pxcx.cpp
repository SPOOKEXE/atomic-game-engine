#include <engine/bake/Pxcx.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
	using engine::bake::PxcxArchive;
	using engine::bake::PxcxLimits;

	void Require(bool condition) {
		if (!condition) std::abort();
	}

	void WriteSeed(const std::filesystem::path &directory) {
		PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson =
			R"JSON({"nodes":[{"id":"seed","type":"vendor.unimplemented","x":0,"y":0,"inputs":[]}]})JSON";
		archive.GraphJson.push_back('\0');

		std::vector<std::byte> bytes;
		std::string failure;
		Require(engine::bake::WritePxcx(archive, bytes, failure));

		std::filesystem::create_directories(directory);
		std::ofstream file(directory / "opaque-node.pxcx", std::ios::binary);
		file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		Require(file.good());
	}

	bool OutputUnchanged(const PxcxArchive &output) {
		return output.OriginalBytes == std::vector<std::byte>{std::byte{0x5A}} && output.HasThumbnailBlock &&
			   output.ThumbnailRgba == std::vector<uint8_t>{0x17} &&
			   output.MetadataPayload == std::vector<std::byte>{std::byte{0x23}} &&
			   output.MetadataNumber == 77 && output.MetadataText == "sentinel" &&
			   output.GraphJson == "sentinel" &&
			   output.Nodes == std::vector<engine::bake::PxcxNodeFact>{{"sentinel", "sentinel", 1, 2}} &&
			   output.Links == std::vector<engine::bake::PxcxLinkFact>{{"sentinel", 3, "sentinel", 4}};
	}
}

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	WriteSeed(std::filesystem::path((*arguments)[2]));
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	if (size > PxcxLimits::MaximumArchiveBytes) return 0;

	PxcxArchive output;
	output.OriginalBytes = {std::byte{0x5A}};
	output.HasThumbnailBlock = true;
	output.ThumbnailRgba = {0x17};
	output.MetadataPayload = {std::byte{0x23}};
	output.MetadataNumber = 77;
	output.MetadataText = "sentinel";
	output.GraphJson = "sentinel";
	output.Nodes.push_back({"sentinel", "sentinel", 1, 2});
	output.Links.push_back({"sentinel", 3, "sentinel", 4});

	std::string failure;
	const auto bytes = std::as_bytes(std::span(input, size));
	if (!engine::bake::ReadPxcx(bytes, output, failure)) {
		Require(!failure.empty());
		Require(OutputUnchanged(output));
		return 0;
	}

	Require(failure.empty());
	Require(output.OriginalBytes.size() == size);
	Require(size == 0 || std::memcmp(output.OriginalBytes.data(), input, size) == 0);
	Require(!output.MetadataText.empty());
	Require(!output.GraphJson.empty() && output.GraphJson.back() == '\0');
	Require(output.Nodes.size() <= PxcxLimits::MaximumNodes);
	Require(output.Links.size() <= PxcxLimits::MaximumLinks);
	Require(output.ThumbnailRgba.empty() || output.ThumbnailRgba.size() == PxcxLimits::ThumbnailRgbaBytes);
	return 0;
}
