#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Localization.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/TextResolution.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.gui.localization")

using engine::core::Name;
using namespace engine::gui;

TEST_CASE("localization prefers exact locale then parent then source", "[gui][localization]") {
	LocalizationCatalogue catalogue;
	REQUIRE(catalogue.Set(CatalogueEntry{"fr", Name("menu.play"), "Jouer {player}"}));
	REQUIRE(catalogue.Set(CatalogueEntry{"fr-CA", Name("menu.play"), "Jouer icitte {player}"}));

	LocalizedMessage message;
	message.Key = Name("menu.play");
	message.Source = "Play {player}";
	REQUIRE(message.AddArgument(LocalizedArgument{Name("player"), "Ada"}));

	CHECK(
		catalogue.Resolve(message, "fr-CA") == "Jouer icitte \xE2\x81\xA8"
											   "Ada\xE2\x81\xA9"
	);
	CHECK(
		catalogue.Resolve(message, "fr-FR") == "Jouer \xE2\x81\xA8"
											   "Ada\xE2\x81\xA9"
	);
	CHECK(
		catalogue.Resolve(message, "de") == "Play \xE2\x81\xA8"
											"Ada\xE2\x81\xA9"
	);
}

TEST_CASE("localization leaves malformed or unknown placeholders visible", "[gui][localization]") {
	LocalizationCatalogue catalogue;
	LocalizedMessage message;
	message.Key = Name("menu.exit");
	message.Source = "Exit {player}";

	CHECK(catalogue.Resolve(message, "en") == "Exit {player}");
	CHECK(catalogue.Resolve(message, "en", true) == "[missing menu.exit] Exit {player}");
}

TEST_CASE("localization bounds message arguments and catalogue entries", "[gui][localization]") {
	LocalizedMessage message;
	for (size_t index = 0; index < 8; index++) {
		REQUIRE(message.AddArgument(LocalizedArgument{Name("argument" + std::to_string(index)), "value"}));
	}
	CHECK_FALSE(message.AddArgument(LocalizedArgument{Name("extra"), "value"}));

	LocalizationCatalogue catalogue;
	CHECK_FALSE(catalogue.Set(CatalogueEntry{"", Name("key"), "value"}));
	CHECK_FALSE(catalogue.Set(CatalogueEntry{std::string(65, 'a'), Name("key"), "value"}));
	CHECK_FALSE(catalogue.Set(CatalogueEntry{"en", Name("key"), std::string(4097, 'a')}));
	CHECK_FALSE(catalogue.SetSourceLocale(std::string(65, 'a')));
}

TEST_CASE("localization CSV decoder is strict and can reuse its report", "[gui][localization]") {
	LocalizationCatalogue catalogue;
	DocumentReport report;
	CHECK_FALSE(DecodeLocalizationCsv("locale,key,text\nen,key,\"value\"tail\n", catalogue, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK_FALSE(DecodeLocalizationCsv("locale,key,text\nen,\xFF,value\n", catalogue, report));
	REQUIRE_FALSE(report.Issues.empty());
	CHECK_FALSE(DecodeLocalizationCsv(
		"locale,key,text\nen," + std::string(DocumentLimits{}.MaximumStringBytes + 1, 'k') + ",value\n",
		catalogue,
		report
	));
	REQUIRE_FALSE(report.Issues.empty());
	REQUIRE(
		DecodeLocalizationCsv("locale,key,text\nen,menu.play,Play\nfr,menu.play,Jouer\n", catalogue, report)
	);
	CHECK(report.Ok());
	LocalizedMessage message{.Key = Name("menu.play"), .Source = "Play"};
	CHECK(catalogue.Resolve(message, "fr-CA") == "Jouer");
}

TEST_CASE(
	"localization uses the project source locale after a viewer's parent locales", "[gui][localization]"
) {
	LocalizationCatalogue catalogue;
	REQUIRE(catalogue.SetSourceLocale("en-AU"));
	REQUIRE(catalogue.Set(CatalogueEntry{"en-AU", Name("greeting"), "G'day"}));
	REQUIRE(catalogue.Set(CatalogueEntry{"fr", Name("greeting"), "Bonjour"}));
	LocalizedMessage message;
	message.Key = Name("greeting");
	message.Source = "Hello";
	CHECK(catalogue.Resolve(message, "fr-CA") == "Bonjour");
	CHECK(catalogue.Resolve(message, "de-DE") == "G'day");
}

TEST_CASE("localization bounds formatted output before appending arguments", "[gui][localization]") {
	LocalizationCatalogue catalogue;
	LocalizedMessage message;
	message.Key = Name("large");
	message.Source = "{value}{value}{value}";
	CHECK_FALSE(message.AddArgument(LocalizedArgument{Name("oversized"), std::string(8193, 'a')}));
	REQUIRE(message.AddArgument(LocalizedArgument{Name("value"), std::string(4096, 'a')}));
	CHECK(catalogue.Resolve(message, "en") == "[localization limit]");
}

TEST_CASE("localization formats typed plural select number and date arguments", "[gui][localization]") {
	LocalizationCatalogue catalogue;
	LocalizedMessage message;
	message.Key = Name("summary");
	message.Source =
		"{count, plural, =0 {No items} one {# item} other {# items}}. "
		"{gender, select, female {She} male {He} other {They}} paid {amount, number} on {day, date}.";
	REQUIRE(message.AddArgument(LocalizedArgument::FromNumber(Name("count"), 2.0)));
	REQUIRE(message.AddArgument(LocalizedArgument{Name("gender"), "female"}));
	REQUIRE(message.AddArgument(LocalizedArgument::FromNumber(Name("amount"), 12.5)));
	REQUIRE(message.AddArgument(LocalizedArgument::FromDate(Name("day"), 0)));
	CHECK(catalogue.Resolve(message, "en") == "2 items. She paid 12.5 on 1970-01-01.");

	message.Arguments[0] = LocalizedArgument::FromNumber(Name("count"), 1.0);
	CHECK(catalogue.Resolve(message, "en") == "1 item. She paid 12.5 on 1970-01-01.");
	message.Arguments[0] = LocalizedArgument::FromNumber(Name("count"), 0.0);
	CHECK(catalogue.Resolve(message, "en") == "No items. She paid 12.5 on 1970-01-01.");
}

TEST_CASE(
	"label localization arguments resolve from the canonical authored component", "[gui][localization]"
) {
	engine::ecs::Store store("localized-label");
	RegisterGuiClasses();
	const auto label = store.CreateInstance(GuiClass("TextLabel"), "Summary");
	store.Set(label, Label{"fallback"});
	store.Set(label, LabelPresentation{Name("summary")});
	LabelLocalizationArguments arguments;
	arguments.Count = 3;
	arguments.Values[0] = {Name("name"), LocalizedArgumentType::String, "Ada"};
	arguments.Values[1] = {Name("total"), LocalizedArgumentType::Number, {}, 12.5};
	arguments.Values[2] = {Name("day"), LocalizedArgumentType::Date, {}, 0.0, 86400};
	store.Set(label, arguments);

	LocalizationCatalogue catalogue;
	REQUIRE(catalogue.Set(CatalogueEntry{"en", Name("summary"), "{name}: {total, number} on {day, date}"}));
	const Label *text = store.Get<Label>(label);
	REQUIRE(text != nullptr);
	CHECK(
		ResolveText(store, label, *text, {&catalogue, "en"}) ==
		std::string("\xE2\x81\xA8") + "Ada" + "\xE2\x81\xA9: 12.5 on 1970-01-02"
	);
}

TEST_CASE("localization isolates inserted strings and rejects hostile formatting", "[gui][localization]") {
	LocalizationCatalogue catalogue;
	LocalizedMessage message;
	message.Key = Name("welcome");
	message.Source = "Welcome, {player}!";
	REQUIRE(message.AddArgument(LocalizedArgument{Name("player"), "\xD7\x90\xD7\x93\xD7\x94"}));
	CHECK(catalogue.Resolve(message, "en") == "Welcome, \xE2\x81\xA8\xD7\x90\xD7\x93\xD7\x94\xE2\x81\xA9!");

	message.Source = "{count, plural, one {one}}";
	REQUIRE(message.AddArgument(LocalizedArgument::FromNumber(Name("count"), 2.0)));
	CHECK(catalogue.Resolve(message, "en") == message.Source);
}

TEST_CASE(
	"localization accepts its nesting boundary and leaves deeper formatting visible", "[gui][localization]"
) {
	LocalizationCatalogue catalogue;
	LocalizedMessage message;
	message.Key = Name("nested");
	REQUIRE(message.AddArgument(LocalizedArgument::FromNumber(Name("count"), 2.0)));

	std::string accepted = "done";
	for (size_t index = 0; index < 8; ++index)
		accepted = "{count, plural, other {" + accepted + "}}";
	message.Source = accepted;
	CHECK(catalogue.Resolve(message, "en") == "done");

	message.Source = "{count, plural, other {" + accepted + "}}";
	CHECK(catalogue.Resolve(message, "en") == message.Source);
}
