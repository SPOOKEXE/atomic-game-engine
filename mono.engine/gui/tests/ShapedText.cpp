#include <engine/core/Paths.hpp>
#include <engine/gui/ShapedText.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

TEST_SUITE_ID("engine.gui.shapedtext")

using engine::core::Name;
using engine::gui::ApplyTextStyles;
using engine::gui::CaretFor;
using engine::gui::FontFace;
using engine::gui::FontPackage;
using engine::gui::LayoutText;
using engine::gui::MAXIMUM_FONT_PACKAGE_BYTES;
using engine::gui::MAXIMUM_SHAPED_GLYPHS;
using engine::gui::SelectionFor;
using engine::gui::ShapeText;
using engine::gui::SourceAt;
using engine::gui::TextDirection;
using engine::gui::TextShapeRequest;
using engine::gui::TextShapeStatus;
using engine::gui::TextStyleSpan;
using engine::gui::TextTruncate;

namespace {
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base());
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	std::vector<std::byte> ReadFont(std::string_view name) {
		const std::filesystem::path path = engine::core::Paths::Fonts() / name;
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) {
			return {};
		}
		const std::streamsize size = file.tellg();
		if (size <= 0) {
			return {};
		}
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) {
			return {};
		}
		return bytes;
	}

	FontPackage Package() {
		FontPackage package;
		const std::vector<std::byte> inter = ReadFont("Inter.ttf");
		if (!inter.empty()) {
			REQUIRE(package.Add(Name("fonts/Inter.ttf"), FontFace::Regular, inter));
		}
		const std::vector<std::byte> noto = ReadFont("NotoSans.ttf");
		if (!noto.empty()) {
			REQUIRE(package.Add(Name("fonts/NotoSans.ttf"), FontFace::Regular, noto));
		}
		return package;
	}
}

TEST_CASE("a package refuses invalid font bytes", "[gui][text]") {
	FontPackage package;
	const uint64_t emptySignature = package.Signature();
	const std::byte invalid[] = {std::byte{0x00}, std::byte{0x01}};
	CHECK_FALSE(package.Add(Name("fonts/invalid.ttf"), FontFace::Regular, invalid));
	CHECK(package.Faces().empty());
	CHECK(package.Signature() == emptySignature);
}

TEST_CASE(
	"a package refuses oversize or duplicate faces without changing the loaded package", "[gui][text]"
) {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());
	const size_t faces = package.Faces().size();
	const uint64_t signature = package.Signature();

	const std::vector<std::byte> oversized(MAXIMUM_FONT_PACKAGE_BYTES + 1);
	CHECK_FALSE(package.Add(Name("fonts/oversized.ttf"), FontFace::Bold, oversized));
	CHECK(package.Faces().size() == faces);
	CHECK(package.Signature() == signature);

	CHECK_FALSE(package.Add(package.Faces().front().Name, FontFace::Bold, package.Faces().front().Bytes));
	CHECK(package.Faces().size() == faces);
	CHECK(package.Signature() == signature);
}

TEST_CASE("shaping owns glyph positions and Unicode boundaries", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());
	CHECK(package.Signature() != FontPackage{}.Signature());

	const auto shaped = ShapeText(
		package,
		TextShapeRequest{
			.Text = "office A\xCC\x81 word",
			.PixelSize = 18.0f,
		}
	);
	REQUIRE(shaped.Status == TextShapeStatus::Ok);
	REQUIRE_FALSE(shaped.Glyphs.empty());
	REQUIRE_FALSE(shaped.Runs.empty());
	CHECK(shaped.Advance > 0.0f);
	CHECK(shaped.GraphemeBoundaries.front() == 0);
	CHECK(shaped.GraphemeBoundaries.back() == 15);
	CHECK(shaped.LineBreakBoundaries.back() == 15);

	for (const auto &glyph : shaped.Glyphs) {
		CHECK(glyph.Face.IsValid());
		CHECK(glyph.SourceByte < 15);
	}
}

TEST_CASE("bidirectional shaping publishes visual direction", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());

	const auto shaped = ShapeText(
		package,
		TextShapeRequest{
			.Text = "abc \xD7\x90\xD7\x91\xD7\x92",
			.Direction = TextDirection::RightToLeft,
			.PixelSize = 16.0f,
		}
	);
	REQUIRE(shaped.Status == TextShapeStatus::Ok);
	REQUIRE_FALSE(shaped.Runs.empty());
	CHECK(shaped.Runs.front().RightToLeft);
}

TEST_CASE("explicit newlines create independent bidi paragraphs", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());
	const auto shaped = ShapeText(
		package,
		TextShapeRequest{
			.Text = "abc\n\xD7\x90\xD7\x91\xD7\x92", .Direction = TextDirection::Automatic, .PixelSize = 18.0f
		}
	);
	REQUIRE(shaped.Status == TextShapeStatus::Ok);
	CHECK(std::any_of(shaped.Glyphs.begin(), shaped.Glyphs.end(), [](const auto &glyph) {
		return glyph.SourceByte >= 4;
	}));
	CHECK(shaped.GraphemeBoundaries.back() == 10);
	const auto layout = LayoutText(
		package,
		TextShapeRequest{.Text = "abc\n\xD7\x90\xD7\x91\xD7\x92", .PixelSize = 18.0f},
		200.0f,
		false,
		TextTruncate::None
	);
	REQUIRE(layout.Status == TextShapeStatus::Ok);
	CHECK(layout.Lines.size() == 2);
}

TEST_CASE("line splitting has a bounded work ceiling", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());
	std::string text;
	for (size_t index = 0; index < 1025; ++index)
		text += "a\n";
	const auto layout = LayoutText(
		package, TextShapeRequest{.Text = text, .PixelSize = 18.0f}, 1.0f, true, TextTruncate::None
	);
	CHECK(layout.Status == TextShapeStatus::TooLarge);
}

TEST_CASE("malformed UTF-8 does not enter the shaper", "[gui][text]") {
	FontPackage package;
	const auto shaped = ShapeText(package, TextShapeRequest{.Text = "\xC3"});
	CHECK(shaped.Status == TextShapeStatus::InvalidUtf8);
	CHECK(shaped.Glyphs.empty());
}

TEST_CASE("shaped grapheme boundaries keep joined emoji together", "[gui][text]") {
	FontPackage package;
	const auto shaped =
		ShapeText(package, TextShapeRequest{.Text = "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB"});
	CHECK(shaped.Status == TextShapeStatus::EmptyPackage);
	CHECK(shaped.GraphemeBoundaries == std::vector<uint32_t>{0, 11});
}

TEST_CASE("missing code points use the package replacement glyph", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());

	const auto replacement = ShapeText(package, TextShapeRequest{.Text = "\xEF\xBF\xBD"});
	const auto missing = ShapeText(package, TextShapeRequest{.Text = "\xF4\x8F\xBF\xBF"});
	REQUIRE(replacement.Status == TextShapeStatus::Ok);
	REQUIRE(missing.Status == TextShapeStatus::Ok);
	REQUIRE(replacement.Glyphs.size() == 1);
	REQUIRE(missing.Glyphs.size() == 1);
	CHECK(missing.Glyphs.front().Index == replacement.Glyphs.front().Index);
	CHECK(missing.Glyphs.front().SourceByte == 0);
}

TEST_CASE("the glyph limit rejects a whole request", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());

	const std::string exact(MAXIMUM_SHAPED_GLYPHS, 'a');
	const auto accepted = ShapeText(package, TextShapeRequest{.Text = exact});
	CHECK(accepted.Status == TextShapeStatus::Ok);
	CHECK(accepted.Glyphs.size() == MAXIMUM_SHAPED_GLYPHS);

	const std::string overflow(MAXIMUM_SHAPED_GLYPHS + 1, 'a');
	const auto rejected = ShapeText(package, TextShapeRequest{.Text = overflow});
	CHECK(rejected.Status == TextShapeStatus::TooLarge);
	CHECK(rejected.Glyphs.empty());
	CHECK(rejected.Runs.empty());
}

TEST_CASE("canonical layout wraps at Unicode break boundaries and maps carets", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());

	const auto first = ShapeText(package, TextShapeRequest{.Text = "alpha ", .PixelSize = 18.0f});
	REQUIRE(first.Status == TextShapeStatus::Ok);
	auto layout = LayoutText(
		package,
		TextShapeRequest{.Text = "alpha beta", .PixelSize = 18.0f},
		first.Advance + 0.5f,
		true,
		TextTruncate::None,
		1.25f
	);
	REQUIRE(layout.Status == TextShapeStatus::Ok);
	REQUIRE(layout.Lines.size() == 2);
	CHECK(layout.Lines[0].SourceEnd == 6);
	CHECK(layout.Lines[1].SourceBegin == 6);
	CHECK(layout.Lines[1].Baseline == Catch::Approx(22.5f));

	const auto atSecondLine = CaretFor(layout, 6);
	CHECK(atSecondLine.Line == 1);
	CHECK(atSecondLine.X == Catch::Approx(0.0f).margin(0.01f));

	const auto selection = SelectionFor(layout, 1, 10);
	REQUIRE(selection.size() == 2);
	CHECK(selection[0].Line == 0);
	CHECK(selection[1].Line == 1);
	CHECK(selection[0].EndX > selection[0].BeginX);
	CHECK(selection[1].EndX > selection[1].BeginX);
}

TEST_CASE("selection keeps disjoint visual islands across mixed bidi runs", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());

	const auto layout = LayoutText(
		package,
		TextShapeRequest{
			.Text = "a\xD7\x90\xD7\x91\xD7\x92"
					"b",
			.PixelSize = 18.0f
		},
		200.0f,
		false,
		TextTruncate::None
	);
	REQUIRE(layout.Status == TextShapeStatus::Ok);
	REQUIRE(layout.Lines.size() == 1);

	// The source range contains the first two Hebrew letters. Visual bidi order
	// places the unselected third letter between them and the leading Latin run.
	const auto selection = SelectionFor(layout, 0, 5);
	REQUIRE(selection.size() == 2);
	CHECK(selection[0].Line == 0);
	CHECK(selection[1].Line == 0);
	CHECK(selection[0].BeginX < selection[0].EndX);
	CHECK(selection[1].BeginX < selection[1].EndX);
	CHECK(selection[0].EndX < selection[1].BeginX);
}

TEST_CASE("selection across a wrapped line boundary returns one island per line", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());

	const auto firstLine = ShapeText(package, TextShapeRequest{.Text = "alpha ", .PixelSize = 18.0f});
	REQUIRE(firstLine.Status == TextShapeStatus::Ok);
	const auto layout = LayoutText(
		package,
		TextShapeRequest{.Text = "alpha beta", .PixelSize = 18.0f},
		firstLine.Advance + 0.5f,
		true,
		TextTruncate::None
	);
	REQUIRE(layout.Status == TextShapeStatus::Ok);
	REQUIRE(layout.Lines.size() == 2);

	const auto selection = SelectionFor(layout, 1, 10);
	REQUIRE(selection.size() == 2);
	CHECK(selection[0].Line == 0);
	CHECK(selection[1].Line == 1);
	CHECK(selection[0].BeginX < selection[0].EndX);
	CHECK(selection[1].BeginX < selection[1].EndX);
}

TEST_CASE("layout rejects too many style spans before shaping", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());

	std::vector<TextStyleSpan> styles(65);
	for (size_t index = 0; index < styles.size(); index++) {
		styles[index].Begin = static_cast<uint32_t>(index);
		styles[index].End = static_cast<uint32_t>(index + 1);
	}
	const auto layout = LayoutText(
		package,
		TextShapeRequest{.Text = "style span hostile input", .PixelSize = 18.0f},
		200.0f,
		false,
		TextTruncate::None,
		1.0f,
		styles
	);
	CHECK(layout.Status == TextShapeStatus::TooLarge);
	CHECK(layout.Glyphs.empty());
	CHECK(layout.Runs.empty());
}

TEST_CASE("canonical layout truncates only at grapheme boundaries", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());

	const auto accent = ShapeText(package, TextShapeRequest{.Text = "A\xCC\x81", .PixelSize = 18.0f});
	const auto dots = ShapeText(package, TextShapeRequest{.Text = "...", .PixelSize = 18.0f});
	REQUIRE(accent.Status == TextShapeStatus::Ok);
	REQUIRE(dots.Status == TextShapeStatus::Ok);
	const auto layout = LayoutText(
		package,
		TextShapeRequest{.Text = "A\xCC\x81long", .PixelSize = 18.0f},
		accent.Advance + dots.Advance + 0.5f,
		false,
		TextTruncate::AtEnd
	);
	REQUIRE(layout.Status == TextShapeStatus::Ok);
	REQUIRE(layout.Lines.size() == 1);
	CHECK(layout.Lines.front().SourceEnd == 3);
	CHECK(layout.Lines.front().Advance <= accent.Advance + dots.Advance + 0.5f);
	CHECK(std::count_if(layout.Glyphs.begin(), layout.Glyphs.end(), [](const auto &glyph) {
			  return glyph.Synthetic;
		  }) == 3);
}

TEST_CASE("rtl caret stops follow visual glyph positions", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());

	const auto layout = LayoutText(
		package,
		TextShapeRequest{
			.Text = "\xD7\x90\xD7\x91\xD7\x92", .Direction = TextDirection::RightToLeft, .PixelSize = 18.0f
		},
		200.0f,
		false,
		TextTruncate::None
	);
	REQUIRE(layout.Status == TextShapeStatus::Ok);
	REQUIRE(layout.Lines.size() == 1);
	const auto first = CaretFor(layout, 0);
	const auto second = CaretFor(layout, 2);
	CHECK(first.Line == 0);
	CHECK(second.Line == 0);
	CHECK(first.X > second.X);
	CHECK(SourceAt(layout, 0, first.X) == 0);
	CHECK(SourceAt(layout, 0, second.X) == 2);
}

TEST_CASE("style metric changes shape the matching contextual clusters", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());
	const TextStyleSpan style{.Begin = 1, .End = 2, .PixelSize = 36.0f, .Font = FontFace::Bold};
	auto layout = LayoutText(
		package,
		TextShapeRequest{.Text = "abc", .PixelSize = 18.0f},
		200.0f,
		false,
		TextTruncate::None,
		1.0f,
		std::span<const TextStyleSpan>(&style, 1)
	);
	REQUIRE(layout.Status == TextShapeStatus::Ok);
	REQUIRE_FALSE(layout.Glyphs.empty());
	ApplyTextStyles(
		layout,
		std::span<const TextStyleSpan>(&style, 1),
		engine::core::Color3{1.0f, 1.0f, 1.0f},
		0.0f,
		FontFace::Regular,
		18.0f
	);
	CHECK(layout.Lines.front().Ascent == Catch::Approx(28.8f));
	CHECK(layout.Glyphs.front().PixelSize == 18.0f);
	CHECK(std::any_of(layout.Glyphs.begin(), layout.Glyphs.end(), [](const auto &glyph) {
		return glyph.SourceByte == 1 && glyph.PixelSize == 36.0f && glyph.Font == FontFace::Bold;
	}));
}

TEST_CASE("rich-text style boundaries retain both ligature source characters", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());
	const TextStyleSpan style{.Begin = 1, .End = 2, .Tint = engine::core::Color3{1.0f, 0.0f, 0.0f}};
	auto layout = LayoutText(
		package,
		TextShapeRequest{.Text = "fi", .PixelSize = 18.0f},
		200.0f,
		false,
		TextTruncate::None,
		1.0f,
		std::span<const TextStyleSpan>(&style, 1)
	);
	REQUIRE(layout.Status == TextShapeStatus::Ok);
	ApplyTextStyles(
		layout,
		std::span<const TextStyleSpan>(&style, 1),
		engine::core::Color3{1.0f, 1.0f, 1.0f},
		0.0f,
		FontFace::Regular,
		18.0f
	);
	CHECK(std::any_of(layout.Glyphs.begin(), layout.Glyphs.end(), [](const auto &glyph) {
		return glyph.SourceByte == 0;
	}));
	CHECK(std::any_of(layout.Glyphs.begin(), layout.Glyphs.end(), [](const auto &glyph) {
		return glyph.SourceByte == 1 && glyph.Tint.R == 1.0f && glyph.Tint.G == 0.0f;
	}));
	CHECK(CaretFor(layout, 1).X > 0.0f);
}

TEST_CASE("large rich-text spans change wrapping from their shaped advances", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());
	const auto plain = LayoutText(
		package, TextShapeRequest{.Text = "aa aa", .PixelSize = 18.0f}, 1000.0f, false, TextTruncate::None
	);
	REQUIRE(plain.Status == TextShapeStatus::Ok);
	const TextStyleSpan large{.Begin = 0, .End = 2, .PixelSize = 48.0f, .Font = FontFace::Bold};
	const auto styledUnwrapped = LayoutText(
		package,
		TextShapeRequest{.Text = "aa aa", .PixelSize = 18.0f},
		1000.0f,
		false,
		TextTruncate::None,
		1.0f,
		std::span<const TextStyleSpan>(&large, 1)
	);
	REQUIRE(styledUnwrapped.Status == TextShapeStatus::Ok);
	const auto styled = LayoutText(
		package,
		TextShapeRequest{.Text = "aa aa", .PixelSize = 18.0f},
		plain.Advance + 0.5f,
		true,
		TextTruncate::None,
		1.0f,
		std::span<const TextStyleSpan>(&large, 1)
	);
	REQUIRE(styled.Status == TextShapeStatus::Ok);
	CHECK(styledUnwrapped.Advance > plain.Advance);
	CHECK(styled.Lines.size() > 1);
	CHECK(styled.Lines.front().Ascent == Catch::Approx(38.4f));
}

TEST_CASE("rtl style spans retain visual order and source carets", "[gui][text]") {
	const StagedAssets assets;
	FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());
	const TextStyleSpan large{.Begin = 2, .End = 4, .PixelSize = 32.0f, .Font = FontFace::Bold};
	auto layout = LayoutText(
		package,
		TextShapeRequest{
			.Text = "\xD7\x90\xD7\x91\xD7\x92", .Direction = TextDirection::RightToLeft, .PixelSize = 18.0f
		},
		200.0f,
		false,
		TextTruncate::None,
		1.0f,
		std::span<const TextStyleSpan>(&large, 1)
	);
	REQUIRE(layout.Status == TextShapeStatus::Ok);
	ApplyTextStyles(
		layout,
		std::span<const TextStyleSpan>(&large, 1),
		engine::core::Color3{1.0f, 1.0f, 1.0f},
		0.0f,
		FontFace::Regular,
		18.0f
	);
	const auto first = CaretFor(layout, 0);
	const auto styled = CaretFor(layout, 2);
	CHECK(first.X > styled.X);
	CHECK(SourceAt(layout, 0, styled.X) == 2);
	CHECK(std::any_of(layout.Glyphs.begin(), layout.Glyphs.end(), [](const auto &glyph) {
		return glyph.SourceByte == 2 && glyph.PixelSize == 32.0f && glyph.Font == FontFace::Bold;
	}));
}
