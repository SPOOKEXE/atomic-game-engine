#include <engine/core/Name.hpp>
#include <engine/gui/Localization.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/UiImportAdapters.hpp>

TEST_SUITE_ID("studio.ui_import_adapters")

TEST_CASE("Studio imports bounded typed design tokens into canonical themes", "[studio][ui_import]") {
	constexpr std::string_view source = R"json(
{
  "version": 1,
  "theme": {
    "id": "theme.dark",
    "name": "Dark",
    "tokens": {
      "BackgroundColor": {"type": "color", "value": [0, 0, 0]},
      "CornerRadius": {"type": "number", "value": 6.5}
    }
  }
}
)json";
	engine::gui::UiDocument document;
	engine::gui::DocumentReport report;
	REQUIRE(studio::ImportUiDesignTokensJson(source, document, report));
	REQUIRE(document.Themes.size() == 1);
	CHECK(document.Themes.front().Id == "theme.dark");
	const engine::gui::StyleValue *background =
		document.Themes.front().Tokens.Find(engine::core::Name("BackgroundColor"));
	REQUIRE(background != nullptr);
	CHECK(background->Color.R == 0.0f);
	const engine::gui::StyleValue *radius =
		document.Themes.front().Tokens.Find(engine::core::Name("CornerRadius"));
	REQUIRE(radius != nullptr);
	CHECK(radius->Number == 6.5f);
}

TEST_CASE(
	"Studio design token import preserves outputs on malformed or bounded input", "[studio][ui_import]"
) {
	engine::gui::UiDocument document;
	document.Version = 77;
	engine::gui::DocumentReport report;
	CHECK_FALSE(studio::ImportUiDesignTokensJson("{", document, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().SourceLine == 1);
	CHECK(document.Version == 77);

	const std::string overdeep(33, '[');
	CHECK_FALSE(studio::ImportUiDesignTokensJson(overdeep, document, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().Message == "JSON nesting limit exceeded");
	CHECK(document.Version == 77);
}

TEST_CASE("Studio rejects unsupported design token fields with a source line", "[studio][ui_import]") {
	constexpr std::string_view source = R"json({
  "version": 1,
  "theme": {"id": "theme", "name": "Theme", "tokens": {}, "extension": true}
})json";
	engine::gui::UiDocument document;
	engine::gui::DocumentReport report;
	CHECK_FALSE(studio::ImportUiDesignTokensJson(source, document, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().Path == "$.theme.extension");
	CHECK(report.Issues.front().SourceLine == 3);
}

TEST_CASE(
	"Studio design token import rejects duplicate keys before JSON overwrites one", "[studio][ui_import]"
) {
	constexpr std::string_view source = R"json({"version":1,"theme":{"id":"theme","name":"Theme","tokens":{
"accent":{"type":"number","value":1},
"accent":{"type":"number","value":2}
}}})json";
	engine::gui::UiDocument document;
	engine::gui::DocumentReport report;
	CHECK_FALSE(studio::ImportUiDesignTokensJson(source, document, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().Message == "token name is duplicated");
	CHECK(report.Issues.front().SourceLine == 2);
}

TEST_CASE("Studio design token import rejects long names before interning them", "[studio][ui_import]") {
	const std::string longName(engine::gui::DocumentLimits{}.MaximumStringBytes + 1, 'a');
	const std::string source =
		"{\"version\":1,\"theme\":{\"id\":\"theme\",\"name\":\"Theme\",\"tokens\":{\"" + longName +
		"\":{\"type\":\"number\",\"value\":1}}}}";
	engine::gui::UiDocument document;
	engine::gui::DocumentReport report;
	CHECK_FALSE(studio::ImportUiDesignTokensJson(source, document, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().Message == "token name exceeds string limit");
}

TEST_CASE("Studio imports quoted localization CSV into a canonical catalogue", "[studio][ui_import]") {
	constexpr std::string_view source = "locale,key,text\n"
										"en,menu.play,Play\n"
										"fr,menu.play,Jouer\n"
										"en,dialog.welcome,\"Hello, player\"\n";
	engine::gui::LocalizationCatalogue catalogue;
	engine::gui::DocumentReport report;
	REQUIRE(studio::ImportUiLocalizationCsv(source, catalogue, report));
	engine::gui::LocalizedMessage message;
	message.Key = engine::core::Name("dialog.welcome");
	CHECK(catalogue.Resolve(message, "en") == "Hello, player");
}

TEST_CASE(
	"Studio localization import rejects duplicate rows without replacing a catalogue", "[studio][ui_import]"
) {
	engine::gui::LocalizationCatalogue catalogue;
	REQUIRE(catalogue.Set({"en", engine::core::Name("keep"), "Keep"}));
	engine::gui::DocumentReport report;
	constexpr std::string_view source = "locale,key,text\nen,same,First\nen,same,Second\n";
	CHECK_FALSE(studio::ImportUiLocalizationCsv(source, catalogue, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().SourceLine == 3);
	engine::gui::LocalizedMessage message;
	message.Key = engine::core::Name("keep");
	CHECK(catalogue.Resolve(message, "en") == "Keep");
}

TEST_CASE("Studio localization reports the actual line after a quoted newline", "[studio][ui_import]") {
	constexpr std::string_view source = "locale,key,text\nen,first,\"one\ntwo\"\nen,first,duplicate\n";
	engine::gui::LocalizationCatalogue catalogue;
	engine::gui::DocumentReport report;
	CHECK_FALSE(studio::ImportUiLocalizationCsv(source, catalogue, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().SourceLine == 4);
}

TEST_CASE(
	"Studio localization stops parsing once the catalogue row limit is reached", "[studio][ui_import]"
) {
	std::string source = "locale,key,text\n";
	for (size_t index = 0; index <= engine::gui::LocalizationCatalogue::MAXIMUM_ENTRIES; index++)
		source += "en,key" + std::to_string(index) + ",text\n";
	engine::gui::LocalizationCatalogue catalogue;
	engine::gui::DocumentReport report;
	CHECK_FALSE(studio::ImportUiLocalizationCsv(source, catalogue, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().Message == "localization entry limit exceeded");
}

TEST_CASE(
	"Studio localization rejects invalid UTF-8 before constructing a catalogue key", "[studio][ui_import]"
) {
	const std::string source = "locale,key,text\nen,broken," + std::string("\xc3\x28", 2) + "\n";
	engine::gui::LocalizationCatalogue catalogue;
	engine::gui::DocumentReport report;
	CHECK_FALSE(studio::ImportUiLocalizationCsv(source, catalogue, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK(report.Issues.front().Message == "CSV locale, key, and text must be valid UTF-8");
}
