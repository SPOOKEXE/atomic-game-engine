// What the script editor offers, and where it decides that from.
//
// **Both halves fail silently, which is why they are reachable from here.** A
// scanner that misreads the caret offers the wrong list — and a wrong list looks
// exactly like an engine whose API does not have the thing you wanted. A list
// containing a name the VM does not have is worse: an author picks it, writes
// it, and finds out at run time in whatever scene reaches that line first.
// Neither needs a window to happen, and neither is visible in a screenshot.
//
// `engine.script.vocabulary` covers the other end of the same pipe — that the
// names handed to this file are names a VM actually installs. This covers what
// is done with them.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Vocabulary.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <studio/Complete.hpp>
#include <vector>

TEST_SUITE_ID("studio.complete")
TEST_DEPENDS("engine.scene.part")

using engine::ecs::Classes;
using engine::script::Language;
using engine::script::NameKind;
using engine::script::ScriptSurface;
using studio::CompleteAt;
using studio::Completion;
using studio::CompletionKind;
using studio::CompletionQuery;
using studio::CompletionSources;
using studio::InsertableClasses;
using studio::ScanBackwards;

namespace {

	// The caret is written as `|` so that a case reads as the thing somebody
	// typed rather than as a string and a number that have to be counted apart.
	CompletionQuery Scan(const std::string &marked) {
		const size_t caret = marked.find('|');
		REQUIRE(caret != std::string::npos);

		static std::string text;
		text = marked;
		text.erase(caret, 1);
		return ScanBackwards(text, caret);
	}

	// A surface standing in for a walked VM, so these cases do not depend on
	// which globals the engine happens to install today — only on what this
	// file does with whatever it is handed.
	ScriptSurface Surface() {
		ScriptSurface surface;
		surface.Globals.push_back({"workspace", NameKind::Value, {}});
		surface.Globals.push_back({"print", NameKind::Function, {}});
		surface.Globals.push_back({"task", NameKind::Container, {"wait", "spawn", "defer"}});
		surface.InstanceMembers = {"Destroy", "FindFirstChild", "GetChildren", "Changed"};
		return surface;
	}

	std::vector<Completion> Complete(
		const std::string &marked,
		const std::vector<std::string> &children = {},
		const Language language = Language::Luau
	) {
		const size_t caret = marked.find('|');
		REQUIRE(caret != std::string::npos);

		std::string text = marked;
		text.erase(caret, 1);

		static ScriptSurface surface;
		surface = Surface();

		CompletionSources sources;
		sources.Language = language;
		sources.Surface = &surface;
		sources.Children = children;

		return CompleteAt(text, caret, sources);
	}

	bool Offers(const std::vector<Completion> &entries, const std::string_view wanted) {
		return std::any_of(entries.begin(), entries.end(), [wanted](const Completion &entry) {
			return entry.Text == wanted;
		});
	}

	// The dimmed hint beside one entry, which is where a narrowed list says
	// whose property it is offering.
	std::string Detail(const std::vector<Completion> &entries, const std::string_view wanted) {
		const auto hit = std::find_if(entries.begin(), entries.end(), [wanted](const Completion &entry) {
			return entry.Text == wanted;
		});
		return hit == entries.end() ? std::string("<not offered>") : hit->Detail;
	}

	struct Fixture {
		Fixture() {
			engine::scene::EnsureClassTree();
		}
	};

}

TEST_CASE("the scanner reads what is under the caret", "[studio][complete]") {
	// **The separator is kept rather than folded into a boolean**, because Luau
	// uses the two for different things — `part:Destroy()` passes the instance
	// and `part.Name` does not — so a scanner that reported "there was a
	// separator" would leave the caller unable to tell a method from a property.
	SECTION("a bare word is a prefix and nothing else") {
		const CompletionQuery query = Scan("local wor|");
		CHECK(query.Prefix == "wor");
		CHECK(query.Subject.empty());
		CHECK(query.Separator == '\0');
		CHECK_FALSE(query.InString);
	}

	SECTION("a trailing dot has a subject and an empty prefix") {
		const CompletionQuery query = Scan("part.|");
		CHECK(query.Prefix.empty());
		CHECK(query.Subject == "part");
		CHECK(query.Separator == '.');
	}

	SECTION("a colon is not a dot") {
		const CompletionQuery query = Scan("part:Des|");
		CHECK(query.Prefix == "Des");
		CHECK(query.Subject == "part");
		CHECK(query.Separator == ':');
	}

	SECTION("a chain is kept whole") {
		const CompletionQuery query = Scan("Enum.Material.Pl|");
		CHECK(query.Prefix == "Pl");
		CHECK(query.Subject == "Enum.Material");
	}

	SECTION("a caret past the end is clamped rather than read off the end") {
		const CompletionQuery query = ScanBackwards("abc", 99);
		CHECK(query.Prefix == "abc");
	}
}

TEST_CASE("the scanner finds the call a string sits in", "[studio][complete]") {
	SECTION("a class name argument") {
		const CompletionQuery query = Scan(R"(local p = Instance.new("Pa|)");
		CHECK(query.InString);
		CHECK(query.Prefix == "Pa");
		CHECK(query.Call == "Instance.new");
	}

	SECTION("a method call keeps its receiver") {
		const CompletionQuery query = Scan(R"(if part:IsA("Base|)");
		CHECK(query.InString);
		CHECK(query.Call == "part:IsA");
	}

	SECTION("a closed string is not a string") {
		// The case that decides whether completion works at all on a line that
		// already contains one: a quote count that did not pair up would switch
		// the popup off for the rest of the line.
		const CompletionQuery query = Scan(R"(print("hi") then wor|)");
		CHECK_FALSE(query.InString);
		CHECK(query.Prefix == "wor");
	}

	SECTION("an escaped quote does not close the string") {
		const CompletionQuery query = Scan(R"(print("a\"b|)");
		CHECK(query.InString);
	}

	SECTION("a string on a previous line does not leak into this one") {
		// Counted from the start of the line rather than the file, because one
		// apostrophe in a comment would otherwise disable everything below it.
		const CompletionQuery query = Scan("-- don't\nwor|");
		CHECK_FALSE(query.InString);
		CHECK(query.Prefix == "wor");
	}
}

TEST_CASE("a class name is offered where a class name goes", "[studio][complete]") {
	const Fixture fixture;

	SECTION("Instance.new offers what can be inserted") {
		const std::vector<Completion> entries = Complete(R"(Instance.new("Pa|)");
		REQUIRE_FALSE(entries.empty());
		CHECK(Offers(entries, "Part"));

		// **The abstract bases are refused here and nowhere else.** The run time
		// would mint an `Instance` perfectly happily — `LuauInstances.cpp` looks the
		// name up and takes whatever it finds — so offering one produces a row
		// nothing knows how to draw.
		CHECK_FALSE(Offers(entries, "PVInstance"));
		CHECK_FALSE(Offers(entries, "BasePart"));

		for (const Completion &entry : entries) {
			CHECK(entry.Kind == CompletionKind::Class);
		}
	}

	SECTION("IsA offers the bases, because that is what it is for") {
		const std::vector<Completion> entries = Complete(R"(part:IsA("Base|)");
		CHECK(Offers(entries, "BasePart"));
	}

	SECTION("prose in a string is left alone") {
		// The popup must not appear over every quoted literal in the file. A
		// list of eighty class names on top of a chat message is worse than no
		// completion at all.
		CHECK(Complete(R"(print("hel|)").empty());
	}
}

TEST_CASE("a dot and a colon offer different things", "[studio][complete]") {
	const Fixture fixture;

	// Luau's colon passes the instance and its dot does not, so a method after
	// a dot would be a call missing its first argument.
	const std::vector<Completion> methods = Complete("part:Des|");
	CHECK(Offers(methods, "Destroy"));

	const std::vector<Completion> properties = Complete("part.Anch|");
	CHECK(Offers(properties, "Anchored"));
	CHECK_FALSE(Offers(properties, "Destroy"));
}

TEST_CASE("a local declared with Instance.new resolves to its class", "[studio][complete]") {
	const Fixture fixture;

	// **Read rather than inferred.** The class is written on the line, so
	// resolving it is not type inference and does not pretend to be — a local
	// from `FindFirstChild` falls back to the union, which is a longer list and
	// never a wrong one.
	const std::vector<Completion> entries = Complete("local p = Instance.new(\"Part\")\np.Anch|");
	CHECK(Offers(entries, "Anchored"));

	// **Every row says whose property it is.** A narrowed row names the class
	// it is claiming for and a union row does not name one at all, which is the
	// only thing that lets an author tell "Part has this" from "something has
	// this" — see `Complete.hpp`.
	CHECK(Detail(entries, "Anchored") == "bool on Part");
}

TEST_CASE("an assignment is followed only where the class is written", "[studio][complete]") {
	const Fixture fixture;

	SECTION("a clone is its receiver's class") {
		// A clone of a `Part` is a `Part` whatever else is true of the file, so
		// carrying the class across `:Clone()` is reading rather than guessing.
		const std::vector<Completion> entries =
			Complete("local part = Instance.new(\"Part\")\nlocal copy = part:Clone()\ncopy.Anch|");
		CHECK(Detail(entries, "Anchored") == "bool on Part");
	}

	SECTION("an alias carries the class it was given") {
		const std::vector<Completion> entries =
			Complete("local part = Instance.new(\"Part\")\nlocal same = part\nsame.Anch|");
		CHECK(Detail(entries, "Anchored") == "bool on Part");
	}

	SECTION("a class named in the call is read out of it") {
		const std::vector<Completion> entries =
			Complete("local hit = workspace:FindFirstChildWhichIsA(\"BasePart\")\nhit.Anch|");
		CHECK(Detail(entries, "Anchored") == "bool on BasePart");
	}

	SECTION("FindFirstChild is a child of unknown class, so the union stands") {
		// **The case that decides whether this feature is worth having.** A
		// child of a `Model` is not a `Model`, so narrowing to the receiver
		// would offer `Model`'s properties for a `Part` — a list that says
		// "this class has this" and is wrong, which an author cannot tell from
		// the truth. The union says "one of these classes has this" instead.
		const std::vector<Completion> entries =
			Complete("local m = Instance.new(\"Model\")\nlocal p = m:FindFirstChild(\"Hit\")\np.Anch|");
		CHECK(Offers(entries, "Anchored"));
		CHECK(Detail(entries, "Anchored") == "bool on some class");
	}

	SECTION("a later assignment nobody can read undoes an earlier one") {
		// The stale answer is the dangerous one: the local was a `Part` and is
		// now whatever the tree held, so the class written two lines up is no
		// longer a fact about it.
		const std::vector<Completion> entries = Complete(
			"local p = Instance.new(\"Part\")\nlocal m = Instance.new(\"Model\")\np = "
			"m:FindFirstChild(\"Hit\")\np.Anch|"
		);
		CHECK(Detail(entries, "Anchored") == "bool on some class");
	}

	SECTION("a trailing comment is not part of the expression") {
		// Both comment markers, because both languages are read by the same
		// code — and a declaration with a note beside it is the ordinary way
		// somebody writes one.
		CHECK(
			Detail(Complete("local p = Instance.new(\"Part\") -- the hitbox\np.Anch|"), "Anchored") ==
			"bool on Part"
		);
		CHECK(
			Detail(
				Complete(
					"const p = Instance.new(\"Part\"); // the hitbox\np.Anch|", {}, Language::JavaScript
				),
				"Anchored"
			) == "bool on Part"
		);
	}

	SECTION("a comparison is not an assignment") {
		const std::vector<Completion> entries =
			Complete("local p = Instance.new(\"Part\")\nif p == other then\np.Anch|");
		CHECK(Detail(entries, "Anchored") == "bool on Part");
	}

	SECTION("JavaScript is the same rule with the other accessor") {
		// The feature was asked for in both languages, and neither the shapes
		// being followed nor the class names in them are Luau's — `.Clone()`
		// and `:Clone()` are one rule.
		const std::vector<Completion> entries = Complete(
			"const part = Instance.new(\"Part\");\nconst copy = part.Clone();\ncopy.Anch|",
			{},
			Language::JavaScript
		);
		CHECK(Detail(entries, "Anchored") == "bool on Part");
	}
}

TEST_CASE("a global's members come from the walk", "[studio][complete]") {
	const Fixture fixture;

	const std::vector<Completion> entries = Complete("task.wa|");
	CHECK(Offers(entries, "wait"));

	// Not the instance surface: `task` is a table and has nothing to do with an
	// instance, so offering `Destroy` under it would be noise from the fallback.
	CHECK_FALSE(Offers(entries, "Destroy"));
}

TEST_CASE("the tree beside a script is offered", "[studio][complete]") {
	const Fixture fixture;

	// The one thing an external language server cannot know, and the reason
	// completion in this editor is worth having at all rather than deferring to
	// luau-lsp.
	const std::vector<Completion> entries = Complete("script.Parent.Hit|", {"HitBox", "Model"});
	CHECK(Offers(entries, "HitBox"));
}

TEST_CASE("keywords and file identifiers fill an empty line", "[studio][complete]") {
	const Fixture fixture;

	const std::vector<Completion> entries = Complete("local counter = 1\nloc|");
	CHECK(Offers(entries, "local"));

	// The word being typed is not offered back, and a word already in the file
	// is — which is what makes completion useful on somebody's own code.
	const std::vector<Completion> named = Complete("local counter = 1\ncoun|");
	CHECK(Offers(named, "counter"));
}

TEST_CASE("a missing VM degrades the list rather than emptying it", "[studio][complete]") {
	const Fixture fixture;

	// A runtime that could not be made must not take classes, properties and
	// keywords down with it: a popup with no engine names is a worse editor,
	// and a popup with nothing at all is a broken one.
	CompletionSources sources;
	sources.Language = Language::Luau;
	sources.Surface = nullptr;

	const std::string text = "part.Anch";
	const std::vector<Completion> entries = CompleteAt(text, text.size(), sources);
	CHECK(Offers(entries, "Anchored"));
}

TEST_CASE("insertable classes exclude services and abstract bases", "[studio][complete]") {
	const Fixture fixture;
	engine::ecs::Store world("complete_services");
	engine::scene::InstallServices(world);

	std::vector<std::string> names;
	for (const engine::ecs::ClassId id : InsertableClasses()) {
		names.emplace_back(Classes::Describe(id).Name.Text());
	}

	REQUIRE_FALSE(names.empty());
	CHECK(std::find(names.begin(), names.end(), "Part") != names.end());

	// **A category rather than nine names.** A world has exactly one of each
	// service, so offering one offers a second that nothing resolves.
	CHECK(std::find(names.begin(), names.end(), "Workspace") == names.end());
	CHECK(std::find(names.begin(), names.end(), "Instance") == names.end());
}

TEST_CASE("the list is ranked and stable", "[studio][complete]") {
	const Fixture fixture;

	const std::vector<Completion> first = Complete("part.Anch|");
	const std::vector<Completion> second = Complete("part.Anch|");

	REQUIRE_FALSE(first.empty());
	REQUIRE(first.size() == second.size());

	// Ties break by name rather than by the order the class table happened to
	// be walked in, so two runs of the same editor agree.
	for (size_t index = 0; index < first.size(); index++) {
		CHECK(first[index].Text == second[index].Text);
	}

	CHECK(std::is_sorted(first.begin(), first.end(), [](const Completion &a, const Completion &b) {
		return a.Score > b.Score || (a.Score == b.Score && a.Text < b.Text);
	}));
}
