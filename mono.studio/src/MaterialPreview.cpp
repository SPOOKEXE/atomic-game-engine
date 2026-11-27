// The material graph preview panel: the selected `MaterialGraph` on a shape.
//
// **Compiled here, under a namespace of its own, rather than demanded from the
// world.** A world compiles the shaders its materials name and nothing else,
// so a graph no part uses yet would never reach the renderer through it - and
// asking the world to demand one would be editor state in the world, which
// `mono.studio/AGENTS.md` forbids. So the panel compiles the graph's document
// the way the world would (`CompileMaterialGraph`, then the shader assembly
// with the world's own modules), installs both stages under
// `studio.material-preview`, and draws through the shared preview slot under
// that same name, like the mesh and procedural previews.
//
// **Two captured images, alternating**, so the picture on screen is never the
// texture being replaced while the draw data that shows it is still in flight.

#include <engine/assets/Builtin.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/resources/ShaderAssembly.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/content/MaterialGraphs.hpp>
#include <engine/scene/content/Shaders.hpp>
#include <engine/ui/Theme.hpp>

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <imgui.h>
#include <studio/Editor.hpp>

namespace studio {
	namespace {
		// The namespace the preview's shader and view live under.
		const engine::core::Name PREVIEW_OWNER("studio.material-preview");
		// The shader's name inside it.
		const engine::core::Name PREVIEW_SHADER("studio.material-preview/graph");
		// Frames a capture waits after the last, so the draw data showing the
		// older image has retired before that image is replaced.
		constexpr uint32_t CAPTURE_SPACING = 4;
		// The picture's side, in pixels.
		constexpr uint32_t PREVIEW_SIDE = 384;
		// Radians a second the turntable turns.
		constexpr float TURN_RATE = 0.6f;

		const char *const SHAPES[] = {"Sphere", "Cube", "Plane"};

		engine::core::Name ShapeMesh(int shape) {
			static const engine::core::Name names[] = {
				engine::core::Name("studio.preview/material-sphere"),
				engine::core::Name("studio.preview/material-cube"),
				engine::core::Name("studio.preview/material-plane"),
			};
			return names[std::clamp(shape, 0, 2)];
		}
	}

	void Editor::DrawMaterialPreview() {
		MaterialPreviewWanted = false;
		if (!ShowMaterialPreview) {
			return;
		}
		if (!ImGui::Begin("Material Preview", &ShowMaterialPreview)) {
			ImGui::End();
			return;
		}
		++MaterialPreviewAge;

		// The selection, if it is one material graph.
		const WorldId world = SelectionWorld.IsValid() ? static_cast<WorldId>(SelectionWorld) : WorldId{};
		engine::ecs::Entity graph{};
		engine::core::Name name;
		std::string error;
		bool changed = false;
		if (Universe && world.IsValid() && Selection.size() == 1) {
			Universe->Enter(world, [&](engine::ecs::Store &store) {
				const engine::ecs::Entity selected = Selection.front();
				if (!store.Alive(selected) || !store.IsA(selected, engine::scene::MaterialGraphClass())) {
					return;
				}
				const engine::scene::ShaderSource *source = store.Get<engine::scene::ShaderSource>(selected);
				if (source == nullptr) {
					return;
				}
				graph = selected;
				name = store.InstanceNameOf(selected);
				if (MaterialPreviewWorld == world && MaterialPreviewGraph == graph &&
					MaterialPreviewRevision == source->Revision) {
					error = MaterialPreviewError;
					return;
				}
				changed = true;
				MaterialPreviewRevision = source->Revision;
				const engine::scene::CompiledGraph compiled =
					engine::scene::CompileMaterialGraph(source->Code, name.Text());
				if (!compiled.Error.empty()) {
					error = compiled.Error;
					return;
				}
				// The world's own modules, as the world's shader library would
				// resolve them.
				const engine::render::ShaderModuleResolver modules =
					[&store](std::string_view module) -> std::optional<std::string> {
					engine::scene::ShaderText held =
						engine::scene::ShaderModuleTextOf(store, engine::core::Name(module));
					if (!held.Found) return std::nullopt;
					return std::move(held.Code);
				};
				const engine::render::ShaderAssembly assembled =
					engine::render::AssembleShader(PREVIEW_SHADER.Text(), compiled.Source, modules);
				if (!assembled.Error.empty()) {
					error = assembled.Error;
					return;
				}
				MaterialPreviewCompiler.SetOptimise(true);
				const auto forward = MaterialPreviewCompiler.Compile(
					assembled.Forward, engine::render::ShaderStage::Fragment, name.Text()
				);
				const auto gbuffer = MaterialPreviewCompiler.Compile(
					assembled.GBuffer, engine::render::ShaderStage::Fragment, name.Text()
				);
				if (forward.Failed || gbuffer.Failed) {
					error = forward.Failed ? forward.Error : gbuffer.Error;
					return;
				}
				if (!Renderer.AddMaterialShader(
						PREVIEW_SHADER, forward.SpirV, PREVIEW_OWNER, gbuffer.SpirV
					)) {
					error = "the renderer refused the compiled graph";
				}
			});
		}
		if (graph == engine::ecs::Entity{}) {
			MaterialPreviewGraph = {};
			ImGui::TextDisabled("Select a MaterialGraph.");
			ImGui::End();
			return;
		}
		MaterialPreviewWorld = world;
		MaterialPreviewGraph = graph;
		MaterialPreviewName = name;
		if (changed) {
			MaterialPreviewError = error;
		}

		ImGui::TextUnformatted(name.Text().data(), name.Text().data() + name.Text().size());
		ImGui::SetNextItemWidth(120);
		ImGui::Combo("##shape", &MaterialPreviewShape, SHAPES, 3);
		ImGui::SameLine();
		ImGui::Checkbox("Turn", &MaterialPreviewTurning);
		if (!MaterialPreviewError.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::TextWrapped("%s", MaterialPreviewError.c_str());
			ImGui::PopStyleColor();
		}

		if (MaterialPreviewTurning) {
			MaterialPreviewOrbit += TURN_RATE * ImGui::GetIO().DeltaTime;
		}
		const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
		if (MaterialPreviewHandle != nullptr && side > 8.0f) {
			ImGui::Image(ImTextureRef(MaterialPreviewHandle), ImVec2(side, side));
			// Dragging the picture turns it.
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
				MaterialPreviewOrbit += ImGui::GetIO().MouseDelta.x * 0.01f;
			}
		} else {
			ImGui::TextDisabled("Rendering...");
		}
		MaterialPreviewWanted = MaterialPreviewError.empty();
		ImGui::End();
	}

	bool Editor::RenderMaterialPreviewSlot() {
		using namespace engine;
		if (!MaterialPreviewWanted || Renderer.Backend().Device == nullptr ||
			MaterialPreviewAge < CAPTURE_SPACING) {
			return false;
		}
		ENGINE_PROFILE_CAT("material preview", core::ProfileCategory::Render);
		// The shapes, uploaded once.
		if (!MaterialPreviewMeshes) {
			const assets::BuiltinMesh shapes[] = {
				assets::BuiltinMesh::Sphere, assets::BuiltinMesh::Cube, assets::BuiltinMesh::Plane
			};
			for (int index = 0; index < 3; ++index) {
				if (!Renderer.AddMesh(ShapeMesh(index), assets::MakeBuiltin(shapes[index]))) {
					ENGINE_WARN("material preview: the renderer refused the {} mesh", SHAPES[index]);
				}
			}
			MaterialPreviewMeshes = true;
		}
		scene::DrawInstance instance;
		instance.Mesh = ShapeMesh(MaterialPreviewShape);
		instance.HalfExtent = MaterialPreviewShape == 2 ? core::Vector3{1, 0.01f, 1} : core::Vector3{1, 1, 1};
		instance.Shader = PREVIEW_SHADER;
		// The plane (on XZ, facing up) stands to face the camera.
		const glm::quat turn = glm::angleAxis(MaterialPreviewOrbit, glm::vec3(0, 1, 0));
		const glm::quat stand = MaterialPreviewShape == 2 ? glm::angleAxis(1.5707963f, glm::vec3(1, 0, 0))
														  : glm::quat(1, 0, 0, 0);
		instance.Frame = core::CFrame(core::Vector3{}, turn * stand);

		scene::Camera camera;
		camera.FieldOfViewRadians = 0.6109f;
		camera.NearPlane = 0.05f;
		camera.FarPlane = 50.0f;
		render::SceneTarget target;
		target.Width = target.Height = PREVIEW_SIDE;
		render::View view;
		view.Camera = camera;
		view.CameraFrame = core::CFrame::LookAt(core::Vector3{0.0f, 1.2f, 4.2f}, core::Vector3{});
		view.Instances = std::span<const scene::DrawInstance>(&instance, 1);
		view.Target = &target;
		view.Slot = PreviewSlot();
		view.WorldName = PREVIEW_OWNER;
		if (LastPostProcessShader.IsValid()) {
			Renderer.ClearPostProcessShader();
			LastPostProcessShader = {};
		}
		Renderer.Render(std::span<const render::View>(&view, 1), Overlay, nullptr, true, &Interface);

		// Into the image not on screen, then show it.
		const int next = MaterialPreviewShown == 0 ? 1 : 0;
		if (!MaterialPreviewTextures[next].IsValid()) {
			MaterialPreviewTextures[next] =
				core::Name(std::string("studio.material-preview/image") + std::to_string(next));
		}
		if (Renderer.CaptureSceneTexture(PreviewSlot(), MaterialPreviewTextures[next])) {
			if (void *handle = Renderer.TextureHandle(MaterialPreviewTextures[next]); handle != nullptr) {
				MaterialPreviewHandle = handle;
				MaterialPreviewShown = next;
			}
		}
		MaterialPreviewAge = 0;
		return true;
	}
}
