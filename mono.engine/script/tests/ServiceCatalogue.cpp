// What the catalogue claims, against what the two VMs actually install.
//
// **This suite is the reason the catalogue is worth having.** A single list of
// services is only an improvement over two lists if something checks it against
// reality — otherwise it is a third place the same fact is written, and the
// least trustworthy of the three, because a row costs nothing to add and nothing
// to get wrong. What went before was exactly that failure without the list:
// Luau bound nine surface services and JavaScript bound five, four were
// reachable from one language and not the other, and the TypeScript declarations
// claimed two of the four anyway. Nobody noticed for two versions.
//
// So every case here asks a running VM rather than reading a header:
//
//   - a service the row says this language binds must be reachable by name
//   - a service it says the language does not bind must refuse, and the refusal
//     must say *which* language has it rather than "no such service"
//   - a name that is in no row at all must still get the old refusal
//
// **`game:GetService(name)` is the question, not the global.** That is what a
// script actually writes, it is the one path both languages share, and it is the
// only place a script can tell "absent" from "absent here".

#include "../src/ServiceCatalogue.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

TEST_SUITE_ID("engine.script.servicecatalogue")

using engine::ecs::Store;
using engine::script::Binds;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;
using engine::script::ServiceAvailability;
using engine::script::ServiceDefinition;
using engine::script::ServiceLanguages;
using engine::script::Services;

namespace {
	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	// The one expression both languages spell nearly the same, so the two halves
	// of every case below differ by a colon and nothing else.
	std::string Fetch(Language language, const std::string &name) {
		return language == Language::Luau ? "return game:GetService('" + name + "') ~= nil"
										  : "game.GetService('" + name + "') !== undefined";
	}
}

TEST_CASE("every service the catalogue claims for a language is reachable in it", "[scripting][services]") {
	// **The claim, checked against a live VM.** A row is a promise that a
	// script can reach that name; nothing but running one can keep it honest.
	//
	// **Studio rows are skipped and that is not a hole.** `BreakpointService`
	// installs only when the runtime carries a debugger, which a plain
	// `MakeRuntime` does not — `engine.script.debugger` is where that pairing is
	// pinned, and asserting it here would be asserting the absence of a
	// debugger.
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		const ServiceLanguages want =
			language == Language::Luau ? ServiceLanguages::Luau : ServiceLanguages::JavaScript;

		Store store = Fresh("catalogue_present");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		for (const ServiceDefinition &definition : Services()) {
			if (definition.Availability != ServiceAvailability::Always) {
				continue;
			}
			if (!Binds(definition.Languages, want)) {
				continue;
			}

			INFO(definition.Name);
			INFO((language == Language::Luau ? "luau" : "javascript"));
			const bool ok = runtime->Run(Fetch(language, definition.Name).c_str());
			INFO(runtime->LastError());
			CHECK(ok);
		}
	}
}

TEST_CASE("a service the other language binds refuses by name", "[scripting][services]") {
	// **The half that makes the mask worth carrying.** Saying "no such service"
	// for something that exists and is bound elsewhere sends an author to check
	// their spelling, which is the one place the answer is not. Four services
	// are in exactly this position today, so this is the sentence a JavaScript
	// author is most likely to see.
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		const ServiceLanguages want =
			language == Language::Luau ? ServiceLanguages::Luau : ServiceLanguages::JavaScript;
		const std::string expected =
			language == Language::Luau ? "not bound for Luau" : "not bound for JavaScript";

		Store store = Fresh("catalogue_absent");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		size_t checked = 0;
		for (const ServiceDefinition &definition : Services()) {
			if (definition.Availability != ServiceAvailability::Always) {
				continue;
			}
			if (Binds(definition.Languages, want)) {
				continue;
			}

			INFO(definition.Name);
			CHECK_FALSE(runtime->Run(Fetch(language, definition.Name).c_str()));
			CHECK(runtime->LastError().find(expected) != std::string::npos);
			checked++;
		}

		// **The JavaScript half must find some**, because the drift is real and
		// the day it is closed this case should be *deleted* rather than left
		// passing vacuously. Luau binds everything today, so its count is
		// legitimately zero.
		if (language == Language::JavaScript) {
			CHECK(checked > 0);
		}
	}
}

TEST_CASE("a name in no row still fails the old way", "[scripting][services]") {
	// **The catalogue must not swallow a typo.** A refusal naming a language is
	// only useful because it is *narrower* than the general one; if every miss
	// got it, it would say nothing. `MarketplaceService` is a real Roblox
	// service this engine does not have, which is exactly the case an author
	// migrating a place hits.
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		Store store = Fresh("catalogue_unknown");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		CHECK_FALSE(runtime->Run(Fetch(language, "MarketplaceService").c_str()));
		CHECK(runtime->LastError().find("not a service this engine provides") != std::string::npos);
	}
}

TEST_CASE("the catalogue holds no duplicate names", "[scripting][services]") {
	// **Two rows for one name is one row that never installs.** Both would run,
	// the second would overwrite the first's global, and `FindService` would
	// answer with whichever came first — so the mask consulted by a refusal and
	// the installer that actually ran could describe different services. Cheap
	// to check and impossible to see by reading the table.
	for (const ServiceDefinition &left : Services()) {
		size_t seen = 0;
		for (const ServiceDefinition &right : Services()) {
			seen += std::string(left.Name) == right.Name ? 1u : 0u;
		}
		INFO(left.Name);
		CHECK(seen == 1);
	}
}

TEST_CASE("a row claims no language it has no installer for", "[scripting][services]") {
	// **The mask and the function pointer are two statements of one fact**, and
	// the catalogue is the only place they can disagree. A row claiming a
	// language whose installer is null would install nothing and then refuse
	// with the *general* message — a service that reads as never having existed,
	// with a table saying otherwise.
	//
	// Checked through the VM rather than by reaching for the private table: the
	// claim is "a script can get this", so a script is what asks.
	for (const ServiceDefinition &definition : Services()) {
		if (definition.Availability != ServiceAvailability::Always) {
			continue;
		}

		for (const Language language : {Language::Luau, Language::JavaScript}) {
			const ServiceLanguages want =
				language == Language::Luau ? ServiceLanguages::Luau : ServiceLanguages::JavaScript;
			if (!Binds(definition.Languages, want)) {
				continue;
			}

			Store store = Fresh("catalogue_installer");
			const auto runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			INFO(definition.Name);
			CHECK(runtime->Run(Fetch(language, definition.Name).c_str()));
		}
	}
}
