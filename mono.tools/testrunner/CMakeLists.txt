# The selective test runner.
#
# A library plus a thin executable, the same shape as a program, so that its
# guts are testable. The tests it grows are ordinary Catch2 suites with
# TEST_SUITE_ID — which means the runner discovers and re-runs its own tests
# like any other. A test runner that cannot see its own tests is one whose bugs
# hide other bugs.
#
# `shared` tier: it links no client or server code, and it is not shipped.

mono_add_library(testrunner
	TIER shared
	NAMESPACE Tool
	# Arguments only. Reusing the engine's declared-option parser is the whole
	# reason `--help` here looks like `--help` on the client.
	DEPS Engine::core
	# SHA-256 for the cascading signature. VENDOR, not VENDOR_PUBLIC: Sha256.hpp
	# holds its state behind a pimpl precisely so that no Crypto++ type reaches a
	# public header and nothing outside this module has to know it is here.
	#
	# This is the first target in the tree to link Crypto++, so it is also what
	# takes cryptopp from "configured" to "compiled". A `tools` build now builds
	# the library too; the client and server still do not.
	VENDOR Vendor::cryptopp
)

add_executable(testrunner app/main.cpp)
target_link_libraries(testrunner PRIVATE Tool::testrunner Engine::core)
target_compile_options(testrunner PRIVATE ${MONO_COMPILE_OPTIONS})

# Tools stage together, apart from the products. A tool is not a program a
# player runs, so it does not get a staged tree of its own.
set_target_properties(testrunner PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
