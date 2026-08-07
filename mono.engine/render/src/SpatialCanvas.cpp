#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>

#include <cmath>
#include <vector>

namespace engine::render {

	namespace {
		using core::Vector2;
		using core::Vector3;
		using ecs::Entity;
		using ecs::Store;

		// What a collector is attached to: its own `Adornee`, or its parent.
		//
		// **The same rule `gui::AdorneeOf` applies to an adornment**, and it is
		// spelled again rather than shared because that function takes a
		// `gui::Adornment` and these are two other components. What matters is
		// that the rule agrees: set wins, an unset one means the parent, and a
		// set-but-dead one is nothing rather than silently falling back — an
		// `Adornee` pointing at a destroyed part is a collector about nothing,
		// and re-aiming it at whatever it is parented to would project it onto
		// the wrong object.
		Entity AttachedTo(const Store &store, Entity collector, Entity adornee) {
			if (adornee != ecs::NULL_ENTITY) {
				return store.Alive(adornee) ? adornee : ecs::NULL_ENTITY;
			}
			return store.ParentOf(collector);
		}

		// The two stud extents of one face of a box.
		//
		// A face is named by the axis it points along, so its extent is the
		// *other* two — and which of those is the canvas's width is what decides
		// whether a sign reads upright or sideways. Front and back look down Z,
		// so they span X across and Y up; left and right look down X and span Z
		// and Y; top and bottom look down Y and span X and Z.
		Vector2 FaceExtent(gui::Face face, const Vector3 &halfExtent) {
			const Vector3 full{halfExtent.X * 2.0f, halfExtent.Y * 2.0f, halfExtent.Z * 2.0f};

			switch (face) {
			case gui::Face::Front:
			case gui::Face::Back:
				return Vector2{full.X, full.Y};
			case gui::Face::Left:
			case gui::Face::Right:
				return Vector2{full.Z, full.Y};
			case gui::Face::Top:
			case gui::Face::Bottom:
				return Vector2{full.X, full.Z};
			}
			return Vector2{full.X, full.Y};
		}

		// How many pixels one stud covers at a given distance from the camera.
		//
		// The vertical field of view spans `2 · d · tan(fov/2)` studs at distance
		// `d`, and that span is the viewport's height in pixels. Vertical rather
		// than horizontal because `scene::Camera` stores the vertical one and
		// because a wider window is meant to show more world, not a bigger one.
		//
		// @return Zero when the camera is degenerate or the point is on top of
		//         it, which the caller reads as "do not resolve this".
		float PixelsPerStud(float fieldOfViewRadians, float distance, float height) {
			const float span = 2.0f * distance * std::tan(fieldOfViewRadians * 0.5f);
			if (!(span > 0.0f) || !(height > 0.0f)) {
				return 0.0f;
			}
			return height / span;
		}
	}

	size_t ResolveSpatialCanvases(Store &store, const gui::Screen &screen) {
		// **Collected before anything is written**, which is `gui::Layout`'s own
		// argument in as many words: adding `SpatialCanvas` to a collector that
		// has never had one moves its row between archetypes, and moving a row
		// out from under the query walking it is what `Store::Each`'s deferral
		// exists to prevent.
		struct Pending {
			Entity Collector;
			Vector2 Size;
			bool Resolved = false;
		};

		std::vector<Pending> pending;

		// The camera, once. A world with no live camera resolves no billboard —
		// there is nothing to measure a stud against — but its surface guis are
		// unaffected, because a face's stud extent is a fact about the part.
		float fieldOfView = 0.0f;
		Vector3 eye;
		bool hasCamera = false;

		if (const scene::ActiveCamera *active = store.Resource<scene::ActiveCamera>();
			active != nullptr && active->Entity != ecs::NULL_ENTITY) {
			const scene::Camera *camera = store.Get<scene::Camera>(active->Entity);
			const scene::Transform *frame = store.Get<scene::Transform>(active->Entity);

			if (camera != nullptr && frame != nullptr) {
				fieldOfView = camera->FieldOfViewRadians;
				eye = frame->Frame.Position;
				hasCamera = true;
			}
		}

		store.Each<const gui::Surface>([&](Entity collector, const gui::Surface &surface) {
			Pending entry;
			entry.Collector = collector;

			// `FixedSize` is not resolved at all rather than resolved to the
			// authored number. Writing the same value the fallback already gives
			// would make the component present on collectors nothing measured,
			// and then "has a resolved canvas" would stop meaning anything.
			if (surface.Sizing != gui::SurfaceSizingMode::PixelsPerStud) {
				pending.push_back(entry);
				return;
			}

			const Entity part = AttachedTo(store, collector, surface.Adornee);
			const scene::Bounds *bounds = part != ecs::NULL_ENTITY ? store.Get<scene::Bounds>(part) : nullptr;

			if (bounds == nullptr || !(surface.PixelsPerStud > 0.0f)) {
				pending.push_back(entry);
				return;
			}

			const Vector2 studs = FaceExtent(surface.On, bounds->HalfExtent);
			entry.Size = Vector2{studs.X * surface.PixelsPerStud, studs.Y * surface.PixelsPerStud};
			entry.Resolved = true;
			pending.push_back(entry);
		});

		store.Each<const gui::Billboard>([&](Entity collector, const gui::Billboard &billboard) {
			Pending entry;
			entry.Collector = collector;

			const Entity part = AttachedTo(store, collector, billboard.Adornee);
			const scene::Transform *frame =
				part != ecs::NULL_ENTITY ? store.Get<scene::Transform>(part) : nullptr;

			if (!hasCamera || frame == nullptr) {
				pending.push_back(entry);
				return;
			}

			const Vector3 to = frame->Frame.Position - eye;
			const float distance = std::sqrt(to.X * to.X + to.Y * to.Y + to.Z * to.Z);

			// **The adornee's own position, and none of the three offsets.**
			// They move where a billboard is *drawn*, which is the projection's
			// job; folding them in here would make the canvas of a billboard
			// with a world-space offset differ from the canvas of one without,
			// for a difference the author expressed as a position.
			const float perStud = PixelsPerStud(fieldOfView, distance, screen.Height);
			if (!(perStud > 0.0f)) {
				pending.push_back(entry);
				return;
			}

			// **Scale is studs and offset is pixels**, which is Roblox's reading
			// of a `BillboardGui`'s `Size` and the one thing about this class
			// that surprises people: the same `UDim2` means something different
			// here than it does on everything else in the tree.
			entry.Size = Vector2{
				billboard.Size.X.Offset + billboard.Size.X.Scale * perStud,
				billboard.Size.Y.Offset + billboard.Size.Y.Scale * perStud,
			};
			entry.Resolved = true;
			pending.push_back(entry);
		});

		size_t resolved = 0;

		for (const Pending &entry : pending) {
			if (entry.Resolved) {
				store.Set(entry.Collector, gui::SpatialCanvas{entry.Size});
				resolved++;
				continue;
			}

			// **Removed rather than left**, which is the half that is easy to
			// skip and expensive to skip. A `SurfaceGui` switched back to
			// `FixedSize`, or one whose adornee was deleted, would otherwise keep
			// the canvas the last frame that could resolve it wrote — and it
			// would keep it forever, looking correct until somebody moved the
			// part it is no longer attached to.
			if (store.Get<gui::SpatialCanvas>(entry.Collector) != nullptr) {
				store.Remove<gui::SpatialCanvas>(entry.Collector);
			}
		}

		return resolved;
	}
}
