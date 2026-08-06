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

		# L7, `shared`. **The list a shipped client's interface is drawn from.**
		# `mono.client` does not link `Engine::ui` and must not, so the second
		# consumer of `gui::DrawList` is here — and it is a *consumer*, not a
		# second compile, which is what stops the two backends drifting about
		# where an element is.
		Engine::gui

		Engine::scene

		# **What a mesh and a texture *are*.** `MeshTable` and `TextureTable`
		# take `assets::MeshData` and `assets::TextureData` and hand back a
		# buffer range and a device texture, which is the whole of what v0.9
		# added here: a `DrawInstance::Mesh` was a name nothing could resolve.
		#
		# Public rather than private, because both tables are public headers and
		# both name the asset types in their signatures — a caller registering a
		# fetched mesh has to be able to spell one.
		Engine::assets

		# **What a mesh and a texture *are*.** `MeshTable` and `TextureTable`
		# take `assets::MeshData` and `assets::TextureData` and hand back a
		# buffer range and a device texture, which is the whole of what v0.9
		# added here: a `DrawInstance::Mesh` was a name nothing could resolve.
		#
		# Public rather than private, because both tables are public headers and
		# both name the asset types in their signatures — a caller registering a
		# fetched mesh has to be able to spell one.
		Engine::assets

	# Both private. SDL's GPU types are behind the pimpl in Renderer.cpp — only
	# `struct SDL_Window;` reaches a public header, as a forward declaration —
	# and no shaderc type appears in ShaderCompiler.hpp at all, which is the
	# reason that wrapper exists.
	#
	# shaderc carries its glslang and SPIRV-Tools dependencies transitively, so
	# naming it is the whole wiring.
	VENDOR SDL3::SDL3 shaderc

	# **The two standalone `stb` headers `mono.vendor/imgui` carries, and not
	# imgui itself.** `imstb_truetype.h` and `imstb_rectpack.h` are
	# public-domain single headers imgui bundles rather than parts of it, so
	# `GlyphAtlas` includes them and links no imgui symbol — which is what lets
	# one atlas serve both the editor and a shipped client without putting the
	# editor's toolkit into a game binary.
	#
	# `VENDOR_INCLUDES` rather than `VENDOR`, which is exactly the distinction
	# that parameter exists for: headers on the include path, no library on the
	# link line.
	VENDOR_INCLUDES ${CMAKE_SOURCE_DIR}/mono.vendor/imgui
)
