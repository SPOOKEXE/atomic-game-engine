// A picture of a mesh, which has to be a render.
//
// **`explorer-plus` is where the shape of this came from.** Its rows carry class
// icons and its 3D preview is a `ViewportFrame` that exists only for the row
// under the cursor — a live camera on a cloned object rather than a cached
// bitmap. That is the same conclusion `Thumbnails.cpp` reaches from the other
// end: a picture of a mesh needs a camera, a pass and a target, and one per row
// of a list of hundreds is not a thing to attempt. One, for the hovered row, is.
//
// ## How it draws
//
// **A viewport slot in the editor's existing round robin, and not a second
// `Render` call.** `Renderer::Render` owns the whole frame — swapchain,
// interface, present — so it draws one world per call, which is why
// `PresentWorld` rotates through the open panels a frame at a time. The preview
// joins that rotation. It refreshes at a fraction of the frame rate and that is
// invisible for a thing being looked at, where a second `Render` a frame would
// be a second present.
//
// ## How it frames
//
// The same arithmetic the reference uses, because it is the arithmetic:
//
//     distance = (radius / tan(fov / 2)) * padding
//
// `assets::MeshData` derives its own bounding box from the vertices — nothing on
// disk states one, deliberately — so the radius is the true one and a mesh that
// lied about its size could not point the camera at nothing.
//
// ## What it refuses
//
// **The triangle count, checked after the decode and before the upload.** That
// is the only place it can be: an `.amesh` states its counts in a header the
// reader has already validated, so the number is free, and what is being avoided
// is a GPU upload and a draw rather than the decode. `explorer-plus` prices its
// clones the same way — walk first, refuse with a sentence, never clone and
// regret it.

#include <engine/assets/Mesh.hpp>
#include <engine/core/Log.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <algorithm>
#include <cdn/LocalStore.hpp>
#include <cmath>
#include <fstream>
#include <studio/Editor.hpp>
#include <studio/Preview.hpp>
#include <vector>

namespace studio {

	namespace {
		// How much wider than the mesh's bounding sphere the frame is.
		constexpr float CAMERA_PADDING = 1.25f;

		// Where the camera stands, as a direction from the mesh.
		//
		// **Three quarters and slightly above**, which is the angle every asset
		// browser uses and for a reason worth stating: a straight-on view of a
		// character is a silhouette, and a top-down one is a hat. This is the
		// one direction that shows depth on almost anything.
		const engine::core::Vector3 CAMERA_DIRECTION{1.0f, 0.65f, 1.0f};

		std::optional<std::vector<std::byte>> ReadWholeFile(const std::filesystem::path &path) {
			std::error_code failure;
			const auto size = std::filesystem::file_size(path, failure);
			if (failure || size == 0 || size > PREVIEW_MAXIMUM_SOURCE_BYTES) {
				return std::nullopt;
			}

			std::ifstream file(path, std::ios::binary);
			if (!file) {
				return std::nullopt;
			}

			std::vector<std::byte> bytes(static_cast<size_t>(size));
			file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
			if (!file) {
				return std::nullopt;
			}
			return bytes;
		}
	}

	std::string Editor::PreviewMeshName(const std::string &name) {
		// Prefixed for `ThumbnailTextureName`'s reason: the renderer resolves a
		// part's mesh out of the same table by name, and a preview registered
		// under the real name would be the geometry every part in the scene got.
		return "studio.preview/" + name;
	}

	PreviewState Editor::LoadPreviewMesh(const std::string &name) {
		const auto found = PreviewMeshes.find(name);
		if (found != PreviewMeshes.end()) {
			return found->second;
		}

		const PreviewState state = BuildPreviewMesh(name);
		PreviewMeshes.emplace(name, state);
		return state;
	}

	PreviewState Editor::BuildPreviewMesh(const std::string &name) {
		// **`cdn::FindInStore` and not a folder spelled here.** This read
		// `raw/<name>` and was right for as long as the publisher walked `raw/`;
		// the day it walked `baked/`, every preview in the editor resolved to a
		// missing file and turned into "no local pixels" with nothing said.
		const std::filesystem::path source = cdn::FindInStore(cdn::DefaultLocalPaths(), name);

		std::error_code failure;
		if (!std::filesystem::is_regular_file(source, failure)) {
			return PreviewState::Unavailable;
		}

		const std::optional<std::vector<std::byte>> bytes = ReadWholeFile(source);
		if (!bytes) {
			return PreviewState::TooLarge;
		}

		// **`assets::Mesh` and nothing else.** A `.pmx` or a `.glb` in the store
		// is a *source* format — the runtime does not decode those, which is
		// `assets::Texture`'s standing rule one kind over — so an unbaked store
		// previews nothing, and the caption says to run `assetc`. Reaching for
		// `bake::ReadModel` here would put an importer behind a hover.
		engine::core::ByteReader reader(*bytes);
		engine::assets::MeshData mesh;
		if (!engine::assets::Mesh::Read(reader, mesh)) {
			return PreviewState::Unavailable;
		}

		const auto triangles = static_cast<uint32_t>(mesh.Indices.size() / 3);
		if (triangles > PREVIEW_MAXIMUM_TRIANGLES) {
			// **Refused before the upload, which is the whole point of checking
			// here.** The decode already happened and was bounded by the file
			// size; what this avoids is device memory and a draw call for
			// something nobody asked to render, only to hover over.
			ENGINE_INFO("preview: {} has {} triangle(s) — past the preview budget", name, triangles);
			return PreviewState::TooLarge;
		}

		// The true bounds, derived from the vertices by `Mesh::Read`. Kept so
		// the camera frames this mesh rather than a unit cube — a bake that was
		// not `--model-size 1` would otherwise be a speck or fill the frame.
		PreviewBounds bounds;
		bounds.Centre = (mesh.Minimum + mesh.Maximum) * 0.5f;

		const engine::core::Vector3 extent = (mesh.Maximum - mesh.Minimum) * 0.5f;
		bounds.Radius = std::max(
			0.05f, std::sqrt(extent.X * extent.X + extent.Y * extent.Y + extent.Z * extent.Z)
		);

		const engine::core::Name key(PreviewMeshName(name));
		if (!Renderer.AddMesh(key, mesh)) {
			return PreviewState::Unavailable;
		}

		PreviewMeshBounds.emplace(name, bounds);
		return PreviewState::Ready;
	}

	bool Editor::DrawPreviewViewport(const std::string &name, float side) {
		const auto bounds = PreviewMeshBounds.find(name);
		if (bounds == PreviewMeshBounds.end()) {
			return false;
		}

		// **What the slot should draw, recorded for `PresentWorld` to pick up.**
		// The render happens outside the interface pass — imgui records draw
		// lists before the renderer runs — so this frame shows whatever the
		// rotation last drew into the slot, and the request applies to the next
		// turn. That is one frame of latency on a thing being hovered.
		PreviewWanted = name;
		PreviewSide = static_cast<uint32_t>(std::max(side, 32.0f));
		return true;
	}

	bool Editor::RenderPreviewSlot() {
		if (PreviewWanted.empty()) {
			return false;
		}

		const auto bounds = PreviewMeshBounds.find(PreviewWanted);
		if (bounds == PreviewMeshBounds.end()) {
			return false;
		}

		engine::render::SceneTarget target;
		target.Width = PreviewSide;
		target.Height = PreviewSide;

		// One instance, at the origin, its own size.
		//
		// **`HalfExtent` is the mesh's own**, not a unit box: the renderer culls
		// against it, and a half-extent smaller than the geometry would clip the
		// preview against a frustum fitted to the wrong thing.
		engine::scene::DrawInstance instance;
		// **Offset so the mesh's own centre lands on the origin**, which is what
		// lets the camera aim at a fixed point: a mesh baked off-centre would
		// otherwise sit outside a frame fitted around nothing.
		instance.Frame = engine::core::CFrame(engine::core::Vector3{} - bounds->second.Centre);
		instance.HalfExtent = engine::core::Vector3{
			bounds->second.Radius, bounds->second.Radius, bounds->second.Radius
		};
		instance.Tint = engine::core::Color3{0.92f, 0.92f, 0.94f};
		instance.Mesh = engine::core::Name(PreviewMeshName(PreviewWanted));

		const engine::scene::DrawInstance one[]{instance};

		engine::scene::Camera lens;
		lens.FieldOfViewRadians = 0.6109f; // 35 degrees, matching the reference.

		// **The near plane is pulled in with the subject.** The default is a
		// tenth of a metre, which clips the front off anything baked at unit
		// scale — the meshes `assetc --model-size 1` produces are exactly that.
		lens.NearPlane = std::max(0.001f, bounds->second.Radius * 0.01f);
		lens.FarPlane = std::max(10.0f, bounds->second.Radius * 100.0f);

		const float distance =
			(bounds->second.Radius / std::tan(lens.FieldOfViewRadians * 0.5f)) * CAMERA_PADDING;

		const engine::core::CFrame eye =
			engine::core::CFrame::LookAt(CAMERA_DIRECTION.Unit() * distance, engine::core::Vector3{});

		// **No interface hook and no surfaces.** This slot draws one mesh into a
		// small square; passing the editor's chrome would draw the whole editor
		// into a 132-pixel texture, and a mirror pass would render a scene that
		// is not there.
		Renderer.Render(
			eye,
			lens,
			std::span<const engine::scene::DrawInstance>(one),
			Overlay,
			{},
			nullptr,
			&target,
			PREVIEW_SLOT
		);
		return true;
	}
}
