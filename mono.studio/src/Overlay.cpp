// The ground grid, the origin axes, the selection outline, and picking.
//
// **Everything here is drawn into a viewport panel's imgui draw list rather than
// by the renderer**, because `mono.engine/render` has no debug-line facility at
// all and adding a primitive path at L12 for an editor's furniture is the wrong
// place for it. See `Editor::OverlaySlot` for why it is appended after
// `DriveCamera` instead of where the panel drew, and `studio/Projection.hpp` for
// the arithmetic and the two traps it exists to avoid.

#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/game/Values.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/ui/Theme.hpp>

#include <glm/gtc/quaternion.hpp>

#include <imgui.h>

#include <studio/Editor.hpp>

#include <algorithm>
#include <cmath>
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

		// The camera this panel is looking through, as it stands *now* — which
		// is after `DriveCamera` and is therefore the camera `PresentWorld` is
		// about to render with.
		const ViewportState *extra = ExtraAt(viewport);
		const CFrame frame = extra != nullptr ? extra->Frame : CameraFrame;

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
		// `PresentWorld` honours it — looking through a camera while ignoring
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

		const engine::scene::CameraMatrices matrices =
			engine::scene::ResolveCamera(frame, lens, slot.Width / slot.Height);

		projection.Matrix = matrices.ViewProjection;
		projection.Eye = frame.Position;
		projection.ImageMin = glm::vec2(slot.X, slot.Y);
		projection.ImageSize = glm::vec2(slot.Width, slot.Height);
		projection.Near = lens.NearPlane;
		return projection;
	}

	void Editor::DrawViewportOverlays() {
		// **The gizmo goes first, and it can swallow the pending pick.** A
		// click that lands on a handle is a drag, not a selection — and which
		// it is cannot be known in `DrawViewport`, because the handle's screen
		// position comes from a projection that is only correct after
		// `DriveCamera`. So the click is recorded there and adjudicated here.
		bool overHandle = false;

		for (size_t index = 0; index < Overlays.size(); index++) {
			if (!Overlays[index].Drawn || Overlays[index].List == nullptr) {
				continue;
			}

			const PanelProjection panel = ProjectionFor(index);
			if (!panel.IsValid()) {
				continue;
			}

			if (DrawGizmo(index, panel) && index == PendingPick.Viewport) {
				overHandle = true;
			}
		}

		if (PendingPick.Wanted) {
			const PendingPickAction pick = PendingPick;
			PendingPick = PendingPickAction{};

			if (!overHandle) {
				PickInViewport(pick.Viewport, pick.X, pick.Y, pick.Add);
			}
		}

		for (size_t index = 0; index < Overlays.size(); index++) {
			OverlaySlot &slot = Overlays[index];
			if (!slot.Drawn || slot.List == nullptr) {
				continue;
			}

			const PanelProjection panel = ProjectionFor(index);
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

			// **Not while this scene is running.** The grid is authoring
			// furniture: it says where the origin is and how big a metre is
			// while you are placing things. During a play test the viewport is
			// showing the game, and a grid over it is a thing the player would
			// never see — which makes "does this look right" impossible to
			// answer from the picture in front of you.
			//
			// Per scene rather than per universe, like everything else about
			// the transport: with two viewports running two worlds, one of them
			// may be playing while the other is still being edited, and the one
			// being edited keeps its grid.
			const bool authoring = ModeOf(ViewportWorld(index)) == RunMode::Edit;

			if (ShowGrid && authoring) {
				const Vector3 eye = panel.Eye;
				const float originX = SnapDown(eye.X, GRID_STEP);
				const float originZ = SnapDown(eye.Z, GRID_STEP);
				const float reach = GRID_RADIUS * GRID_STEP;

				for (int step = -GRID_RADIUS; step <= GRID_RADIUS; step++) {
					const float offset = static_cast<float>(step) * GRID_STEP;

					// Fade with distance from the camera so the grid ends in a
					// horizon rather than a rectangle.
					const float fade = 1.0f - std::abs(static_cast<float>(step)) /
												  static_cast<float>(GRID_RADIUS);
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
				// Y is not drawn along the ground because it is not on it —
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
			const WorldId shown = ViewportWorld(index);
			if (shown.IsValid() && shown == SelectionWorld && !Selection.empty() &&
				Universe != nullptr) {
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
							{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
							{7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}
						};

						for (const auto &edge : EDGES) {
							segment(corner[edge[0]], corner[edge[1]], outline, 1.5f);
						}
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

		glm::vec2 tips[3]{};
		bool tipVisible[3]{};

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

			tipVisible[axis] = panel.WorldToPanel(centre + AXES[axis] * length, tips[axis]);
		}

		// --- what the cursor is over ------------------------------------------

		if (Dragging.Axis < 0) {
			for (int axis = 0; axis < 3; axis++) {
				if (ring) {
					for (const glm::vec2 &point : ringPoints[axis]) {
						const glm::vec2 gap = cursor - point;
						if (std::sqrt(gap.x * gap.x + gap.y * gap.y) <= GRAB_PIXELS) {
							hovered = axis;
							break;
						}
					}
					continue;
				}

				if (!originVisible || !tipVisible[axis]) {
					continue;
				}

				// Distance from the cursor to the handle's screen segment. A
				// screen-space test rather than a world one, because the thing
				// being clicked is a line on the screen.
				const glm::vec2 along = tips[axis] - origin;
				const float lengthSquared = along.x * along.x + along.y * along.y;
				if (lengthSquared < 1.0f) {
					continue;
				}

				const glm::vec2 toCursor = cursor - origin;
				const float t = std::clamp(
					(toCursor.x * along.x + toCursor.y * along.y) / lengthSquared, 0.0f, 1.0f
				);
				const glm::vec2 nearest = origin + along * t;
				const glm::vec2 gap = cursor - nearest;

				if (std::sqrt(gap.x * gap.x + gap.y * gap.y) <= GRAB_PIXELS) {
					hovered = axis;
				}
			}
		}

		// --- the handles -------------------------------------------------------

		for (int axis = 0; axis < 3; axis++) {
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

			if (!originVisible || !tipVisible[axis]) {
				continue;
			}

			list->AddLine(
				ImVec2(origin.x, origin.y),
				ImVec2(tips[axis].x, tips[axis].y),
				colour,
				lit ? 4.0f : 2.5f
			);

			// **A cube for scale, a dot for move**, so the two modes are told
			// apart by their shape rather than only by which button is lit.
			if (mode == ToolMode::Scale) {
				const float half = lit ? 5.5f : 4.0f;
				list->AddRectFilled(
					ImVec2(tips[axis].x - half, tips[axis].y - half),
					ImVec2(tips[axis].x + half, tips[axis].y + half),
					colour
				);
			} else {
				list->AddCircleFilled(ImVec2(tips[axis].x, tips[axis].y), lit ? 5.5f : 4.0f, colour);
			}
		}

		// --- the drag ---------------------------------------------------------

		const Ray ray = panel.PanelToRay(cursor);

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
				Dragging.Viewport = viewport;
				Dragging.Grabbed = grabbed;
				Dragging.GrabbedPoint = grabbedPoint;
				Dragging.Mode = mode;

				Universe->Enter(world, [&](Store &store) {
					for (const Entity instance : Selection) {
						if (!store.Alive(instance)) {
							continue;
						}
						if (const auto *transform = store.Get<engine::scene::Transform>(instance)) {
							Dragging.Instances.push_back(instance);
							Dragging.Before.push_back(transform->Frame);

							const auto *bounds = store.Get<engine::scene::Bounds>(instance);
							Dragging.BeforeSize.push_back(
								bounds != nullptr ? bounds->HalfExtent : Vector3{}
							);
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
					if (IntersectRayPlane(centre, axis, ray, now)) {
						// The signed angle between where the drag began on this
						// plane and where the cursor is now, measured about the
						// axis. `atan2` of the cross onto the axis against the
						// dot is the branch-free version, and it is the one that
						// keeps working past a quarter turn.
						const Vector3 from = (Dragging.GrabbedPoint - centre).Unit();
						const Vector3 to = (now - centre).Unit();

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
					if (ClosestPointOnAxis(centre, axis, ray, now)) {
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
								// — which is the composition, written out,
								// rather than a position and an orientation
								// computed separately and hoped to agree.
								const glm::quat spin =
									glm::angleAxis(radians, glm::vec3(axis.X, axis.Y, axis.Z));

								const CFrame turn = CFrame(centre) * CFrame(Vector3{}, spin) *
													CFrame(centre).Inverse();

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

								Vector3 grown = was;
								const float half = delta * 0.5f;
								if (Dragging.Axis == 0) {
									grown.X = std::max(was.X + half, 0.01f);
								} else if (Dragging.Axis == 1) {
									grown.Y = std::max(was.Y + half, 0.01f);
								} else {
									grown.Z = std::max(was.Z + half, 0.01f);
								}

								store.Set<engine::scene::Bounds>(
									instance, engine::scene::Bounds{grown}
								);
								break;
							}

							case ToolMode::Select:
								break;
							}

							if (Dragging.Mode != ToolMode::Scale) {
								store.Set<engine::scene::Transform>(instance, moved);
							}
						}
					});
				}
			} else {
				// Released. One command for the whole drag, and only when it
				// actually moved something — see `GizmoDrag`.
				if (Dragging.Moved && Commands != nullptr) {
					// **Which property the command names has to match what the
					// drag changed.** A scale that recorded a `CFrame` would
					// undo by restoring a position nothing had moved, and leave
					// the size it actually changed exactly where it was — an
					// undo that reports success and reverses nothing.
					const bool scaling = Dragging.Mode == ToolMode::Scale;
					const engine::core::Name property(scaling ? "Size" : "CFrame");
					const char *what = scaling	? "Resize"
										: Dragging.Mode == ToolMode::Rotate ? "Rotate"
																			: "Move";

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
								// `scene::Part` already makes — recording the
								// half under the full name would halve the part
								// on undo.
								before.Type = engine::ecs::PropertyType::Vector3;
								before.Vector3 = Dragging.BeforeSize[index] * 2.0f;

								after.Type = engine::ecs::PropertyType::Vector3;
								after.Vector3 = bounds->HalfExtent * 2.0f;
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
						}
					});

					MarkModified();
				}

				Dragging = GizmoDrag{};
			}
		}

		// A click on a handle is not a click on the world behind it.
		return hovered >= 0 || Dragging.Axis >= 0;
	}

	void Editor::PickInViewport(size_t viewport, float x, float y, bool add) {
		const WorldId shown = ViewportWorld(viewport);
		if (!shown.IsValid() || Universe == nullptr) {
			return;
		}

		const PanelProjection panel = ProjectionFor(viewport);
		if (!panel.IsValid() || !panel.ContainsPanel(glm::vec2(x, y))) {
			return;
		}

		const Ray ray = panel.PanelToRay(glm::vec2(x, y));

		// Built from what is drawable now. See the declaration for why this is
		// not the physics broadphase.
		std::vector<engine::spatial::Proxy> proxies;

		Universe->Enter(shown, [&](Store &store) {
			store.Each<engine::scene::Transform, engine::scene::Bounds>(
				[&](Entity entity,
					const engine::scene::Transform &transform,
					const engine::scene::Bounds &bounds) {
					engine::spatial::Proxy proxy;
					proxy.Id = entity.Id;
					proxy.Bounds = AABB::FromOrientedBox(transform.Frame, bounds.HalfExtent);
					proxy.Layers = engine::spatial::LayerMask::All();
					proxies.push_back(proxy);
				}
			);
		});

		if (proxies.empty()) {
			return;
		}

		engine::spatial::HashGrid grid;
		grid.Rebuild(proxies);

		const std::optional<engine::core::RayHit> hit =
			engine::spatial::Raycast(grid, ray, PICK_REACH);

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

		Select(shown, Entity(hit->Id), add);
	}
}
