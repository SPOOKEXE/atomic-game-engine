// What a script gets from `HttpService`, and what it deliberately does not.
//
// **The round trip is the test that matters.** JSON is a second traversal of the
// tree `Codec.hpp` already walks, and the two failure modes of a second
// traversal are that it disagrees with the first about what a Lua table is, and
// that it loses something on the way out that nothing notices until the way
// back. So the first case here encodes one value carrying every shape at once -
// nesting, an array, an object, a float, a negative, an empty table and a string
// needing every escape - and asserts the decoded tree equals the original.
//
// The rest pin the decisions that are *choices* rather than consequences: a
// cycle refused, a mixed table as an object, a NaN refused, a number outside a
// double refused, malformed text refused, and a GUID that is shaped like one and
// does not repeat.
//
// **And the three that are not here.** `RequestAsync`, `GetAsync` and
// `PostAsync` are absent on purpose - see `script/src/HttpService.cpp` - so a
// test asserts they are nil. That is the one assertion in this file whose job is
// to fail if somebody adds them without taking the decision first.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.script.httpservice")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {
	// Registered before the store exists, for `MeshCatalogue`'s reason: a
	// resource id minted before the explicit registration lands takes the
	// compiler's spelling of the type and aborts when the real one arrives.
	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	void MustRun(Runtime &runtime, const char *source) {
		INFO(source);
		const bool ok = runtime.Run(source);
		INFO(runtime.LastError());
		REQUIRE(ok);
	}
}

TEST_CASE("a value survives an encode and a decode", "[scripting][http]") {
	// **One value carrying every shape**, because the failure this catches is a
	// writer and a reader that disagree about one of them - and a suite with a
	// case per shape tests each half against itself rather than against the
	// other.
	Store store = Fresh("http_roundtrip");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local original = {
			list = { 1, 2, 3 },
			nested = { deep = { deeper = { 42 } } },
			float = 0.1,
			negative = -273.15,
			big = 1e30,
			zero = 0,
			yes = true,
			no = false,
			empty = {},
			-- Quote, backslash, newline, tab, and a byte above 0x7F. The last is
			-- what a naive encoder mangles: a Lua string is bytes and nothing
			-- here decodes an encoding, so it has to come back identical.
			awkward = "he said \"hi\"\\\n\tcaf\195\169",
		}

		local text = HttpService:JSONEncode(original)
		local back = HttpService:JSONDecode(text)

		local function same(left, right, where)
			assert(type(left) == type(right), where .. ": type changed")
			if type(left) ~= "table" then
				assert(left == right, where .. ": " .. tostring(left) .. " became " .. tostring(right))
				return
			end
			for key, value in pairs(left) do
				same(value, right[key], where .. "." .. tostring(key))
			end
			for key in pairs(right) do
				assert(left[key] ~= nil, where .. "." .. tostring(key) .. " appeared")
			end
		end

		same(original, back, "value")

		-- **0.1 is the one that catches a `%f`.** A writer that spelled it with
		-- six decimal places would round-trip every other number in this table
		-- and lose this one.
		assert(back.float == 0.1, "0.1 came back as " .. tostring(back.float))
		assert(back.big == 1e30, "1e30 came back as " .. tostring(back.big))
	)");
}

TEST_CASE("one table encodes to one document", "[scripting][http]") {
	// **The determinism guarantee, which is why map keys are sorted.** A Lua
	// table is a hash map, so writing entries in iteration order would make the
	// *string* depend on insertion history - and that string may be stored in a
	// property and replicated. `Codec.hpp` §1 is the argument; this is the check.
	Store store = Fresh("http_stable");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		-- **These five keys are chosen, not arbitrary.** Luau walks them in hash
		-- order, which for this set is neither the order they were written in
		-- nor sorted - so an encoder that followed iteration order fails the
		-- line below, where a set whose hash order happened to be alphabetical
		-- would have let one through. The second assertion is what keeps that
		-- true if the VM's hashing ever changes.
		local fruit = { zebra = 1, apple = 2, mango = 3, kiwi = 4, pear = 5 }
		local written = HttpService:JSONEncode(fruit)
		assert(written == '{"apple":2,"kiwi":4,"mango":3,"pear":5,"zebra":1}', written)

		local seen = {}
		for key in pairs(fruit) do
			seen[#seen + 1] = key
		end
		assert(table.concat(seen, ",") ~= "apple,kiwi,mango,pear,zebra", "the VM sorted these for us")

		-- An array keeps its own order, which is the thing an array *is*.
		assert(HttpService:JSONEncode({ 3, 1, 2 }) == "[3,1,2]")
	)");
}

TEST_CASE("what a Lua table becomes is decided, not incidental", "[scripting][http]") {
	// Every one of these is a choice `HttpService.cpp` states a reason for, and
	// each is inherited from the codec rather than invented - which is the point:
	// a value crossing a bus and the same value written as JSON must be the same
	// shape.
	Store store = Fresh("http_shapes");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		-- Dense 1..n is an array.
		assert(HttpService:JSONEncode({ "a", "b" }) == '["a","b"]')

		-- **A mixed table is an object**, and its numeric keys become decimal
		-- text. Not an array with the named entries silently dropped.
		assert(HttpService:JSONEncode({ 1, 2, x = 3 }) == '{"1":1,"2":2,"x":3}')

		-- A table with a hole is an object too, so the tail is not lost.
		local holed = { [1] = "a", [3] = "c" }
		assert(HttpService:JSONEncode(holed) == '{"1":"a","3":"c"}', HttpService:JSONEncode(holed))

		-- **An empty table is `{}` and not `[]`.** Lua cannot tell an empty list
		-- from an empty map, and this picks the same answer the codec picks so
		-- one value cannot have two shapes. Roblox writes `[]`.
		assert(HttpService:JSONEncode({}) == "{}")

		-- Scalars encode as themselves, which is what makes the service usable
		-- for a single value rather than only for a table.
		assert(HttpService:JSONEncode("hi") == '"hi"')
		assert(HttpService:JSONEncode(true) == "true")
		assert(HttpService:JSONEncode(nil) == "null")
	)");
}

TEST_CASE("a value with no JSON form is refused where it happens", "[scripting][http]") {
	Store store = Fresh("http_refusals");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local function refused(f, what)
			local ok = pcall(f)
			assert(not ok, what .. " was accepted")
		end

		-- **A cycle is an error, not a hang.** The check is the codec's, which
		-- is why it costs a hash lookup rather than a depth counter.
		local loop = {}
		loop.self = loop
		refused(function() return HttpService:JSONEncode(loop) end, "a cycle")

		-- **A NaN and an infinity are refused rather than written as `null`.**
		-- JavaScript writes `null`, which turns a broken number into a missing
		-- field a long way from the arithmetic that produced it.
		refused(function() return HttpService:JSONEncode({ n = 0 / 0 }) end, "a NaN")
		refused(function() return HttpService:JSONEncode({ n = math.huge }) end, "an infinity")

		-- The three value types cross a bus, where both ends are this engine.
		-- JSON's readers are not, and a shape invented here would decode back as
		-- a plain table rather than as the type it went in as.
		refused(function() return HttpService:JSONEncode({ v = Vector3.new(1, 2, 3) }) end, "a Vector3")

		-- A function and an instance mean nothing outside this world.
		refused(function() return HttpService:JSONEncode({ f = print }) end, "a function")
		refused(function() return HttpService:JSONEncode({ i = Instance.new("Part") }) end, "an Instance")

		-- Past `CODEC_MAX_DEPTH`, which is sixteen.
		local deep = {}
		local tip = deep
		for _ = 1, 40 do
			tip.next = {}
			tip = tip.next
		end
		refused(function() return HttpService:JSONEncode(deep) end, "a deeply nested table")
	)");
}

TEST_CASE("malformed text is refused rather than half-read", "[scripting][http]") {
	Store store = Fresh("http_malformed");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		local function refused(text)
			local ok = pcall(function() return HttpService:JSONDecode(text) end)
			assert(not ok, "'" .. text .. "' was accepted")
		end

		refused("")
		refused("{")
		refused('{"a":}')
		refused('{"a" 1}')
		refused("[1,2")
		refused("[1,,2]")
		refused("tru")
		refused("1..2")

		-- **Text after the value is an error.** Far more often a truncated
		-- concatenation than a document with a comment on the end, and a parser
		-- that stops at the first complete value hides which.
		refused('{"a":1} oops')

		-- **A number outside a double is malformed rather than an infinity.**
		-- The reader's one invariant is that it never produces a value the
		-- writer would refuse.
		refused("1e400")

		-- An unescaped newline inside a string, and a lone surrogate, both of
		-- which a lenient reader would let through.
		refused('"a\nb"')
		refused('"\\uD83D"')

		-- What is accepted: escapes, a paired surrogate, and nesting.
		local decoded = HttpService:JSONDecode('{"t":"a\\tb","u":"\\u00e9","p":"\\uD83D\\uDE00","n":[1,[2]]}')
		assert(decoded.t == "a\tb", "a tab escape")
		assert(decoded.u == "\195\169", "\\u00e9 is two UTF-8 bytes")
		assert(#decoded.p == 4, "a surrogate pair is four UTF-8 bytes, got " .. #decoded.p)
		assert(decoded.n[1] == 1 and decoded.n[2][1] == 2, "arrays are one-based")

		-- `null` is `nil`, which is the only thing Lua has to say "no value".
		assert(HttpService:JSONDecode('{"a":null}').a == nil)
	)");
}

TEST_CASE("a GUID is shaped like one and does not repeat", "[scripting][http]") {
	Store store = Fresh("http_guid");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		-- **Braces by default**, which is Roblox's default and the surprising
		-- half of the signature.
		local braced = HttpService:GenerateGUID()
		assert(#braced == 38, "braced is 38 characters, got " .. #braced)
		assert(braced:sub(1, 1) == "{" and braced:sub(-1) == "}", braced)

		local bare = HttpService:GenerateGUID(false)
		assert(#bare == 36, "bare is 36 characters, got " .. #bare)
		assert(bare:match("^%x%x%x%x%x%x%x%x%-%x%x%x%x%-%x%x%x%x%-%x%x%x%x%-%x%x%x%x%x%x%x%x%x%x%x%x$"), bare)

		-- Upper case, like Roblox's, so a script comparing one against a literal
		-- copied out of the output window does not fail on nothing.
		assert(bare == bare:upper(), bare)

		-- Version 4 and the RFC 4122 variant, so the string parses as a UUID for
		-- anything that reads it as one. A statement about the shape, not a
		-- claim about the entropy - see `HttpService.cpp`.
		assert(bare:sub(15, 15) == "4", "version nibble: " .. bare:sub(15, 15))
		assert(bare:sub(20, 20):match("[89AB]"), "variant nibble: " .. bare:sub(20, 20))

		-- **Two draws differ**, which is the whole use of the thing. Determinism
		-- means the *sequence* repeats between runs, never that one call repeats
		-- itself.
		local seen = {}
		for _ = 1, 64 do
			local guid = HttpService:GenerateGUID(false)
			assert(seen[guid] == nil, "a GUID repeated: " .. guid)
			seen[guid] = true
		end
	)");
}

TEST_CASE("two runs of one world draw the same GUIDs", "[scripting][http]") {
	// **The reason this service could exist at all.** `script/AGENTS.md` sets one
	// test for anything added to this VM - what can it observe that a recording
	// cannot reproduce? A GUID from system entropy fails it, and neither
	// `just determinism` nor `just replay-check` would have said so: both drive
	// the server with no `--game`, so neither has a script runtime in it.
	// **Raised rather than returned, because nothing hands a value back.** Each
	// chunk runs on its own sandboxed thread with its own globals, so a global
	// written by one is invisible to the next and there is no binding that reads
	// one out. `error(text)` puts the string somewhere C++ can see it, and the
	// chunk name and line are identical between two runs of one source - so
	// comparing the whole message compares the GUIDs.
	static constexpr const char *DRAW = R"(
		local drawn = {}
		for index = 1, 8 do
			drawn[index] = HttpService:GenerateGUID(false)
		end
		error(table.concat(drawn, ","), 0)
	)";

	const auto drawnBy = [](Store &store) {
		const auto runtime = MakeRuntime(store, Language::Luau);
		REQUIRE(runtime != nullptr);
		REQUIRE_FALSE(runtime->Run(DRAW));
		return std::string(runtime->LastError());
	};

	// **Same world name, same stream.** The salt is the world's name and the
	// index is the draw counter, so two runtimes over two identically named
	// worlds are exactly the two runs a recording has to reproduce.
	Store first = Fresh("http_replay");
	Store second = Fresh("http_replay");
	const std::string one = drawnBy(first);
	const std::string two = drawnBy(second);

	INFO(one);
	CHECK(one.find('-') != std::string::npos);
	CHECK(one == two);

	// A world with a different name draws differently, so two worlds in one
	// universe do not hand out the same identifiers.
	Store other = Fresh("http_replay_other");
	CHECK(drawnBy(other) != one);
}

TEST_CASE("a URL escape is RFC 3986's and not a form body's", "[scripting][http]") {
	Store store = Fresh("http_url");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		-- **A space is `%20` and never `+`.** The plus form belongs to
		-- `x-www-form-urlencoded` and means a literal plus everywhere else in a
		-- URL, so an encoder that used it is right in a form body and wrong in a
		-- path.
		assert(HttpService:UrlEncode("a b") == "a%20b", HttpService:UrlEncode("a b"))

		-- The unreserved set goes through untouched, and everything else does
		-- not - including the characters a lazy encoder leaves alone.
		assert(HttpService:UrlEncode("aZ0-_.~") == "aZ0-_.~")
		assert(HttpService:UrlEncode("a/b?c=d&e") == "a%2Fb%3Fc%3Dd%26e", HttpService:UrlEncode("a/b?c=d&e"))
		assert(HttpService:UrlEncode("+") == "%2B")

		-- Byte by byte, so UTF-8 comes out as one `%XX` per byte. Upper-case
		-- hex, which RFC 3986 §2.1 prefers.
		assert(HttpService:UrlEncode("\195\169") == "%C3%A9", HttpService:UrlEncode("\195\169"))
	)");
}

TEST_CASE("the request methods are absent", "[scripting][http]") {
	// **The assertion whose job is to fail.** If somebody adds `RequestAsync` by
	// reflex, this is what says so - and `script/src/HttpService.cpp` is what
	// says why the decision has to come first. An absent method refuses at the
	// call site in the script that asked, which is how every other unprovided
	// service member behaves.
	Store store = Fresh("http_absent");
	const auto runtime = MakeRuntime(store, Language::Luau);
	REQUIRE(runtime != nullptr);

	MustRun(*runtime, R"(
		assert(HttpService.RequestAsync == nil, "RequestAsync is a decision nobody has taken")
		assert(HttpService.GetAsync == nil, "GetAsync is a decision nobody has taken")
		assert(HttpService.PostAsync == nil, "PostAsync is a decision nobody has taken")

		-- And the service itself is one object, whichever way a script reaches
		-- it - `GetService` resolves a global rather than building a second
		-- table that behaves alike.
		assert(game:GetService("HttpService") == HttpService)
	)");
}
