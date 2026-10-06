# L11 host boundary shared by Studio and command-line exporters.
mono_add_library(imagegraphexport
    TIER shared
    DEPS
        Engine::imagegraphfont
        Engine::core
        Engine::assets
        Engine::imagegraph
        Engine::imagegraphio
        Engine::imagegraphphysics
        Engine::bake
        Engine::parallel
    VENDOR
        Vendor::json
        Vendor::miniz
        glm::glm
)

# Independent file hosts reuse private Input and Json helpers.
set_target_properties(engine_imagegraphexport PROPERTIES UNITY_BUILD OFF)

if(TARGET test_imagegraphexport)
    target_link_libraries(test_imagegraphexport PRIVATE Vendor::miniz)
endif()
