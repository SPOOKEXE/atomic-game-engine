// What a method written once actually does, in both languages.
//
// **This suite is the reason the neutral layer is worth having.** One method
// table is only an improvement over two hand-written ones if something runs both
// VMs and compares the answers — otherwise it is a third place the same fact is
// written. What went before was exactly that failure without the table: Luau's
// instance methods numbered thirty and JavaScript's twenty-one, the pivot pair,
// the three tag calls and the four attribute calls were reachable from one
// language and not the other, and `mono.tools/bindings` declared every one of
// them in the TypeScript surface an author writes against.
//
// So every case here runs a real VM of each language rather than reading a
// table:
//
//   - every row in `NeutralInstanceMethods` must be a callable member in both
//   - the same script must produce the same answer in both
//   - a wrong argument must be refused by both
//
// **The answer crosses as `workspace.Name`, because a property is already
// neutral.** `Runtime::Run` reports whether a chunk ran and not what it
// evaluated to, so a value has to be written somewhere C++ can read it — and the
// property surface is the one channel this suite is not testing, which is what
// makes it usable as the wire.

#include "../src/ScriptCall.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.script.scriptcall")

using engine::ecs::Store;
using engine::script::InstanceMethod;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::NeutralInstanceMethods;

namespace {
	const std::vector<Language> LANGUAGES = {Language::Luau, Language::JavaScript};

	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	// `a:b(...)` in Luau and `a.b(...)` in JavaScript.
	//
	// **The whole of what differs between the two halves of most cases below**,
	// which is the point: a method that is written once is called almost the same
	// way, so a case can be one string with one substitution rather than two
	// scripts somebody has to keep in step.
	std::string Send(Language language, std::string_view receiver, std::string_view call) {
		return std::string(receiver) + (language == Language::Luau ? ":" : ".") + std::string(call) + "\n";
	}

	// `local x = ...` and `let x = ...`.
	std::string Let(Language language, std::string_view name, std::string_view value) {
		return (language == Language::Luau ? "local " : "let ") + std::string(name) + " = " +
			   std::string(value) + "\n";
	}

	// `x == nil` and `x === null`.
	//
	// The two spellings of "nothing", which the adapters are deliberately
	// different about: Luau answers `nil` and JavaScript answers `null` rather
	// than `undefined`, matching every other instance method in each language.
	std::string IsNil(Language language, std::string_view expression) {
		return std::string(expression) + (language == Language::Luau ? " == nil" : " === null");
	}

	// Writes one value where the test can read it.
	std::string Say(Language language, std::string_view expression) {
		const std::string text = language == Language::Luau ? "tostring(" : "String(";
		return "workspace.Name = " + text + std::string(expression) + ")\n";
	}

	// A fresh part, which almost every case starts with.
	std::string APart(Language language) {
		return Let(language, "part", "Instance.new('Part')");
	}

	// Runs a chunk in one language and hands back what `Say` wrote.
	//
	// @param language Which VM.
	// @param source   The chunk.
	// @param beats    How many heartbeats to pump afterwards, for a case whose
	//        answer is delivered at a barrier rather than written on the spot.
	std::string Answer(Language language, const std::string &source, int beats = 0) {
		Store store = Fresh("scriptcall");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		// **Held before the chunk runs, because the chunk renames it.**
		// `scene::WorkspaceOf` is a lookup by name over the roots, so asking
		// afterwards finds nothing and `InstallServices` would helpfully mint a
		// second Workspace — which reads as the answer never having been
		// written.
		const engine::ecs::Entity workspace = engine::scene::WorkspaceOf(store);
		REQUIRE(workspace != engine::ecs::NULL_ENTITY);

		INFO(source);
		const bool ok = runtime->Run(source.c_str());
		INFO(runtime->LastError());
		REQUIRE(ok);

		for (int beat = 0; beat < beats; beat++) {
			const bool beaten = runtime->Heartbeat(1.0f / 60.0f);
			INFO(runtime->LastError());
			REQUIRE(beaten);
		}

		return std::string(store.InstanceNameOf(workspace).Text());
	}

	// One parity case: what to run, and what both languages must say.
	struct ParityCase {
		const char *Name;

		// Built per language, because a few of them genuinely differ — counting
		// a map's keys is a `for` loop in one and `Object.keys` in the other.
		// What is asserted is the *answer*, never the spelling.
		std::function<std::string(Language)> Body;

		const char *Expected;

		// Beats to pump before reading, for a signal that lands at a barrier.
		int Beats = 0;
	};
}

TEST_CASE("every neutral method is a member in both languages", "[scripting][scriptcall]") {
	// **The structural half, and the one that would have caught the drift.** A
	// method missing from a language is `undefined` in JavaScript and a missing
	// member in Luau, and neither says anything until a script reaches it — which
	// is how nine methods stayed absent from one VM across two versions with the
	// type declarations claiming all nine.
	for (const Language language : LANGUAGES) {
		Store store = Fresh("scriptcall_members");
		const auto runtime = MakeRuntime(store, language);
		REQUIRE(runtime != nullptr);

		for (const InstanceMethod &method : NeutralInstanceMethods()) {
			// **No local, because every JavaScript chunk shares one global
			// scope** — a second `let part` in the same VM is a `SyntaxError`
			// before a line of it runs, which would fail this case for a reason
			// that has nothing to do with the method being asked about.
			const std::string name(method.Name);
			const std::string subject = "Instance.new('Part')." + name;
			const std::string source =
				language == Language::Luau
					? "assert(type(" + subject + ") == 'function', '" + name + " is missing')\n"
					: "if (typeof " + subject + " !== 'function') { throw new Error('" + name +
						  " is missing') }\n";

			INFO(method.Name);
			const bool ok = runtime->Run(source.c_str());
			INFO(runtime->LastError());
			CHECK(ok);
		}
	}
}

TEST_CASE("a neutral method answers the same in both languages", "[scripting][scriptcall]") {
	// **The case that matters most.** Everything else about the layer is
	// arrangement; this is the property it exists to buy, and it is asserted
	// against a literal as well as against the other language so that a method
	// broken identically in both still fails.
	const std::vector<ParityCase> CASES = {
		{"GetPivot reads a placement",
		 [](Language language) {
			 return APart(language) + "part.Position = Vector3.new(3, 4, 5)\n" +
					Say(language,
						language == Language::Luau ? "part:GetPivot().Position.X"
												   : "part.GetPivot().Position.X");
		 },
		 "3"},

		{"PivotTo writes one",
		 [](Language language) {
			 return APart(language) + Send(language, "part", "PivotTo(CFrame.new(1, 2, 3))") +
					Say(language, "part.Position.Y");
		 },
		 "2"},

		{"AddTag answers whether it took",
		 [](Language language) {
			 return APart(language) +
					Say(language, language == Language::Luau ? "part:AddTag('door')" : "part.AddTag('door')");
		 },
		 "true"},

		{"HasTag finds one that was added",
		 [](Language language) {
			 return APart(language) + Send(language, "part", "AddTag('door')") +
					Say(language, language == Language::Luau ? "part:HasTag('door')" : "part.HasTag('door')");
		 },
		 "true"},

		{"RemoveTag takes it back",
		 [](Language language) {
			 return APart(language) + Send(language, "part", "AddTag('door')") +
					Send(language, "part", "RemoveTag('door')") +
					Say(language, language == Language::Luau ? "part:HasTag('door')" : "part.HasTag('door')");
		 },
		 "false"},

		{"an attribute round-trips as a number",
		 [](Language language) {
			 return APart(language) + Send(language, "part", "SetAttribute('Health', 75)") +
					Say(language,
						language == Language::Luau ? "part:GetAttribute('Health')"
												   : "part.GetAttribute('Health')");
		 },
		 "75"},

		{"as a string",
		 [](Language language) {
			 return APart(language) + Send(language, "part", "SetAttribute('Faction', 'red')") +
					Say(language,
						language == Language::Luau ? "part:GetAttribute('Faction')"
												   : "part.GetAttribute('Faction')");
		 },
		 "red"},

		{"as a boolean",
		 [](Language language) {
			 return APart(language) + Send(language, "part", "SetAttribute('Alive', true)") +
					Say(language,
						language == Language::Luau ? "part:GetAttribute('Alive')"
												   : "part.GetAttribute('Alive')");
		 },
		 "true"},

		{"and as a datatype",
		 [](Language language) {
			 return APart(language) + Send(language, "part", "SetAttribute('Home', Vector3.new(1, 2, 3))") +
					Say(language,
						language == Language::Luau ? "part:GetAttribute('Home').Z"
												   : "part.GetAttribute('Home').Z");
		 },
		 "3"},

		{"an attribute nobody set is nothing",
		 [](Language language) {
			 return APart(language) + Say(language,
										  IsNil(
											  language,
											  language == Language::Luau ? "part:GetAttribute('Health')"
																		 : "part.GetAttribute('Health')"
										  ));
		 },
		 "true"},

		// **The optional argument, which is the one shape `IsNil` exists for.**
		// `SetAttribute(name)` and `SetAttribute(name, nil)` both remove, and an
		// adapter that read a missing argument as anything else would make the
		// first of the two silently do nothing.
		{"an omitted value removes",
		 [](Language language) {
			 return APart(language) + Send(language, "part", "SetAttribute('Health', 75)") +
					Send(language, "part", "SetAttribute('Health')") +
					Say(language,
						IsNil(
							language,
							language == Language::Luau ? "part:GetAttribute('Health')"
													   : "part.GetAttribute('Health')"
						));
		 },
		 "true"},

		{"GetAttributes answers every name",
		 [](Language language) {
			 const std::string set = Send(language, "part", "SetAttribute('a', 1)") +
									 Send(language, "part", "SetAttribute('b', 2)");
			 if (language == Language::Luau) {
				 return APart(language) + set +
						"local all = part:GetAttributes()\n"
						"local seen = 0\n"
						"for _ in pairs(all) do seen += 1 end\n" +
						Say(language, "seen");
			 }
			 return APart(language) + set + "let all = part.GetAttributes()\n" +
					Say(language, "Object.keys(all).length");
		 },
		 "2"},

		// **The empty return, which is its own case rather than a corollary.** A
		// map built and never filled is where a `nullptr` for "nothing" would
		// surface, and it is what a script iterating an untouched instance gets.
		{"GetAttributes on an instance with none is empty",
		 [](Language language) {
			 if (language == Language::Luau) {
				 return APart(language) +
						"local seen = 0\n"
						"for _ in pairs(part:GetAttributes()) do seen += 1 end\n" +
						Say(language, "seen");
			 }
			 return APart(language) + Say(language, "Object.keys(part.GetAttributes()).length");
		 },
		 "0"},

		// **The signal, which is the return the two VMs build differently.** One
		// mints a tagged userdata and the other an object of a registered class,
		// and what has to be the same is that both name the entry in one
		// `SignalTable` and are delivered by one barrier.
		{"GetAttributeChangedSignal delivers at the barrier",
		 [](Language language) {
			 if (language == Language::Luau) {
				 return APart(language) + "part.Parent = workspace\n"
										  "part:GetAttributeChangedSignal('Score'):Connect(function()\n"
										  "	workspace.Name = 'fired'\n"
										  "end)\n"
										  "part:SetAttribute('Score', 1)\n";
			 }
			 return APart(language) + "part.Parent = workspace\n"
									  "part.GetAttributeChangedSignal('Score').Connect(function () {\n"
									  "	workspace.Name = 'fired'\n"
									  "})\n"
									  "part.SetAttribute('Score', 1)\n";
		 },
		 "fired",
		 1},
	};

	for (const ParityCase &probe : CASES) {
		INFO(probe.Name);

		const std::string luau = Answer(Language::Luau, probe.Body(Language::Luau), probe.Beats);
		const std::string javascript =
			Answer(Language::JavaScript, probe.Body(Language::JavaScript), probe.Beats);

		INFO(luau);
		INFO(javascript);
		CHECK(luau == javascript);
		CHECK(luau == std::string(probe.Expected));
	}
}

TEST_CASE("the player pair answers the same in both languages", "[scripting][scriptcall]") {
	// **The first two methods written *only* once**, where the nine before them
	// were migrations of code that already existed twice. `Players:
	// GetPlayerFromCharacter` and `Player:LoadCharacter` have never had a
	// per-language binding and now never will.
	//
	// Its own case rather than rows in the parity list above, because both need
	// a player in the world first and that setup is three lines neither of the
	// others wants.
	const std::vector<ParityCase> PROBES = {
		{"a fresh player has no body",
		 [](Language language) {
			 return Let(language,
						"players",
						"game" + std::string(language == Language::Luau ? ":" : ".") +
							"GetService('Players')") +
					Say(language, IsNil(language, "players.LocalPlayer"));
		 },
		 "true"},

		{"a body knows whose it is",
		 [](Language language) {
			 // **The round trip is the assertion.** `LoadCharacter` hands back
			 // the model and `GetPlayerFromCharacter` hands the player back, so
			 // the two are each other's inverse or this says false.
			 const std::string colon = language == Language::Luau ? ":" : ".";
			 return Let(language, "players", "game" + colon + "GetService('Players')") +
					Let(language, "who", "Instance.new('Player')") + "who.Parent = players\n" +
					Let(language, "body", "who" + colon + "LoadCharacter()") +
					// **`Equals` and not `==`**, which is the whole reason that
					// method now exists: JavaScript builds a fresh handle per
					// call, so `===` on two handles to one player is always
					// false. The first version of this case used `==` and caught
					// exactly that.
					Say(language, "who" + colon + "Equals(who" + colon + "GetPlayerFromCharacter(body))");
		 },
		 "true"},

		{"a model nobody drives belongs to nobody",
		 [](Language language) {
			 // **Nil for an NPC, which is Roblox's answer too**, and the reason
			 // `Character::Owner` is read rather than every player being walked.
			 const std::string colon = language == Language::Luau ? ":" : ".";
			 return Let(language, "loose", "Instance.new('Model')") +
					Say(language, IsNil(language, "loose" + colon + "GetPlayerFromCharacter(loose)"));
		 },
		 "true"},
	};

	for (const ParityCase &probe : PROBES) {
		INFO(probe.Name);
		const std::string luau = Answer(Language::Luau, probe.Body(Language::Luau), probe.Beats);
		const std::string javascript =
			Answer(Language::JavaScript, probe.Body(Language::JavaScript), probe.Beats);

		INFO(luau);
		INFO(javascript);
		CHECK(luau == javascript);
		CHECK(luau == std::string(probe.Expected));
	}
}

TEST_CASE("a wrong argument type is refused in both languages", "[scripting][scriptcall]") {
	// **A reader raises rather than answering**, which is what lets a neutral
	// method body read its arguments straight through. The two idioms are
	// different — a Luau error and a thrown `TypeError` — and what has to match
	// is that neither one quietly does the wrong thing.
	//
	// `AddTag` is here beside `PivotTo` for a reason: `JS_ToCString` will turn an
	// object into `"[object Object]"` quite happily, so a string reader that let
	// it decide would tag a part `[object Object]` where Luau refuses.
	const std::vector<const char *> PROBES = {
		"PivotTo(5)",
		"AddTag({})",

		// **An instance reader refuses a string**, which the JavaScript side did
		// not until this method existed: `JsEntityOf` answers a null entity for
		// anything at all, so a script passing the wrong thing would have been
		// reading whatever row zero happens to be rather than being told.
		"GetPlayerFromCharacter('nobody')",
	};

	for (const Language language : LANGUAGES) {
		for (const char *probe : PROBES) {
			Store store = Fresh("scriptcall_refusal");
			const auto runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			const std::string source = APart(language) + Send(language, "part", probe);

			INFO(source);
			CHECK_FALSE(runtime->Run(source.c_str()));
			CHECK_FALSE(runtime->LastError().empty());
		}
	}
}

TEST_CASE("the neutral table holds no duplicate names", "[scripting][scriptcall]") {
	// **Two rows for one name is one row nothing installs.** Both would be
	// walked, the second would overwrite the first, and the method a script
	// reached would be whichever came last — while the table read as offering
	// both. Cheap to check and impossible to see by reading the list.
	for (const InstanceMethod &left : NeutralInstanceMethods()) {
		size_t seen = 0;
		for (const InstanceMethod &right : NeutralInstanceMethods()) {
			seen += std::string(left.Name) == right.Name ? 1u : 0u;
		}
		INFO(left.Name);
		CHECK(seen == 1);
	}
}
