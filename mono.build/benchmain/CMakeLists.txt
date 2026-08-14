# One main() for every module's benchmark binary.
#
# It links `mono_suites` and nothing else: a benchmark binary answers
# `--mono-suites` the way a test binary does, so mono.tools/benchrunner gets the
# same discovery and the same cascading signature - but it links no test
# framework, because a benchmark measures and does not assert, and a framework's
# per-case overhead would land inside the numbers.

add_library(mono_benchmain STATIC BenchMain.cpp)
add_library(Engine::benchmain ALIAS mono_benchmain)

target_compile_features(mono_benchmain PUBLIC cxx_std_20)
target_include_directories(mono_benchmain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mono_benchmain PUBLIC Engine::suites)
