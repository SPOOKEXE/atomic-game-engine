# One main() for every module's test binary. It exists so that a test binary can
# report the suite identifiers it contains before Catch2 sees the command line —
# that listing is what mono.tools/testrunner reads to decide what to re-run.

add_library(mono_testmain STATIC TestMain.cpp)
add_library(Engine::testmain ALIAS mono_testmain)

target_compile_features(mono_testmain PUBLIC cxx_std_20)
target_include_directories(mono_testmain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(mono_testmain PUBLIC Catch2::Catch2)
