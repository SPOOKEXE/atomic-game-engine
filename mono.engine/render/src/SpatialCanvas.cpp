#include "Primitives.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
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

		// How far off a face a canvas floats before its own `ZOffset` is added.
		//
		// A canvas exactly on a face is coplanar with it and z-fights, which
		// reads as the sign flickering rather than as a depth precision problem -
		// so the bias is unconditional and `SurfaceGui.ZOffset` adds to it rather
		// than replacing it. See `gui::Surface::ZOffset`.
		constexpr float SURFACE_BIAS = 1.0e-3f;

		void FaceCanvas(
			gui::Face face,
			const scene::Transform &placement,
			const scene::Bounds &bounds,
			float zOffset,
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
			canvas.Origin =
				centre - canvas.AxisX * 0.5f - canvas.AxisY * 0.5f + normal * (SURFACE_BIAS + zOffset);
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
		return ResolveSpatialCanvases(store, screen, nullptr, nullptr);
	}

	size_t ResolveSpatialCanvases(
		Store &store,
		const gui::Screen &screen,
		const scene::Camera *requestedCamera,
		const core::CFrame *requestedFrame
	) {
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

		if (requestedCamera != nullptr && requestedFrame != nullptr) {
			fieldOfView = requestedCamera->FieldOfViewRadians;
			eye = requestedFrame->Position;
			eyeFrame = *requestedFrame;
			hasCamera = true;
		} else if (const scene::ActiveCamera *active = store.Resource<scene::ActiveCamera>();
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
			entry.Canvas.Interactive = surface.Active;
			entry.Canvas.MaxDistance = surface.MaxDistance;
			FaceCanvas(surface.On, *placement, *bounds, surface.ZOffset, entry.Canvas);

			// **Measured to the canvas's own centre rather than to the part's**,
			// which is the difference a `PixelsPerStud` sign on the end of a long
			// wall notices: the part's origin can be tens of studs from the face
			// being drawn, and a range test against it fades out a sign the player
			// is standing in front of.
			//
			// **A world with no camera is not culled at all**, rather than culled
			// against a distance of zero. That is the honest reading of "nobody
			// has measured this yet", and it is what keeps a headless layout test
			// asserting the same rectangle whether or not a camera exists.
			if (hasCamera) {
				const Vector3 centre =
					entry.Canvas.Origin + entry.Canvas.AxisX * 0.5f + entry.Canvas.AxisY * 0.5f;
				const Vector3 away = centre - eye;
				entry.Canvas.CurrentDistance = std::sqrt(away.Dot(away));
				entry.Canvas.Visible =
					!(surface.MaxDistance > 0.0f && entry.Canvas.CurrentDistance > surface.MaxDistance);
			}

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
			const float measured = std::sqrt(to.X * to.X + to.Y * to.Y + to.Z * to.Z);

			// **The size is computed from the stepped distance and the range test
			// from the measured one.** `DistanceStep` exists to stop text
			// re-fitting every frame a player walks, which is a fact about the
			// scale; quantising the *cull* as well would make a billboard at the
			// limit appear and disappear a step early or late for no stated
			// reason. Roblox's `CurrentDistance` is the stepped one, so that is
			// what is stored.
			const float distance =
				billboard.DistanceStep > 0.0f
					? std::round(measured / billboard.DistanceStep) * billboard.DistanceStep
					: measured;

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
				const Vector3 extents{
					bounds->HalfExtent.X * billboard.ExtentsOffset.X,
					bounds->HalfExtent.Y * billboard.ExtentsOffset.Y,
					bounds->HalfExtent.Z * billboard.ExtentsOffset.Z,
				};

				// **Both offsets add; neither overrides the other.** The local one
				// turns with the adornee and the world one does not, which is what
				// makes an arrow on a spinning part and a name tag above it two
				// settings of one pair rather than two mutually exclusive modes.
				entry.Canvas.Origin = entry.Canvas.Origin + frame->Frame.VectorToWorldSpace(extents) +
									  Vector3{
										  bounds->HalfExtent.X * billboard.ExtentsOffsetWorldSpace.X,
										  bounds->HalfExtent.Y * billboard.ExtentsOffsetWorldSpace.Y,
										  bounds->HalfExtent.Z * billboard.ExtentsOffsetWorldSpace.Z,
									  };
			}

			// **`SizeOffset` moves the anchor and therefore the origin**, which is
			// where it has to be applied: the origin is the billboard's centre and
			// every reader downstream - the projection, the pointer, the draw -
			// takes it as such. Roblox's sign convention is that a positive offset
			// moves the *anchor* towards the bottom left, so the quad moves the
			// other way, which is the negation below.
			entry.Canvas.Origin =
				entry.Canvas.Origin -
				eyeFrame.RightVector() * (billboard.SizeOffset.X * entry.Canvas.WorldSize.X) +
				eyeFrame.UpVector() * (billboard.SizeOffset.Y * entry.Canvas.WorldSize.Y);

			entry.Canvas.LightInfluence = std::clamp(billboard.LightInfluence, 0.0f, 1.0f);
			entry.Canvas.Brightness = std::max(billboard.Brightness, 0.0f);
			entry.Canvas.AlwaysOnTop = billboard.AlwaysOnTop;
			entry.Canvas.Interactive = billboard.Active;
			entry.Canvas.MaxDistance = billboard.MaxDistance;
			entry.Canvas.CurrentDistance = distance;
			entry.Canvas.Visible = !(billboard.MaxDistance > 0.0f && measured > billboard.MaxDistance);
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
		const gui::Pointer &pointer,
		const gui::Router &router,
		SpatialPointer &out,
		const scene::Camera *requestedCamera,
		const core::CFrame *requestedFrame,
		float projectionAspect,
		const core::CFrame *requestedGeometryFrame
	) {
		if (!pointer.Inside || screen.Width <= 0.0f || screen.Height <= 0.0f) {
			return false;
		}

		const scene::Camera *lens = requestedCamera;
		const core::CFrame *frame = requestedFrame;
		if (lens == nullptr || frame == nullptr) {
			const scene::ActiveCamera *active = store.Resource<scene::ActiveCamera>();
			if (active == nullptr || active->Entity == ecs::NULL_ENTITY) {
				return false;
			}
			lens = store.Get<scene::Camera>(active->Entity);
			const scene::Transform *camera = store.Get<scene::Transform>(active->Entity);
			if (lens == nullptr || camera == nullptr) {
				return false;
			}
			frame = &camera->Frame;
		}
		const core::CFrame &geometryFrame =
			requestedGeometryFrame != nullptr ? *requestedGeometryFrame : *frame;

		const float aspect = projectionAspect > 0.0f ? projectionAspect : screen.Width / screen.Height;
		const glm::mat4 viewProjection = scene::ResolveCamera(*frame, *lens, aspect).ViewProjection;
		const glm::mat4 inverse = glm::inverse(viewProjection);
		const float x = pointer.Position.X / screen.Width * 2.0f - 1.0f;
		const float y = 1.0f - pointer.Position.Y / screen.Height * 2.0f;
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
		const float rayLength = (farPoint - rayOrigin).Magnitude();
		if (!std::isfinite(rayLength) || !(rayLength > 0.0f)) {
			return false;
		}

		bool found = false;
		bool foundOnTop = false;
		float foundDistance = std::numeric_limits<float>::infinity();
		size_t foundOrder = 0;
		const Entity capturedCollector = router.CapturedCollector(store);

		store.Each<const gui::SpatialCanvas>([&](Entity collector, const gui::SpatialCanvas &spatial) {
			// **`Interactive` is asked before anything is projected**, and it is
			// the difference between a sign and a control panel. A surface gui
			// that takes the pointer intercepts every click crossing its plane,
			// including the ones aimed at the part behind it - so Roblox makes it
			// opt-in and `gui::Surface::Active` carries the argument.
			if (!spatial.Interactive || !spatial.Visible || spatial.Size.X <= 0.0f ||
				spatial.Size.Y <= 0.0f) {
				return;
			}
			if (spatial.MaxDistance > 0.0f &&
				(geometryFrame.Position - spatial.Origin).Magnitude() > spatial.MaxDistance) {
				return;
			}

			Vector3 origin = spatial.Origin;
			Vector3 axisX = spatial.AxisX;
			Vector3 axisY = spatial.AxisY;
			Vector3 normal = spatial.Normal;
			if (spatial.Kind == gui::SpatialCanvasKind::Surface &&
				!CanvasFacesViewer(normal, geometryFrame.Position - spatial.Origin)) {
				return;
			}
			if (spatial.Kind == gui::SpatialCanvasKind::Billboard) {
				const Vector3 up = geometryFrame.UpVector();
				const SpatialQuad quad = BillboardQuad(
					spatial.Origin,
					geometryFrame.RightVector(),
					up,
					geometryFrame.Position - spatial.Origin,
					spatial.BillboardStuds,
					spatial.BillboardPixels,
					CanvasPixelsPerStud(viewProjection, spatial.Origin, up, screen.Height),
					geometryFrame.LookVector() * -1.0f
				);
				origin = quad.Origin;
				axisX = quad.AxisX;
				axisY = quad.AxisY;
				// The quad's shading normal points toward the eye; its plane is
				// perpendicular to the camera direction, even off axis.
				normal = geometryFrame.LookVector() * -1.0f;
			}

			const float denominator = ray.Dot(normal);
			if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-6f ||
				(spatial.Kind == gui::SpatialCanvasKind::Surface && denominator >= 0.0f)) {
				return;
			}

			const float distance = (origin - rayOrigin).Dot(normal) / denominator;
			if (!std::isfinite(distance) || !(distance > 0.0f) || distance > rayLength) {
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
			if (!std::isfinite(across) || !std::isfinite(down) ||
				(collector != capturedCollector &&
				 (across < 0.0f || across > 1.0f || down < 0.0f || down > 1.0f))) {
				return;
			}

			const Vector2 canvas{across * spatial.Size.X, down * spatial.Size.Y};
			if (!std::isfinite(canvas.X) || !std::isfinite(canvas.Y)) {
				return;
			}
			auto candidate = pointer;
			candidate.Position = canvas;
			candidate.Collector = collector;
			candidate.ScreenOnly = false;
			if (!router.WantsPointer(store, list, candidate)) {
				return;
			}

			size_t paintOrder = 0;
			for (size_t index = list.Commands.size(); index > 0; index--) {
				if (list.Commands[index - 1].Collector == collector &&
					gui::ContainsDrawCommand(store, list, list.Commands[index - 1], canvas)) {
					paintOrder = index;
					break;
				}
			}

			const bool wins =
				!found || (spatial.AlwaysOnTop && !foundOnTop) ||
				(spatial.AlwaysOnTop == foundOnTop &&
				 (spatial.AlwaysOnTop
					  ? paintOrder > foundOrder
					  : distance < foundDistance || (distance == foundDistance && paintOrder > foundOrder)));
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

	gui::Pointer RouteGuiPointer(
		Store &store,
		const gui::DrawList &list,
		const gui::Screen &screen,
		const gui::Router &router,
		gui::Pointer pointer,
		const scene::Camera *camera,
		const core::CFrame *frame,
		float projectionAspect,
		const core::CFrame *geometryFrame
	) {
		pointer.Collector = ecs::NULL_ENTITY;
		pointer.ScreenOnly = true;
		const Entity captured = router.CapturedCollector(store);
		const bool spatialCapture =
			captured != ecs::NULL_ENTITY && (store.Get<gui::SpatialCanvas>(captured) != nullptr ||
											 store.IsA(captured, gui::GuiClass("SurfaceGui")) ||
											 store.IsA(captured, gui::GuiClass("BillboardGui")));
		if (!spatialCapture && router.WantsPointer(store, list, pointer)) {
			return pointer;
		}

		SpatialPointer spatial;
		if (ResolveSpatialPointer(
				store, list, screen, pointer, router, spatial, camera, frame, projectionAspect, geometryFrame
			)) {
			pointer.Position = spatial.Position;
			pointer.Collector = spatial.Collector;
			pointer.ScreenOnly = false;
		} else if (spatialCapture) {
			pointer.Collector = captured;
			pointer.ScreenOnly = false;
			pointer.Inside = false;
			pointer.Cancelled = true;
		}
		return pointer;
	}
}
