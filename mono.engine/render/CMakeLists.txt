# L12 [client]. Owns its GLSL: the shaders live beside the code that binds them,
# and mono_add_library compiles shaders/ into shaderstage/render/, which every
# program linking this module then stages as <program>/shaders/render/.
#
# A server/ directory that has grown a shaders/ folder is a link-line mistake
# anyone can see without a graph query.

mono_add_library(render
	TIER client
	DEPS
		Engine::core

		# L7, and below this. `scene::DrawInstance` is what a world publishes and
		# `scene::Camera` is what it is looked at through; converting either into
		# a GPU layout is this module's job and nobody else's, so this module is
		# where they arrive. The alternative was a `render::Instance` the client
		# converted into — which is two descriptions of one frame, and the debt
		# `scene` exists to pay off.
		# L9. **What is drawn, decided before anything is bound.** The frustum
		# and the cull are arithmetic over a `shared` type, so they live where
		# a headless host can also reach them — this module owns the device and
		# not the decisions.
		Engine::graph
		Engine::scene

	# Both private. SDL's GPU types are behind the pimpl in Renderer.cpp — only
	# `struct SDL_Window;` reaches a public header, as a forward declaration —
	# and no shaderc type appears in ShaderCompiler.hpp at all, which is the
	# reason that wrapper exists.
	#
	# shaderc carries its glslang and SPIRV-Tools dependencies transitively, so
	# naming it is the whole wiring.
	VENDOR SDL3::SDL3 shaderc
)
