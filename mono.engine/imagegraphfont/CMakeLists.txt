# Reusable font observations and artifact ownership carry no foreign bake exporter.
mono_add_library(imagegraphfont
    TIER shared
    DEPS
        Engine::core
        Engine::assets
        Engine::gui
        Engine::imagegraph
    VENDOR
        Vendor::json
)

if(TARGET test_imagegraphfont)
    target_link_libraries(test_imagegraphfont PRIVATE Vendor::json)
endif()
