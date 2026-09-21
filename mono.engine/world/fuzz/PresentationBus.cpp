#include <engine/world/PresentationBus.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
	void Require(bool condition) {
		if (!condition) {
			std::abort();
		}
	}
}

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") {
		return 0;
	}
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	engine::world::PresentationMessage message;
	message.From = {"source", "image", 1, 1};
	message.To = {"destination", "image", 2, 1};
	message.Sequence = 1;
	message.Correlation = 7;
	message.Payload.resize(512, std::byte{0x87});
	engine::core::ByteWriter writer;
	Require(engine::world::WritePresentationMessage(writer, message));
	std::ofstream file(directory / "message.pbs", std::ios::binary);
	file.write(
		reinterpret_cast<const char *>(writer.Bytes().data()), static_cast<std::streamsize>(writer.Size())
	);
	Require(file.good());
	file.close();
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	engine::core::ByteReader reader(std::as_bytes(std::span(input, size)));
	engine::world::PresentationMessage parsed;
	parsed.Correlation = 0xABCDEu;
	parsed.From.World = "untouched";
	if (!engine::world::ReadPresentationMessage(reader, parsed)) {
		Require(reader.Failed());
		Require(parsed.Correlation == 0xABCDEu && parsed.From.World == "untouched");
		return 0;
	}
	engine::core::ByteWriter writer;
	Require(engine::world::WritePresentationMessage(writer, parsed));
	const auto consumed = std::as_bytes(std::span(input, size - reader.Remaining()));
	Require(std::ranges::equal(writer.Bytes(), consumed));
	return 0;
}
