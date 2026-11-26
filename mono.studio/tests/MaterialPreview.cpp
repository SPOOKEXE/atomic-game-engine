#include <engine/assets/Texture.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/resources/TextureTable.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/content/MaterialGraphs.hpp>
#include <engine/scene/content/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <SDL3/SDL_gpu.h>

#include <cstring>
#include <imgui.h>
#include <studio/Editor.hpp>

TEST_SUITE_ID("studio.materialpreview")
TEST_DEPENDS("studio.viewports")

namespace studio {
	// Reaches the panel's state the way the editor's own frame does.
	struct MaterialPreviewProbe {
		static bool Initialise(Editor &editor) {
			Options options;
			options.Headless = true;
			options.StartViewports = 1;
			return editor.Initialise(options);
		}

		// A graph in the active world, selected.
		static void SelectGraph(Editor &editor, const char *document) {
			engine::ecs::Entity graph{};
			editor.Universe->Enter(editor.Active, [&](engine::ecs::Store &store) {
				graph = store.CreateInstance(engine::scene::MaterialGraphClass(), "Glow");
				REQUIRE(engine::scene::SetShaderSource(store, graph, document));
			});
			editor.SelectionWorld = editor.Active;
			editor.Selection = std::vector<engine::ecs::Entity>{graph};
			editor.ShowMaterialPreview = true;
		}

		// One frame of the panel, then the preview slot as the editor's
		// rotation would draw it.
		static bool Frame(Editor &editor) {
			ImGui::NewFrame();
			editor.DrawMaterialPreview();
			ImGui::Render();
			return editor.RenderMaterialPreviewSlot();
		}

		static const std::string &Error(const Editor &editor) {
			return editor.MaterialPreviewError;
		}

		// The picture last captured, as 8-bit RGBA (red first), or empty.
		static engine::assets::TextureData Picture(Editor &editor) {
			engine::assets::TextureData out;
			if (editor.MaterialPreviewShown < 0) return out;
			const engine::core::Name name = editor.MaterialPreviewTextures[editor.MaterialPreviewShown];
			auto *texture = static_cast<SDL_GPUTexture *>(editor.Renderer.TextureHandle(name));
			SDL_GPUDevice *device = static_cast<SDL_GPUDevice *>(editor.Renderer.Backend().Device);
			uint32_t width = 0, height = 0;
			if (texture == nullptr || device == nullptr || !editor.Renderer.TextureSize(name, width, height) || width == 0)
				return out;
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = width * height * 4;
			SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &info);
			SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
			SDL_GPUTextureRegion region{};
			region.texture = texture;
			region.w = width;
			region.h = height;
			region.d = 1;
			SDL_GPUTextureTransferInfo destination{};
			destination.transfer_buffer = transfer;
			destination.pixels_per_row = width;
			destination.rows_per_layer = height;
			SDL_DownloadFromGPUTexture(copy, &region, &destination);
			SDL_EndGPUCopyPass(copy);
			SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
			SDL_WaitForGPUFences(device, true, &fence, 1);
			SDL_ReleaseGPUFence(device, fence);
			const auto format = static_cast<SDL_GPUTextureFormat>(editor.Renderer.Backend().ColourFormat);
			const bool bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
			out.Width = width;
			out.Height = height;
			out.Pixels.resize(size_t(width) * height * 4);
			if (const auto *mapped = static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, transfer, false))) {
				std::memcpy(out.Pixels.data(), mapped, out.Pixels.size());
				SDL_UnmapGPUTransferBuffer(device, transfer);
			}
			SDL_ReleaseGPUTransferBuffer(device, transfer);
			if (bgra)
				for (size_t at = 0; at < out.Pixels.size(); at += 4)
					std::swap(out.Pixels[at], out.Pixels[at + 2]);
			return out;
		}
	};
}

TEST_CASE("the material preview draws the selected graph", "[studio][material][gpu][.]") {
	studio::Editor editor;
	REQUIRE(studio::MaterialPreviewProbe::Initialise(editor));
	// A graph that glows red whatever the light.
	studio::MaterialPreviewProbe::SelectGraph(
		editor,
		"parameter vec3 colour = 1, 0, 0\n"
		"output Albedo = colour\n"
		"output Emissive = colour\n"
	);
	bool drawn = false;
	for (int frame = 0; frame < 30 && !drawn; ++frame)
		drawn = studio::MaterialPreviewProbe::Frame(editor);
	INFO(studio::MaterialPreviewProbe::Error(editor));
	REQUIRE(studio::MaterialPreviewProbe::Error(editor).empty());
	REQUIRE(drawn);
	const auto picture = studio::MaterialPreviewProbe::Picture(editor);
	REQUIRE(picture.Width > 0);
	REQUIRE(picture.Pixels.size() >= size_t(picture.Width) * picture.Height * 4);
	// The middle of the picture is the shape, wearing the graph.
	const size_t at = (size_t(picture.Height / 2) * picture.Width + picture.Width / 2) * 4;
	const int r = std::to_integer<int>(picture.Pixels[at]), g = std::to_integer<int>(picture.Pixels[at + 1]),
			  b = std::to_integer<int>(picture.Pixels[at + 2]);
	INFO("centre " << r << " " << g << " " << b);
	CHECK(r > 120);
	CHECK(r > 3 * g);
	CHECK(r > 3 * b);
}

TEST_CASE("the material preview reports a graph that does not compile", "[studio][material][gpu][.]") {
	studio::Editor editor;
	REQUIRE(studio::MaterialPreviewProbe::Initialise(editor));
	studio::MaterialPreviewProbe::SelectGraph(editor, "node a = add uv normal\n");
	(void)studio::MaterialPreviewProbe::Frame(editor);
	CHECK(studio::MaterialPreviewProbe::Error(editor).starts_with("1: "));
}
