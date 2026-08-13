// What an editor is told a script may name, checked against what a script
// actually may name.
//
// **The failure this exists to catch has happened twice in this module and both
// times it was silent.** `Values.cpp` records `Magnitude` and `Unit`, promised
// by `engine.d.luau` for two versions while the run time answered "Vector3 has
// no member 'Unit'" — a script that typechecked clean and failed anyway.
// `JsSurface.cpp` records a hand-written `10` on a list of sixteen methods,
// which left the last six uninstalled with nothing warning. Both are the same
// shape: a description of the surface kept somewhere other than the surface.
//
// So the assertions here run *against a live VM*. Offering a name that does not
// resolve is worse than offering nothing — an author picks it out of a list,
// writes it, and finds out at run time in whatever scene reaches that line
// first, which is exactly the experience `Vocabulary.hpp` was written to end.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/Vocabulary.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.script.vocabulary")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::NameKind;
using engine::script::Runtime;
using engine::script::ScriptSurface;
using engine::script::VocabularyEntry;

namespace {

	// A runtime over a furnished world, because `workspace` and `game` are only
	// worth describing when the services they reach actually exist.
	struct Vm {
		Store World{"vocabulary"};
		std::unique_ptr<Runtime> Script;

		explicit Vm(const Language language) {
			engine::scene::InstallServices(World);
			Script = MakeRuntime(World, language);
		}
	};

	bool Has(const std::vector<std::string> &names, const std::string_view wanted) {
		return std::find(names.begin(), names.end(), wanted) != names.end();
	}

	bool HasGlobal(const ScriptSurface &surface, const std::string_view wanted) {
		return std::any_of(surface.Globals.begin(), surface.Globals.end(), [wanted](const auto &entry) {
			return entry.Name == wanted;
		});
	}

	const VocabularyEntry *GlobalNamed(const ScriptSurface &surface, const std::string_view wanted) {
		for (const VocabularyEntry &entry : surface.Globals) {
			if (entry.Name == wanted) {
				return &entry;
			}
		}
		return nullptr;
	}

}

TEST_CASE("a runtime describes the surface it installed", "[script][vocabulary]") {
	// The names an author reaches for first. Not an exhaustive list — the point
	// of walking the VM is that nothing here has to be — but enough that a walk
	// which quietly returned nothing is a failure rather than a pass.
	//
	// **Both VMs, and only what both have.** The two genuinely differ, and
	// `UserInputService` being Luau's alone is a fact about the engine rather
	// than a gap; asserting it here for JavaScript would be asserting a bug.
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		Vm vm(language);
		REQUIRE(vm.Script != nullptr);

		const ScriptSurface surface = vm.Script->Surface();

		INFO("language " << static_cast<int>(language) << ", " << surface.Globals.size() << " globals");

		for (const std::string_view expected :
			 {"Instance", "workspace", "game", "task", "Enum", "World", "Vector3", "CFrame", "print"}) {
			INFO("global " << expected);
			CHECK(HasGlobal(surface, expected));
		}

		// `task` is a plain table in both VMs, which is what makes it the
		// honest test of the member walk: a container whose members come from
		// an `__index` function would answer empty and prove nothing.
		const VocabularyEntry *task = GlobalNamed(surface, "task");
		REQUIRE(task != nullptr);
		CHECK(task->Kind == NameKind::Container);
		for (const std::string_view expected : {"wait", "spawn", "defer", "delay", "cancel"}) {
			INFO("task." << expected);
			CHECK(Has(task->Members, expected));
		}

		// The instance surface, which is a different table from the globals in
		// both VMs and so a separate way for the walk to be wrong.
		for (const std::string_view expected :
			 {"Destroy", "Clone", "GetChildren", "FindFirstChild", "IsA", "SetComponent"}) {
			INFO("instance member " << expected);
			CHECK(Has(surface.InstanceMembers, expected));
		}
	}
}

TEST_CASE("every offered instance member resolves on a real instance", "[script][vocabulary]") {
	// **This is the one that would have caught both historical bugs**, and it
	// is written as a loop over what is offered rather than as a list of names,
	// so it grows with the surface instead of having to be remembered.
	//
	// It matters most for the Luau signals. Methods live in a table the walk
	// reads, so they cannot be wrong by construction; `.Changed` and the
	// thirteen beside it are a chain of string comparisons in `InstanceIndex`
	// with a list kept next to them, and a list beside a branch is exactly the
	// arrangement that goes stale. Nothing can enumerate a branch, so nothing
	// can check the other direction — a signal added to the chain and not to
	// the list is invisible here, and that is stated rather than papered over.
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		Vm vm(language);
		REQUIRE(vm.Script != nullptr);

		// Before anything runs. JavaScript's chunks share one global object, so
		// a script that assigned a global would show up as engine surface.
		const ScriptSurface surface = vm.Script->Surface();
		REQUIRE_FALSE(surface.InstanceMembers.empty());

		const bool luau = language == Language::Luau;

		// `Instance.new` in both, and JavaScript's is a property rather than a
		// construct signature — `new Instance("Part")` is the shape the
		// bindings deliberately did not take, so that one spelling works in
		// both languages.
		std::string source =
			luau ? "local subject = Instance.new(\"Part\")\n" : "var subject = Instance.new(\"Part\");\n";

		for (const std::string &member : surface.InstanceMembers) {
			if (luau) {
				source += "assert(subject." + member + " ~= nil, \"" + member + "\")\n";
			} else {
				source +=
					"if (subject." + member + " === undefined) { throw new Error(\"" + member + "\"); }\n";
			}
		}

		INFO("language " << static_cast<int>(language));
		const bool ran = vm.Script->Run(source);
		INFO(vm.Script->LastError());
		CHECK(ran);
	}
}

TEST_CASE("a withheld name is one the VM actually installs", "[script][vocabulary]") {
	// The list of names not to offer is only worth having while every entry is
	// real. An entry for a global that no longer exists is dead weight that
	// reads as a deliberate refusal, and the next person to look would go
	// hunting for a stub that was deleted two versions ago.
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		Vm vm(language);
		REQUIRE(vm.Script != nullptr);

		const ScriptSurface surface = vm.Script->Surface();

		for (const std::string_view withheld : engine::script::Withheld(language)) {
			INFO("withheld " << withheld << " for language " << static_cast<int>(language));
			CHECK(HasGlobal(surface, withheld));
		}
	}
}

TEST_CASE("the two languages are described apart", "[script][vocabulary]") {
	// **Not a tidiness check.** `engine.d.ts` once declared `UserInputService`
	// and `ContextActionService` for JavaScript because its prelude was written
	// by mirroring Luau's, and neither global existed in that VM — a TypeScript
	// file naming one typechecks and then fails. The walk cannot make that
	// mistake, and this pins the property so that a future convenience does not
	// reintroduce it.
	//
	// **`ContextActionService` left this list at v0.16 and `UserInputService` and
	// `SoundService` followed it**, which is the honest half of the same check:
	// each is described once now and both VMs install it, so asserting an absence
	// here would be asserting a gap that has been closed. The last two went when
	// `ServiceProperty` gave a live property a neutral shape — a userdata's
	// `__index` on one side and `JS_DefinePropertyGetSet` on the other, from one
	// list of names.
	//
	// **`require` is what is left, and it is not a service.** Modules are
	// Luau-only because `OpenRequire` compiles a chunk, and a JavaScript one
	// would be a second loader rather than a second binding — which is the same
	// shape as `BreakpointService`, absent here only because a plain runtime
	// installs no studio service in either VM.
	Vm luau(Language::Luau);
	Vm javascript(Language::JavaScript);

	const ScriptSurface luauSurface = luau.Script->Surface();
	const ScriptSurface javascriptSurface = javascript.Script->Surface();

	for (const std::string_view luauOnly : {"require"}) {
		INFO(luauOnly);
		CHECK(HasGlobal(luauSurface, luauOnly));
		CHECK_FALSE(HasGlobal(javascriptSurface, luauOnly));
	}

	// The other direction, so this is a description of the difference rather
	// than a claim that one VM is a subset of the other.
	for (const std::string_view javascriptOnly : {"typeOf", "nil"}) {
		INFO(javascriptOnly);
		CHECK(HasGlobal(javascriptSurface, javascriptOnly));
		CHECK_FALSE(HasGlobal(luauSurface, javascriptOnly));
	}
}

TEST_CASE("keywords are offered and are not engine names", "[script][vocabulary]") {
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		const std::span<const std::string_view> keywords = engine::script::Keywords(language);
		REQUIRE_FALSE(keywords.empty());

		// Sorted, because the completion list breaks ties by name and a source
		// in registration order would put two runs in a different order for no
		// reason anybody could see.
		CHECK(std::is_sorted(keywords.begin(), keywords.end()));
	}

	const std::span<const std::string_view> luau = engine::script::Keywords(Language::Luau);
	CHECK(std::find(luau.begin(), luau.end(), "local") != luau.end());

	const std::span<const std::string_view> javascript = engine::script::Keywords(Language::JavaScript);
	CHECK(std::find(javascript.begin(), javascript.end(), "const") != javascript.end());
	CHECK(std::find(javascript.begin(), javascript.end(), "local") == javascript.end());
}
