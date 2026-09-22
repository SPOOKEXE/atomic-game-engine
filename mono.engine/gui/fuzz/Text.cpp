#include <engine/core/Name.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Localization.hpp>
#include <engine/gui/RichText.hpp>
#include <engine/gui/ShapedText.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
	void Require(bool condition) {
		if (!condition) std::abort();
	}

	void WriteBytes(const std::filesystem::path &path, std::span<const std::byte> bytes) {
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		Require(file.good());
	}
}

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	const std::string_view markup =
		"<font color=\"#4ca\"><b>Hello</b></font> &amp; {count, plural, one {item} other {items}}";
	WriteBytes(directory / "markup.txt", std::as_bytes(std::span(markup)));

	std::ifstream font(
		std::filesystem::path(MONO_SOURCE_DIRECTORY) / "mono.vendor/imgui/misc/fonts/ProggyTiny.ttf",
		std::ios::binary | std::ios::ate
	);
	Require(font.good());
	const std::streamsize fontSize = font.tellg();
	Require(fontSize > 0);
	std::vector<std::byte> bytes(static_cast<size_t>(fontSize));
	font.seekg(0);
	Require(static_cast<bool>(font.read(reinterpret_cast<char *>(bytes.data()), fontSize)));
	WriteBytes(directory / "valid-font.ttf", bytes);
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	using namespace engine;
	using namespace gui;
	const std::string_view source(reinterpret_cast<const char *>(input), size);
	Label label;
	label.Rich = true;
	std::string plain;
	std::vector<DrawSpan> spans;
	const bool parsed = ParseRichText(source, label, plain, spans);
	if (parsed) {
		uint32_t previousEnd = 0;
		for (const DrawSpan &span : spans) {
			Require(span.Begin <= span.End && span.End <= plain.size());
			Require(span.Begin >= previousEnd);
			previousEnd = span.End;
		}
	}

	LocalizationCatalogue catalogue;
	LocalizedMessage message;
	message.Key = core::Name("fuzz.message");
	message.Source =
		std::string(source.substr(0, std::min(source.size(), LocalizationCatalogue::MAXIMUM_MESSAGE_BYTES)));
	Require(message.AddArgument(LocalizedArgument::FromNumber(core::Name("count"), 2.0)));
	Require(message.AddArgument(LocalizedArgument{core::Name("name"), "Ada"}));
	const std::string localized = catalogue.Resolve(message, "en-AU", true);
	Require(localized.size() <= LocalizationCatalogue::MAXIMUM_OUTPUT_BYTES);

	FontPackage package;
	const auto fontBytes = std::as_bytes(std::span(input, size));
	if (package.Add(core::Name("fuzz.ttf"), FontFace::Regular, fontBytes)) {
		const ShapedText shaped = LayoutText(
			package,
			TextShapeRequest{
				"office \xD7\x90\xD7\x91\xD7\x92\nA\xCC\x81",
				FontFace::Regular,
				TextDirection::Automatic,
				16.0f
			},
			80.0f,
			true,
			TextTruncate::AtEnd
		);
		Require(
			shaped.Status == TextShapeStatus::Ok || shaped.Status == TextShapeStatus::TooLarge ||
			shaped.Status == TextShapeStatus::NoUsableFont
		);
		Require(shaped.Glyphs.size() <= MAXIMUM_SHAPED_GLYPHS);
	}
	return 0;
}
