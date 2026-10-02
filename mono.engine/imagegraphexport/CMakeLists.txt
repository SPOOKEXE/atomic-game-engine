# L11 host boundary shared by Studio and command-line exporters.
mono_add_library(imagegraphexport
    TIER shared
    DEPS
        Engine::core
        Engine::assets
        Engine::imagegraph
        Engine::imagegraphio
        Engine::bake
        Engine::parallel
    VENDOR
        Vendor::json
        glm::glm
)
