# L7. What a 2D thing in a game *is* - the widget set a game builds its own
# interface out of.
#
# `shared`, and that is the load-bearing half. A server authors a `ScreenGui`
# and replicates it, so a type only a client could name would be a type only a
# client could build one out of. That is `scene`'s argument for `Part` and it is
# the same one, one dimension down.
#
# **Not `mono.engine/ui`.** That module is Dear ImGui and is the *editor's*
# toolkit at L12 `client`; this is the engine's own and nothing a game ships
# touches that one. They coexist deliberately for a version - `ui/AGENTS.md`
# says why - and they share validated font bytes and canonical shaped positions.
# Each backend owns its raster storage on the side of the seam where a
# rasteriser can exist.
#
# No `scene`. Layout is arithmetic over `core/types`, and the one thing that
# looked like a reason to link `scene` - `NormalId` for a surface gui - is a
# six-member enum registered by name. Both modules are `shared`, so the tier
# check would not have caught the edge; AGENTS.md is where it is refused.
mono_add_library(gui
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
	VENDOR
		freetype
		harfbuzz
		utf8proc
		SheenBidi
		Vendor::unibreak
)

option(MONO_FUZZ_GUI "Build hostile GUI parsers with libFuzzer and sanitizers" OFF)
if(MONO_FUZZ_GUI)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "MONO_FUZZ_GUI requires Clang with libFuzzer")
	endif()

	# Each target compiles the boundary it exercises. Linking only Engine::gui
	# would leave these parsers outside libFuzzer's coverage instrumentation.
	set(gui_fuzz_options -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g)

	add_executable(fuzz_gui_document fuzz/Document.cpp src/Document.cpp)
	target_include_directories(fuzz_gui_document PRIVATE include)
	target_link_libraries(fuzz_gui_document PRIVATE Engine::gui freetype harfbuzz utf8proc SheenBidi Vendor::unibreak)
	target_compile_options(fuzz_gui_document PRIVATE ${gui_fuzz_options})
	target_link_options(fuzz_gui_document PRIVATE -fsanitize=fuzzer,address,undefined)

	add_executable(fuzz_gui_text fuzz/Text.cpp src/RichText.cpp src/Localization.cpp src/ShapedText.cpp)
	target_include_directories(fuzz_gui_text PRIVATE include)
	target_compile_definitions(fuzz_gui_text PRIVATE MONO_SOURCE_DIRECTORY="${CMAKE_SOURCE_DIR}")
	target_link_libraries(fuzz_gui_text PRIVATE Engine::gui freetype harfbuzz utf8proc SheenBidi Vendor::unibreak)
	target_compile_options(fuzz_gui_text PRIVATE ${gui_fuzz_options})
	target_link_options(fuzz_gui_text PRIVATE -fsanitize=fuzzer,address,undefined)

	add_executable(fuzz_gui_binding fuzz/Binding.cpp src/Binding.cpp)
	target_include_directories(fuzz_gui_binding PRIVATE include)
	target_link_libraries(fuzz_gui_binding PRIVATE Engine::gui)
	target_compile_options(fuzz_gui_binding PRIVATE ${gui_fuzz_options})
	target_link_options(fuzz_gui_binding PRIVATE -fsanitize=fuzzer,address,undefined)

	foreach(target IN ITEMS fuzz_gui_document fuzz_gui_text fuzz_gui_binding)
		set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/fuzz")
	endforeach()
endif()
