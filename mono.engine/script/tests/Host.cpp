// The host seam, from both directions.
//
// **What is under test is the crossing, not a host.** A studio's plugin API is
// `mono.studio`'s and has its own suite; what belongs here is that a value
// survives the trip, that a function handed over can be called back, and that a
// refusal arrives as an ordinary script error rather than as a crash.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Host.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.script.host")
TEST_DEPENDS("engine.script.scripting")

using engine::ecs::Entity;
using engine::ecs::Store;
using engine::script::HostArguments;
using engine::script::HostCallback;
using engine::script::HostSurface;
using engine::script::HostTag;
using engine::script::HostValue;
using engine::script::Language;
using engine::script::MakeRuntime;

namespace {
	// A host that records what it was asked and answers what it was told to.
	class Recorder final : public HostSurface {
	  public:
		std::string_view GlobalName() const override {
			return "test";
		}

		std::vector<std::string> Names() const override {
			return {"Echo", "Remember", "Refuse", "Instances", "Table"};
		}

		bool Call(
			std::string_view name, HostArguments arguments, HostValue &result, std::string &failure
		) override {
			Called.emplace_back(name);

			if (name == "Refuse") {
				failure = "told to";
				return false;
			}
			if (name == "Remember") {
				if (!arguments.empty() && arguments[0].Tag == HostTag::Callback) {
					Handler = arguments[0].Callback;
				}
				return true;
			}
			if (name == "Instances") {
				result = HostValue::List({HostValue::Of(First), HostValue::Of(Second)});
				return true;
			}
			if (name == "Table") {
				HostValue map(HostTag::Map);
				map.Entries.emplace_back("count", HostValue::Of(2.0));
				map.Entries.emplace_back("name", HostValue::Of(std::string_view("thing")));
				result = map;
				return true;
			}

			// Echo: hand the first argument straight back, whatever it was.
			if (!arguments.empty()) {
				Seen = arguments[0];
				result = arguments[0];
			}
			return true;
		}

		std::vector<std::string> Called;
		HostValue Seen;
		HostCallback Handler;
		Entity First;
		Entity Second;
	};
}

TEST_CASE("a host installs its own global and only its own names", "[script][host]") {
	engine::scene::EnsureClassTree();
	Store store("host");

	Recorder host;
	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->SetHost(&host);

	// **The host names the global**, so an editor's API reads as `plugin` and a
	// test's as `test` rather than both as plumbing.
	REQUIRE(runtime->Run("assert(type(test) == 'table', 'no host global')"));

	// A name the host does not list is not a member, which turns a typo into an
	// error at the call site rather than a refusal from inside the program.
	CHECK(runtime->Run("assert(test.Missing == nil, 'an unlisted name is a member')"));

	// And a world with no host has no global at all.
	const auto bare = MakeRuntime(store, Language::Luau);
	CHECK(bare->Run("assert(test == nil, 'a game script got a host')"));
}

TEST_CASE("every value shape survives the crossing", "[script][host]") {
	engine::scene::EnsureClassTree();
	Store store("host");

	Recorder host;
	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->SetHost(&host);

	REQUIRE(runtime->Run(
		"assert(test.Echo(7) == 7, 'number')\n"
		"assert(test.Echo('hello') == 'hello', 'string')\n"
		"assert(test.Echo(true) == true, 'boolean')\n"
		"assert(test.Echo(nil) == nil, 'nil')\n"
		"assert(test.Echo(Vector3.new(1, 2, 3)) == Vector3.new(1, 2, 3), 'Vector3')\n"
		"local list = test.Echo({1, 2, 3})\n"
		"assert(#list == 3 and list[2] == 2, 'array')\n"
		"local map = test.Echo({ a = 1, b = 'two' })\n"
		"assert(map.a == 1 and map.b == 'two', 'map')\n"
	));

	// **An instance crosses, which is the whole reason this is not
	// `ScriptValue`.** A handle means something inside one process against one
	// store, and an editor answering "what is selected" has to be able to use it.
	Entity part;
	{ part = store.CreateInstance(engine::scene::PartClass(), "Crate"); }
	host.First = part;
	host.Second = store.CreateInstance(engine::scene::PartClass(), "Barrel");

	REQUIRE(runtime->Run(
		"local held = test.Instances()\n"
		"assert(#held == 2, 'two instances')\n"
		"assert(held[1].Name == 'Crate', 'the first crossed')\n"
		"assert(held[2].Name == 'Barrel', 'the second crossed')\n"
		"assert(test.Echo(held[1]) == held[1], 'and back again')\n"
	));

	// A map answered by the host reads as a table.
	REQUIRE(runtime->Run(
		"local answer = test.Table()\n"
		"assert(answer.count == 2 and answer.name == 'thing', 'a map came back wrong')\n"
	));
}

TEST_CASE("a refusal is a script error naming the reason", "[script][host]") {
	engine::scene::EnsureClassTree();
	Store store("host");

	Recorder host;
	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->SetHost(&host);

	// **An ordinary error, not a crash.** A host that aborted would take the
	// program down with a plugin's typo.
	CHECK_FALSE(runtime->Run("test.Refuse()"));
	CHECK(runtime->LastError().find("told to") != std::string::npos);

	// The program is still usable afterwards, which is the property a refusal
	// has and an abort does not.
	CHECK(runtime->Run("assert(test.Echo(1) == 1)"));

	// A script may catch it like any other error.
	CHECK(runtime->Run(
		"local ok, message = pcall(function() test.Refuse() end)\n"
		"assert(not ok, 'the refusal did not raise')\n"
		"assert(string.find(message, 'told to') ~= nil, 'the reason was lost: ' .. message)\n"
	));
}

TEST_CASE("a function handed over can be called back", "[script][host]") {
	engine::scene::EnsureClassTree();
	Store store("host");

	Recorder host;
	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->SetHost(&host);

	REQUIRE(runtime->Run(
		"calls = 0\n"
		"lastArgument = nil\n"
		"test.Remember(function(value)\n"
		"  calls += 1\n"
		"  lastArgument = value\n"
		"end)\n"
	));

	REQUIRE(host.Handler.Valid());

	// **The other direction of the seam**, which is what makes a button
	// possible: the handler lives in the plugin's VM and the press happens in
	// the host's frame.
	const HostValue argument = HostValue::Of(std::string_view("pressed"));
	const HostValue arguments[] = {argument};

	CHECK(runtime->Invoke(host.Handler, arguments));
	CHECK(runtime->Invoke(host.Handler, arguments));

	// The chunk's globals are its own, so the count is read by asking the same
	// runtime rather than by reaching into it.
	CHECK(runtime->Run("assert(calls == nil, 'a second chunk shares globals')"));

	// An unknown callback is refused rather than crashing.
	CHECK_FALSE(runtime->Invoke(HostCallback{999}, {}));
	CHECK_FALSE(runtime->Invoke(HostCallback{}, {}));

	// Released, and then it is unknown too.
	runtime->Release(host.Handler);
	CHECK_FALSE(runtime->Invoke(host.Handler, arguments));
}

TEST_CASE("a handler that raises is reported rather than propagated", "[script][host]") {
	engine::scene::EnsureClassTree();
	Store store("host");

	Recorder host;
	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->SetHost(&host);

	REQUIRE(runtime->Run("test.Remember(function() error('inside the handler') end)"));
	REQUIRE(host.Handler.Valid());

	// **False rather than a throw**, because the caller is a host drawing a
	// frame: a button whose handler is wrong must not take the editor with it.
	CHECK_FALSE(runtime->Invoke(host.Handler, {}));

	// And the runtime still works.
	CHECK(runtime->Run("assert(test.Echo(1) == 1)"));
}

TEST_CASE("a value with no host representation is refused at the call", "[script][host]") {
	engine::scene::EnsureClassTree();
	Store store("host");

	Recorder host;
	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->SetHost(&host);

	// A coroutine has nothing a host could hold. Named at the argument rather
	// than silently becoming nil, which would be a host acting on an argument
	// the script did not pass.
	CHECK_FALSE(runtime->Run("test.Echo(coroutine.create(function() end))"));

	// A table nested past the depth limit is the other one, and it is what
	// stops a self-referencing table recursing until the stack runs out.
	CHECK_FALSE(runtime->Run(
		"local deep = {}\n"
		"local at = deep\n"
		"for _ = 1, 40 do at.next = {}; at = at.next end\n"
		"test.Echo(deep)\n"
	));

	CHECK(runtime->Run("assert(test.Echo(1) == 1)"));
}
