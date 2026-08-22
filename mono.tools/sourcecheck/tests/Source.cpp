#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sourcecheck/Source.hpp>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("tools.sourcecheck.source")

using sourcecheck::Crossings;
using sourcecheck::Enums;
using sourcecheck::Includes;
using sourcecheck::Member;
using sourcecheck::Parse;
using sourcecheck::Record;
using sourcecheck::Records;
using sourcecheck::Strip;
using sourcecheck::Waivers;

namespace {

	const Record *Find(const std::vector<Record> &records, std::string_view name) {
		for (const Record &record : records) {
			if (record.Name == name) {
				return &record;
			}
		}
		return nullptr;
	}

	const Member *Field(const Record &record, std::string_view name) {
		for (const Member &member : record.Members) {
			if (member.Name == name) {
				return &member;
			}
		}
		return nullptr;
	}
}

TEST_CASE("Strip keeps every offset", "[sourcecheck]") {
	const std::string source = "int A; // a comment\nint B;\n";
	const std::string stripped = Strip(source);

	REQUIRE(stripped.size() == source.size());
	CHECK(stripped.find("comment") == std::string::npos);
	// The offset of `int B` has to be the same in both, or every line number
	// this tool reports is the line number of something else.
	CHECK(stripped.find("int B") == source.find("int B"));
}

TEST_CASE("Strip leaves a brace inside a raw string alone", "[sourcecheck]") {
	// The failure this catches is a shader embedded in a `.cpp`: a `}` inside
	// one would close a record the scanner is still reading, and every
	// declaration after it would land in the wrong scope.
	const std::string source = "struct A { const char *S = R\"(} // not a comment)\"; int X; };";
	const std::vector<Record> records = Records(Strip(source));

	REQUIRE(records.size() == 1);
	CHECK(Field(records[0], "X") != nullptr);
}

TEST_CASE("Records reads members, namespaces and nesting", "[sourcecheck]") {
	const std::string source = R"CPP(
namespace engine::world {

	struct Envelope {
		core::Name Key;
		std::vector<std::byte> Payload;

		struct Inner {
			int Deep = 0;
		};
	};
}
)CPP";
	const std::vector<Record> records = Records(Strip(source));

	const Record *envelope = Find(records, "Envelope");
	REQUIRE(envelope != nullptr);
	CHECK(envelope->Namespace == "engine::world");
	CHECK(envelope->Enclosing.empty());
	// The nested record contributes itself, not its members.
	CHECK(envelope->Members.size() == 2);
	REQUIRE(Field(*envelope, "Key") != nullptr);
	CHECK(Field(*envelope, "Key")->Type == "core::Name");
	CHECK(Field(*envelope, "Payload")->Type == "std::vector<std::byte>");

	const Record *inner = Find(records, "Inner");
	REQUIRE(inner != nullptr);
	CHECK(inner->Enclosing == "Envelope");
}

TEST_CASE("Records tells behaviour from an argument list", "[sourcecheck]") {
	// The ECS-copy rule turns entirely on this answer, so it is the one thing
	// in this file worth several cases.
	const std::string source = R"CPP(
struct Request {
	int Count = 0;
};

struct Defaulted {
	int Count = Compute();
};

class Object {
  public:
	void Step();

  private:
	int Count = 0;
};

struct Constructed {
	explicit Constructed(int count);
	int Count = 0;
};
)CPP";
	const std::vector<Record> records = Records(Strip(source));

	REQUIRE(Find(records, "Request") != nullptr);
	CHECK_FALSE(Find(records, "Request")->HasBehaviour);
	// An initialiser that calls something is still data.
	CHECK_FALSE(Find(records, "Defaulted")->HasBehaviour);
	CHECK(Find(records, "Object")->HasBehaviour);
	CHECK(Find(records, "Constructed")->HasBehaviour);
	// An access specifier has no `;` after it, so it arrives glued to the
	// declaration below it and has to be taken off the type.
	REQUIRE(Field(*Find(records, "Object"), "Count") != nullptr);
	CHECK(Field(*Find(records, "Object"), "Count")->Type == "int");
}

TEST_CASE("Records does not read an enumeration as a record", "[sourcecheck]") {
	// `enum class X : uint8_t { A, B }` looks exactly like a record with a base
	// list, and reading it as one mints a member out of its last enumerator.
	const std::string source = "namespace n { enum class Kind : uint8_t { First, Second }; }";
	const std::vector<Record> records = Records(Strip(source));
	CHECK(Find(records, "Kind") == nullptr);

	const std::vector<sourcecheck::Enumeration> enums = Enums(Strip(source));
	REQUIRE(enums.size() == 1);
	CHECK(enums[0].Name == "Kind");
	CHECK(enums[0].Namespace == "n");
}

TEST_CASE("Records skips a forward declaration", "[sourcecheck]") {
	const std::string source = "struct Later; class Other; struct Real { int X = 0; };";
	const std::vector<Record> records = Records(Strip(source));
	REQUIRE(records.size() == 1);
	CHECK(records[0].Name == "Real");
}

TEST_CASE("Includes reads both delimiters", "[sourcecheck]") {
	const std::string source = "#include <engine/core/Name.hpp>\n#include \"Local.hpp\"\n";
	const std::vector<std::string> found = Includes(source);
	REQUIRE(found.size() == 2);
	CHECK(found[0] == "engine/core/Name.hpp");
	// A quoted include is a string literal, so it survives only because
	// `Includes` reads the original text rather than the stripped one.
	CHECK(found[1] == "Local.hpp");
}

TEST_CASE("A waiver covers the declaration under it", "[sourcecheck]") {
	const std::string source = "struct A {\n"
							   "\t// arch-waiver ecs-copy: the reason, which is what makes it count.\n"
							   "\tint X = 0;\n"
							   "};\n";
	const std::vector<sourcecheck::Waiver> waivers = Waivers(source);
	REQUIRE(waivers.size() == 1);
	CHECK(waivers[0].Rule == "ecs-copy");
	CHECK(waivers[0].Reason == "the reason, which is what makes it count.");
	CHECK(waivers[0].Line == 2);
	CHECK(waivers[0].Covers == 3);
}

TEST_CASE("A waiver reaches past the rest of its own comment block", "[sourcecheck]") {
	const std::string source = "// arch-waiver world-pointer: the first line of a reason,\n"
							   "// and the second line of the same one.\n"
							   "struct A {\n"
							   "};\n";
	const std::vector<sourcecheck::Waiver> waivers = Waivers(source);
	REQUIRE(waivers.size() == 1);
	CHECK(waivers[0].Covers == 3);
	// The whole reason, not the first line of it. Half a reason in a build log
	// is the half that reads like an excuse.
	CHECK(waivers[0].Reason == "the first line of a reason, and the second line of the same one.");
}

TEST_CASE("A marker is only a marker when the comment starts with it", "[sourcecheck]") {
	// This is what keeps the tool's own prose about `// arch-waiver` and its
	// help text - which prints an example - from being read as markers by a
	// scan of the repository that contains them.
	const std::string source = "// Written `// arch-waiver ecs-copy: a reason` above the declaration.\n"
							   "const char *Help = \"  // arch-crossing\";\n"
							   "struct A {\n"
							   "};\n";
	CHECK(Waivers(source).empty());
	CHECK(Crossings(source).empty());
}

TEST_CASE("A crossing marker names the declaration below it", "[sourcecheck]") {
	const std::string source = "// arch-crossing - what leaves a world.\n"
							   "struct Envelope {\n"
							   "};\n";
	const std::vector<size_t> found = Crossings(source);
	REQUIRE(found.size() == 1);
	CHECK(found[0] == 2);
}

TEST_CASE("Parse fills a file in one pass", "[sourcecheck]") {
	const sourcecheck::File file = Parse(
		"alpha/include/alpha/A.hpp",
		"#include <beta/B.hpp>\n"
		"// arch-crossing\n"
		"struct A { int X = 0; };\n"
	);
	CHECK(file.Path == "alpha/include/alpha/A.hpp");
	REQUIRE(file.Includes.size() == 1);
	REQUIRE(file.Records.size() == 1);
	REQUIRE(file.Crossings.size() == 1);
	CHECK(file.Crossings[0] == file.Records[0].Line);
}
