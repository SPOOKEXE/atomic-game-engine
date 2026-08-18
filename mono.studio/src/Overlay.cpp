// The ground grid, the origin axes, the selection outline, and picking.
//
// **Everything here is drawn into a viewport panel's imgui draw list rather than
// by the renderer**, because `mono.engine/render` has no debug-line facility at
// all and adding a primitive path at L12 for an editor's furniture is the wrong
// place for it. See `Editor::OverlaySlot` for why it is appended after
// `DriveCamera` instead of where the panel drew, and `studio/Projection.hpp` for
// the arithmetic and the two traps it exists to avoid.

#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/ui/Theme.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <optional>
#include <studio/Editor.hpp>
#include <vector>

namespace studio {

	using engine::core::AABB;
	using engine::core::CFrame;
	using engine::core::Ray;
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;
	using engine::world::WorldId;

	namespace {
		// How far the grid reaches from the camera, in cells.
		//
		// **Bounded and camera-relative rather than a fixed slab at the
		// origin.** A grid anchored at the origin disappears the moment
		// somebody flies away from it, which is exactly when they most need to
		// know which way is up.
		constexpr int GRID_RADIUS = 40;

		// Metres per cell, and per heavy cell.
		constexpr float GRID_STEP = 4.0f;
		constexpr int GRID_MAJOR = 5;

		// Lines are drawn thinner than the axes so the axes read as different
		// things rather than as brighter grid lines.
		constexpr float GRID_THICKNESS = 1.0f;
		constexpr float AXIS_THICKNESS = 1.6f;

		// Snaps a coordinate down to the grid, so the lines stay put in the
		// world while the camera moves rather than sliding along with it.
		float SnapDown(float value, float step) {
			return std::floor(value / step) * step;
		}

		// A colour that fades with distance from the camera, so the grid has a
		// horizon instead of ending in a hard rectangle.
		ImU32 GridColour(float fade, bool major) {
			const float alpha = std::clamp(fade, 0.0f, 1.0f) * (major ? 0.5f : 0.25f);
			return ImGui::GetColorU32(ImVec4(0.6f, 0.65f, 0.75f, alpha));
		}
	}

	PanelProjection Editor::ProjectionFor(size_t viewport) {
		PanelProjection projection;

		if (viewport >= Overlays.size() || !Overlays[viewport].Drawn) {
			return projection;
		}

		const OverlaySlot &slot = Overlays[viewport];
		if (slot.Width <= 0.0f || slot.Height <= 0.0f) {
			return projection;
		}

		// The camera this panel is looking through, as it stands *now* - which
		// is after `DriveCamera` and is therefore the camera `PresentWorld` is
		// about to render with.
		const ViewportState *extra = ExtraAt(viewport);
		CFrame frame = extra != nullptr ? extra->Frame : CameraFrame;

		// **The lens does not need `PresentWorld`'s far-plane adjustment.** For
		// a perspective matrix the divide is by `w = -z_view`, which depends on
		// neither the near nor the far plane, so the x and y a point lands at
		// are decided by the field of view, the aspect ratio and the camera
		// frame alone. Reproducing `FarPlane = max(FarPlane, reach * 40)` here
		// would be a second copy of a number that cannot affect the answer.
		engine::scene::Camera lens;

		const WorldId shown = ViewportWorld(viewport);
		const Entity follow = extra != nullptr ? extra->Follow : FollowCamera;

		// A followed camera brings its own field of view, exactly as
		// `PresentWorld` honours it - looking through a camera while ignoring
		// its lens is looking through something else.
		if (follow != NULL_ENTITY && shown.IsValid() && Universe != nullptr) {
			Universe->Enter(shown, [&](Store &store) {
				if (store.Alive(follow)) {
					if (const auto *component = store.Get<engine::scene::Camera>(follow)) {
						lens = *component;
					}
				}
			});
		}

		// **A client view is drawn from the client's own camera, so the overlay
		// has to be too.** `PresentWorld` reads a replica's `ActiveCamera` back
		// after presenting it - a replica holding a character places its eye
		// behind the body and `AimReplicaViewer` steps aside - and this
		// projection was still the editor's free camera. Everything built from
		// it was therefore aimed somewhere the panel was not looking: the grid
		// slid across the floor as the player walked, the selection outline sat
		// beside the part it outlined, and a click picked whatever was under the
		// free camera's ray.
		//
		// **Read rather than resolved.** `AimReplicaViewer` writes, and an
		// overlay pass that placed a camera would be a second author of the
		// eye - running in the imgui half of the frame, where the store is being
		// read by three other passes.
		if (shown.IsValid() && Universe != nullptr && IsReplicaWorld(shown)) {
			Universe->Enter(shown, [&](Store &store) {
				const auto *active = store.Resource<engine::scene::ActiveCamera>();
				if (active == nullptr || !store.Alive(active->Entity)) {
					return;
				}
				if (const auto *placement = store.Get<engine::scene::Transform>(active->Entity)) {
					frame = placement->Frame;
				}
				if (const auto *found = store.Get<engine::scene::Camera>(active->Entity)) {
					lens = *found;
				}
			});
		}

		const engine::scene::CameraMatrices matrices =
			engine::scene::ResolveCamera(frame, lens, slot.Width / slot.Height);

		projection.Matrix = matrices.ViewProjection;
		projection.Eye = frame.Position;
		projection.ImageMin = glm::vec2(slot.X, slot.Y);
		projection.ImageSize = glm::vec2(slot.Width, slot.Height);
		projection.Near = lens.NearPlane;
		return projection;
	}

	namespace {
		// How far past the front face the facing marker reaches, as a fraction
		// of the part's largest half-extent.
		//
		// **Proportional rather than fixed**, because it is a property of the
		// part rather than a control: a marker that stayed one metre long would
		// be invisible on a baseplate and enormous on a bolt.
		constexpr float FACING_REACH = 0.9f;

		// The look is blue and the up is green, which are the same two colours
		// the Z and Y handles already use. A third palette for the same two
		// directions would be a third thing to learn.
		constexpr ImU32 FACING_LOOK = IM_COL32(85, 130, 235, 235);
		constexpr ImU32 FACING_UP = IM_COL32(115, 215, 105, 235);
	}

	void Editor::DrawViewportOverlays() {
		// **One projection per panel, resolved once and shared by everything
		// below.** Three passes want it - the gizmo, the pending pick, and the
		// grid and outline - and each `ProjectionFor` is a `glm::inverse`, a
		// `glm::perspective`, a matrix product and, for a followed camera, a
		// `Universe::Enter` that builds two `std::function`s. Resolving it once
		// per panel per frame turns two-and-a-bit computations into one.
		//
		// It also makes the pick provably honest. The gizmo loop decides
		// whether a click landed on a handle; `PickInViewport` then decides
		// what that click hit in the world. Those two used to adjudicate the
		// same click against two separately built matrices that merely ought to
		// agree.
		//
		// **This is not the cached world state `mono.studio/AGENTS.md` forbids.**
		// A local dies at the end of this function, so nothing here can be one
		// frame stale - it cannot outlive the frame that read the camera. The
		// rule is about a panel keeping a copy of something the store owns
		// *between* frames; a value read once and used three times inside one
		// frame is the same read the three call sites were each making.
		//
		// Safe only because nothing between the passes moves a camera, and
		// nothing does: `DriveCamera` ran earlier in the `camera` span and this
		// function only draws.
		std::vector<PanelProjection> projections(Overlays.size());
		for (size_t index = 0; index < Overlays.size(); index++) {
			projections[index] = ProjectionFor(index);
		}

		// **The gizmo goes first, and it can swallow the pending pick.** A
		// click that lands on a handle is a drag, not a selection - and which
		// it is cannot be known in `DrawViewport`, because the handle's screen
		// position comes from a projection that is only correct after
		// `DriveCamera`. So the click is recorded there and adjudicated here.
		bool overHandle = false;

		for (size_t index = 0; index < Overlays.size(); index++) {
			if (!Overlays[index].Drawn || Overlays[index].List == nullptr) {
				continue;
			}

			const PanelProjection &panel = projections[index];
			if (!panel.IsValid()) {
				continue;
			}

			if (DrawGizmo(index, panel) && index == PendingPick.Viewport) {
				overHandle = true;
			}

			// **After the gizmo, and it declines while a handle is held.** Both
			// write placements, and two of them running against one selection
			// is two answers to where it is.
			DragOnSurface(index, panel);
		}

		if (PendingPick.Wanted) {
			const PendingPickAction pick = PendingPick;
			PendingPick = PendingPickAction{};

			// The bound check replaces the one `ProjectionFor` used to make on
			// the pick's behalf, now that the projection arrives as an argument
			// rather than being fetched by index inside.
			if (!overHandle && pick.Viewport < projections.size()) {
				PickInViewport(pick.Viewport, pick.X, pick.Y, pick.Add, projections[pick.Viewport]);
			}
		}

		// **The game's own UI, before the editor's furniture below it.** The
		// grid, the axes and the selection outline are tools for looking at the
		// world; a `ScreenGui` is part of the game. Drawing the tools last is
		// what keeps a selection outline visible through a full-screen menu the
		// game happens to have open.
		//
		// Outside the projection loop below, because a `ScreenGui` has no
		// camera: it is laid out against the panel rectangle and nothing else,
		// so a panel whose camera cannot be resolved still draws its UI.
		for (size_t index = 0; index < Overlays.size(); index++) {
			DrawViewportGui(index);
		}

		for (size_t index = 0; index < Overlays.size(); index++) {
			OverlaySlot &slot = Overlays[index];
			if (!slot.Drawn || slot.List == nullptr) {
				continue;
			}

			const PanelProjection &panel = projections[index];
			if (!panel.IsValid()) {
				continue;
			}

			ImDrawList *list = slot.List;

			// **Clipped to the panel, or the grid draws over the explorer.** A
			// draw list is the window's, and a line projected off the edge is
			// still a line in it.
			list->PushClipRect(
				ImVec2(slot.X, slot.Y), ImVec2(slot.X + slot.Width, slot.Y + slot.Height), true
			);

			const auto segment = [&](Vector3 from, Vector3 to, ImU32 colour, float thickness) {
				glm::vec2 a{};
				glm::vec2 b{};
				if (panel.ProjectSegment(from, to, a, b)) {
					list->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), colour, thickness);
				}
			};

			// **Not over a client replica.** The grid is authoring furniture:
			// it says where the origin is and how big a metre is while you are
			// placing things. A replica panel is the one picture in this editor
			// that is exactly what a player sees, so editor furniture drawn on
			// it makes "does this look right" impossible to answer from the
			// picture in front of you.
			//
			// **Running is not the same as being a player's view.** `Server` and
			// `Play` still simulate a world the author owns and keeps placing
			// things in, and Roblox's studio keeps its grid through both for the
			// same reason. The author who wants the clean picture turns
			// `ShowGrid` off, which is one click and already exists - the mode
			// does not need to decide it for them.
			//
			// Per scene rather than per universe, like everything else about
			// the transport: with two viewports on two worlds, one may be a
			// replica while the other is authored, and the authored one keeps
			// its grid.
			//
			// A replica carries no run record of its own, so `ModeOf` answers
			// `Edit` for it and mode alone would never exclude it.
			const WorldId shown = ViewportWorld(index);
			const bool authoring = !IsReplicaWorld(shown);

			if (ShowGrid && authoring) {
				const Vector3 eye = panel.Eye;
				const float originX = SnapDown(eye.X, GRID_STEP);
				const float originZ = SnapDown(eye.Z, GRID_STEP);
				const float reach = GRID_RADIUS * GRID_STEP;

				for (int step = -GRID_RADIUS; step <= GRID_RADIUS; step++) {
					const float offset = static_cast<float>(step) * GRID_STEP;

					// Fade with distance from the camera so the grid ends in a
					// horizon rather than a rectangle.
					const float fade =
						1.0f - std::abs(static_cast<float>(step)) / static_cast<float>(GRID_RADIUS);
					const bool major = step % GRID_MAJOR == 0;
					const ImU32 colour = GridColour(fade, major);

					const float x = originX + offset;
					const float z = originZ + offset;

					// **The axes are drawn separately below, so the two lines
					// that would sit under them are skipped.** Drawing both
					// leaves a grey line showing through a coloured one, which
					// reads as the axis being the wrong colour.
					if (std::abs(x) > 0.001f) {
						segment(
							Vector3{x, 0.0f, originZ - reach},
							Vector3{x, 0.0f, originZ + reach},
							colour,
							GRID_THICKNESS
						);
					}
					if (std::abs(z) > 0.001f) {
						segment(
							Vector3{originX - reach, 0.0f, z},
							Vector3{originX + reach, 0.0f, z},
							colour,
							GRID_THICKNESS
						);
					}
				}

				// The origin axes, in the conventional colours: X red, Z blue.
				// Y is not drawn along the ground because it is not on it -
				// a vertical line at the origin instead, so "up" has a mark.
				const float reachAxis = GRID_RADIUS * GRID_STEP;
				segment(
					Vector3{originX - reachAxis, 0.0f, 0.0f},
					Vector3{originX + reachAxis, 0.0f, 0.0f},
					ImGui::GetColorU32(ImVec4(0.85f, 0.30f, 0.32f, 0.65f)),
					AXIS_THICKNESS
				);
				segment(
					Vector3{0.0f, 0.0f, originZ - reachAxis},
					Vector3{0.0f, 0.0f, originZ + reachAxis},
					ImGui::GetColorU32(ImVec4(0.32f, 0.50f, 0.90f, 0.65f)),
					AXIS_THICKNESS
				);
				segment(
					Vector3{0.0f, 0.0f, 0.0f},
					Vector3{0.0f, GRID_STEP, 0.0f},
					ImGui::GetColorU32(ImVec4(0.45f, 0.85f, 0.40f, 0.65f)),
					AXIS_THICKNESS
				);
			}

			// The selection, boxed. **Drawn per panel rather than once**,
			// because two viewports showing the same world both have to show it
			// and they have different projections.
			if (shown.IsValid() && shown == SelectionWorld && !Selection.empty() && Universe != nullptr) {
				const ImU32 outline = engine::ui::AccentColour();

				Universe->Enter(shown, [&](Store &store) {
					for (const Entity instance : Selection) {
						if (!store.Alive(instance)) {
							continue;
						}

						const auto *transform = store.Get<engine::scene::Transform>(instance);
						const auto *bounds = store.Get<engine::scene::Bounds>(instance);
						if (transform == nullptr || bounds == nullptr) {
							continue;
						}

						// The eight corners of the oriented box, joined as
						// twelve edges. An axis-aligned box round an oriented
						// part would be a box that does not touch it.
						const Vector3 half = bounds->HalfExtent;
						Vector3 corner[8];
						for (int index8 = 0; index8 < 8; index8++) {
							const Vector3 local{
								(index8 & 1) ? half.X : -half.X,
								(index8 & 2) ? half.Y : -half.Y,
								(index8 & 4) ? half.Z : -half.Z
							};
							corner[index8] = transform->Frame.PointToWorldSpace(local);
						}

						static constexpr int EDGES[12][2] = {
							{0, 1},
							{1, 3},
							{3, 2},
							{2, 0},
							{4, 5},
							{5, 7},
							{7, 6},
							{6, 4},
							{0, 4},
							{1, 5},
							{2, 6},
							{3, 7}
						};

						for (const auto &edge : EDGES) {
							segment(corner[edge[0]], corner[edge[1]], outline, 1.5f);
						}

						if (!ShowFacing) {
							continue;
						}

						// --- which way it is facing ------------------------
						//
						// **A box says nothing about its orientation.** Two
						// parts sitting identically may be turned a quarter
						// apart, and nothing in the outline distinguishes them
						// - which matters the moment anything is placed by
						// script, welded, or driven along its own look.
						//
						// So: a line out of the front face to a ball, and a
						// ring round the ball with an arrow at the point that
						// is up. The line is the look and the arrow is the
						// roll, which together are the whole of the rotation a
						// person can act on.
						const Vector3 look = transform->Frame.LookVector();
						const Vector3 up = transform->Frame.UpVector();

						// In metres and proportional to the part, unlike the
						// gizmo's pixels: this is a property of the thing being
						// looked at rather than a control being aimed at, so it
						// should grow with the part and shrink into the
						// distance exactly as the part does.
						const float reach = std::max({half.X, half.Y, half.Z, 0.05f}) * FACING_REACH;

						const Vector3 face = transform->Frame.Position + look * half.Z;
						const Vector3 ballAt = face + look * reach;

						segment(face, ballAt, FACING_LOOK, 2.0f);

						glm::vec2 ball{};
						if (!panel.WorldToPanel(ballAt, ball)) {
							continue;
						}
						list->AddCircleFilled(ImVec2(ball.x, ball.y), 4.5f, FACING_LOOK);

						// The ring lies in the plane the look is normal to, so
						// it reads as a collar round the line rather than as a
						// second circle floating beside it.
						const Vector3 side = look.Cross(up).Unit();
						const float ringRadius = reach * 0.42f;

						constexpr int RING = 24;
						glm::vec2 previous{};
						bool havePrevious = false;
						for (int step = 0; step <= RING; step++) {
							const float angle =
								6.2831853f * static_cast<float>(step) / static_cast<float>(RING);
							const Vector3 at = ballAt + up * (std::cos(angle) * ringRadius) +
											   side * (std::sin(angle) * ringRadius);

							glm::vec2 screen{};
							if (!panel.WorldToPanel(at, screen)) {
								havePrevious = false;
								continue;
							}
							if (havePrevious) {
								list->AddLine(
									ImVec2(previous.x, previous.y),
									ImVec2(screen.x, screen.y),
									FACING_UP,
									1.5f
								);
							}
							previous = screen;
							havePrevious = true;
						}

						// The head sits where the ring is highest and points
						// away from the ball, so "which way is up" is answered
						// by one glance rather than by counting.
						const Vector3 tip = ballAt + up * (ringRadius * 1.55f);
						const Vector3 base = ballAt + up * ringRadius;
						segment(base, tip, FACING_UP, 2.0f);
						segment(tip, base + side * (ringRadius * 0.42f), FACING_UP, 2.0f);
						segment(tip, base - side * (ringRadius * 0.42f), FACING_UP, 2.0f);
					}
				});
			}

			list->PopClipRect();
		}
	}

	bool Editor::SelectionCentre(WorldId world, Vector3 &centre) {
		if (Selection.empty() || !world.IsValid() || Universe == nullptr) {
			return false;
		}

		Vector3 total;
		size_t counted = 0;

		const std::vector<Entity> selected = Selection;
		Universe->Enter(world, [&](Store &store) {
			for (const Entity instance : selected) {
				if (!store.Alive(instance)) {
					continue;
				}
				if (const auto *transform = store.Get<engine::scene::Transform>(instance)) {
					total = total + transform->Frame.Position;
					counted++;
				}
			}
		});

		if (counted == 0) {
			return false;
		}

		centre = total * (1.0f / static_cast<float>(counted));
		return true;
	}

	bool Editor::DrawGizmo(size_t viewport, const PanelProjection &panel) {
		const WorldId world = ViewportWorld(viewport);
		if (world != SelectionWorld) {
			return false;
		}

		Vector3 centre;
		if (!SelectionCentre(world, centre)) {
			return false;
		}

		OverlaySlot &slot = Overlays[viewport];
		ImDrawList *list = slot.List;

		// **The handle length is in pixels, not metres.** A fixed world length
		// is a gizmo that fills the screen up close and vanishes at distance,
		// which is the one thing a manipulator must not do. Scaling by the
		// distance to the eye keeps it the same size on screen.
		const float distance = (centre - panel.Eye).Magnitude();
		const float length = std::max(distance * 0.12f, 0.001f);

		static constexpr Vector3 AXES[3] = {
			Vector3{1.0f, 0.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f}, Vector3{0.0f, 0.0f, 1.0f}
		};
		static constexpr ImU32 COLOURS[3] = {
			IM_COL32(220, 80, 85, 255), IM_COL32(115, 215, 105, 255), IM_COL32(85, 130, 235, 255)
		};

		glm::vec2 origin{};
		const bool originVisible = panel.WorldToPanel(centre, origin);

		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const glm::vec2 cursor(mouse.x, mouse.y);

		int hovered = -1;
		int hoveredSign = 1;
		constexpr float GRAB_PIXELS = 8.0f;
		constexpr ImU32 LIT = IM_COL32(255, 235, 140, 255);

		// **The mode is the drag's, not the toolbar's, once a drag is under
		// way.** Changing tool mid-drag would otherwise turn a move into a
		// rotation halfway through, applied against frames captured for the
		// other operation.
		const ToolMode mode = Dragging.Axis >= 0 ? Dragging.Mode : CurrentTool;

		if (mode == ToolMode::Select) {
			return false;
		}

		// **Every ray measurement is made from where the drag began, not from
		// where the selection is now.** `Grabbed` is a distance along the axis
		// from the centre at grab time, and a live centre has already moved by
		// the delta being applied - so the next frame measures nothing, puts
		// everything back, and the frame after measures the full delta again.
		// That is the flicker between the old and the new position, and this
		// line is the fix. See `GizmoDrag::Centre`.
		const bool holding = Dragging.Axis >= 0 && Dragging.Viewport == viewport;
		const Vector3 anchor = holding ? Dragging.Centre : centre;

		// **Two arms per axis since v0.13.** One arm is a handle you cannot
		// reach from half the angles a person orbits to: the negative side of
		// the part faces you and there is nothing on it to grab. Both arms of a
		// move do the same thing - the delta is measured along the axis either
		// way - and both arms of a scale differ only in which face grows, which
		// is what `GizmoDrag::Sign` carries.
		//
		// A rotate ring is already both-sided, so it takes none of this.
		static constexpr int SIDES = 2;
		static constexpr float SIGN_OF[SIDES] = {1.0f, -1.0f};

		glm::vec2 tips[3][SIDES]{};
		bool tipVisible[3][SIDES]{};

		// **A rotate gizmo is a ring, so it is hit-tested as one.** Sharing the
		// axis-line test would put the grab region down the middle of the ring
		// rather than on it, which is the one place a person will not click.
		const bool ring = mode == ToolMode::Rotate;

		// The ring's points, kept so the hover test and the drawing agree
		// exactly rather than being computed twice from the same formula.
		constexpr int RING_STEPS = 48;
		std::vector<glm::vec2> ringPoints[3];

		for (int axis = 0; axis < 3; axis++) {
			if (ring) {
				// Two perpendiculars to the axis, so the circle can be walked.
				const Vector3 normal = AXES[axis];
				const Vector3 first = AXES[(axis + 1) % 3];
				const Vector3 second = AXES[(axis + 2) % 3];

				ringPoints[axis].reserve(RING_STEPS + 1);
				for (int step = 0; step <= RING_STEPS; step++) {
					const float angle =
						6.2831853f * static_cast<float>(step) / static_cast<float>(RING_STEPS);
					const Vector3 at =
						centre + first * (std::cos(angle) * length) + second * (std::sin(angle) * length);

					glm::vec2 screen{};
					if (panel.WorldToPanel(at, screen)) {
						ringPoints[axis].push_back(screen);
					}
				}
				(void)normal;
				continue;
			}

			for (int side = 0; side < SIDES; side++) {
				tipVisible[axis][side] =
					panel.WorldToPanel(centre + AXES[axis] * (length * SIGN_OF[side]), tips[axis][side]);
			}
		}

		// --- what the cursor is over ------------------------------------------

		if (Dragging.Axis < 0) {
			for (int axis = 0; axis < 3; axis++) {
				if (ring) {
					for (const glm::vec2 &point : ringPoints[axis]) {
						const glm::vec2 gap = cursor - point;
						if (std::sqrt(gap.x * gap.x + gap.y * gap.y) <= GRAB_PIXELS) {
							hovered = axis;
							hoveredSign = 1;
							break;
						}
					}
					continue;
				}

				for (int side = 0; side < SIDES; side++) {
					if (!originVisible || !tipVisible[axis][side]) {
						continue;
					}

					// Distance from the cursor to the handle's screen segment. A
					// screen-space test rather than a world one, because the
					// thing being clicked is a line on the screen.
					const glm::vec2 along = tips[axis][side] - origin;
					const float lengthSquared = along.x * along.x + along.y * along.y;
					if (lengthSquared < 1.0f) {
						continue;
					}

					const glm::vec2 toCursor = cursor - origin;
					const float t =
						std::clamp((toCursor.x * along.x + toCursor.y * along.y) / lengthSquared, 0.0f, 1.0f);
					const glm::vec2 nearest = origin + along * t;
					const glm::vec2 gap = cursor - nearest;

					if (std::sqrt(gap.x * gap.x + gap.y * gap.y) <= GRAB_PIXELS) {
						hovered = axis;
						hoveredSign = static_cast<int>(SIGN_OF[side]);
					}
				}
			}
		}

		// --- the handles -------------------------------------------------------

		for (int axis = 0; axis < 3; axis++) {
			// The ring has no arms, so it is lit as a whole.
			const bool lit = Dragging.Axis == axis || (Dragging.Axis < 0 && hovered == axis);
			const ImU32 colour = lit ? LIT : COLOURS[axis];

			if (ring) {
				if (ringPoints[axis].size() > 1) {
					list->AddPolyline(
						reinterpret_cast<const ImVec2 *>(ringPoints[axis].data()),
						static_cast<int>(ringPoints[axis].size()),
						colour,
						ImDrawFlags_None,
						lit ? 3.5f : 2.0f
					);
				}
				continue;
			}

			for (int side = 0; side < SIDES; side++) {
				if (!originVisible || !tipVisible[axis][side]) {
					continue;
				}

				// **Lit per arm, not per axis.** Both arms of an axis do the
				// same thing to a move, but the one under the cursor is the one
				// a person is about to take hold of - and for a scale in
				// `ScaleSide::Side` they genuinely differ, so highlighting the
				// pair would be the manipulator lying about which face grows.
				const int sign = static_cast<int>(SIGN_OF[side]);
				const bool armLit = Dragging.Axis == axis
										? Dragging.Sign == sign
										: (Dragging.Axis < 0 && hovered == axis && hoveredSign == sign);
				const ImU32 armColour = armLit ? LIT : COLOURS[axis];

				const glm::vec2 &tip = tips[axis][side];
				list->AddLine(
					ImVec2(origin.x, origin.y), ImVec2(tip.x, tip.y), armColour, armLit ? 4.0f : 2.5f
				);

				// **A cube for scale, a dot for move**, so the two modes are
				// told apart by their shape rather than only by which button is
				// lit.
				if (mode == ToolMode::Scale) {
					const float half = armLit ? 5.5f : 4.0f;
					list->AddRectFilled(
						ImVec2(tip.x - half, tip.y - half), ImVec2(tip.x + half, tip.y + half), armColour
					);
				} else {
					list->AddCircleFilled(ImVec2(tip.x, tip.y), armLit ? 5.5f : 4.0f, armColour);
				}
			}
		}

		// --- the drag ---------------------------------------------------------

		// **Built only when something below will read it.** `PanelToRay`
		// inverts the matrix, and `Projection.cpp` declines to cache that
		// inverse on the grounds that a panel issues one ray per click rather
		// than one per pixel - which was true of the ray's *consumers* and not
		// of this line, which ran every frame for every panel with a selection
		// in it. The guard is what makes the premise hold again.
		//
		// It is strictly wider than the two branches that use `ray`: the grab
		// below needs `hovered >= 0`, and the live drag needs
		// `Dragging.Axis >= 0`. Neither can run with this false, so the
		// degenerate default is never read.
		Ray ray;
		if (Dragging.Axis >= 0 || hovered >= 0) {
			ray = panel.PanelToRay(cursor);
		}

		if (Dragging.Axis < 0 && hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			bool grabbedOk = false;
			float grabbed = 0.0f;
			Vector3 grabbedPoint;

			if (mode == ToolMode::Rotate) {
				grabbedOk = IntersectRayPlane(centre, AXES[hovered], ray, grabbedPoint);
			} else {
				grabbedOk = ClosestPointOnAxis(centre, AXES[hovered], ray, grabbed);
			}

			if (grabbedOk) {
				Dragging = GizmoDrag{};
				Dragging.Axis = hovered;
				Dragging.Sign = hoveredSign;
				Dragging.Viewport = viewport;
				Dragging.Grabbed = grabbed;
				Dragging.GrabbedPoint = grabbedPoint;
				Dragging.Centre = centre;
				Dragging.Mode = mode;
				Dragging.Sides = ScaleSides;
				Dragging.Pivots = PivotEditing && mode != ToolMode::Scale;

				Universe->Enter(world, [&](Store &store) {
					for (const Entity instance : Selection) {
						if (!store.Alive(instance)) {
							continue;
						}
						if (const auto *transform = store.Get<engine::scene::Transform>(instance)) {
							Dragging.Instances.push_back(instance);
							Dragging.Before.push_back(transform->Frame);

							const auto *bounds = store.Get<engine::scene::Bounds>(instance);
							Dragging.BeforeSize.push_back(bounds != nullptr ? bounds->HalfExtent : Vector3{});

							const auto *pivot = store.Get<engine::scene::Pivot>(instance);
							Dragging.BeforePivot.push_back(pivot != nullptr ? pivot->Offset : CFrame{});
						}
					}
				});
			}
		}

		if (Dragging.Axis >= 0 && Dragging.Viewport == viewport) {
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				const Vector3 axis = AXES[Dragging.Axis];

				float delta = 0.0f;
				float radians = 0.0f;
				bool tracking = false;

				if (Dragging.Mode == ToolMode::Rotate) {
					Vector3 now;
					if (IntersectRayPlane(anchor, axis, ray, now)) {
						// The signed angle between where the drag began on this
						// plane and where the cursor is now, measured about the
						// axis. `atan2` of the cross onto the axis against the
						// dot is the branch-free version, and it is the one that
						// keeps working past a quarter turn.
						const Vector3 from = (Dragging.GrabbedPoint - anchor).Unit();
						const Vector3 to = (now - anchor).Unit();

						const Vector3 crossed = from.Cross(to);
						radians = std::atan2(crossed.Dot(axis), from.Dot(to));

						if (SnapEnabled && SnapDegrees > 0.0f) {
							const float step = SnapDegrees * 0.0174532925f;
							radians = std::round(radians / step) * step;
						}

						tracking = std::abs(radians) > 0.0001f;
					}
				} else {
					float now = 0.0f;
					if (ClosestPointOnAxis(anchor, axis, ray, now)) {
						delta = now - Dragging.Grabbed;

						if (SnapEnabled && SnapDistance > 0.0f) {
							delta = std::round(delta / SnapDistance) * SnapDistance;
						}

						tracking = std::abs(delta) > 0.0001f;
					}
				}

				if (tracking) {
					Dragging.Moved = true;

					// **Applied from the frames captured on grab**, not
					// accumulated onto the live ones. Accumulating drifts,
					// because each frame's delta is computed against a
					// centre that the previous frame already moved.
					Universe->Enter(world, [&](Store &store) {
						for (size_t index = 0; index < Dragging.Instances.size(); index++) {
							const Entity instance = Dragging.Instances[index];
							if (!store.Alive(instance)) {
								continue;
							}

							const auto *transform = store.Get<engine::scene::Transform>(instance);
							if (transform == nullptr) {
								continue;
							}

							// **Rotation is carried through from the frame
							// captured on grab, not from the live one.** A
							// translate gizmo must not touch orientation,
							// and rebuilding the frame from a position
							// alone would quietly reset it.
							// **A pivot edit moves the handle and leaves the
							// part exactly where it is**, which is the whole
							// point of the mode: a door whose hinge is in the
							// wrong place is fixed by moving the hinge, and an
							// editor that moved the door with it would leave the
							// author where they started.
							//
							// The delta is in world space and `Pivot::Offset` is
							// in the part's own, so it is carried across by the
							// frame the part was grabbed at. `PivotOf` composes
							// them the same way round.
							if (Dragging.Pivots) {
								const CFrame &frame = Dragging.Before[index];
								const CFrame &was = Dragging.BeforePivot[index];

								CFrame offset = was;
								if (Dragging.Mode == ToolMode::Move) {
									// The world delta carried into the part's own
									// frame, so dragging the X handle moves the
									// pivot along the world X whatever the part
									// is turned to. A *vector* rather than a
									// point: a translation has no origin.
									offset.Position =
										was.Position + frame.Inverse().VectorToWorldSpace(axis * delta);
								} else {
									// **About the pivot's own position, not the
									// selection's centre.** A move gizmo swings a
									// group because that is what turning a group
									// means; a pivot orbiting the group would put
									// the hinge somewhere nobody pointed at.
									//
									// The axis is a world one, so it is carried
									// into the part's frame the same way the
									// translation is.
									const Vector3 local = frame.Inverse().VectorToWorldSpace(axis).Unit();
									const glm::quat spin =
										glm::angleAxis(radians, glm::vec3(local.X, local.Y, local.Z));

									offset = CFrame(was.Position) * CFrame(Vector3{}, spin) *
											 CFrame(was.Position).Inverse() * was;
								}

								(void)store.SetPropertyAuthored(
									instance, engine::core::Name("PivotOffset"), &offset, sizeof(offset)
								);
								continue;
							}

							engine::scene::Transform moved = *transform;
							moved.Frame = Dragging.Before[index];

							switch (Dragging.Mode) {
							case ToolMode::Move:
								moved.Frame.Position = Dragging.Before[index].Position + axis * delta;
								break;

							case ToolMode::Rotate: {
								// **About the selection's centre, not each
								// instance's own origin.** Turning a group has
								// to swing it, and rotating every member in
								// place would leave the group's shape unchanged
								// while every part spun.
								//
								// Translate to the centre, turn, translate back
								// - which is the composition, written out,
								// rather than a position and an orientation
								// computed separately and hoped to agree.
								const glm::quat spin =
									glm::angleAxis(radians, glm::vec3(axis.X, axis.Y, axis.Z));

								const CFrame turn =
									CFrame(anchor) * CFrame(Vector3{}, spin) * CFrame(anchor).Inverse();

								moved.Frame = turn * Dragging.Before[index];
								break;
							}

							case ToolMode::Scale: {
								// Grows along the axis, from the size captured
								// on grab. Clamped above zero: a part with a
								// negative half-extent is inside out.
								const Vector3 was = Dragging.BeforeSize[index];
								if (was.X <= 0.0f && was.Y <= 0.0f && was.Z <= 0.0f) {
									break;
								}

								// **Outward, whichever arm was grabbed.**
								// `delta` is measured along the positive axis,
								// so pulling the negative handle away from the
								// part is a negative delta and the same outward
								// motion. Multiplying by the sign turns both
								// into "how much bigger".
								const float growth = delta * static_cast<float>(Dragging.Sign);

								// How much the stored *half*-extent gains. The
								// three modes differ only here, and each is one
								// line, which is the argument for a list over
								// three code paths.
								float gain = 0.0f;
								switch (Dragging.Sides) {
								case ScaleSide::Side:
									// One face moves by `growth`, so the half
									// gains half of it - and the centre has to
									// move the same amount, or the other face
									// would move too. That shift is applied
									// below.
									gain = growth * 0.5f;
									break;

								case ScaleSide::Both:
									// Both faces move by `growth`, so the part
									// gains twice it and stays put.
									gain = growth;
									break;

								case ScaleSide::BothHalf:
									// Both faces move by half of `growth`. The
									// part gains exactly `growth` and stays
									// put, which is what a snapped drag wants:
									// the size lands on the step rather than on
									// twice it.
									gain = growth * 0.5f;
									break;
								}

								const float wasHalf = Dragging.Axis == 0   ? was.X
													  : Dragging.Axis == 1 ? was.Y
																		   : was.Z;

								// **The clamp decides the shift too.** A part
								// driven down onto the floor size would
								// otherwise go on sliding while its size stood
								// still, which is a face moving through the one
								// it is supposed to be anchored to.
								constexpr float SMALLEST_HALF = 0.01f;
								const float nowHalf = std::max(wasHalf + gain, SMALLEST_HALF);
								const float applied = nowHalf - wasHalf;

								Vector3 grown = was;
								if (Dragging.Axis == 0) {
									grown.X = nowHalf;
								} else if (Dragging.Axis == 1) {
									grown.Y = nowHalf;
								} else {
									grown.Z = nowHalf;
								}

								// **Along the part's own axis, not the world
								// one.** The size that changed is a local
								// component - `grown.X` is the part's X however
								// it is turned - so the centre has to move
								// along the same local direction or a rotated
								// part would slide sideways as it grew.
								if (Dragging.Sides == ScaleSide::Side) {
									const Vector3 out =
										Dragging.Before[index].VectorToWorldSpace(axis).Unit();
									moved.Frame.Position =
										Dragging.Before[index].Position +
										out * (applied * static_cast<float>(Dragging.Sign));
								}

								// **Through the property, not onto the component.**
								// This wrote `Bounds` directly, and `Size` is
								// declared as writing `Bounds` *and* `Collider` -
								// `scene::SizeProperty` says why in as many
								// words: a setter that moves only the first
								// leaves a part drawn at one size and collided at
								// another, and nothing reports it. Dragging the
								// scale gizmo was the one caller that bypassed
								// it, so a part resized in the editor kept the
								// hitbox it was created with.
								//
								// `Size` is the full extent over a stored half,
								// which is the same conversion the undo command
								// below makes - and making both go through the
								// property is what keeps them one answer.
								const Vector3 full = grown * 2.0f;
								(void)store.SetPropertyAuthored(
									instance, engine::core::Name("Size"), &full, sizeof(full)
								);
								break;
							}

							case ToolMode::Select:
								break;
							}

							// **A one-sided scale writes both.** It is the
							// only drag that changes a size *and* a placement,
							// because holding the far face still while the near
							// one moves is exactly a centre that moves by half
							// of what the size gained.
							if (Dragging.Mode != ToolMode::Scale || Dragging.Sides == ScaleSide::Side) {
								store.Set<engine::scene::Transform>(instance, moved);
							}
						}
					});
				}
			} else {
				// Released. One command for the whole drag, and only when it
				// actually moved something - see `GizmoDrag`.
				if (Dragging.Moved && Commands != nullptr) {
					// **Which property the command names has to match what the
					// drag changed.** A scale that recorded a `CFrame` would
					// undo by restoring a position nothing had moved, and leave
					// the size it actually changed exactly where it was - an
					// undo that reports success and reverses nothing.
					const bool scaling = Dragging.Mode == ToolMode::Scale;
					const engine::core::Name property(
						Dragging.Pivots ? "PivotOffset" : (scaling ? "Size" : "CFrame")
					);
					const char *what = Dragging.Pivots					   ? "Edit Pivot"
									   : scaling						   ? "Resize"
									   : Dragging.Mode == ToolMode::Rotate ? "Rotate"
																		   : "Move";

					// **One waypoint for the whole drag.** Without this each
					// property recorded is its own undo step, so dragging five
					// parts took five presses of Ctrl+Z to put back - and a
					// one-sided resize, which writes a size *and* a placement
					// for every part, took ten. A drag is one action to the
					// person who made it.
					//
					// A recording that could not be opened is a plugin holding
					// one; the commands still record, just ungrouped, which is
					// exactly what happened before this line existed. Refusing
					// to record the drag at all would be a worse answer to
					// somebody else's bookkeeping.
					const std::optional<std::string> group = Commands->TryBeginRecording(what, what);

					// A one-sided resize moves the centre to hold the far face
					// still, so the placement changed too and has to be part of
					// the same step.
					const bool alsoMoved = scaling && Dragging.Sides == ScaleSide::Side;

					Universe->Enter(world, [&](Store &store) {
						for (size_t index = 0; index < Dragging.Instances.size(); index++) {
							const Entity instance = Dragging.Instances[index];
							if (!store.Alive(instance)) {
								continue;
							}

							engine::game::PropertyValue before;
							engine::game::PropertyValue after;

							if (scaling) {
								const auto *bounds = store.Get<engine::scene::Bounds>(instance);
								if (bounds == nullptr) {
									continue;
								}

								// `Size` is the full extent and `Bounds` holds
								// the half, which is the conversion
								// `scene::Part` already makes - recording the
								// half under the full name would halve the part
								// on undo.
								before.Type = engine::ecs::PropertyType::Vector3;
								before.Vector3 = Dragging.BeforeSize[index] * 2.0f;

								after.Type = engine::ecs::PropertyType::Vector3;
								after.Vector3 = bounds->HalfExtent * 2.0f;
							} else if (Dragging.Pivots) {
								const auto *pivot = store.Get<engine::scene::Pivot>(instance);
								if (pivot == nullptr) {
									continue;
								}

								before.Type = engine::ecs::PropertyType::CFrame;
								before.CFrame = Dragging.BeforePivot[index];

								after.Type = engine::ecs::PropertyType::CFrame;
								after.CFrame = pivot->Offset;
							} else {
								const auto *transform = store.Get<engine::scene::Transform>(instance);
								if (transform == nullptr) {
									continue;
								}

								before.Type = engine::ecs::PropertyType::CFrame;
								before.CFrame = Dragging.Before[index];

								after.Type = engine::ecs::PropertyType::CFrame;
								after.CFrame = transform->Frame;
							}

							Commands->RecordProperty(world, instance, property, before, after, what);

							if (!alsoMoved) {
								continue;
							}

							const auto *transform = store.Get<engine::scene::Transform>(instance);
							if (transform == nullptr) {
								continue;
							}

							engine::game::PropertyValue wasFrame;
							wasFrame.Type = engine::ecs::PropertyType::CFrame;
							wasFrame.CFrame = Dragging.Before[index];

							engine::game::PropertyValue isFrame;
							isFrame.Type = engine::ecs::PropertyType::CFrame;
							isFrame.CFrame = transform->Frame;

							Commands->RecordProperty(
								world, instance, engine::core::Name("CFrame"), wasFrame, isFrame, what
							);
						}
					});

					if (group) {
						Commands->FinishRecording(*group, FinishOperation::Commit);
					}

					MarkModified();
				}

				Dragging = GizmoDrag{};
			}
		}

		// A click on a handle is not a click on the world behind it.
		return hovered >= 0 || Dragging.Axis >= 0;
	}

	std::optional<engine::core::RayHit>
	Editor::RaycastWorld(WorldId world, const Ray &ray, std::span<const Entity> ignore) {
		if (!world.IsValid() || Universe == nullptr) {
			return std::nullopt;
		}

		// Built from what is drawable now. See the declaration for why this is
		// not the physics broadphase.
		std::vector<engine::spatial::Proxy> proxies;

		Universe->Enter(world, [&](Store &store) {
			store.Each<engine::scene::Transform, engine::scene::Bounds>(
				[&](Entity entity,
					const engine::scene::Transform &transform,
					const engine::scene::Bounds &bounds) {
					// **A locked part is left out of the grid rather than
					// filtered out of the hit**, which is the difference between
					// "cannot be picked" and "can be picked and is then
					// ignored": the second one still swallows the ray, so a
					// locked wall in front of the thing somebody wants would
					// make that thing unclickable too. Leaving it out lets the
					// ray carry on to whatever is behind it, which is what
					// locking a wall was for.
					//
					// `Visual` rather than a set the editor keeps: locking is
					// authoring data that survives a save - see
					// `scene::Visual::Locked`.
					if (const auto *visual = store.Get<engine::scene::Visual>(entity);
						visual != nullptr && visual->Locked) {
						return;
					}

					// **The same argument, one door along.** A part being
					// dragged is left out rather than skipped in the hit,
					// because a part that swallowed its own ray would rest on
					// itself: the surface under the cursor would be the thing
					// in the author's hand, and it would never reach the floor.
					if (std::find(ignore.begin(), ignore.end(), entity) != ignore.end()) {
						return;
					}

					engine::spatial::Proxy proxy;
					proxy.Id = entity.Id;
					proxy.Bounds = AABB::FromOrientedBox(transform.Frame, bounds.HalfExtent);
					proxy.Layers = engine::spatial::LayerMask::All();
					proxies.push_back(proxy);
				}
			);
		});

		if (proxies.empty()) {
			return std::nullopt;
		}

		engine::spatial::HashGrid grid;
		grid.Rebuild(proxies);
		return engine::spatial::Raycast(grid, ray, PICK_REACH);
	}

	bool Editor::DragOnSurface(size_t viewport, const PanelProjection &panel) {
		// **Select's own manipulation, and it needs no handles.** Every other
		// tool asks you to hit a line a few pixels wide; this one is the whole
		// part, which is how a person expects to move something in a picture of
		// a room. Roblox's drag, and the reason Select is not simply "no tool".
		const WorldId world = ViewportWorld(viewport);
		if (world != SelectionWorld || !world.IsValid() || Universe == nullptr) {
			return false;
		}

		const bool holding = SurfaceDragging.Active && SurfaceDragging.Viewport == viewport;

		// **Never while a handle is held.** A gizmo drag and a surface drag both
		// write placements, and two of them running against one selection is
		// two answers to where it is.
		if (!holding && (CurrentTool != ToolMode::Select || Dragging.Axis >= 0)) {
			return false;
		}

		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const glm::vec2 cursor(mouse.x, mouse.y);

		// --- starting one ----------------------------------------------------

		if (!holding) {
			// **Past the threshold, not on the press.** A click is a selection
			// and a drag is a move, and imgui already draws that line - the
			// pick in `DrawViewport` is recorded only for a release that never
			// crossed it, so the two cannot both fire.
			if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left) || !panel.ContainsPanel(cursor)) {
				return false;
			}
			if (!(viewport == 0 ? ViewportHovered
								: (ExtraAt(viewport) != nullptr && ExtraAt(viewport)->Hovered))) {
				return false;
			}

			// The part under where the drag *began*, not under the cursor now -
			// by the time this fires the pointer has already moved, and picking
			// from where it is would grab whatever it happened to have travelled
			// over.
			const ImVec2 began = ImVec2(
				mouse.x - ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x,
				mouse.y - ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y
			);

			// A drag that began outside this panel is somebody else's gesture
			// arriving over the top of it - a slider released across the
			// viewport, most often.
			const glm::vec2 from(began.x, began.y);
			if (!panel.ContainsPanel(from)) {
				return false;
			}

			const std::optional<engine::core::RayHit> grabbed =
				RaycastWorld(world, panel.PanelToRay(from), {});
			if (!grabbed) {
				return false;
			}

			const Entity taken(grabbed->Id);

			// **Dragging something unselected selects it first**, which is what
			// a person means by putting the pointer on a thing and pulling it.
			// Dragging something already in a selection moves the whole
			// selection, which is what they mean the rest of the time.
			if (std::find(Selection.begin(), Selection.end(), taken) == Selection.end()) {
				Select(world, taken, false);
				SelectionAnchor = taken;
			}

			SurfaceDragging = SurfaceGrab{};
			SurfaceDragging.Viewport = viewport;
			SurfaceDragging.Primary = taken;

			Universe->Enter(world, [&](Store &store) {
				for (const Entity instance : Selection) {
					if (!store.Alive(instance)) {
						continue;
					}
					if (const auto *transform = store.Get<engine::scene::Transform>(instance)) {
						SurfaceDragging.Instances.push_back(instance);
						SurfaceDragging.Before.push_back(transform->Frame);
					}
				}
			});

			if (SurfaceDragging.Instances.empty()) {
				SurfaceDragging = SurfaceGrab{};
				return false;
			}

			SurfaceDragging.Active = true;
			return true;
		}

		// --- releasing one ---------------------------------------------------

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			if (SurfaceDragging.Moved && Commands != nullptr) {
				// One waypoint for the whole drag, exactly as the gizmo's
				// release does - a drag is one action to the person who made it
				// however many parts it carried.
				const std::optional<std::string> group = Commands->TryBeginRecording("Move", "Move");

				Universe->Enter(world, [&](Store &store) {
					for (size_t index = 0; index < SurfaceDragging.Instances.size(); index++) {
						const Entity instance = SurfaceDragging.Instances[index];
						const auto *transform = store.Get<engine::scene::Transform>(instance);
						if (!store.Alive(instance) || transform == nullptr) {
							continue;
						}

						engine::game::PropertyValue before;
						before.Type = engine::ecs::PropertyType::CFrame;
						before.CFrame = SurfaceDragging.Before[index];

						engine::game::PropertyValue after;
						after.Type = engine::ecs::PropertyType::CFrame;
						after.CFrame = transform->Frame;

						Commands->RecordProperty(
							world, instance, engine::core::Name("CFrame"), before, after, "Move"
						);
					}
				});

				if (group) {
					Commands->FinishRecording(*group, FinishOperation::Commit);
				}
				MarkModified();
			}

			SurfaceDragging = SurfaceGrab{};
			return false;
		}

		// --- carrying one ----------------------------------------------------

		const std::optional<engine::core::RayHit> resting =
			RaycastWorld(world, panel.PanelToRay(cursor), SurfaceDragging.Instances);
		if (!resting) {
			// **Nothing under the cursor leaves the selection where it is.** A
			// part dropped at some arbitrary distance down a ray pointed at the
			// sky is a part somebody then has to go and find; standing still is
			// the answer they can undo by moving the mouse back.
			return true;
		}

		const size_t primary = static_cast<size_t>(std::distance(
			SurfaceDragging.Instances.begin(),
			std::find(
				SurfaceDragging.Instances.begin(), SurfaceDragging.Instances.end(), SurfaceDragging.Primary
			)
		));
		if (primary >= SurfaceDragging.Instances.size()) {
			return true;
		}

		const CFrame &was = SurfaceDragging.Before[primary];

		Vector3 half;
		Universe->Enter(world, [&](Store &store) {
			if (const auto *bounds = store.Get<engine::scene::Bounds>(SurfaceDragging.Instances[primary])) {
				half = bounds->HalfExtent;
			}
		});

		// **The rotation is decided before the position, because the position
		// depends on it.** How far the box reaches toward the surface is a
		// function of which way it is turned, so aligning after placing would
		// leave the part hanging above a slope or buried in it.
		const glm::quat rotation = DragAligns ? AlignedTo(was, resting->Normal) : was.Rotation();

		const CFrame turned(was.Position, rotation);
		Vector3 position = resting->Position + resting->Normal * SupportAlong(turned, half, resting->Normal);

		// Snapping applies to where it lands, not to how far it travelled: a
		// drag has no origin to step from, and rounding the destination is what
		// puts parts on a shared grid rather than on parallel ones.
		if (SnapEnabled && SnapDistance > 0.0f) {
			position.X = std::round(position.X / SnapDistance) * SnapDistance;
			position.Z = std::round(position.Z / SnapDistance) * SnapDistance;
		}

		// **One rigid transform, applied to every member from its captured
		// frame.** Moving each instance to the cursor would pile a selection
		// into one place; carrying the group by the part that was grabbed is
		// what keeps a built thing built.
		const CFrame target(position, rotation);
		const CFrame carry = target * was.Inverse();

		SurfaceDragging.Moved = true;

		Universe->Enter(world, [&](Store &store) {
			for (size_t index = 0; index < SurfaceDragging.Instances.size(); index++) {
				const Entity instance = SurfaceDragging.Instances[index];
				const auto *transform = store.Get<engine::scene::Transform>(instance);
				if (!store.Alive(instance) || transform == nullptr) {
					continue;
				}

				engine::scene::Transform moved = *transform;
				moved.Frame = index == primary ? target : carry * SurfaceDragging.Before[index];
				store.Set<engine::scene::Transform>(instance, moved);
			}
		});

		return true;
	}

	void Editor::PickInViewport(size_t viewport, float x, float y, bool add, const PanelProjection &panel) {
		const WorldId shown = ViewportWorld(viewport);
		if (!shown.IsValid() || Universe == nullptr) {
			return;
		}

		if (!panel.IsValid() || !panel.ContainsPanel(glm::vec2(x, y))) {
			return;
		}

		const Ray ray = panel.PanelToRay(glm::vec2(x, y));
		const std::optional<engine::core::RayHit> hit = RaycastWorld(shown, ray, {});

		if (!hit.has_value()) {
			// **A click on nothing clears the selection**, unless it is adding
			// to one. That is what every editor does and what makes the
			// viewport feel like the thing being edited rather than a picture
			// of it.
			if (!add) {
				ClearSelection();
			}
			return;
		}

		const Entity picked(hit->Id);
		Select(shown, picked, add);
		SelectionAnchor = picked;

		// **Shown in the tree as well as highlighted in the viewport.** Clicking
		// a part is how an author asks "what is this and where does it live",
		// and an explorer that left it collapsed six levels down answered half
		// the question. Studio reveals it; so does this.
		OpenPathTo(shown, picked);
		RevealSelection = true;
	}

	void Editor::DrawViewportGui(size_t index) {
		if (index >= Overlays.size() || Universe == nullptr) {
			return;
		}

		OverlaySlot &slot = Overlays[index];
		if (!slot.Drawn || slot.List == nullptr || slot.Width <= 0.0f || slot.Height <= 0.0f) {
			return;
		}

		const WorldId shown = ViewportWorld(index);
		if (!shown.IsValid()) {
			return;
		}

		const ViewportState *viewport = ExtraAt(index);
		const engine::render::SceneTarget &display = viewport != nullptr ? viewport->Target : WorldTarget;
		const float scaleX = display.IsValid() ? static_cast<float>(display.Width) / slot.Width : 1.0f;
		const float scaleY = display.IsValid() ? static_cast<float>(display.Height) / slot.Height : 1.0f;

		engine::gui::CompileRequest request;
		request.Display.Width = display.IsValid() ? static_cast<float>(display.Width) : slot.Width;
		request.Display.Height = display.IsValid() ? static_cast<float>(display.Height) : slot.Height;

		// **Fed back from the previous frame's routing, deliberately.** The
		// hover is computed from the list a compile produced, so a compile that
		// read this frame's hover would depend on its own output.
		// `Router::Hovered` says so at length; the cost is that a button
		// appearing under a stationary pointer lights up one frame later.
		request.Hovered = GuiRouters[index].Hovered();
		request.Pressed = GuiRouters[index].Pressed();

		// The pointer in canvas space, which is the panel's own corner as the
		// origin. A `ScreenGui` inside a panel is laid out from that corner, so
		// anything else would hit-test against a canvas nobody drew.
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		engine::gui::Pointer pointer;
		pointer.Position = engine::core::Vector2{
			(mouse.x - slot.X) * scaleX,
			(mouse.y - slot.Y) * scaleY,
		};
		pointer.Down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
		pointer.ScreenOnly = true;

		// **imgui owns the mouse whenever it is over its own chrome**, and a
		// panel docked over the viewport is exactly that. Without this the
		// game's UI would receive clicks meant for the explorer sitting on top
		// of it - the keyboard twin of which `DrawShortcuts` already fixed.
		//
		// **And it owns the mouse while an item is held, which used to swallow
		// every press.** `IsWindowHovered` is false whenever *any* item is
		// active - imgui's `g.ActiveId != 0` test - and the item active through
		// a click in the viewport is this panel's own `##surface`, the button
		// `DrawViewport` lays over the image so the camera can be driven. So on
		// the frame the mouse went down, `Inside` went false, `Pick` found
		// nothing and `Router::Holding` never latched: no press shade on an
		// `AutoButtonColor` button, and no `Activated` ever.
		//
		// **This panel's own drag is the one blocked item worth seeing
		// through**, rather than `AllowWhenBlockedByActiveItem`, which would
		// see through all of them - a slider being dragged in the properties
		// panel is imgui's mouse and the game's UI should not light up under it
		// on the way past.
		const bool driving = viewport != nullptr ? viewport->Active : ViewportActive;

		pointer.Inside = (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || driving) &&
						 ImGui::IsMouseHoveringRect(
							 ImVec2(slot.X, slot.Y), ImVec2(slot.X + slot.Width, slot.Y + slot.Height), false
						 );

		std::vector<engine::gui::GuiEvent> events;
		Universe->Enter(shown, [&](Store &store) {
			// **Per panel, because a panel is a canvas with its own camera.**
			// A billboard is as many pixels across as the viewport it is
			// projected into makes it, so two panels looking at one world from
			// two distances are *supposed* to disagree - which is why this is
			// resolved here rather than once for the world.
			//
			// Before `Rebuild`, which runs the layout inside itself.
			engine::render::ResolveSpatialCanvases(store, request.Display);
			GuiLists[index].Rebuild(store, request);
			(void)ViewportImages.Render(Renderer, store, GuiLists[index].Commands(), PreviewSlot() + 1);
			if (engine::gui::PickScreen(store, GuiLists[index].Commands(), pointer.Position) == NULL_ENTITY) {
				engine::render::SpatialPointer spatial;
				if (engine::render::ResolveSpatialPointer(
						store, GuiLists[index].Commands(), request.Display, pointer.Position, spatial
					)) {
					pointer.Position = spatial.Position;
					pointer.Collector = spatial.Collector;
					pointer.ScreenOnly = false;
				}
			}

			// **Copied out of the router's span before the world is left.**
			// `Router::Update` returns a view into a vector it reuses every
			// frame, and the runtime this is about to be handed to is reached
			// outside this callback.
			const std::span<const engine::gui::GuiEvent> produced =
				GuiRouters[index].Update(store, GuiLists[index].Commands(), pointer);
			events.assign(produced.begin(), produced.end());
		});

		// **Handed to the VM, which is what turns a click into a `.Activated`.**
		// This is the join the v0.8 plan left last, and the editor is still not
		// the place that makes it: `Runtime::DeliverGuiEvents` queues, and the
		// runtime's own `Heartbeat` dispatches at the barrier alongside every
		// other signal. What the editor does is forward, which is the only part
		// it is allowed to know about.
		//
		// **Only for a world that is running**, because a runtime is what a
		// `WorldRun` holds - an edit-mode viewport routes and paints so that
		// hover and press shades behave while authoring, and there is no VM to
		// tell. That is the same split `RunMode` already draws everywhere else.
		if (!events.empty()) {
			for (const WorldRun &run : Runs) {
				if (run.World != shown || run.Runtime == nullptr) {
					continue;
				}
				run.Runtime->DeliverGuiEvents(events);
				break;
			}
		}
	}
}
