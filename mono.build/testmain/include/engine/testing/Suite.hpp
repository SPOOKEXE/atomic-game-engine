#pragma once

// Suite identifiers, and the dependencies between them.
//
// A test file declares what it is and what it sits on top of:
//
//     TEST_SUITE_ID("engine.ecs.column.core")
//     TEST_DEPENDS("engine.core.memory.arena")
//
// Identifiers are hand-declared because a person knows what a test is about.
// The set of *files* a test touches is not declared here - a hand-written file
// list goes stale silently, and a stale list means a skipped test that should
// have run. mono.tools/testrunner derives that from the compiler's .d files
// instead. repo_layout.md §14.
//
// Both macros register at static-initialisation time, so `--mono-suites` can
// print what a binary actually contains rather than what a script guessed.

#include <string_view>
#include <vector>

namespace engine::testing {

	// One declared test suite, as the registry holds it.
	//
	// **The unit the runner re-runs at.** A suite is what `TEST_SUITE_ID` names,
	// and its granularity is the granularity of a selective test run - which is
	// why one file per public header is the default rather than one per module.
	struct Suite {
		// What the suite is called, from `TEST_SUITE_ID`. The name the runner
		// prints and the key it selects on.
		std::string_view Id;
		// __FILE__ of the declaration. The runner needs a suite's translation
		// unit to look up its header closure, and asking the compiler beats
		// scanning sources for the macro and hoping the regex holds.
		std::string_view File;
		// The suites this one needs to have passed first, from `TEST_DEPENDS`.
		// Ids rather than pointers, because a dependency may be declared in a
		// translation unit this one has never seen.
		std::vector<std::string_view> Depends;
	};

	// Every suite this binary declared, filled before `main` runs.
	//
	// **Static initialisation is the whole mechanism.** A suite registers by
	// declaring a namespace-scope object, so linking a test file is what puts it
	// in the list - there is no list to maintain and no way to add a file and
	// forget to register it.
	class Registry {
	  public:
		// Declares a suite, or returns the existing one if the identifier
		// repeats. Repeating is legitimate: a suite may span several files,
		// and the first file to declare it is the one recorded.
		static Suite &Declare(std::string_view id, std::string_view file = {});

		// Every declared suite, in declaration order.
		//
		// @return The suites. Valid for the life of the program.
		static const std::vector<Suite> &All();

	  private:
		Registry() = delete;
	};

	// Registration objects. Namespace-scope statics, one per macro use.
	struct SuiteDeclaration {
		// Registers one suite as a side effect of being constructed.
		//
		// @param id   What the suite is called.
		// @param file `__FILE__` at the declaration, for the header closure.
		SuiteDeclaration(std::string_view id, std::string_view file) {
			Registry::Declare(id, file);
		}
	};

	// Registers one edge of the dependency graph, the same way.
	struct SuiteDependency {
		// Records that `id` needs `dependsOn` first.
		//
		// @param id        The suite that depends.
		// @param dependsOn The suite it depends on. Declared or not - the
		//        runner resolves ids, so the order the two are linked in does
		//        not matter.
		SuiteDependency(std::string_view id, std::string_view dependsOn) {
			Registry::Declare(id).Depends.push_back(dependsOn);
		}
	};
}

// Token pasting, in the two steps it takes.
//
// **Two macros because one does not work.** `a##b` pastes its arguments before
// they are expanded, so a single macro given `__LINE__` produces the identifier
// `MonoTestSuiteDependency__LINE__`. The outer one expands its arguments first
// and the inner one pastes what came out.
//@{
#define ENGINE_TESTING_CONCAT_(a, b) a##b
#define ENGINE_TESTING_CONCAT(a, b) ENGINE_TESTING_CONCAT_(a, b)
//@}

// The identifier is stashed in a file-local constant so that TEST_DEPENDS below
// does not have to repeat it, which is the version that drifts.
#define TEST_SUITE_ID(id)                                                                                    \
	namespace {                                                                                              \
		constexpr std::string_view MonoTestSuiteId = id;                                                     \
		const ::engine::testing::SuiteDeclaration MonoTestSuiteDeclaration{id, __FILE__};                    \
	}

// Declares that this file's suite needs another one to have run first.
//
// Uses the id `TEST_SUITE_ID` stashed above, so the name is written once. The
// object is named by line number, which is what allows several of these in one
// file.
#define TEST_DEPENDS(dependency)                                                                             \
	namespace {                                                                                              \
		const ::engine::testing::SuiteDependency                                                             \
			ENGINE_TESTING_CONCAT(MonoTestSuiteDependency, __LINE__){MonoTestSuiteId, dependency};           \
	}
