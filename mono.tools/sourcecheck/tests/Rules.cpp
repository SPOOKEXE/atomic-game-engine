#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sourcecheck/Rules.hpp>
#include <sourcecheck/Source.hpp>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("tools.sourcecheck.rules")

using sourcecheck::CheckEcsCopy;
using sourcecheck::CheckNameId;
using sourcecheck::CheckPublicHeader;
using sourcecheck::CheckWorldPointer;
using sourcecheck::Finding;
using sourcecheck::State;
using sourcecheck::Tree;

namespace {

	// A tree assembled in memory, so a rule can be exercised without a directory
	// on disk. `tests/fixtures/` is the other half: this decides what a rule
	// says, and those decide that the program around it still reaches the rule.
	struct Builder {
		Tree Result;

		// `path` is `<module>/include/<module>/<name>` for a public header and
		// anything else for a private one.
		Builder &Add(std::string_view path, std::string_view text) {
			sourcecheck::File file = sourcecheck::Parse(path, text);
			const size_t slash = file.Path.find('/');
			file.ModuleDir = file.Path.substr(0, slash);
			file.Module = file.ModuleDir;
			const std::string prefix = file.ModuleDir + "/include/";
			if (file.Path.starts_with(prefix)) {
				file.IncludePath = file.Path.substr(prefix.size());
			}
			Result.Files.push_back(std::move(file));
			return *this;
		}
	};

	bool Mentions(const std::vector<Finding> &findings, std::string_view text) {
		for (const Finding &finding : findings) {
			if (finding.Message.find(text) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	constexpr std::string_view COMPONENT = R"CPP(
namespace alpha {
	enum class Stance : uint8_t { Standing, Crouched };

	struct Health {
		float Current = 100.0f;
		Stance Posture = Stance::Standing;
	};
}
)CPP";

	constexpr std::string_view REGISTER = R"CPP(
#include <alpha/Health.hpp>
namespace alpha {
	inline void RegisterAlpha() {
		engine::ecs::Components::Register<Health>("alpha.Health");
	}
}
)CPP";

	Builder WithComponent() {
		Builder builder;
		builder.Add("alpha/include/alpha/Health.hpp", COMPONENT);
		builder.Add("alpha/src/Register.hpp", REGISTER);
		return builder;
	}
}

TEST_CASE("A long-lived object holding a component is a copy", "[sourcecheck]") {
	Builder builder = WithComponent();
	builder.Add("beta/include/beta/Panel.hpp", R"CPP(
#include <alpha/Health.hpp>
namespace beta {
	class Panel {
	  public:
		void Draw();
	  private:
		alpha::Health Bar;
	};
}
)CPP");

	const std::vector<Finding> findings = CheckEcsCopy(builder.Result);
	REQUIRE(findings.size() == 1);
	CHECK(findings[0].Status == State::Open);
	CHECK(Mentions(findings, "`Health` is a registered component"));
}

TEST_CASE("An argument list holding a component is not a copy", "[sourcecheck]") {
	// The discriminator the whole rule turns on. `render::View` carries a
	// camera because a draw call needs one, and a value passed to a call is not
	// a second authority for it.
	Builder builder = WithComponent();
	builder.Add("beta/include/beta/Request.hpp", R"CPP(
#include <alpha/Health.hpp>
namespace beta {
	struct DrawRequest {
		alpha::Health Snapshot;
	};
}
)CPP");

	CHECK(CheckEcsCopy(builder.Result).empty());
}

TEST_CASE("A component's companion enumeration is the same fact", "[sourcecheck]") {
	// `docs/ARCH_REVIEW.md` A4 in miniature: the copy was the enumeration a
	// component declares a field with, not the component, so a rule matching
	// whole components would have missed the instance it was written from.
	Builder builder = WithComponent();
	builder.Add("beta/include/beta/Panel.hpp", R"CPP(
#include <alpha/Health.hpp>
namespace beta {
	class Panel {
	  public:
		void Draw();
	  private:
		alpha::Stance Posture = alpha::Stance::Standing;
	};
}
)CPP");

	CHECK(Mentions(CheckEcsCopy(builder.Result), "declares its own field with"));
}

TEST_CASE("An unqualified name matches only inside its own module", "[sourcecheck]") {
	// There are four unrelated structs named `Entry` in this repository. Before
	// the qualification rule this check reported eighteen findings and twelve of
	// them were two modules sharing a common noun.
	Builder builder = WithComponent();
	builder.Add("beta/include/beta/Panel.hpp", R"CPP(
namespace beta {
	struct Health {
		int Unrelated = 0;
	};

	class Panel {
	  public:
		void Draw();
	  private:
		Health Bar;
	};
}
)CPP");

	CHECK(CheckEcsCopy(builder.Result).empty());
}

TEST_CASE("A waiver needs a reason to count", "[sourcecheck]") {
	Builder waived = WithComponent();
	waived.Add("beta/include/beta/Panel.hpp", R"CPP(
#include <alpha/Health.hpp>
namespace beta {
	class Panel {
	  public:
		void Draw();
	  private:
		// arch-waiver ecs-copy: a reason, which is what makes it count.
		alpha::Health Bar;
	};
}
)CPP");
	const std::vector<Finding> honoured = CheckEcsCopy(waived.Result);
	REQUIRE(honoured.size() == 1);
	CHECK(honoured[0].Status == State::Waived);
	CHECK(honoured[0].Reason == "a reason, which is what makes it count.");

	Builder bare = WithComponent();
	bare.Add("beta/include/beta/Panel.hpp", R"CPP(
#include <alpha/Health.hpp>
namespace beta {
	class Panel {
	  public:
		void Draw();
	  private:
		// arch-waiver ecs-copy:
		alpha::Health Bar;
	};
}
)CPP");
	const std::vector<Finding> refused = CheckEcsCopy(bare.Result);
	REQUIRE(refused.size() == 1);
	CHECK(refused[0].Status == State::Open);
	CHECK(Mentions(refused, "no reason after the colon"));
}

TEST_CASE("A known violation is reported and does not gate", "[sourcecheck]") {
	// The third state, and the reason it exists: a violation somebody cannot fix
	// this afternoon must stay in front of whoever reads the output rather than
	// disappearing into the waived count.
	Builder builder = WithComponent();
	builder.Add("beta/include/beta/Panel.hpp", R"CPP(
#include <alpha/Health.hpp>
namespace beta {
	class Panel {
	  public:
		void Draw();
	  private:
		// arch-waiver ecs-copy: known violation - the fix needs a decision
		// about which world owns this, which is not this change.
		alpha::Health Bar;
	};
}
)CPP");
	const std::vector<Finding> findings = CheckEcsCopy(builder.Result);
	REQUIRE(findings.size() == 1);
	CHECK(findings[0].Status == State::Known);
}

TEST_CASE("A pointer inside a crossing type is found through its fields", "[sourcecheck]") {
	Builder builder;
	builder.Add("world/include/world/Bus.hpp", R"CPP(
namespace world {
	struct Payload {
		const uint8_t *Bytes = nullptr;
	};

	// arch-crossing
	struct Envelope {
		Payload Body;
	};
}
)CPP");

	const std::vector<Finding> findings = CheckWorldPointer(builder.Result);
	REQUIRE(findings.size() == 1);
	CHECK(Mentions(findings, "Envelope::Body::Bytes is a pointer"));
}

TEST_CASE("A view is a pointer with a nicer name", "[sourcecheck]") {
	Builder builder;
	builder.Add("world/include/world/Bus.hpp", R"CPP(
namespace world {
	// arch-crossing
	struct Envelope {
		std::span<const uint8_t> Bytes;
		std::vector<uint8_t> Copy;
	};
}
)CPP");

	const std::vector<Finding> findings = CheckWorldPointer(builder.Result);
	REQUIRE(findings.size() == 1);
	CHECK(Mentions(findings, "std::span"));
}

TEST_CASE("A type with no crossing marker is not walked", "[sourcecheck]") {
	// Said out loud because it is the rule's limit: an unmarked type that starts
	// crossing is invisible here, which is why the marker sits beside the
	// paragraph in `world/Bus.hpp` that already claims the same thing.
	Builder builder;
	builder.Add("world/include/world/Bus.hpp", R"CPP(
namespace world {
	struct Envelope {
		const uint8_t *Bytes = nullptr;
	};
}
)CPP");

	CHECK(CheckWorldPointer(builder.Result).empty());
}

TEST_CASE("An id reaching a writer is a name that will not survive", "[sourcecheck]") {
	Builder builder;
	builder.Add("codec/src/Write.hpp", "void W() { writer.WriteUInt32(name.Id()); }");
	CHECK(Mentions(CheckNameId(builder.Result), "Ids are assigned in first-seen order"));

	Builder raw;
	raw.Add("codec/src/Write.hpp", "void W() { writer.WriteRaw(&name, sizeof(engine::core::Name)); }");
	CHECK(Mentions(CheckNameId(raw.Result), "object representation"));

	Builder read;
	read.Add("codec/src/Read.hpp", "Name R() { return Name::FromId(reader.ReadUInt32()); }");
	CHECK(Mentions(CheckNameId(read.Result), "fed straight from a reader"));
}

TEST_CASE("The sanctioned name paths are not findings", "[sourcecheck]") {
	Builder builder;
	builder.Add(
		"codec/src/Write.hpp",
		"void W() {\n"
		"\twriter.WriteName(name);\n"
		"\twriter.WriteString(name.Text());\n"
		"\tTable[name.Id()] = value;\n"
		"}\n"
	);
	CHECK(CheckNameId(builder.Result).empty());
}

TEST_CASE("A header nothing outside includes is not public", "[sourcecheck]") {
	Builder builder;
	builder.Add("alpha/include/alpha/Public.hpp", "void Reachable();");
	builder.Add("alpha/include/alpha/Private.hpp", "void OnlyAlphaCallsThis();");
	builder.Add("alpha/src/Alpha.hpp", "#include <alpha/Private.hpp>\n#include <alpha/Public.hpp>\n");
	builder.Add("beta/src/Beta.hpp", "#include <alpha/Public.hpp>\n");

	const std::vector<Finding> findings = CheckPublicHeader(builder.Result);
	REQUIRE(findings.size() == 1);
	CHECK(findings[0].Path == "alpha/include/alpha/Private.hpp");
}

TEST_CASE("A program's own main counts as an includer", "[sourcecheck]") {
	// Without this, every header of every program in the repository reports as
	// private - a program's surface is what its `main.cpp` consumes, and there
	// is no module above it to consume anything.
	Builder builder;
	builder.Add("studio/include/studio/Editor.hpp", "void Run();");
	builder.Add("studio/app/main.hpp", "#include <studio/Editor.hpp>\n");

	CHECK(CheckPublicHeader(builder.Result).empty());
}

TEST_CASE("Only the reporting rule declines to gate", "[sourcecheck]") {
	CHECK(sourcecheck::Gating("ecs-copy"));
	CHECK(sourcecheck::Gating("world-pointer"));
	CHECK(sourcecheck::Gating("name-id"));
	CHECK_FALSE(sourcecheck::Gating("public-header"));
	CHECK(sourcecheck::RuleNames().size() == 4);
}

TEST_CASE("A tests directory is read for includes and never reported against", "[sourcecheck]") {
	Builder builder = WithComponent();
	builder.Add("beta/tests/Panel.hpp", R"CPP(
#include <alpha/Health.hpp>
namespace beta {
	class Fixture {
	  public:
		void Run();
	  private:
		alpha::Health Bar;
	};
}
)CPP");
	builder.Result.Files.back().Test = true;

	CHECK(CheckEcsCopy(builder.Result).empty());

	// The include still counts. A suite in *another* module consuming a header
	// is real use of a public surface - `mono.unified_tests` is nothing else -
	// so the header is not reported even though its only outside reader is a
	// test.
	CHECK_FALSE(Mentions(CheckPublicHeader(builder.Result), "alpha/Health.hpp"));
}
