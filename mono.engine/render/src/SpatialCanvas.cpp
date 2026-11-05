#include "Primitives.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <new>
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
		std::optional<gui::SpatialCanvas> ResolveSurfaceCanvas(
			const Store &store,
			Entity collector,
			const gui::Surface &surface,
			bool hasCamera,
			const Vector3 &eye
		) {
			gui::SpatialCanvas canvas;

			const Entity part = AttachedTo(store, collector, surface.Adornee);
			const scene::Bounds *bounds = part != ecs::NULL_ENTITY ? store.Get<scene::Bounds>(part) : nullptr;
			const scene::Transform *placement =
				part != ecs::NULL_ENTITY ? store.Get<scene::Transform>(part) : nullptr;

			if (bounds == nullptr || placement == nullptr ||
				(surface.Sizing == gui::SurfaceSizingMode::PixelsPerStud &&
				 !(surface.PixelsPerStud > 0.0f))) {
				return {};
			}

			canvas.Kind = gui::SpatialCanvasKind::Surface;
			canvas.Size = surface.CanvasSize;
			if (surface.Sizing == gui::SurfaceSizingMode::PixelsPerStud) {
				const Vector2 studs = FaceExtent(surface.On, bounds->HalfExtent);
				canvas.Size = Vector2{studs.X * surface.PixelsPerStud, studs.Y * surface.PixelsPerStud};
			}
			canvas.LightInfluence = std::clamp(surface.LightInfluence, 0.0f, 1.0f);
			canvas.Brightness = std::max(surface.Brightness, 0.0f);
			canvas.AlwaysOnTop = surface.AlwaysOnTop;
			canvas.Interactive = surface.Active;
			canvas.MaxDistance = surface.MaxDistance;
			FaceCanvas(surface.On, *placement, *bounds, surface.ZOffset, canvas);

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
				const Vector3 centre = canvas.Origin + canvas.AxisX * 0.5f + canvas.AxisY * 0.5f;
				const Vector3 away = centre - eye;
				canvas.CurrentDistance = std::sqrt(away.Dot(away));
				canvas.Visible =
					!(surface.MaxDistance > 0.0f && canvas.CurrentDistance > surface.MaxDistance);
			}

			return canvas;
		}

		std::optional<gui::SpatialCanvas> ResolveBillboardCanvas(
			const Store &store,
			Entity collector,
			const gui::Billboard &billboard,
			const gui::Screen &screen,
			bool hasCamera,
			float fieldOfView,
			const Vector3 &eye,
			const core::CFrame &eyeFrame
		) {
			gui::SpatialCanvas canvas;

			const Entity part = AttachedTo(store, collector, billboard.Adornee);
			const scene::Transform *frame =
				part != ecs::NULL_ENTITY ? store.Get<scene::Transform>(part) : nullptr;
			const scene::Bounds *bounds = part != ecs::NULL_ENTITY ? store.Get<scene::Bounds>(part) : nullptr;

			if (!hasCamera || frame == nullptr) {
				return {};
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
				return {};
			}

			// **Scale is studs and offset is pixels**, which is Roblox's reading
			// of a `BillboardGui`'s `Size` and the one thing about this class
			// that surprises people: the same `UDim2` means something different
			// here than it does on everything else in the tree.
			canvas.Kind = gui::SpatialCanvasKind::Billboard;
			canvas.Size = Vector2{
				billboard.Size.X.Offset + billboard.Size.X.Scale * perStud,
				billboard.Size.Y.Offset + billboard.Size.Y.Scale * perStud,
			};
			canvas.WorldSize = Vector2{
				billboard.Size.X.Scale + billboard.Size.X.Offset / perStud,
				billboard.Size.Y.Scale + billboard.Size.Y.Offset / perStud,
			};
			canvas.BillboardStuds = Vector2{billboard.Size.X.Scale, billboard.Size.Y.Scale};
			canvas.BillboardPixels = Vector2{billboard.Size.X.Offset, billboard.Size.Y.Offset};
			canvas.Origin = frame->Frame.Position + billboard.StudsOffsetWorldSpace +
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
				canvas.Origin = canvas.Origin + frame->Frame.VectorToWorldSpace(extents) +
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
			canvas.Origin = canvas.Origin -
							eyeFrame.RightVector() * (billboard.SizeOffset.X * canvas.WorldSize.X) +
							eyeFrame.UpVector() * (billboard.SizeOffset.Y * canvas.WorldSize.Y);

			canvas.LightInfluence = std::clamp(billboard.LightInfluence, 0.0f, 1.0f);
			canvas.Brightness = std::max(billboard.Brightness, 0.0f);
			canvas.AlwaysOnTop = billboard.AlwaysOnTop;
			canvas.Interactive = billboard.Active;
			canvas.MaxDistance = billboard.MaxDistance;
			canvas.CurrentDistance = distance;
			canvas.Visible = !(billboard.MaxDistance > 0.0f && measured > billboard.MaxDistance);
			return canvas;
		}

		std::optional<gui::SpatialCanvas> ResolveCurrentSpatialCanvas(
			const Store &store,
			Entity collector,
			const gui::Screen &screen,
			const scene::Camera *camera,
			const core::CFrame *geometryFrame
		) {
			if (!store.Alive(collector) || ((camera == nullptr) != (geometryFrame == nullptr))) return {};
			const bool hasCamera = camera != nullptr;
			const Vector3 eye = hasCamera ? geometryFrame->Position : Vector3{};
			const core::CFrame eyeFrame = hasCamera ? *geometryFrame : core::CFrame{};
			const float fieldOfView = hasCamera ? camera->FieldOfViewRadians : 0.0f;
			if (const auto *billboard = store.Get<gui::Billboard>(collector))
				return ResolveBillboardCanvas(
					store, collector, *billboard, screen, hasCamera, fieldOfView, eye, eyeFrame
				);
			if (const auto *surface = store.Get<gui::Surface>(collector))
				return ResolveSurfaceCanvas(store, collector, *surface, hasCamera, eye);
			if (const auto *canvas = store.Get<gui::SpatialCanvas>(collector)) return *canvas;
			return {};
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
			const auto canvas = ResolveSurfaceCanvas(store, collector, surface, hasCamera, eye);
			pending.push_back({collector, canvas.value_or(gui::SpatialCanvas{}), canvas.has_value()});
		});
		store.Each<const gui::Billboard>([&](Entity collector, const gui::Billboard &billboard) {
			const auto canvas = ResolveBillboardCanvas(
				store, collector, billboard, screen, hasCamera, fieldOfView, eye, eyeFrame
			);
			pending.push_back({collector, canvas.value_or(gui::SpatialCanvas{}), canvas.has_value()});
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

	bool MatchesSpatialCollectors(
		Store &store,
		const gui::PointerFrame &frame,
		const gui::Screen &screen,
		const scene::Camera *camera,
		const core::CFrame *geometryFrame
	) {
		if (!frame.Valid() || frame.StoreIdentity() != store.Identity() ||
			((camera == nullptr) != (geometryFrame == nullptr)))
			return false;
		const auto resolve = [&](Entity collector) -> std::optional<gui::SpatialCanvas> {
			return ResolveCurrentSpatialCanvas(store, collector, screen, camera, geometryFrame);
		};
		const auto equal = [](const gui::SpatialCanvas &a, const gui::SpatialCanvas &b) {
			return a.Size == b.Size && a.Origin == b.Origin && a.AxisX == b.AxisX && a.AxisY == b.AxisY &&
				   a.WorldSize == b.WorldSize && a.BillboardStuds == b.BillboardStuds &&
				   a.BillboardPixels == b.BillboardPixels && a.Normal == b.Normal &&
				   a.LightInfluence == b.LightInfluence && a.Brightness == b.Brightness &&
				   a.MaxDistance == b.MaxDistance && a.CurrentDistance == b.CurrentDistance &&
				   a.Kind == b.Kind && a.AlwaysOnTop == b.AlwaysOnTop && a.Visible == b.Visible &&
				   a.Interactive == b.Interactive;
		};
		try {
			std::vector<uint64_t> displayed;
			displayed.reserve(frame.SpatialCanvasCount());
			for (size_t index = 0; index < frame.SpatialCanvasCount(); ++index) {
				Entity collector;
				gui::SpatialCanvas saved;
				if (!frame.SpatialCanvasAt(index, collector, saved)) return false;
				const auto current = resolve(collector);
				if (!current || !equal(saved, *current)) return false;
				displayed.push_back(collector.Id);
			}
			std::sort(displayed.begin(), displayed.end());
			bool unchanged = true;
			const auto checkNew = [&](Entity collector) {
				if (!unchanged || std::binary_search(displayed.begin(), displayed.end(), collector.Id))
					return;
				if (resolve(collector)) unchanged = false;
			};
			store.Each<const gui::Surface>([&](Entity entity, const gui::Surface &) { checkNew(entity); });
			store.Each<const gui::Billboard>([&](Entity entity, const gui::Billboard &) {
				checkNew(entity);
			});
			store.Each<const gui::SpatialCanvas>([&](Entity entity, const gui::SpatialCanvas &) {
				checkNew(entity);
			});
			return unchanged;
		} catch (const std::bad_alloc &) {
			return false;
		}
	}

	bool SpatialCollectorResolvable(
		const Store &store,
		Entity collector,
		const gui::Screen &screen,
		const scene::Camera *camera,
		const core::CFrame *geometryFrame
	) {
		const auto canvas = ResolveCurrentSpatialCanvas(store, collector, screen, camera, geometryFrame);
		return canvas && canvas->Interactive && canvas->Visible && canvas->Size.X > 0.0f &&
			   canvas->Size.Y > 0.0f;
	}

	bool SpatialDepthPixel(
		const gui::Screen &screen,
		const gui::Pointer &pointer,
		const SpatialDepthFrameInfo &frame,
		uint32_t &x,
		uint32_t &y
	) {
		if (!pointer.Inside || !std::isfinite(screen.Width) || !std::isfinite(screen.Height) ||
			!(screen.Width > 0.0f) || !(screen.Height > 0.0f) || !frame.Width || !frame.Height ||
			!std::isfinite(pointer.Position.X) || !std::isfinite(pointer.Position.Y) ||
			pointer.Position.X < 0.0f || pointer.Position.Y < 0.0f || pointer.Position.X >= screen.Width ||
			pointer.Position.Y >= screen.Height)
			return false;
		x = static_cast<uint32_t>(double(pointer.Position.X) / screen.Width * frame.Width);
		y = static_cast<uint32_t>(double(pointer.Position.Y) / screen.Height * frame.Height);
		return true;
	}

	bool MakeSpatialPickRay(
		const gui::Screen &screen,
		const gui::Pointer &pointer,
		const SpatialDepthFrameInfo &frame,
		SpatialPickRay &ray
	) {
		if (!pointer.Inside || !(screen.Width > 0.0f) || !(screen.Height > 0.0f)) return false;
		const glm::mat4 inverse = glm::inverse(frame.ViewProjection);
		const auto derive = [&](double logicalX,
								double logicalY,
								Vector3 &origin,
								Vector3 &direction,
								float &length) {
			const float x = static_cast<float>(logicalX / screen.Width * 2.0 - 1.0);
			const float y = static_cast<float>(1.0 - logicalY / screen.Height * 2.0);
			const glm::vec4 nearWorld = inverse * glm::vec4{x, y, 0.0f, 1.0f};
			const glm::vec4 farWorld = inverse * glm::vec4{x, y, 1.0f, 1.0f};
			if (!std::isfinite(nearWorld.w) || !std::isfinite(farWorld.w) ||
				std::abs(nearWorld.w) <= 1.0e-6f || std::abs(farWorld.w) <= 1.0e-6f)
				return false;
			origin = {nearWorld.x / nearWorld.w, nearWorld.y / nearWorld.w, nearWorld.z / nearWorld.w};
			const Vector3 farPoint{farWorld.x / farWorld.w, farWorld.y / farWorld.w, farWorld.z / farWorld.w};
			length = (farPoint - origin).Magnitude();
			if (!std::isfinite(length) || !(length > 0.0f)) return false;
			direction = (farPoint - origin).Unit();
			return true;
		};
		SpatialPickRay candidate;
		if (!derive(
				pointer.Position.X,
				pointer.Position.Y,
				candidate.Origin,
				candidate.Direction,
				candidate.Length
			))
			return false;
		candidate.LogicalHeight = screen.Height;
		uint32_t pixelX = 0, pixelY = 0;
		if (SpatialDepthPixel(screen, pointer, frame, pixelX, pixelY)) {
			const double pixelCenterX = (double(pixelX) + 0.5) * screen.Width / frame.Width;
			const double pixelCenterY = (double(pixelY) + 0.5) * screen.Height / frame.Height;
			candidate.HasPixelRay = derive(
				pixelCenterX,
				pixelCenterY,
				candidate.PixelOrigin,
				candidate.PixelDirection,
				candidate.PixelLength
			);
			if (!candidate.HasPixelRay) return false;
		}
		ray = candidate;
		return true;
	}

	bool ProjectSpatialCollector(
		const SpatialCollector &collector,
		const SpatialDepthFrameInfo &frame,
		const SpatialPickRay &ray,
		bool captured,
		SpatialPickCandidate &candidate
	) {
		const gui::SpatialCanvas &spatial = collector.Canvas;
		if (!std::isfinite(ray.Length) || !(ray.Length > 0.0f) || !std::isfinite(ray.LogicalHeight) ||
			!(ray.LogicalHeight > 0.0f))
			return false;
		if (!spatial.Interactive || !spatial.Visible || !(spatial.Size.X > 0.0f) || !(spatial.Size.Y > 0.0f))
			return false;
		const core::CFrame &eye = frame.GeometryFrame;
		if (spatial.MaxDistance > 0.0f && (eye.Position - spatial.Origin).Magnitude() > spatial.MaxDistance)
			return false;
		Vector3 origin = spatial.Origin;
		Vector3 axisX = spatial.AxisX;
		Vector3 axisY = spatial.AxisY;
		Vector3 normal = spatial.Normal;
		if (spatial.Kind == gui::SpatialCanvasKind::Surface &&
			!CanvasFacesViewer(normal, eye.Position - spatial.Origin))
			return false;
		if (spatial.Kind == gui::SpatialCanvasKind::Billboard) {
			const Vector3 up = eye.UpVector();
			const SpatialQuad quad = BillboardQuad(
				spatial.Origin,
				eye.RightVector(),
				up,
				eye.Position - spatial.Origin,
				spatial.BillboardStuds,
				spatial.BillboardPixels,
				CanvasPixelsPerStud(frame.ViewProjection, spatial.Origin, up, ray.LogicalHeight),
				eye.LookVector() * -1.0f
			);
			origin = quad.Origin;
			axisX = quad.AxisX;
			axisY = quad.AxisY;
			normal = eye.LookVector() * -1.0f;
		}
		const float denominator = ray.Direction.Dot(normal);
		if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-6f ||
			(spatial.Kind == gui::SpatialCanvasKind::Surface && denominator >= 0.0f))
			return false;
		const float distance = (origin - ray.Origin).Dot(normal) / denominator;
		if (!std::isfinite(distance) || !(distance > 0.0f) || distance > ray.Length) return false;
		const Vector3 hit = ray.Origin + ray.Direction * distance;
		const Vector3 local = hit - origin;
		const float xSpan = axisX.Dot(axisX), ySpan = axisY.Dot(axisY);
		if (!(xSpan > 0.0f) || !(ySpan > 0.0f)) return false;
		const float across = local.Dot(axisX) / xSpan, down = local.Dot(axisY) / ySpan;
		if (!std::isfinite(across) || !std::isfinite(down) ||
			(!captured && (across < 0.0f || across > 1.0f || down < 0.0f || down > 1.0f)))
			return false;
		const Vector2 position{across * spatial.Size.X, down * spatial.Size.Y};
		if (!std::isfinite(position.X) || !std::isfinite(position.Y)) return false;
		Vector3 depthHit = hit;
		if (ray.HasPixelRay && !captured) {
			if (!std::isfinite(ray.PixelLength) || !(ray.PixelLength > 0.0f)) return false;
			const float pixelDenominator = ray.PixelDirection.Dot(normal);
			if (!std::isfinite(pixelDenominator) || std::abs(pixelDenominator) <= 1.0e-6f) return false;
			const float pixelDistance = (origin - ray.PixelOrigin).Dot(normal) / pixelDenominator;
			if (!std::isfinite(pixelDistance) || !(pixelDistance > 0.0f) || pixelDistance > ray.PixelLength)
				return false;
			depthHit = ray.PixelOrigin + ray.PixelDirection * pixelDistance;
			const Vector3 pixelLocal = depthHit - origin;
			const float pixelAcross = pixelLocal.Dot(axisX) / xSpan;
			const float pixelDown = pixelLocal.Dot(axisY) / ySpan;
			if (!std::isfinite(pixelAcross) || !std::isfinite(pixelDown) || pixelAcross < 0.0f ||
				pixelAcross > 1.0f || pixelDown < 0.0f || pixelDown > 1.0f)
				return false;
		}
		const glm::vec4 clip = frame.ViewProjection * glm::vec4{depthHit.X, depthHit.Y, depthHit.Z, 1.0f};
		if (!std::isfinite(clip.z) || !std::isfinite(clip.w) || clip.w == 0.0f) return false;
		const float depth = clip.z / clip.w;
		if (!std::isfinite(depth)) return false;
		candidate = {collector.Collector, position, distance, depth, spatial.AlwaysOnTop};
		return true;
	}

	bool ConsiderSpatialCandidate(
		SpatialPickSelection &selection,
		const SpatialPickCandidate &candidate,
		size_t paintOrder,
		std::optional<float> sceneDepth,
		bool bypassOcclusion
	) {
		if (candidate.Collector == ecs::NULL_ENTITY || !std::isfinite(candidate.Distance) ||
			candidate.Distance < 0.0f || !std::isfinite(candidate.DeviceDepth))
			return false;
		if (!candidate.AlwaysOnTop && !bypassOcclusion) {
			if (!sceneDepth || !std::isfinite(*sceneDepth) || *sceneDepth < 0.0f || *sceneDepth > 1.0f ||
				candidate.DeviceDepth > *sceneDepth)
				return false;
		}
		const auto &previous = selection.Candidate;
		const bool wins = !selection.HasCandidate || (candidate.AlwaysOnTop && !previous.AlwaysOnTop) ||
						  (candidate.AlwaysOnTop == previous.AlwaysOnTop &&
						   (candidate.AlwaysOnTop ? paintOrder > selection.PaintOrder
												  : candidate.Distance < previous.Distance ||
														(candidate.Distance == previous.Distance &&
														 paintOrder > selection.PaintOrder)));
		if (!wins) return false;
		selection = {candidate, paintOrder, true};
		return true;
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
		SpatialDepthFrameInfo projection;
		projection.ViewProjection = scene::ResolveCamera(*frame, *lens, aspect).ViewProjection;
		projection.GeometryFrame = geometryFrame;
		SpatialPickRay pickRay;
		if (!MakeSpatialPickRay(screen, pointer, projection, pickRay)) return false;

		SpatialPickSelection selection;
		const Entity capturedCollector = router.CapturedCollector(store);

		store.Each<const gui::SpatialCanvas>([&](Entity collector, const gui::SpatialCanvas &spatial) {
			SpatialPickCandidate projected;
			if (!ProjectSpatialCollector(
					{collector, spatial}, projection, pickRay, collector == capturedCollector, projected
				))
				return;
			const Vector2 canvas = projected.Position;
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

			if (!ConsiderSpatialCandidate(selection, projected, paintOrder, {}, true)) return;

			out.Collector = collector;
			out.Position = canvas;
		});

		return selection.HasCandidate;
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
