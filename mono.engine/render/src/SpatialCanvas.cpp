#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <cmath>
#include <limits>
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
		// set-but-dead one is nothing rather than silently falling back - an
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
		// *other* two - and which of those is the canvas's width is what decides
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

		Vector3 FaceNormal(gui::Face face) {
			switch (face) {
			case gui::Face::Right:
				return Vector3{1.0f, 0.0f, 0.0f};
			case gui::Face::Top:
				return Vector3{0.0f, 1.0f, 0.0f};
			case gui::Face::Back:
				return Vector3{0.0f, 0.0f, 1.0f};
			case gui::Face::Left:
				return Vector3{-1.0f, 0.0f, 0.0f};
			case gui::Face::Bottom:
				return Vector3{0.0f, -1.0f, 0.0f};
			case gui::Face::Front:
				return Vector3{0.0f, 0.0f, -1.0f};
			}
			return Vector3{0.0f, 0.0f, -1.0f};
		}

		void FaceCanvas(
			gui::Face face,
			const scene::Transform &placement,
			const scene::Bounds &bounds,
			gui::SpatialCanvas &canvas
		) {
			Vector3 localRight;
			Vector3 localDown;
			float reach = 0.0f;
			switch (face) {
			case gui::Face::Right:
				localRight = Vector3{0.0f, 0.0f, -1.0f};
				localDown = Vector3{0.0f, -1.0f, 0.0f};
				reach = bounds.HalfExtent.X;
				break;
			case gui::Face::Top:
				localRight = Vector3{1.0f, 0.0f, 0.0f};
				localDown = Vector3{0.0f, 0.0f, 1.0f};
				reach = bounds.HalfExtent.Y;
				break;
			case gui::Face::Back:
				localRight = Vector3{1.0f, 0.0f, 0.0f};
				localDown = Vector3{0.0f, -1.0f, 0.0f};
				reach = bounds.HalfExtent.Z;
				break;
			case gui::Face::Left:
				localRight = Vector3{0.0f, 0.0f, 1.0f};
				localDown = Vector3{0.0f, -1.0f, 0.0f};
				reach = bounds.HalfExtent.X;
				break;
			case gui::Face::Bottom:
				localRight = Vector3{1.0f, 0.0f, 0.0f};
				localDown = Vector3{0.0f, 0.0f, -1.0f};
				reach = bounds.HalfExtent.Y;
				break;
			case gui::Face::Front:
				localRight = Vector3{-1.0f, 0.0f, 0.0f};
				localDown = Vector3{0.0f, -1.0f, 0.0f};
				reach = bounds.HalfExtent.Z;
				break;
			}

			const Vector2 extent = FaceExtent(face, bounds.HalfExtent);
			const Vector3 normal = placement.Frame.VectorToWorldSpace(FaceNormal(face));
			canvas.Normal = normal;
			canvas.AxisX = placement.Frame.VectorToWorldSpace(localRight) * extent.X;
			canvas.AxisY = placement.Frame.VectorToWorldSpace(localDown) * extent.Y;
			const Vector3 centre = placement.Frame.Position + normal * reach;
			canvas.Origin = centre - canvas.AxisX * 0.5f - canvas.AxisY * 0.5f + normal * 1.0e-3f;
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
			gui::SpatialCanvas Canvas;
			bool Resolved = false;
		};

		std::vector<Pending> pending;

		// The camera, once. A world with no live camera resolves no billboard -
		// there is nothing to measure a stud against - but its surface guis are
		// unaffected, because a face's stud extent is a fact about the part.
		float fieldOfView = 0.0f;
		Vector3 eye;
		core::CFrame eyeFrame;
		bool hasCamera = false;

		if (const scene::ActiveCamera *active = store.Resource<scene::ActiveCamera>();
			active != nullptr && active->Entity != ecs::NULL_ENTITY) {
			const scene::Camera *camera = store.Get<scene::Camera>(active->Entity);
			const scene::Transform *frame = store.Get<scene::Transform>(active->Entity);

			if (camera != nullptr && frame != nullptr) {
				fieldOfView = camera->FieldOfViewRadians;
				eye = frame->Frame.Position;
				eyeFrame = frame->Frame;
				hasCamera = true;
			}
		}

		store.Each<const gui::Surface>([&](Entity collector, const gui::Surface &surface) {
			Pending entry;
			entry.Collector = collector;

			const Entity part = AttachedTo(store, collector, surface.Adornee);
			const scene::Bounds *bounds = part != ecs::NULL_ENTITY ? store.Get<scene::Bounds>(part) : nullptr;
			const scene::Transform *placement =
				part != ecs::NULL_ENTITY ? store.Get<scene::Transform>(part) : nullptr;

			if (bounds == nullptr || placement == nullptr ||
				(surface.Sizing == gui::SurfaceSizingMode::PixelsPerStud &&
				 !(surface.PixelsPerStud > 0.0f))) {
				pending.push_back(entry);
				return;
			}

			entry.Canvas.Kind = gui::SpatialCanvasKind::Surface;
			entry.Canvas.Size = surface.CanvasSize;
			if (surface.Sizing == gui::SurfaceSizingMode::PixelsPerStud) {
				const Vector2 studs = FaceExtent(surface.On, bounds->HalfExtent);
				entry.Canvas.Size = Vector2{studs.X * surface.PixelsPerStud, studs.Y * surface.PixelsPerStud};
			}
			entry.Canvas.LightInfluence = std::clamp(surface.LightInfluence, 0.0f, 1.0f);
			entry.Canvas.Brightness = std::max(surface.Brightness, 0.0f);
			entry.Canvas.AlwaysOnTop = surface.AlwaysOnTop;
			FaceCanvas(surface.On, *placement, *bounds, entry.Canvas);
			entry.Resolved = true;
			pending.push_back(entry);
		});

		store.Each<const gui::Billboard>([&](Entity collector, const gui::Billboard &billboard) {
			Pending entry;
			entry.Collector = collector;

			const Entity part = AttachedTo(store, collector, billboard.Adornee);
			const scene::Transform *frame =
				part != ecs::NULL_ENTITY ? store.Get<scene::Transform>(part) : nullptr;
			const scene::Bounds *bounds = part != ecs::NULL_ENTITY ? store.Get<scene::Bounds>(part) : nullptr;

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
			entry.Canvas.Kind = gui::SpatialCanvasKind::Billboard;
			entry.Canvas.Size = Vector2{
				billboard.Size.X.Offset + billboard.Size.X.Scale * perStud,
				billboard.Size.Y.Offset + billboard.Size.Y.Scale * perStud,
			};
			entry.Canvas.WorldSize = Vector2{
				billboard.Size.X.Scale + billboard.Size.X.Offset / perStud,
				billboard.Size.Y.Scale + billboard.Size.Y.Offset / perStud,
			};
			entry.Canvas.BillboardStuds = Vector2{billboard.Size.X.Scale, billboard.Size.Y.Scale};
			entry.Canvas.BillboardPixels = Vector2{billboard.Size.X.Offset, billboard.Size.Y.Offset};
			entry.Canvas.Origin = frame->Frame.Position + billboard.StudsOffsetWorldSpace +
								  eyeFrame.RightVector() * billboard.StudsOffset.X +
								  eyeFrame.UpVector() * billboard.StudsOffset.Y +
								  eyeFrame.LookVector() * billboard.StudsOffset.Z;
			if (bounds != nullptr) {
				entry.Canvas.Origin =
					entry.Canvas.Origin + frame->Frame.VectorToWorldSpace(
											  Vector3{
												  bounds->HalfExtent.X * billboard.ExtentsOffset.X,
												  bounds->HalfExtent.Y * billboard.ExtentsOffset.Y,
												  bounds->HalfExtent.Z * billboard.ExtentsOffset.Z,
											  }
										  );
			}
			entry.Canvas.LightInfluence = std::clamp(billboard.LightInfluence, 0.0f, 1.0f);
			entry.Canvas.AlwaysOnTop = billboard.AlwaysOnTop;
			entry.Canvas.MaxDistance = billboard.MaxDistance;
			entry.Canvas.Visible = !(billboard.MaxDistance > 0.0f && distance > billboard.MaxDistance);
			entry.Resolved = true;
			pending.push_back(entry);
		});

		size_t resolved = 0;

		for (const Pending &entry : pending) {
			if (entry.Resolved) {
				store.Set(entry.Collector, entry.Canvas);
				resolved++;
				continue;
			}

			// **Removed rather than left**, which is the half that is easy to
			// skip and expensive to skip. A `SurfaceGui` switched back to
			// `FixedSize`, or one whose adornee was deleted, would otherwise keep
			// the canvas the last frame that could resolve it wrote - and it
			// would keep it forever, looking correct until somebody moved the
			// part it is no longer attached to.
			if (store.Get<gui::SpatialCanvas>(entry.Collector) != nullptr) {
				store.Remove<gui::SpatialCanvas>(entry.Collector);
			}
		}

		return resolved;
	}

	bool ResolveSpatialPointer(
		Store &store,
		const gui::DrawList &list,
		const gui::Screen &screen,
		const Vector2 &point,
		SpatialPointer &out
	) {
		const scene::ActiveCamera *active = store.Resource<scene::ActiveCamera>();
		if (active == nullptr || active->Entity == ecs::NULL_ENTITY || screen.Width <= 0.0f ||
			screen.Height <= 0.0f) {
			return false;
		}

		const scene::Camera *lens = store.Get<scene::Camera>(active->Entity);
		const scene::Transform *camera = store.Get<scene::Transform>(active->Entity);
		if (lens == nullptr || camera == nullptr) {
			return false;
		}

		const float aspect = screen.Width / screen.Height;
		const glm::mat4 inverse =
			glm::inverse(scene::ResolveCamera(camera->Frame, *lens, aspect).ViewProjection);
		const float x = point.X / screen.Width * 2.0f - 1.0f;
		const float y = 1.0f - point.Y / screen.Height * 2.0f;
		const glm::vec4 nearClip{x, y, 0.0f, 1.0f};
		const glm::vec4 farClip{x, y, 1.0f, 1.0f};
		const glm::vec4 nearWorld = inverse * nearClip;
		const glm::vec4 farWorld = inverse * farClip;
		if (std::abs(nearWorld.w) <= 1.0e-6f || std::abs(farWorld.w) <= 1.0e-6f) {
			return false;
		}

		const Vector3 rayOrigin{
			nearWorld.x / nearWorld.w,
			nearWorld.y / nearWorld.w,
			nearWorld.z / nearWorld.w,
		};
		const Vector3 farPoint{
			farWorld.x / farWorld.w,
			farWorld.y / farWorld.w,
			farWorld.z / farWorld.w,
		};
		const Vector3 ray = (farPoint - rayOrigin).Unit();

		bool found = false;
		bool foundOnTop = false;
		float foundDistance = std::numeric_limits<float>::infinity();
		size_t foundOrder = 0;

		store.Each<const gui::SpatialCanvas>([&](Entity collector, const gui::SpatialCanvas &spatial) {
			if (!spatial.Visible || spatial.Size.X <= 0.0f || spatial.Size.Y <= 0.0f) {
				return;
			}

			Vector3 origin = spatial.Origin;
			Vector3 axisX = spatial.AxisX;
			Vector3 axisY = spatial.AxisY;
			Vector3 normal = spatial.Normal;
			if (spatial.Kind == gui::SpatialCanvasKind::Billboard) {
				axisX = camera->Frame.RightVector() * spatial.WorldSize.X;
				axisY = camera->Frame.UpVector() * -spatial.WorldSize.Y;
				origin = spatial.Origin - axisX * 0.5f - axisY * 0.5f;
				normal = camera->Frame.Position - spatial.Origin;
				if (normal.MagnitudeSquared() <= 0.0f) {
					return;
				}
				normal = normal.Unit();
			}

			const float denominator = ray.Dot(normal);
			if (std::abs(denominator) <= 1.0e-6f ||
				(spatial.Kind == gui::SpatialCanvasKind::Surface && denominator >= 0.0f)) {
				return;
			}

			const float distance = (origin - rayOrigin).Dot(normal) / denominator;
			if (!(distance > 0.0f) || (spatial.MaxDistance > 0.0f && distance > spatial.MaxDistance)) {
				return;
			}

			const Vector3 hit = rayOrigin + ray * distance;
			const Vector3 local = hit - origin;
			const float xSpan = axisX.Dot(axisX);
			const float ySpan = axisY.Dot(axisY);
			if (!(xSpan > 0.0f) || !(ySpan > 0.0f)) {
				return;
			}

			const float across = local.Dot(axisX) / xSpan;
			const float down = local.Dot(axisY) / ySpan;
			if (across < 0.0f || across > 1.0f || down < 0.0f || down > 1.0f) {
				return;
			}

			const Vector2 canvas{across * spatial.Size.X, down * spatial.Size.Y};
			if (gui::PickInCollector(store, list, collector, canvas) == ecs::NULL_ENTITY) {
				return;
			}

			size_t paintOrder = 0;
			for (size_t index = list.Commands.size(); index > 0; index--) {
				if (list.Commands[index - 1].Collector == collector) {
					paintOrder = index;
					break;
				}
			}

			const bool wins =
				!found || (spatial.AlwaysOnTop && !foundOnTop) ||
				(spatial.AlwaysOnTop == foundOnTop &&
				 (distance < foundDistance || (distance == foundDistance && paintOrder > foundOrder)));
			if (!wins) {
				return;
			}

			found = true;
			foundOnTop = spatial.AlwaysOnTop;
			foundDistance = distance;
			foundOrder = paintOrder;
			out.Collector = collector;
			out.Position = canvas;
		});

		return found;
	}
}
