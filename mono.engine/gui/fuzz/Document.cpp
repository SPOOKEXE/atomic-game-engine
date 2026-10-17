#include <engine/core/Bytes.hpp>
#include <engine/gui/Document.hpp>
#include <engine/gui/Registration.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {
	void Require(bool condition) {
		if (!condition) std::abort();
	}

	void WriteSeed(const std::filesystem::path &directory) {
		using namespace engine::gui;
		RegisterGuiClasses();
		UiDocument document;
		document.Roots.push_back(DocumentNode{.Id = "root", .Class = "Frame", .Name = "Root"});
		engine::core::ByteWriter writer;
		DocumentReport report;
		Require(EncodeDocument(document, writer, report));
		std::ofstream file(directory / "frame.uidoc", std::ios::binary);
		const auto bytes = writer.Bytes();
		file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		Require(file.good());
	}
}

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	engine::gui::RegisterGuiClasses();
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	WriteSeed(directory);
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	using namespace engine;
	using namespace gui;
	const auto bytes = std::as_bytes(std::span(input, size));
	core::ByteReader reader(bytes);
	UiDocument document;
	document.Version = 0xA11CEu;
	DocumentReport report;
	if (!DecodeDocument(reader, document, report)) {
		Require(document.Version == 0xA11CEu && document.Roots.empty() && document.Themes.empty());
		return 0;
	}
	Require(reader.AtEnd());
	Require(ValidateDocument(document, report));
	core::ByteWriter canonical;
	Require(EncodeDocument(document, canonical, report));
	Require(std::ranges::equal(canonical.Bytes(), bytes));
	core::ByteReader roundTripReader(canonical.Bytes());
	UiDocument roundTrip;
	Require(DecodeDocument(roundTripReader, roundTrip, report) && roundTripReader.AtEnd());
	core::ByteWriter repeated;
	Require(EncodeDocument(roundTrip, repeated, report));
	Require(std::ranges::equal(canonical.Bytes(), repeated.Bytes()));
	return 0;
}
