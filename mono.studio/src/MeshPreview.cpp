// A picture of a mesh, which has to be a render.
//
// **`explorer-plus` is where the shape of this came from.** Its rows carry class
// icons and its 3D preview is a `ViewportFrame` that exists only for the row
// under the cursor - a live camera on a cloned object rather than a cached
// bitmap. That is the same conclusion `Thumbnails.cpp` reaches from the other
// end: a picture of a mesh needs a camera, a pass and a target, and one per row
// of a list of hundreds is not a thing to attempt. One, for the hovered row, is.
//
// ## How it draws
//
// **A viewport slot in the editor's existing round robin, and not a second
// `Render` call.** `Renderer::Render` owns the whole frame - swapchain,
// interface, present - so it draws one world per call, which is why
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
// `assets::MeshData` derives its own bounding box from the vertices - nothing on
// disk states one, deliberately - so the radius is the true one and a mesh that
// lied about its size could not point the camera at nothing.
//
// ## What it refuses
//
// **The triangle count, checked after the decode and before the upload.** That
// is the only place it can be: an `.amesh` states its counts in a header the
// reader has already validated, so the number is free, and what is being avoided
// is a GPU upload and a draw rather than the decode. `explorer-plus` prices its
// clones the same way - walk first, refuse with a sentence, never clone and
// regret it.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/Builtin.hpp>
#include <engine/assets/Material.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Resample.hpp>
#include <engine/assets/Texture.hpp>
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

		// How big a preview asked for by nobody in particular is rendered.
		//
		// **The size of the picture that gets kept, not of the row.** A row is
		// thirty-two pixels and a hover panel is a hundred and thirty-two, and
		// what `RenderPreviewSlot` captures is a thumbnail both of them draw -
		// so this is sized for the larger of the two rather than for whichever
		// asked first.
		constexpr float PREVIEW_AUTOMATIC_SIDE = 128.0f;

		// Where the camera stands, as a direction from the mesh.
		//
		// **Three quarters and slightly above**, which is the angle every asset
		// browser uses and for a reason worth stating: a straight-on view of a
		// character is a silhouette, and a top-down one is a hat. This is the
		// one direction that shows depth on almost anything.
		const engine::core::Vector3 CAMERA_DIRECTION{1.0f, 0.65f, 1.0f};

		// How fast the turntable goes round.
		//
		// **Slow enough to read, which is slower than it feels like it should
		// be.** A full turn every eight seconds lets somebody take in one side
		// before the next arrives; at a turn a second the silhouette is a blur
		// and the preview is worse than a still. `MeshGrid.luau`'s pedestals run
		// at about the same rate for the same reason.
		constexpr double TURNTABLE_RADIANS_PER_SECOND = 0.785;

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

		// **`Pending` is not remembered, and that is what makes a fetch work.**
		// A material whose colour map is not on this machine asks the delivery
		// client for it and answers `Pending`; caching that would mean the
		// answer never changed once the sheet landed, which is the same
		// "unavailable for ever" this was meant to fix. Every other state is
		// terminal and is cached, so a decode happens once.
		if (state != PreviewState::Pending) {
			PreviewMeshes.emplace(name, state);
		}
		return state;
	}

	void Editor::PumpRenderedPreviews() {
		// Taken rather than read, so a row that scrolls off screen stops being
		// asked for - the queue is rebuilt from what was drawn every frame.
		const std::vector<std::string> wanted = std::move(PreviewQueue);
		PreviewQueue.clear();

		// The cursor owns the slot when there is one. `DrawHoverPreview` would
		// overwrite the request anyway; returning here saves the decode.
		if (!HoverShowing.empty() || !PreviewWanted.empty()) {
			return;
		}

		// **At most one *build* per frame.** `LoadPreviewMesh` decodes a file
		// and uploads a mesh the first time it is asked, so walking a screenful
		// of new rows in one frame is a screenful of decodes in one frame - the
		// stall `PumpThumbnails` bounds for the bitmap half and this bounds for
		// the rendered one.
		bool built = false;

		for (const std::string &name : wanted) {
			if (PreviewMeshes.find(name) == PreviewMeshes.end()) {
				if (built) {
					continue;
				}
				built = true;
			}

			if (LoadPreviewMesh(name) != PreviewState::Ready) {
				continue;
			}

			// The first one that can be drawn takes the slot. The rest keep
			// their place in the list and get their turn on a later frame,
			// because they will record themselves again while they are visible.
			if (DrawPreviewViewport(name, PREVIEW_AUTOMATIC_SIDE)) {
				return;
			}
		}
	}

	PreviewState Editor::BuildPreviewMaterial(const std::string &name, engine::core::Name &texture) {
		const std::filesystem::path source = cdn::FindInStore(cdn::DefaultLocalPaths(), name);

		std::error_code failure;
		if (!std::filesystem::is_regular_file(source, failure)) {
			return PreviewState::Unavailable;
		}

		const std::optional<std::vector<std::byte>> bytes = ReadWholeFile(source);
		if (!bytes) {
			return PreviewState::TooLarge;
		}

		engine::assets::MaterialData material;
		engine::core::ByteReader reader(*bytes);
		if (!engine::assets::Material::Read(reader, material)) {
			return PreviewState::Unavailable;
		}

		// **A material that names no texture previews as a bare sphere, and that
		// is the honest answer rather than a failure.** `assets::Material`'s own
		// rule is that an untextured material and an unknown one read alike to a
		// consumer - but here they are genuinely different: this one was read,
		// and what it says is that there is nothing to sample. A sphere in the
		// default grey is exactly what such a material puts on a part.
		if (material.ColourMap.empty()) {
			return PreviewState::Ready;
		}

		// **Past this point a missing sheet is `Unavailable`, not a bare
		// sphere.** The material names a texture, so a grey ball would be a
		// picture of something the material is not - the failure mode `Preview.hpp`
		// calls a preview that lies rather than one that is absent. A store
		// published from another machine has the `.amat` and not its pixels,
		// which is precisely this case.
		const std::filesystem::path sheet = cdn::FindInStore(cdn::DefaultLocalPaths(), material.ColourMap);
		if (!std::filesystem::is_regular_file(sheet, failure)) {
			// **Asked for rather than given up on**, which is the other half of
			// v0.12's preview fix. A store published from another machine has
			// the `.amat` and not its pixels, and the editor already knows how
			// to fetch one - `DownloadAsset` - so answering `Unavailable` here
			// was refusing to do the one thing that would have made the preview
			// possible.
			//
			// **Once per sheet, not once per frame.** `ContentAsked` is the same
			// set the content pump uses to avoid re-requesting a texture a world
			// named, and this is the same question about the same name.
			const engine::core::Name key(material.ColourMap);
			if (ContentClient && ContentAsked.insert(key.Id()).second) {
				DownloadAsset(material.ColourMap);
			}

			// `Pending` rather than `Unavailable`, so `LoadPreviewMesh` does not
			// remember the answer and the preview is built when the sheet
			// arrives. With no delivery configured there is nothing to wait for,
			// and the honest answer is that it is not here.
			return ContentClient ? PreviewState::Pending : PreviewState::Unavailable;
		}

		const std::optional<std::vector<std::byte>> sheetBytes = ReadWholeFile(sheet);
		if (!sheetBytes) {
			return PreviewState::TooLarge;
		}

		// **`assets::Texture` and not the importer.** A `.amat`'s colour map is
		// always a baked name - `assetc` rewrites it through `BakedName` for
		// exactly this reason - so reaching for `bake::ReadImage` here would be
		// covering for a material that should never have been published.
		engine::assets::TextureData decoded;
		engine::core::ByteReader sheetReader(*sheetBytes);
		if (!engine::assets::Texture::Read(sheetReader, decoded)) {
			return PreviewState::Unavailable;
		}

		// Resampled to a bound, because nothing evicts a preview - see
		// `PREVIEW_MATERIAL_SIDE`. Aspect is kept rather than squared off: a
		// sheet that is not square is unusual and stretching it would put the
		// distortion on the sphere.
		engine::assets::TextureData fitted;
		const uint32_t longest = std::max(decoded.Width, decoded.Height);
		if (longest > PREVIEW_MATERIAL_SIDE) {
			const double scale = static_cast<double>(PREVIEW_MATERIAL_SIDE) / longest;
			const auto width = static_cast<uint32_t>(std::max(1.0, decoded.Width * scale));
			const auto height = static_cast<uint32_t>(std::max(1.0, decoded.Height * scale));
			if (!engine::assets::ResizeImage(decoded, width, height, fitted)) {
				return PreviewState::Unavailable;
			}
		} else {
			fitted = decoded;
		}

		// Prefixed for `PreviewMeshName`'s reason, and it matters more here: a
		// colour map registered under its real name would replace the content
		// one, so every part in the scene wearing that material would quietly
		// drop to a 256-pixel copy of it.
		const engine::core::Name key(PreviewMeshName(material.ColourMap));
		if (!Renderer.AddTexture(key, fitted)) {
			return PreviewState::Unavailable;
		}

		texture = key;
		return PreviewState::Ready;
	}

	PreviewState Editor::BuildPreviewMesh(const std::string &name) {
		engine::assets::MeshData mesh;
		engine::core::Name texture;

		// **A built-in is generated, not read**, and asking the store for one
		// would have been a file that is deliberately not there. `engine.Cube`
		// and its five siblings exist in every process with no content at all -
		// which is the whole reason the picker offers them - so a preview that
		// only knew how to open files would show the six meshes that are always
		// available as the six that can never be previewed.
		if (engine::assets::KindOfName(name) == engine::assets::AssetKind::Material) {
			// **A material is the engine's own sphere wearing it.** There is no
			// geometry to show - a `.amat` is a texture reference - so the
			// preview has to supply a shape, and a sphere is the one that shows a
			// map at every angle at once: the top faces the light, the limb goes
			// to grazing, and the silhouette is the same for every material so
			// two of them can be told apart by their surface rather than by their
			// outline. That is the same reason every material browser ever
			// written uses one.
			//
			// `MakeSphere` duplicates its seam column, so the map wraps without
			// running backwards across one column of triangles - which is what
			// makes this legible rather than merely round.
			const PreviewState state = BuildPreviewMaterial(name, texture);
			if (state != PreviewState::Ready) {
				return state;
			}
			mesh = engine::assets::MakeBuiltin(engine::assets::BuiltinMesh::Sphere);
		} else if (engine::assets::BuiltinMesh builtin; engine::assets::BuiltinFromName(name, builtin)) {
			mesh = engine::assets::MakeBuiltin(builtin);
		} else {
			// **`cdn::FindInStore` and not a folder spelled here.** This read
			// `raw/<name>` and was right for as long as the publisher walked
			// `raw/`; the day it walked `baked/`, every preview in the editor
			// resolved to a missing file and turned into "no local pixels" with
			// nothing said.
			const std::filesystem::path source = cdn::FindInStore(cdn::DefaultLocalPaths(), name);

			std::error_code failure;
			if (!std::filesystem::is_regular_file(source, failure)) {
				return PreviewState::Unavailable;
			}

			const std::optional<std::vector<std::byte>> bytes = ReadWholeFile(source);
			if (!bytes) {
				return PreviewState::TooLarge;
			}

			// **`assets::Mesh` and nothing else.** A `.pmx` or a `.glb` in the
			// store is a *source* format - the runtime does not decode those,
			// which is `assets::Texture`'s standing rule one kind over - so an
			// unbaked store previews nothing, and the caption says to run
			// `assetc`. Reaching for `bake::ReadModel` here would put an importer
			// behind a hover.
			engine::core::ByteReader reader(*bytes);
			if (!engine::assets::Mesh::Read(reader, mesh)) {
				return PreviewState::Unavailable;
			}
		}

		const auto triangles = static_cast<uint32_t>(mesh.Indices.size() / 3);
		if (triangles > PREVIEW_MAXIMUM_TRIANGLES) {
			// **Refused before the upload, which is the whole point of checking
			// here.** The decode already happened and was bounded by the file
			// size; what this avoids is device memory and a draw call for
			// something nobody asked to render, only to hover over.
			ENGINE_INFO("preview: {} has {} triangle(s) - past the preview budget", name, triangles);
			return PreviewState::TooLarge;
		}

		// The true bounds, derived from the vertices by `Mesh::Read`. Kept so
		// the camera frames this mesh rather than a unit cube - a bake that was
		// not `--model-size 1` would otherwise be a speck or fill the frame.
		PreviewBounds bounds;
		bounds.Centre = (mesh.Minimum + mesh.Maximum) * 0.5f;

		const engine::core::Vector3 extent = (mesh.Maximum - mesh.Minimum) * 0.5f;
		bounds.Radius =
			std::max(0.05f, std::sqrt(extent.X * extent.X + extent.Y * extent.Y + extent.Z * extent.Z));

		bounds.Texture = texture;

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
		// The render happens outside the interface pass - imgui records draw
		// lists before the renderer runs - so this frame shows whatever the
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
		instance.HalfExtent =
			engine::core::Vector3{bounds->second.Radius, bounds->second.Radius, bounds->second.Radius};
		// **White under a texture and a light grey without one.** The grey exists
		// so an untextured mesh is not a black silhouette; multiplying a
		// material's own colour map by it would darken every material preview by
		// eight per cent against the part the same material draws on, which is
		// the one thing a material preview must not do - it is being looked at to
		// judge a colour.
		instance.Texture = bounds->second.Texture;
		instance.Tint = instance.Texture.IsValid() ? engine::core::Color3{1.0f, 1.0f, 1.0f}
												   : engine::core::Color3{0.92f, 0.92f, 0.94f};
		instance.Mesh = engine::core::Name(PreviewMeshName(PreviewWanted));

		const engine::scene::DrawInstance one[]{instance};

		engine::scene::Camera lens;
		lens.FieldOfViewRadians = 0.6109f; // 35 degrees, matching the reference.

		// **The near plane is pulled in with the subject.** The default is a
		// tenth of a metre, which clips the front off anything baked at unit
		// scale - the meshes `assetc --model-size 1` produces are exactly that.
		lens.NearPlane = std::max(0.001f, bounds->second.Radius * 0.01f);
		lens.FarPlane = std::max(10.0f, bounds->second.Radius * 100.0f);

		const float distance =
			(bounds->second.Radius / std::tan(lens.FieldOfViewRadians * 0.5f)) * CAMERA_PADDING;

		// **It turns, and a still frame is what made a preview hard to read.**
		// A mesh seen from one fixed angle hides the half facing away, and at
		// forty-eight pixels the near half of most things is a blob - which is
		// exactly the case a picker is for. A turntable shows the silhouette
		// changing, and a silhouette is what somebody recognises an asset by.
		//
		// **The camera orbits rather than the mesh spinning**, which matters for
		// one reason: the light is fixed in world space, so a rotating *model*
		// would carry its shading round with it and read as a flat cut-out. An
		// orbiting eye moves the highlight across the surface, which is the half
		// of a turntable that says the thing has depth.
		//
		// `AnimationSeconds` is the editor's own clock - the same one driving
		// flipbooks - so the preview turns at the frame rate rather than at the
		// rate the rotation happens to hand it slots.
		const auto angle = static_cast<float>(AnimationSeconds * TURNTABLE_RADIANS_PER_SECOND);
		const engine::core::Vector3 direction{
			CAMERA_DIRECTION.X * std::cos(angle) - CAMERA_DIRECTION.Z * std::sin(angle),
			CAMERA_DIRECTION.Y,
			CAMERA_DIRECTION.X * std::sin(angle) + CAMERA_DIRECTION.Z * std::cos(angle),
		};

		const engine::core::CFrame eye =
			engine::core::CFrame::LookAt(direction.Unit() * distance, engine::core::Vector3{});

		// **Studio chrome is the host overlay, not the graph interface.** The mesh
		// goes to the preview slot while the already-built Studio draw list goes
		// directly to the swapchain after the graph has finished.
		//
		// Leaving out that host overlay leaves nothing at all touching the
		// swapchain, and `Renderer::Render` then clears it and presents,
		// deliberately, because presenting a texture the driver handed back
		// unwritten shows uninitialised memory. So every frame spent on a preview
		// presented a cleared window. Hovering a mesh row blanked the entire
		// editor and moving the cursor away brought it back.
		//
		// **No surfaces, though**, and that part was right: a mirror pass here
		// would render a scene that is not there.
		engine::render::View view;
		view.CameraFrame = eye;
		view.Camera = lens;
		view.Instances = one;
		view.Target = &target;
		view.Slot = PreviewSlot();
		Renderer.Render(std::span<const engine::render::View>(&view, 1), Overlay, nullptr, true, &Interface);

		// **What the slot now holds, so a row can draw it.** There is one slot,
		// so exactly one mesh in the list can be live at a time - and a row that
		// painted the slot's texture without checking whose it was would show the
		// hovered mesh's picture in every mesh row on screen.
		PreviewShowing = PreviewWanted;

		// **And kept, so every other row can show it too.** The slot itself is
		// scratch - a handful of them, drawn into on rotation - which is why
		// this preview could only ever be the row under the cursor. A capture is
		// an ordinary texture-table entry, so it lands in the same thumbnail
		// cache as a decoded picture and is evicted by the same rule.
		CachePreviewThumbnail(PreviewWanted);
		return true;
	}

	void Editor::CachePreviewThumbnail(const std::string &name) {
		if (name.empty()) {
			return;
		}

		// **Once per asset, not once per frame.** The preview turns, so this
		// would otherwise release and recreate a texture every frame the cursor
		// sat still - and the frozen angle a thumbnail wants is any of them.
		const auto found = Thumbnails.find(name);
		if (found != Thumbnails.end() && found->second.Handle != nullptr) {
			return;
		}

		const engine::core::Name key(ThumbnailTextureName(name));
		if (!Renderer.CaptureSceneTexture(PreviewSlot(), key)) {
			return;
		}

		// **Written into `Thumbnails` rather than kept beside it**, which is
		// what makes eviction work without knowing this exists: `PumpThumbnails`
		// drops the least recently drawn by calling `DropTexture` on exactly
		// this name. A second cache would have been a second thing to evict, and
		// the one nobody wrote a policy for.
		Entry entry;
		entry.Handle = Renderer.TextureHandle(key);
		entry.State = entry.Handle != nullptr ? PreviewState::Ready : PreviewState::Unavailable;
		entry.LastSeen = ThumbnailClock;
		Thumbnails[name] = entry;
	}
}
