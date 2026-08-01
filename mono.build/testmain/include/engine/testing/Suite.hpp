#pragma once

// Suite identifiers, and the dependencies between them.
//
// A test file declares what it is and what it sits on top of:
//
//     TEST_SUITE_ID("engine.ecs.column.core")
//     TEST_DEPENDS("engine.core.memory.arena")
//
// Identifiers are hand-declared because a person knows what a test is about.
// The set of *files* a test touches is not declared here — a hand-written file
// list goes stale silently, and a stale list means a skipped test that should
// have run. mono.tools/testrunner derives that from the compiler's .d files
// instead. repo_layout.md §14.
//
// Both macros register at static-initialisation time, so `--mono-suites` can
// print what a binary actually contains rather than what a script guessed.

#include <string_view>
#include <vector>

namespace engine::testing {

	struct Suite {
		std::string_view Id;
		// __FILE__ of the declaration. The runner needs a suite's translation
		// unit to look up its header closure, and asking the compiler beats
		// scanning sources for the macro and hoping the regex holds.
		std::string_view File;
		std::vector<std::string_view> Depends;
	};

	class Registry {
	  public:
		// Declares a suite, or returns the existing one if the identifier
		// repeats. Repeating is legitimate: a suite may span several files,
		// and the first file to declare it is the one recorded.
		static Suite &Declare(std::string_view id, std::string_view file = {});

		static const std::vector<Suite> &All();

	  private:
		Registry() = delete;
	};

	// Registration objects. Namespace-scope statics, one per macro use.
	struct SuiteDeclaration {
		SuiteDeclaration(std::string_view id, std::string_view file) {
			Registry::Declare(id, file);
		}
	};

	struct SuiteDependency {
		SuiteDependency(std::string_view id, std::string_view dependsOn) {
			Registry::Declare(id).Depends.push_back(dependsOn);
		}
	};
}

#define ENGINE_TESTING_CONCAT_(a, b) a##b
#define ENGINE_TESTING_CONCAT(a, b) ENGINE_TESTING_CONCAT_(a, b)

// The identifier is stashed in a file-local constant so that TEST_DEPENDS below
// does not have to repeat it, which is the version that drifts.
#define TEST_SUITE_ID(id)                                                                                    \
	namespace {                                                                                              \
		constexpr std::string_view MonoTestSuiteId = id;                                                     \
		const ::engine::testing::SuiteDeclaration MonoTestSuiteDeclaration{id, __FILE__};                    \
	}

#define TEST_DEPENDS(dependency)                                                                             \
	namespace {                                                                                              \
		const ::engine::testing::SuiteDependency                                                             \
			ENGINE_TESTING_CONCAT(MonoTestSuiteDependency, __LINE__){MonoTestSuiteId, dependency};           \
	}
