# The engine API dump generator.
#
# A library rather than code inside `bindings` so its Catch2 suite can feed it
# small declaration files. `bindings` links it and writes `api-dump.json` in the
# same run that writes the declarations, so the dump cannot describe a different
# surface than `engine.d.luau` does.
#
# `Luau.Analysis` reads the declarations: the dump takes signatures from the type
# checker instead of a second parser.
mono_add_library(apidump
	TIER shared
	NAMESPACE Tool
	DEPS Engine::core
	VENDOR Vendor::luau_analysis Vendor::json
)

if(TARGET test_apidump)
	target_link_libraries(test_apidump PRIVATE Vendor::json)
endif()
