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

#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Audio.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <utility>
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

	// What a case needs in the world before its chunk runs.
	//
	// **A callback rather than a fixture, because the service cases each want a
	// different world**: a mesh catalogue, a key held down, an input state that
	// is simply absent. Empty for every instance-method case, which is the
	// majority.
	using Setup = std::function<void(Store &)>;

	// Runs a chunk in one language and hands back what `Say` wrote.
	//
	// @param language Which VM.
	// @param source   The chunk.
	// @param beats    How many heartbeats to pump afterwards, for a case whose
	//        answer is delivered at a barrier rather than written on the spot.
	// @param setup    What the world holds before the chunk runs.
	std::string Answer(Language language, const std::string &source, int beats = 0, const Setup &setup = {}) {
		Store store = Fresh("scriptcall");

		// **Before the runtime, not after.** `MakeRuntime` installs the services
		// against this store, and a resource a service reads at install time
		// would otherwise arrive too late — `OpenWorkspace` is the one that
		// already depends on the order.
		if (setup) {
			setup(store);
		}

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

		// What the world holds first, for a case that reads one.
		Setup World;
	};

	// Runs one case in both languages and asserts they agree with each other and
	// with the literal.
	//
	// **Both, and the literal is not redundant**: two languages that are broken
	// identically agree, and agreeing is exactly what this suite is for.
	void Both(const ParityCase &probe) {
		INFO(probe.Name);

		const std::string luau = Answer(Language::Luau, probe.Body(Language::Luau), probe.Beats, probe.World);
		const std::string javascript =
			Answer(Language::JavaScript, probe.Body(Language::JavaScript), probe.Beats, probe.World);

		INFO(luau);
		INFO(javascript);
		CHECK(luau == javascript);
		CHECK(luau == std::string(probe.Expected));
	}

	// `a:b(...)` and `a.b(...)` as an **expression** rather than a statement,
	// which is what `Say` needs and `Send` is not.
	std::string Call(Language language, std::string_view receiver, std::string_view call) {
		return std::string(receiver) + (language == Language::Luau ? ":" : ".") + std::string(call);
	}

	// How many entries a list has, in each language's own spelling.
	std::string Length(Language language, const std::string &expression) {
		return language == Language::Luau ? "#(" + expression + ")" : "(" + expression + ").length";
	}

	// The first entry of a list.
	//
	// **The one thing about a list the two languages genuinely disagree on**, and
	// it is deliberate on both sides: a Luau array is one-based because `#` and
	// `ipairs` mean one-based, and a JavaScript one is a real `Array`.
	std::string First(Language language, const std::string &expression) {
		return "(" + expression + ")" + (language == Language::Luau ? "[1]" : "[0]");
	}

	// Two meshes in the catalogue, one of which sorts first.
	void TwoMeshes(Store &store) {
		REQUIRE(engine::scene::RecordMesh(store, engine::core::Name("props/zebra.amesh"), 7));
		REQUIRE(engine::scene::RecordMesh(store, engine::core::Name("props/anvil.amesh"), 3));
	}

	// One key held down, so the first beat is its press edge.
	void KeyHeld(Store &store) {
		engine::scene::InputState input;
		input.Down.Set(engine::scene::KeyCode::E, true);
		store.SetResource(input);
	}

	// A world with a window, which is what a client's holds and a headless
	// server's does not.
	//
	// **Both resources, because `UserInputService` reads two.** The pointer and
	// the device flags come off `scene::InputState`; `MouseDeltaSensitivity` is
	// the camera controller's own number under Roblox's name, so a world without
	// one answers the default and drops a write.
	void Windowed(Store &store) {
		engine::scene::InputState input;
		input.MousePosition = engine::core::Vector2{12.0f, 34.0f};
		store.SetResource(input);
		store.SetResource(engine::scene::CameraController{});
	}

	// One key down, one button down and a pointer that moved, so the first beat
	// carries a key edge, a button edge and a motion report.
	void OneOfEverything(Store &store) {
		engine::scene::InputState input;
		input.Down.Set(engine::scene::KeyCode::E, true);
		input.Buttons = 1u << static_cast<uint8_t>(engine::scene::MouseButton::Left);
		input.MousePosition = engine::core::Vector2{12.0f, 34.0f};
		input.MouseDelta = engine::core::Vector2{3.0f, 4.0f};
		store.SetResource(input);
	}

	// A window that has just regained focus, which is a `PreviousFocused` of
	// false and a `Focused` of true — an edge and not a state.
	void FocusRegained(Store &store) {
		engine::scene::InputState input;
		input.Focused = true;
		input.PreviousFocused = false;
		store.SetResource(input);
	}

	// `local X = game:GetService('X')`, which is how a Roblox script reaches one.
	//
	// **The local is not decoration on the Luau side.** `luaL_sandbox` enables
	// `safeenv`, so a bare `Global.Field` compiles to a `GETIMPORT` resolved once
	// per closure and a live property would read as a frozen one —
	// `DEFERRED.md` D00030. JavaScript has no such edge and gets the same
	// spelling for free, which is what makes one string serve both.
	//
	// @param language Which VM.
	// @param name     The service, and the local it lands in.
	std::string AService(Language language, std::string_view name) {
		const std::string colon = language == Language::Luau ? ":" : ".";
		return Let(language, name, "game" + colon + "GetService('" + std::string(name) + "')");
	}

	// Two expressions as `"<left>/<right>"`, so one case can assert two answers.
	//
	// **Each half is stringified rather than the whole**, and both reasons are
	// Luau's: `..` refuses a boolean and a nil outright, and it binds tighter than
	// `==` — so a bare `a .. '/' .. b == nil` is a concatenation error *and* the
	// wrong precedence. Wrapping each side settles both, and costs the JavaScript
	// spelling nothing it was not already doing.
	std::string Join(Language language, std::string_view left, std::string_view right) {
		const std::string open = language == Language::Luau ? "tostring(" : "String(";
		const std::string glue = language == Language::Luau ? " .. '/' .. " : " + '/' + ";
		return open + std::string(left) + ")" + glue + open + std::string(right) + ")";
	}
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

TEST_CASE("a neutral service method answers the same in both languages", "[scripting][scriptcall]") {
	// **The five services that stopped being Luau's at v0.16**, each asked the
	// same question in each VM. Before the neutral layer reached
	// `ServiceSurface`, half of these scripts were a `TypeError` in JavaScript
	// because the global did not exist at all — which is what makes this the
	// case that matters rather than a coverage exercise.
	const std::vector<ParityCase> CASES = {
		// --- ContentService ------------------------------------------------
		//
		// **Seeded, because an empty catalogue is the same answer for the right
		// answer and the wrong one.** A world with nothing registered answers
		// zero to every question here, so a binding that returned an empty list
		// unconditionally would pass an unseeded case in both languages.
		{"GetMeshes counts what arrived",
		 [](Language language) {
			 return Say(language, Length(language, Call(language, "ContentService", "GetMeshes()")));
		 },
		 "2",
		 0,
		 TwoMeshes},

		// **Sorted, which is what makes a layout reproducible** — and the index
		// is the one place the two languages genuinely differ, because a Luau
		// array is one-based and a JavaScript one is not.
		{"and hands them back sorted",
		 [](Language language) {
			 return Say(language, First(language, Call(language, "ContentService", "GetMeshes()")));
		 },
		 "props/anvil.amesh",
		 0,
		 TwoMeshes},

		{"GetTriangleCount answers about a mesh rather than a part",
		 [](Language language) {
			 return Say(language, Call(language, "ContentService", "GetTriangleCount('props/anvil.amesh')"));
		 },
		 "3",
		 0,
		 TwoMeshes},

		{"a mesh nothing recorded counts zero",
		 [](Language language) {
			 return Say(language, Call(language, "ContentService", "GetTriangleCount('props/never.amesh')"));
		 },
		 "0"},

		// **The record return, which is the one `ScriptValue` decision this work
		// had to take.** Three numbers under three names go through
		// `ReturnValue`, so a table in one language and an object in the other
		// have to read the same way.
		{"GetFlipbook answers a record of three numbers",
		 [](Language language) {
			 return Say(
				 language, Call(language, "ContentService", "GetFlipbook('fx/fire.atex')") + ".Frames"
			 );
		 },
		 "9",
		 0,
		 [](Store &store) {
			 REQUIRE(
				 engine::scene::RecordTexture(
					 store,
					 engine::core::Name("fx/fire.atex"),
					 engine::scene::FlipbookFacts{.Side = 3, .Frames = 9, .FrameRate = 12.0f}
				 )
			 );
		 }},

		{"a still image has no flipbook",
		 [](Language language) {
			 return Say(
				 language, IsNil(language, Call(language, "ContentService", "GetFlipbook('fx/none.atex')"))
			 );
		 },
		 "true"},

		// --- CollectionService ----------------------------------------------
		{"GetTagged finds what AddTag put there",
		 [](Language language) {
			 return APart(language) + "part.Parent = workspace\n" +
					Send(language, "CollectionService", "AddTag(part, 'door')") +
					Say(language, Length(language, Call(language, "CollectionService", "GetTagged('door')")));
		 },
		 "1"},

		// **The two spellings are one call**, which is what the file header
		// claimed before there was a layer that could make it true: the instance
		// method tags it and the service finds it.
		{"the instance method and the service are one mechanism",
		 [](Language language) {
			 return APart(language) + "part.Parent = workspace\n" + Send(language, "part", "AddTag('door')") +
					Say(language, Call(language, "CollectionService", "HasTag(part, 'door')"));
		 },
		 "true"},

		{"GetTags answers what one instance carries, sorted",
		 [](Language language) {
			 return APart(language) + Send(language, "part", "AddTag('zebra')") +
					Send(language, "part", "AddTag('anvil')") +
					Say(language, First(language, Call(language, "CollectionService", "GetTags(part)")));
		 },
		 "anvil"},

		{"an unregistered tag is an empty list rather than an error",
		 [](Language language) {
			 return Say(
				 language, Length(language, Call(language, "CollectionService", "GetTagged('nobody')"))
			 );
		 },
		 "0"},

		// --- HttpService -----------------------------------------------------
		//
		// **The encoder's rules are the codec's, so one table must be one
		// document in both VMs** — a map's keys sorted, an array one-based in
		// Luau and zero-based here, and the same text out of each.
		{"JSONEncode writes one document from either language",
		 [](Language language) {
			 const std::string table = language == Language::Luau ? "{b = 2, a = 1}" : "{b: 2, a: 1}";
			 return Say(language, Call(language, "HttpService", "JSONEncode(" + table + ")"));
		 },
		 R"({"a":1,"b":2})"},

		{"an array survives the round trip one-based",
		 [](Language language) {
			 const std::string list = language == Language::Luau ? "{10, 20, 30}" : "[10, 20, 30]";
			 return Say(
				 language,
				 Call(
					 language,
					 "HttpService",
					 "JSONDecode(" + Call(language, "HttpService", "JSONEncode(" + list + ")") + ")"
				 ) + (language == Language::Luau ? "[2]" : "[1]")
			 );
		 },
		 "20"},

		{"JSONDecode reads an object",
		 [](Language language) {
			 return Say(language, Call(language, "HttpService", R"(JSONDecode('{"score":42}'))") + ".score");
		 },
		 "42"},

		// **A GUID is deterministic, which is what makes this a parity case at
		// all.** `core::Random` over a draw index and a salt from the world's
		// name gives the same sixteen bytes in both VMs — so the two answers are
		// compared against each other *and* against a literal, and a binding that
		// drew from a clock would fail both.
		{"GenerateGUID draws the same sequence in both",
		 [](Language language) {
			 return Say(language, Call(language, "HttpService", "GenerateGUID(false)"));
		 },
		 "EA90FF9A-9A3E-498F-B626-EA711F6C37F4"},

		// **An empty table is `{}` and not `[]` in both**, which is `Codec.hpp`'s
		// choice rather than JSON's: Lua cannot tell an empty list from an empty
		// map, so one of the two has to be picked and the bus's pick is the one a
		// document has to agree with. JavaScript *can* tell them apart, so
		// `JSONEncode([])` there answers `[]` — an asymmetry the languages
		// genuinely have and not one either binding invented.
		{"an empty table is an object in both",
		 [](Language language) { return Say(language, Call(language, "HttpService", "JSONEncode({})")); },
		 "{}"},

		{"UrlEncode escapes to RFC 3986",
		 [](Language language) {
			 return Say(language, Call(language, "HttpService", "UrlEncode('a b/c~d')"));
		 },
		 "a%20b%2Fc~d"},

		// --- CrossWorldService ------------------------------------------------
		//
		// **`true` here means "queued inside this world's budget" and not
		// "delivered", which is what `Ticket::Expected` answers.** A world with
		// no router around it still takes the envelope; whether anybody is
		// listening is a reply this world never sees the far side of. The
		// declaration says "false when the world is not running", and that
		// sentence describes the *router's* answer rather than this one — worth
		// pinning either way, because the two VMs must say the same thing.
		{"Send answers the same in both when nothing is listening",
		 [](Language language) {
			 const std::string message = language == Language::Luau ? "{a = 1}" : "{a: 1}";
			 return Say(language, Call(language, "CrossWorldService", "Send('nowhere', " + message + ")"));
		 },
		 "true"},

		// --- ContextActionService ---------------------------------------------
		//
		// **The pump is half the assertion.** A bound action that is never called
		// is the failure this engine refuses twice over, and this language had no
		// input pump at all until the service crossed — so the case holds a key
		// down, beats once, and asserts the handler ran *and* was handed Roblox's
		// three arguments.
		{"a bound action fires with a name, a state and an InputObject",
		 [](Language language) {
			 if (language == Language::Luau) {
				 return "ContextActionService:BindAction('open', function(name, state, input)\n"
						"	workspace.Name = name .. '/' .. state.Name .. '/' .. input.KeyCode.Name\n"
						"end, false, Enum.KeyCode.E)\n";
			 }
			 return "ContextActionService.BindAction('open', function (name, state, input) {\n"
					"	workspace.Name = name + '/' + state.Name + '/' + input.KeyCode.Name\n"
					"}, false, Enum.KeyCode.E)\n";
		 },
		 "open/Begin/E",
		 1,
		 KeyHeld},

		// **The highest claim wins and the rest never see it**, which is the rule
		// `ActionStack` holds and the reason the stack had to become shared
		// rather than be written twice.
		{"the highest priority claim takes the key",
		 [](Language language) {
			 if (language == Language::Luau) {
				 return "ContextActionService:BindActionAtPriority('low', function()\n"
						"	workspace.Name = 'low'\n"
						"end, false, 1, Enum.KeyCode.E)\n"
						"ContextActionService:BindActionAtPriority('high', function()\n"
						"	workspace.Name = 'high'\n"
						"end, false, 9, Enum.KeyCode.E)\n";
			 }
			 return "ContextActionService.BindActionAtPriority('low', function () {\n"
					"	workspace.Name = 'low'\n"
					"}, false, 1, Enum.KeyCode.E)\n"
					"ContextActionService.BindActionAtPriority('high', function () {\n"
					"	workspace.Name = 'high'\n"
					"}, false, 9, Enum.KeyCode.E)\n";
		 },
		 "high",
		 1,
		 KeyHeld},

		// **Unbinding has to release the handler and stop the call**, and the
		// second half is what a test can see: the name the world starts with is
		// what it still has after the beat.
		{"an unbound action does not fire",
		 [](Language language) {
			 const std::string bind =
				 language == Language::Luau
					 ? "ContextActionService:BindAction('open', function()\n	workspace.Name = "
					   "'fired'\nend, "
					   "false, Enum.KeyCode.E)\n"
					 : "ContextActionService.BindAction('open', function () {\n	workspace.Name = 'fired'\n}, "
					   "false, Enum.KeyCode.E)\n";
			 return bind + Send(language, "ContextActionService", "UnbindAllActions()") +
					Say(language, "'quiet'");
		 },
		 "quiet",
		 1,
		 KeyHeld},
	};

	for (const ParityCase &probe : CASES) {
		Both(probe);
	}
}

TEST_CASE("an optional flag reads each language's own truthiness", "[scripting][scriptcall]") {
	// **The one place a ported method deliberately answers differently, pinned
	// rather than left to be rediscovered.** `GenerateGUID(wrapInCurlyBraces)`
	// is read with `ScriptCall::OptionalBoolean`, which is `lua_toboolean` on one
	// side and `JS_ToBool` on the other — and only `nil` and `false` are falsy in
	// Luau where `0`, `""` and `NaN` are falsy in JavaScript. So `GenerateGUID(0)`
	// wraps in one and does not in the other.
	//
	// A strict reader that raised for a non-boolean would agree in both and would
	// refuse the `if x then`-shaped code every Lua author writes. The engine takes
	// the language, and this case is the record of that — with the *rest* of the
	// answer identical, which is what says the divergence is the flag and nothing
	// else.
	const std::string luau = Answer(Language::Luau, Say(Language::Luau, "HttpService:GenerateGUID(0)"));
	const std::string javascript =
		Answer(Language::JavaScript, Say(Language::JavaScript, "HttpService.GenerateGUID(0)"));

	CHECK(luau == "{" + javascript + "}");
	CHECK(javascript == "EA90FF9A-9A3E-498F-B626-EA711F6C37F4");
}

TEST_CASE("a wrong argument to a service method is refused in both", "[scripting][scriptcall]") {
	// **A service reader raises exactly as an instance one does**, which is the
	// property that lets a neutral body read straight through. `AsNumber` is here
	// because `JS_ToFloat64` would take `"5"` and `[]` quite happily where
	// `luaL_checknumber` refuses a table — the same class of coercion `AddTag({})`
	// caught on the instance side.
	const std::vector<std::pair<const char *, const char *>> PROBES = {
		{"CollectionService", "AddTag('notaninstance', 'door')"},
		{"CollectionService", "GetTags(5)"},
		{"HttpService", "UrlEncode({})"},
		{"ContentService", "GetTriangleCount({})"},
		{"ContextActionService", "BindAction('open', 'notafunction', false)"},
		{"ContextActionService", "BindActionAtPriority('open', function() end, false, {})"},

		// **A numeric string is a string in both, which it was not.**
		// `luaL_checknumber` accepts `"5"` and `JS_IsNumber` does not, so this
		// call bound at priority five in one language and threw in the other
		// until `AsNumber` was made an exact type check — the same divergence
		// `ReadLuauAttribute` closed for `SetAttribute("n", "5")`.
		{"ContextActionService", "BindActionAtPriority('open', function() end, false, '5')"},
	};

	for (const Language language : LANGUAGES) {
		for (const auto &[service, probe] : PROBES) {
			Store store = Fresh("scriptcall_service_refusal");
			const auto runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			// The one probe whose *body* differs, because an empty function is
			// spelled differently and only one of the six contains one.
			std::string source = Send(language, service, probe);
			if (language == Language::JavaScript) {
				source = Send(language, service, probe);
				const size_t at = source.find("function() end");
				if (at != std::string::npos) {
					source.replace(at, std::string("function() end").size(), "function () {}");
				}
			}

			INFO(source);
			CHECK_FALSE(runtime->Run(source.c_str()));
			CHECK_FALSE(runtime->LastError().empty());
		}
	}
}

TEST_CASE("a service property is live in both languages", "[scripting][scriptcall]") {
	// **The case the whole property mechanism exists to make true, and it is the
	// one that has already failed once.** A Luau service is a *userdata* because
	// `luaL_sandbox` enables `safeenv`, which compiles a field read off a
	// constant global table into a `GETIMPORT` resolved once and cached — so the
	// first read of `MouseBehavior` won forever and a live value read as a frozen
	// one. JavaScript has no such caching, but a property installed as a plain
	// value rather than an accessor would reproduce the bug exactly.
	//
	// So every case here **reads, writes and reads again**, and asserts the
	// second read moved. A binding that snapshots would pass the first half of
	// each string and fail the second, in whichever language did it.
	const std::vector<ParityCase> CASES = {
		{"Volume sees its own write",
		 [](Language language) {
			 return AService(language, "SoundService") + Let(language, "before", "SoundService.Volume") +
					"SoundService.Volume = 0.25\n" +
					Say(language, Join(language, "before", "SoundService.Volume"));
		 },
		 "1/0.25"},

		// Above 1 is legal for `Sound.Volume`'s reason: a mixer sums, and the
		// clamp happens once at the output stage.
		{"and keeps a value above one",
		 [](Language language) {
			 return AService(language, "SoundService") + "SoundService.Volume = 2\n" +
					Say(language, "SoundService.Volume");
		 },
		 "2"},

		// **An enum-valued property, which is the one that could genuinely have
		// failed to cross.** `ScriptCall::ReturnEnum` is what lets it: the two
		// VMs spell an `EnumItem` differently and each builds its own, where a
		// `ScriptValue` — which crosses a world — has no tag for one and must not
		// gain one.
		{"MouseBehavior round-trips as an EnumItem",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Let(language, "before", "UserInputService.MouseBehavior.Name") +
					"UserInputService.MouseBehavior = Enum.MouseBehavior.LockCenter\n" +
					Say(language, Join(language, "before", "UserInputService.MouseBehavior.Name"));
		 },
		 "Default/LockCenter",
		 0,
		 Windowed},

		// **A bare string where an `EnumItem` is expected**, which is the same
		// latitude `part.AlphaMode = "Clip"` has and which `ScriptCall::ReadEnum`
		// gives both languages from one reader.
		{"and takes a bare member name",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					"UserInputService.MouseBehavior = 'LockCurrentPosition'\n" +
					Say(language, "UserInputService.MouseBehavior.Name");
		 },
		 "LockCurrentPosition",
		 0,
		 Windowed},

		{"MouseDeltaSensitivity sees its own write",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Let(language, "before", "UserInputService.MouseDeltaSensitivity") +
					"UserInputService.MouseDeltaSensitivity = 2\n" +
					Say(language, Join(language, "before", "UserInputService.MouseDeltaSensitivity"));
		 },
		 "1/2",
		 0,
		 Windowed},

		{"KeyboardEnabled is true on a world with a window",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language,
						Join(language, "UserInputService.KeyboardEnabled", "UserInputService.MouseEnabled"));
		 },
		 "true/true",
		 0,
		 Windowed},

		// **The headless answer, which is the ordinary one on a server.** A world
		// with no input state is a world nobody is typing at, and both languages
		// have to say so rather than raise.
		{"and false on one without",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language, "UserInputService.KeyboardEnabled");
		 },
		 "false"},

		// **Present and false, which is better than absent**: a Roblox place
		// branches on these to pick a control scheme.
		{"the devices this engine does not have are present and false",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language,
						Join(language, "UserInputService.TouchEnabled", "UserInputService.VREnabled"));
		 },
		 "false/false",
		 0,
		 Windowed},
	};

	for (const ParityCase &probe : CASES) {
		Both(probe);
	}
}

TEST_CASE("the two input surfaces answer the same in both languages", "[scripting][scriptcall]") {
	// **The methods that came across with the properties.** Four of the six on
	// `UserInputService` needed a return this interface did not have —
	// `ReturnVector2` for the pointer pair, `ReturnEnums` for the key list and
	// `ReturnInputObjects` for the button list — and each is here because its
	// caller asked, which is the rule `ScriptCall.hpp` states.
	const std::vector<ParityCase> CASES = {
		{"IsKeyDown finds a held key",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language, Call(language, "UserInputService", "IsKeyDown(Enum.KeyCode.E)"));
		 },
		 "true",
		 0,
		 KeyHeld},

		{"and not one nobody pressed",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language, Call(language, "UserInputService", "IsKeyDown(Enum.KeyCode.Q)"));
		 },
		 "false",
		 0,
		 KeyHeld},

		{"IsMouseButtonPressed finds a held button",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language,
						Call(
							language,
							"UserInputService",
							"IsMouseButtonPressed(Enum.UserInputType.MouseButton1)"
						));
		 },
		 "true",
		 0,
		 OneOfEverything},

		// **`Enum.UserInputType` names three sources that are not buttons**, and
		// "is `MouseMovement` pressed" is a question with no answer. False in
		// both rather than a cast past the end of the button bits.
		{"and answers false for a member that is not a button",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language,
						Call(
							language,
							"UserInputService",
							"IsMouseButtonPressed(Enum.UserInputType.MouseMovement)"
						));
		 },
		 "false",
		 0,
		 OneOfEverything},

		{"GetMouseLocation answers a Vector2",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language, Call(language, "UserInputService", "GetMouseLocation()") + ".Y");
		 },
		 "34",
		 0,
		 Windowed},

		{"GetMouseDelta answers this frame's motion",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language, Call(language, "UserInputService", "GetMouseDelta()") + ".X");
		 },
		 "3",
		 0,
		 OneOfEverything},

		// **A list of `EnumItem`s and not of strings**, so what comes out is what
		// `IsKeyDown` takes — the round trip a surface owes.
		{"GetKeysPressed answers what IsKeyDown would take",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language,
						First(language, Call(language, "UserInputService", "GetKeysPressed()")) + ".Name");
		 },
		 "E",
		 0,
		 KeyHeld},

		{"and is empty on a world with no window",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language, Length(language, Call(language, "UserInputService", "GetKeysPressed()")));
		 },
		 "0"},

		// **`InputObject`s and not `EnumItem`s**, which is Roblox's shape: the
		// object carries where the pointer was as well as which button it is.
		{"GetMouseButtonsPressed answers InputObjects",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language,
						First(language, Call(language, "UserInputService", "GetMouseButtonsPressed()")) +
							".UserInputType.Name");
		 },
		 "MouseButton1",
		 0,
		 OneOfEverything},

		{"and carries where the pointer was",
		 [](Language language) {
			 return AService(language, "UserInputService") +
					Say(language,
						First(language, Call(language, "UserInputService", "GetMouseButtonsPressed()")) +
							".Position.X");
		 },
		 "12",
		 0,
		 OneOfEverything},
	};

	for (const ParityCase &probe : CASES) {
		Both(probe);
	}
}

TEST_CASE("the listener pair reads the same in both languages", "[scripting][scriptcall]") {
	// **The one method on either surface that answers twice**, and therefore the
	// one place a caller writes two different lines. Luau takes two values and
	// JavaScript destructures the array `ScriptCall` packs them into — the same
	// class of difference as a Luau array being one-based, which is why the
	// bodies below differ and the *answers* do not.
	const std::vector<ParityCase> CASES = {
		{"a world nobody told listens through the camera",
		 [](Language language) {
			 const std::string take = language == Language::Luau
										  ? "local mode, ear = SoundService:GetListener()\n"
										  : "let [mode, ear] = SoundService.GetListener()\n";
			 return AService(language, "SoundService") + take +
					Say(language, Join(language, "mode.Name", IsNil(language, "ear")));
		 },
		 "Camera/true"},

		{"and keeps what SetListener was given",
		 [](Language language) {
			 const std::string take = language == Language::Luau
										  ? "local mode, ear = SoundService:GetListener()\n"
										  : "let [mode, ear] = SoundService.GetListener()\n";
			 return AService(language, "SoundService") + APart(language) + "part.Name = 'Ear'\n" +
					"part.Parent = workspace\n" +
					Send(language, "SoundService", "SetListener(Enum.ListenerType.ObjectPosition, part)") +
					take + Say(language, Join(language, "mode.Name", "ear.Name"));
		 },
		 "ObjectPosition/Ear"},

		// **Nil for an instance that has gone away**, rather than a handle to a
		// dead row — `client::SoundStage` falls back to the camera for the same
		// case, so the two agree about what the setting now means.
		{"a destroyed listener reads as nothing",
		 [](Language language) {
			 const std::string take = language == Language::Luau
										  ? "local mode, ear = SoundService:GetListener()\n"
										  : "let [mode, ear] = SoundService.GetListener()\n";
			 return AService(language, "SoundService") + APart(language) + "part.Parent = workspace\n" +
					Send(language, "SoundService", "SetListener(Enum.ListenerType.ObjectPosition, part)") +
					Send(language, "part", "Destroy()") + take +
					Say(language, Join(language, "mode.Name", IsNil(language, "ear")));
		 },
		 "ObjectPosition/true"},
	};

	for (const ParityCase &probe : CASES) {
		Both(probe);
	}
}

TEST_CASE("an input signal delivers the same report in both languages", "[scripting][scriptcall]") {
	// **A binding is not a pump, and shipping one without the other is the
	// failure this module names twice.** `UserInputService`'s five signals became
	// reachable from JavaScript with the service; a signal that exists and never
	// fires reads as a broken engine rather than an unfinished one, so
	// `PumpJsInput` grew the whole of `PumpInput`'s signal half at the same time.
	const std::vector<ParityCase> CASES = {
		{"InputBegan carries the key that moved",
		 [](Language language) {
			 if (language == Language::Luau) {
				 return AService(language, "UserInputService") +
						"UserInputService.InputBegan:Connect(function(input)\n"
						"	workspace.Name = input.KeyCode.Name\n"
						"end)\n";
			 }
			 return AService(language, "UserInputService") +
					"UserInputService.InputBegan.Connect(function (input) {\n"
					"	workspace.Name = input.KeyCode.Name\n"
					"})\n";
		 },
		 "E",
		 1,
		 KeyHeld},

		// **`InputChanged` was reachable and silent for six versions in Luau and
		// would have been born silent here.** Motion and the wheel are the two
		// things this engine can report changing.
		{"InputChanged carries the pointer's motion",
		 [](Language language) {
			 if (language == Language::Luau) {
				 return AService(language, "UserInputService") +
						"UserInputService.InputChanged:Connect(function(input)\n"
						"	workspace.Name = input.Delta.Y .. '/' .. input.UserInputType.Name\n"
						"end)\n";
			 }
			 return AService(language, "UserInputService") +
					"UserInputService.InputChanged.Connect(function (input) {\n"
					"	workspace.Name = input.Delta.Y + '/' + input.UserInputType.Name\n"
					"})\n";
		 },
		 "4/MouseMovement",
		 1,
		 OneOfEverything},

		// **The focus pair is called with nothing**, which is Roblox's signature
		// and the reason `FireInputSignal` takes a null report.
		{"WindowFocused fires with no argument",
		 [](Language language) {
			 if (language == Language::Luau) {
				 return AService(language, "UserInputService") +
						"UserInputService.WindowFocused:Connect(function()\n"
						"	workspace.Name = 'focused'\n"
						"end)\n";
			 }
			 return AService(language, "UserInputService") +
					"UserInputService.WindowFocused.Connect(function () {\n"
					"	workspace.Name = 'focused'\n"
					"})\n";
		 },
		 "focused",
		 1,
		 FocusRegained},

		// **The five share one `SignalKind` and are told apart by name**, so a
		// listener on one must not hear another's edge. This is the case that
		// would fail if the name filter were dropped.
		{"a listener on one signal does not hear another",
		 [](Language language) {
			 if (language == Language::Luau) {
				 return AService(language, "UserInputService") +
						"UserInputService.InputEnded:Connect(function()\n"
						"	workspace.Name = 'ended'\n"
						"end)\n" +
						Say(language, "'quiet'");
			 }
			 return AService(language, "UserInputService") +
					"UserInputService.InputEnded.Connect(function () {\n"
					"	workspace.Name = 'ended'\n"
					"})\n" +
					Say(language, "'quiet'");
		 },
		 "quiet",
		 1,
		 KeyHeld},
	};

	for (const ParityCase &probe : CASES) {
		Both(probe);
	}
}

TEST_CASE("a service property refuses the same writes in both languages", "[scripting][scriptcall]") {
	// **A read-only property refuses by name in both**, which is what the null
	// setter on a `ServiceProperty` row means. On the Luau side that falls out of
	// the userdata's `__newindex`; on this one the accessor pair is installed
	// with a setter that raises, because leaving it undefined would drop the
	// write silently in sloppy mode.
	//
	// **The unknown name is here too, and it needed the object sealed.** A Luau
	// service with properties is a userdata and a userdata has no fields, so a
	// typo raises; an extensible JavaScript object would have kept it as a new
	// property and read the typo back forever.
	const std::vector<std::pair<const char *, const char *>> PROBES = {
		{"UserInputService", "TouchEnabled = true"},
		{"UserInputService", "KeyboardEnabled = false"},
		{"SoundService", "AmbientReverb = 1"},
		{"UserInputService", "MouseIconEnabled = true"},

		// A wrong type for a property that *is* writable, which is the reader
		// raising rather than the row refusing.
		{"SoundService", "Volume = {}"},
		{"UserInputService", "MouseBehavior = 5"},
	};

	for (const Language language : LANGUAGES) {
		for (const auto &[service, probe] : PROBES) {
			Store store = Fresh("scriptcall_property_refusal");
			const auto runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);

			const std::string source =
				AService(language, service) + std::string(service) + "." + probe + "\n";

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
