# The suite registry, and one main() per kind of binary that uses it.
#
# `mono_suites` is the registry alone: the identifiers a binary declares and the
# dependencies between them, which is what mono.tools/testrunner reads over
# `--mono-suites` to decide what a change could have affected. It links nothing,
# so a benchmark binary gets the same discovery and the same cascading signature
# without linking a test framework it has no use for.

add_library(mono_suites STATIC Suites.cpp)
add_library(Engine::suites ALIAS mono_suites)

target_compile_features(mono_suites PUBLIC cxx_std_20)
target_include_directories(mono_suites PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# One main() for every module's test binary. It exists so that a test binary can
# report the suite identifiers it contains before Catch2 sees the command line.

if(NOT MONO_BUILD_TESTS)
	return()
endif()

add_library(mono_testmain STATIC TestMain.cpp)
add_library(Engine::testmain ALIAS mono_testmain)

target_compile_features(mono_testmain PUBLIC cxx_std_20)
target_link_libraries(mono_testmain PUBLIC Engine::suites Catch2::Catch2)
