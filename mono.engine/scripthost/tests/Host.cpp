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
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.scripthost.host")
TEST_DEPENDS("engine.scripthost.scripting")

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
			// A dotted name is a service - see `OpenHost`. Both shapes are
			// listed here so one case can check that they do not interfere.
			return {
				"Echo", "Remember", "Refuse", "Instances", "Table", "Thing.Get", "Thing.Set", "Other.Get"
			};
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

			if (name == "Thing.Get" || name == "Other.Get") {
				// Answers which service was called, so a case can tell two
				// services apart rather than only that one answered.
				result = HostValue::Of(std::string_view(name));
				return true;
			}
			if (name == "Thing.Set") {
				// Records what it was passed, which is how the colon-call case
				// checks that the service table was dropped and the real
				// argument was not.
				Seen = arguments.empty() ? HostValue{} : arguments[0];
				Received = arguments.size();
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
		size_t Received = 0;
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

// --- services ------------------------------------------------------------------

TEST_CASE("a dotted host name becomes a service", "[script][host]") {
	engine::scene::EnsureClassTree();
	Store store("host");

	Recorder host;
	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->SetHost(&host);

	// **A service is a global table**, which is what makes `game:GetService`
	// find it for free: that function resolves a service by looking up a global
	// of the same name, so nothing in it had to learn about hosts.
	REQUIRE(runtime->Run(
		"assert(type(Thing) == 'table', 'no service global')\n"
		"assert(type(Thing.Get) == 'function', 'no method')\n"
		"assert(game:GetService('Thing') == Thing, 'GetService found a different object')\n"
	));

	// Two services are two tables, and neither holds the other's methods.
	REQUIRE(runtime->Run(
		"assert(Thing.Get() == 'Thing.Get', 'the wrong service answered')\n"
		"assert(Other.Get() == 'Other.Get', 'the wrong service answered')\n"
		"assert(Thing ~= Other, 'two services are one table')\n"
		"assert(Other.Set == nil, 'a method leaked between services')\n"
	));

	// And the flat half is untouched: a dotted name is not on the host's own
	// table and a bare one is not a service.
	REQUIRE(runtime->Run(
		"assert(test.Echo ~= nil, 'the flat table lost a name')\n"
		"assert(test.Thing == nil, 'a service leaked onto the flat table')\n"
		"assert(Echo == nil, 'a flat name became a global')\n"
	));
}

TEST_CASE("a service method takes a colon or a dot", "[script][host]") {
	engine::scene::EnsureClassTree();
	Store store("host");

	Recorder host;
	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->SetHost(&host);

	// **`Thing:Get()` is what a Roblox script writes**, and it passes the
	// service table as the first argument - which the host has no use for.
	REQUIRE(runtime->Run("assert(Thing:Get() == 'Thing.Get', 'the colon form did not work')"));

	// The dot form is the same call with no self.
	REQUIRE(runtime->Run("assert(Thing.Get() == 'Thing.Get', 'the dot form did not work')"));

	// **A method whose first real argument is a table must not lose it**, which
	// is why the skip compares against *this* service's table rather than
	// asking whether argument one is a table.
	REQUIRE(runtime->Run("Thing:Set({ 1, 2, 3 })"));
	CHECK(host.Received == 1);
	CHECK(host.Seen.Tag == HostTag::Array);
	CHECK(host.Seen.Items.size() == 3);

	// The same call by dot passes the same one argument.
	host.Received = 0;
	REQUIRE(runtime->Run("Thing.Set({ 1, 2 })"));
	CHECK(host.Received == 1);
	CHECK(host.Seen.Items.size() == 2);

	// And another service's table is not the one this closure drops, so passing
	// it is passing an argument.
	host.Received = 0;
	REQUIRE(runtime->Run("Thing.Set(Other)"));
	CHECK(host.Received == 1);
}

TEST_CASE("an empty table crosses as an array", "[script][host]") {
	engine::scene::EnsureClassTree();
	Store store("host");

	Recorder host;
	const auto runtime = MakeRuntime(store, Language::Luau);
	runtime->SetHost(&host);

	// **`{}` is the same Luau value whichever way it is read**, and the binding
	// has to pick one. A host expecting a map finds no entries under either tag;
	// a host expecting a *list* gets a tag it refuses - so the ambiguity is
	// harmless in one direction and not in the other.
	//
	// `Selection:Set({})` is the call this exists for: it is how a plugin
	// deselects everything, and it was refused before.
	REQUIRE(runtime->Run("test.Echo({})"));
	CHECK(host.Seen.Tag == HostTag::Array);
	CHECK(host.Seen.Items.empty());

	// A table with named keys is still a map, and one with a first element is
	// still an array - the empty case is the only one that moved.
	REQUIRE(runtime->Run("test.Echo({ a = 1 })"));
	CHECK(host.Seen.Tag == HostTag::Map);

	REQUIRE(runtime->Run("test.Echo({ 1 })"));
	CHECK(host.Seen.Tag == HostTag::Array);

	// And it comes back as a table either way, so a script cannot tell.
	REQUIRE(runtime->Run("local back = test.Echo({}) assert(type(back) == 'table' and #back == 0)"));
}
