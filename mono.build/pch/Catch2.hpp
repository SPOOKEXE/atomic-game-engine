// Catch2 added to every precompiled header, for the targets that can see it.
//
// `catch_tostring.hpp` pulls in `<chrono>`, which pulls in `<format>`, so every
// test source parses both again. `__has_include` reads the target's own include
// path, so only targets that link Catch2 get it, and the rest of the PCH stays
// the same for every library.
//
// Measured on `dev` (GCC, -O0, full debug, ccache off) over the sources of
// test_core, test_ecs, test_scene, test_render and test_studio: test objects
// went from 755 to 683 CPU-seconds, -9.5%. Adding glm's quaternion header the
// same way was also measured, at -1% on libraries, and left out.
#pragma once

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#endif
