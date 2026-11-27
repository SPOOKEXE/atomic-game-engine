#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <deadcode/Elf.hpp>
#include <deadcode/Graph.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

TEST_SUITE_ID("tools.deadcode.graph")

using namespace deadcode;

namespace {

	constexpr uint8_t GLOBAL = 1;
	constexpr uint8_t WEAK = 2;

	// A hand-made object: section 1 is `.text`, section 2 `.init_array`.
	// Functions are laid out 16 bytes apart in `.text`, in the order given.
	struct Fake {
		ObjectFile Object;

		Fake() {
			Object.Sections = {{}, {".text", 1, 6, 0x1000}, {".init_array", 14, 3, 8}};
			Object.Symbols.push_back({});
			Object.Relocations = {{1, {}}, {2, {}}};
		}

		uint32_t Function(const std::string &name, uint8_t bind = GLOBAL) {
			const uint64_t at = 16 * Object.Symbols.size();
			Object.Symbols.push_back({name, at, 16, 1, SYMBOL_FUNC, bind});
			return uint32_t(Object.Symbols.size() - 1);
		}

		uint32_t Undefined(const std::string &name) {
			Object.Symbols.push_back({name, 0, 0, SECTION_UNDEFINED, 0, GLOBAL});
			return uint32_t(Object.Symbols.size() - 1);
		}

		// `from` refers to `to` from inside its body.
		void Call(uint32_t from, uint32_t to) {
			Object.Relocations[0].Entries.push_back({Object.Symbols[from].Value + 4, to, 4, -4});
		}

		void Startup(uint32_t to) {
			Object.Relocations[1].Entries.push_back({0, to, 1, 0});
		}
	};

	std::vector<std::string> Names(const std::vector<Function> &functions) {
		std::vector<std::string> names;
		for (const Function &function : functions) {
			names.push_back(function.Name);
		}
		std::sort(names.begin(), names.end());
		return names;
	}
}

TEST_CASE("a function nothing calls is dead and its callees with it", "[deadcode]") {
	Fake program;
	const uint32_t main = program.Function("main");
	const uint32_t used = program.Function("_Z4usedv", 0);
	const uint32_t orphan = program.Function("_Z6orphanv");
	const uint32_t helper = program.Function("_Z6helperv", 0);
	program.Call(main, used);
	program.Call(orphan, helper);

	const Analysis analysis = Analyse({{"a.cpp", Role::Ship, program.Object}});

	REQUIRE(Names(analysis.Dead) == std::vector<std::string>{"_Z6helperv", "_Z6orphanv"});
	REQUIRE(analysis.FunctionsPerSource.at("a.cpp") == 4);
}

TEST_CASE("calls between objects are joined by name", "[deadcode]") {
	Fake program;
	const uint32_t main = program.Function("main");
	program.Call(main, program.Undefined("_Z7libraryv"));

	Fake library;
	library.Function("_Z7libraryv");
	library.Function("_Z6unusedv");

	const Analysis analysis =
		Analyse({{"main.cpp", Role::Ship, program.Object}, {"lib.cpp", Role::Ship, library.Object}});

	REQUIRE(Names(analysis.Dead) == std::vector<std::string>{"_Z6unusedv"});
}

TEST_CASE("static initialisers and vendor callbacks are roots", "[deadcode]") {
	Fake program;
	program.Function("main");
	program.Startup(program.Function("_Z11initialiserv", 0));
	program.Function("_Z8callbackv");

	Fake vendor;
	vendor.Undefined("_Z8callbackv");

	const Analysis analysis =
		Analyse({{"a.cpp", Role::Ship, program.Object}, {"vendor", Role::Vendor, vendor.Object}});

	REQUIRE(analysis.Dead.empty());
}

TEST_CASE("code only a test reaches is reported apart from dead code", "[deadcode]") {
	Fake library;
	library.Function("_Z6testedv");

	Fake test;
	test.Call(test.Function("main"), test.Undefined("_Z6testedv"));

	const Analysis analysis =
		Analyse({{"lib.cpp", Role::Ship, library.Object}, {"tests/t.cpp", Role::Test, test.Object}});

	REQUIRE(analysis.Dead.empty());
	REQUIRE(Names(analysis.TestOnly) == std::vector<std::string>{"_Z6testedv"});
}

TEST_CASE("an inline function emitted by many objects is reported once", "[deadcode]") {
	Fake a;
	a.Function("main");
	a.Function("_Z6inlinev", WEAK);
	Fake b;
	b.Function("_Z6inlinev", WEAK);

	const Analysis analysis = Analyse({{"a.cpp", Role::Ship, a.Object}, {"b.cpp", Role::Ship, b.Object}});

	REQUIRE(analysis.Dead.size() == 1);
	REQUIRE(analysis.Dead[0].Inline);
	REQUIRE(Demangle(analysis.Dead[0].Name) == "inline()");
}

TEST_CASE("library code instantiated in a first-party object is not reported", "[deadcode]") {
	Fake program;
	program.Function("main");
	program.Function("_ZNSt6vectorIiE9push_backEv", WEAK);
	program.Function("_ZN8nlohmann6detail8unescapeEv", 0);
	program.Function("_ZdlPvm");
	program.Function("deflate");
	program.Function("_ZL13stbtt__isfontPh", 0);
	program.Function("_ZN4game4DeadEv");

	const Analysis analysis = Analyse({{"a.cpp", Role::Ship, program.Object}});

	REQUIRE(Names(analysis.Dead) == std::vector<std::string>{"_ZN4game4DeadEv"});
}

TEST_CASE("an object with two functions in one section is flagged", "[deadcode]") {
	Fake one;
	one.Function("alone");
	REQUIRE_FALSE(SharesCodeSection(one.Object));

	one.Function("neighbour");
	REQUIRE(SharesCodeSection(one.Object));
}

#ifdef __linux__
TEST_CASE("the reader finds main in a real ELF file", "[deadcode]") {
	std::ifstream file("/proc/self/exe", std::ios::binary);
	std::ostringstream bytes;
	bytes << file.rdbuf();

	ObjectFile object;
	std::string error;
	REQUIRE(ReadObject(bytes.str(), object, error));
	const bool found = std::any_of(object.Symbols.begin(), object.Symbols.end(), [](const Symbol &symbol) {
		return symbol.Name == "main" && symbol.Type == SYMBOL_FUNC;
	});
	REQUIRE(found);
	REQUIRE_FALSE(ReadObject("not an object", object, error));
}
#endif
