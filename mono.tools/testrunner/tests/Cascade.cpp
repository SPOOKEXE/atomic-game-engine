#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <random>
#include <testrunner/Runner.hpp>

TEST_SUITE_ID("tools.testrunner.cascade")
TEST_DEPENDS("tools.testrunner.sha256")

namespace fs = std::filesystem;
using testrunner::CacheEntry;
using testrunner::ComputeSignatures;
using testrunner::DependencyClosures;
using testrunner::Suite;

namespace {

	// A directory that cleans itself up. The runner works on real files, so
	// testing it on real files keeps the test honest about what it covers.
	struct Scratch {
		fs::path Root;

		Scratch() {
			std::random_device device;
			Root = fs::temp_directory_path() /
				   ("testrunner-" + std::to_string(device()) + std::to_string(device()));
			fs::create_directories(Root);
		}
		~Scratch() {
			std::error_code error;
			fs::remove_all(Root, error);
		}

		fs::path Write(const std::string &name, std::string_view contents) const {
			const fs::path path = Root / name;
			std::ofstream file(path, std::ios::trunc);
			file << contents;
			return fs::weakly_canonical(path);
		}
	};

	Suite Make(const std::string &id, const fs::path &source, std::vector<std::string> depends = {}) {
		Suite suite;
		suite.Id = id;
		suite.Source = source;
		suite.Depends = std::move(depends);
		return suite;
	}

	std::map<std::string, std::string>
	Sign(const std::vector<Suite> &suites, const DependencyClosures &closures) {
		std::vector<std::string> warnings;
		return ComputeSignatures(suites, closures, warnings);
	}
}

TEST_CASE("a signature changes when the source does", "[cascade]") {
	Scratch scratch;
	const auto source = scratch.Write("A.cpp", "one");

	const std::vector<Suite> suites{Make("a", source)};
	const DependencyClosures closures{{source, {source}}};

	const auto before = Sign(suites, closures).at("a");
	scratch.Write("A.cpp", "two");
	const auto after = Sign(suites, closures).at("a");

	REQUIRE(before != after);
}

TEST_CASE("a signature changes when an included header does", "[cascade]") {
	Scratch scratch;
	const auto source = scratch.Write("A.cpp", "source");
	const auto header = scratch.Write("A.hpp", "one");

	const std::vector<Suite> suites{Make("a", source)};
	const DependencyClosures closures{{source, {source, header}}};

	const auto before = Sign(suites, closures).at("a");

	// The whole point: the test's own source did not change.
	scratch.Write("A.hpp", "two");
	const auto after = Sign(suites, closures).at("a");

	REQUIRE(before != after);
}

TEST_CASE("a change cascades through declared dependencies and no further", "[cascade]") {
	Scratch scratch;
	const auto low = scratch.Write("Low.cpp", "low");
	const auto middle = scratch.Write("Middle.cpp", "middle");
	const auto high = scratch.Write("High.cpp", "high");
	const auto other = scratch.Write("Other.cpp", "other");

	const std::vector<Suite> suites{
		Make("low", low),
		Make("middle", middle, {"low"}),
		Make("high", high, {"middle"}),
		Make("other", other),
	};
	const DependencyClosures closures{
		{low, {low}},
		{middle, {middle}},
		{high, {high}},
		{other, {other}},
	};

	const auto before = Sign(suites, closures);
	scratch.Write("Low.cpp", "changed");
	const auto after = Sign(suites, closures);

	// Everything transitively above the change, and nothing else. This is the
	// property a timestamp or a flat file list cannot give.
	REQUIRE(before.at("low") != after.at("low"));
	REQUIRE(before.at("middle") != after.at("middle"));
	REQUIRE(before.at("high") != after.at("high"));
	REQUIRE(before.at("other") == after.at("other"));
}

TEST_CASE("include order does not change the signature", "[cascade]") {
	Scratch scratch;
	const auto source = scratch.Write("A.cpp", "source");
	const auto first = scratch.Write("First.hpp", "first");
	const auto second = scratch.Write("Second.hpp", "second");

	const std::vector<Suite> suites{Make("a", source)};

	// The compiler may report a translation unit's includes in any order. If
	// that reached the digest, a cache would miss for no reason.
	const auto forwards = Sign(suites, {{source, {source, first, second}}}).at("a");
	const auto backwards = Sign(suites, {{source, {source, second, first}}}).at("a");

	REQUIRE(forwards == backwards);
}

TEST_CASE("a signature is stable across runs", "[cascade]") {
	Scratch scratch;
	const auto source = scratch.Write("A.cpp", "stable");

	const std::vector<Suite> suites{Make("a", source)};
	const DependencyClosures closures{{source, {source}}};

	// Stability is what lets CI and a laptop share a cache, so nothing
	// path-dependent or time-dependent may reach the digest.
	REQUIRE(Sign(suites, closures).at("a") == Sign(suites, closures).at("a"));
}

TEST_CASE("a missing file hashes as absent rather than failing", "[cascade]") {
	Scratch scratch;
	const auto source = scratch.Write("A.cpp", "source");
	const fs::path generated = scratch.Root / "Generated.hpp";

	const std::vector<Suite> suites{Make("a", source)};
	const DependencyClosures closures{{source, {source, generated}}};

	const auto absent = Sign(suites, closures).at("a");

	// A generated header appearing changes the signature, which is right: the
	// translation unit now includes something it did not before.
	scratch.Write("Generated.hpp", "now here");
	REQUIRE(Sign(suites, closures).at("a") != absent);
}

TEST_CASE("a dependency cycle terminates and is reported", "[cascade]") {
	Scratch scratch;
	const auto first = scratch.Write("First.cpp", "first");
	const auto second = scratch.Write("Second.cpp", "second");

	const std::vector<Suite> suites{
		Make("first", first, {"second"}),
		Make("second", second, {"first"}),
	};
	const DependencyClosures closures{{first, {first}}, {second, {second}}};

	std::vector<std::string> warnings;
	const auto signatures = ComputeSignatures(suites, closures, warnings);

	REQUIRE(signatures.size() == 2);
	REQUIRE_FALSE(warnings.empty());
}

TEST_CASE("an unknown dependency is reported, not fatal", "[cascade]") {
	Scratch scratch;
	const auto source = scratch.Write("A.cpp", "a");

	const std::vector<Suite> suites{Make("a", source, {"does.not.exist"})};
	const DependencyClosures closures{{source, {source}}};

	std::vector<std::string> warnings;
	const auto signatures = ComputeSignatures(suites, closures, warnings);

	REQUIRE(signatures.count("a") == 1);
	REQUIRE_FALSE(warnings.empty());
}

TEST_CASE("a missing header closure is reported rather than silently narrowing", "[cascade]") {
	Scratch scratch;
	const auto source = scratch.Write("A.cpp", "a");

	// Falling back to the source alone under-covers, and under-covering
	// silently is the failure mode this whole tool exists to avoid.
	std::vector<std::string> warnings;
	ComputeSignatures({Make("a", source)}, {}, warnings);

	REQUIRE_FALSE(warnings.empty());
}

TEST_CASE("the cache round-trips", "[cache]") {
	Scratch scratch;
	const fs::path path = scratch.Root / "smart-tests.txt";

	const std::map<std::string, CacheEntry> written{
		{"engine.core.types", {"abc123", true}},
		{"engine.ecs.store", {"def456", false}},
	};
	REQUIRE(testrunner::SaveCache(path, written));

	const auto read = testrunner::LoadCache(path);
	REQUIRE(read.size() == 2);
	REQUIRE(read.at("engine.core.types").Signature == "abc123");
	REQUIRE(read.at("engine.core.types").Passed);
	REQUIRE_FALSE(read.at("engine.ecs.store").Passed);
}

TEST_CASE("the cache is text a person can read and edit", "[cache]") {
	Scratch scratch;
	const fs::path path = scratch.Root / "smart-tests.txt";
	REQUIRE(testrunner::SaveCache(path, {{"engine.core.types", {"abc123", true}}}));

	std::ifstream file(path);
	std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

	// It is called smart-tests.txt. A JSON blob behind a .txt extension is a
	// name that lies, and the point of the format is that a diff of it reads.
	REQUIRE(contents.find("engine.core.types\tpass\tabc123") != std::string::npos);
	REQUIRE(contents.find('{') == std::string::npos);
}

TEST_CASE("a cache from another version is discarded, not misread", "[cache]") {
	Scratch scratch;
	const fs::path path = scratch.Root / "smart-tests.txt";

	{
		std::ofstream file(path);
		file << "# atomic smart-tests cache v0\n";
		file << "engine.core.types\tpass\tabc123\n";
	}

	// Signatures from a different version were computed differently. Reading
	// them would skip suites on the strength of numbers that do not mean the
	// same thing.
	REQUIRE(testrunner::LoadCache(path).empty());
}

TEST_CASE("a missing cache is empty rather than an error", "[cache]") {
	REQUIRE(testrunner::LoadCache("/nonexistent/smart-tests.txt").empty());
}
