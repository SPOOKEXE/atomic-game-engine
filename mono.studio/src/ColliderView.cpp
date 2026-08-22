// Drawing what a part actually collides as.
//
// **The one thing a collider has that nothing on screen shows.** A part is drawn
// at its `scene::Bounds` and collides at its `scene::Collider`, and those are
// the same number for a `MakePart` box and nothing keeps them so - a mesh part
// whose hull was baked from the wrong model, a rock whose collider is still the
// crate it was copied from, a terrain chunk whose triangles do not reach the
// edge of the tile. Every one of those reads as "the physics is wrong" and every
// one is visible in a second with an outline on it.
//
// **An overlay rather than a render pass**, which is `Editor::DrawGizmo`'s
// arrangement and is what makes this cheap: the studio already projects world
// points into a panel and draws lines into an `ImDrawList`, so a collider
// outline is a list of segments and no pipeline, no shader and no target.
//
// **A colour per face rather than one colour per part**, which is what the
// shapes need to be readable: a hull drawn in one colour is a tangle of lines
// with no way to see which edges belong together, and a triangle mesh is worse.
// The colour is a function of the face's own index, so it is the same colour
// every frame - a hash that changed per frame would be a shape that flickers.

#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Viewports.hpp>

namespace studio {

	namespace {
		using engine::core::CFrame;
		using engine::core::Vector3;

		// How far from the eye a collider is still outlined, in metres.
		//
		// **A reach and not a count**, because what makes this unusable is a
		// world of ten thousand parts each contributing twelve segments to an
		// `ImDrawList` - sixty thousand lines a frame at four viewports. A reach
		// bounds it by what the author can actually see and is the same shape of
		// answer `Editor::PICK_REACH` gives for the same reason.
		constexpr float OUTLINE_REACH = 120.0f;

		// How many triangles of one mesh collider are outlined.
		//
		// A terrain chunk is thousands of triangles and an outline of all of
		// them is a solid block of colour that says nothing. The nearest few
		// hundred is a picture of the surface under the part being looked at,
		// which is the question this view is asked.
		constexpr size_t OUTLINE_TRIANGLES = 512;

		// How many segments one frame may draw, across every viewport.
		//
		// The backstop for a world that defeats the reach - a hundred mesh
		// colliders in one room. Past it the view stops adding rather than the
		// frame stopping.
		constexpr size_t OUTLINE_SEGMENTS = 60000;

		// A stable colour for one face of one part.
		//
		// **A hash of the two indices, not a counter and not a random draw.** A
		// counter gives adjacent faces adjacent hues, which is exactly the case
		// the colours exist to separate; a random draw gives a shape that
		// flickers. The mix below is the usual integer scramble, and it is the
		// same colour for the same face on every frame of every session.
		ImU32 ColourOf(uint32_t part, uint32_t face) {
			uint32_t mixed = part * 2654435761u + face * 40503u;
			mixed ^= mixed >> 15;
			mixed *= 2246822519u;
			mixed ^= mixed >> 13;

			// **Full saturation and a fixed lightness**, so every face reads as
			// its own colour against a lit scene. Drawn from a hue wheel rather
			// than from three independent bytes, which produces greys and browns
			// about a third of the time.
			const float hue = static_cast<float>(mixed & 0xFFFFu) / 65535.0f;
			float red = 0.0f;
			float green = 0.0f;
			float blue = 0.0f;
			ImGui::ColorConvertHSVtoRGB(hue, 0.75f, 1.0f, red, green, blue);

			return IM_COL32(
				static_cast<int>(red * 255.0f),
				static_cast<int>(green * 255.0f),
				static_cast<int>(blue * 255.0f),
				220
			);
		}

		// The eight corners of an oriented box, in the order the edge table
		// below indexes them: bit 0 is X, bit 1 is Y, bit 2 is Z.
		void BoxCorners(const CFrame &frame, const Vector3 &half, Vector3 (&out)[8]) {
			for (int index = 0; index < 8; index++) {
				const Vector3 local{
					(index & 1) != 0 ? half.X : -half.X,
					(index & 2) != 0 ? half.Y : -half.Y,
					(index & 4) != 0 ? half.Z : -half.Z,
				};
				out[index] = frame.Position + frame.VectorToWorldSpace(local);
			}
		}

		// The twelve edges of a box, as corner index pairs, grouped so that the
		// four edges of each face share a colour - which is what makes the box
		// read as six quads rather than as a wire cage.
		constexpr int BOX_FACES[6][4] = {
			{0, 2, 6, 4}, // -X
			{1, 5, 7, 3}, // +X
			{0, 4, 5, 1}, // -Y
			{2, 3, 7, 6}, // +Y
			{0, 1, 3, 2}, // -Z
			{4, 6, 7, 5}, // +Z
		};
	}

	void Editor::DrawColliderOutlines(size_t viewport, const PanelProjection &panel) {
		if (!ShowColliders) {
			return;
		}

		const WorldId world = ViewportWorld(viewport);
		OverlaySlot &slot = Overlays[viewport];
		ImDrawList *list = slot.List;
		if (list == nullptr || Universe == nullptr) {
			return;
		}

		size_t drawn = 0;

		const auto segment = [&](const Vector3 &from, const Vector3 &to, ImU32 colour) {
			if (drawn >= OUTLINE_SEGMENTS) {
				return;
			}
			glm::vec2 a{};
			glm::vec2 b{};
			if (panel.ProjectSegment(from, to, a, b)) {
				list->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), colour, 1.0f);
			}
			drawn++;
		};

		// A face, filled in the same colour its edges are drawn in.
		//
		// **Filled *and* outlined rather than one or the other.** A fill is how
		// a shape reads as a solid - a hull of forty faces is a tangle of lines
		// and an unmistakable object - and the outline is how two overlapping
		// shapes stay separable once the fills are on top of each other. The
		// fill is a third of the alpha the edges get, for the same reason.
		//
		// **Projected corner by corner and refused if any of them is behind the
		// eye.** `ProjectSegment` clips a line and there is no such thing as
		// clipping a polygon into an `ImDrawList` without triangulating it, so a
		// face that crosses the near plane is outlined and not filled - which is
		// the honest half of the picture rather than a wrong one.
		const auto face = [&](const Vector3 *corners, size_t count, ImU32 colour) {
			// The same budget the segments answer to. A fill is a triangle fan
			// in the same draw list, and a hundred mesh colliders would put
			// fifty thousand of them in it - which is the case
			// `OUTLINE_SEGMENTS` exists to bound.
			if (!ColliderFill || drawn >= OUTLINE_SEGMENTS || count < 3 || count > 8) {
				return;
			}

			ImVec2 points[8];
			for (size_t at = 0; at < count; at++) {
				glm::vec2 first{};
				glm::vec2 second{};
				if (!panel.ProjectSegment(corners[at], corners[(at + 1) % count], first, second)) {
					return;
				}
				points[at] = ImVec2(first.x, first.y);
			}

			list->AddConvexPolyFilled(points, static_cast<int>(count), (colour & 0x00FFFFFFu) | 0x30000000u);
		};

		Universe->Enter(world, [&](engine::ecs::Store &store) {
			// Resolved once for the walk, not per part - the same decision
			// `physics::NarrowPhase` makes about the same table.
			const engine::scene::CollisionShapes *shapes = engine::scene::CollisionShapesOf(store);

			store.Each<
				const engine::scene::Transform,
				const engine::scene::Collider>([&](engine::ecs::Entity part,
												   const engine::scene::Transform &placement,
												   const engine::scene::Collider &collider) {
				if (drawn >= OUTLINE_SEGMENTS) {
					return;
				}
				if ((placement.Frame.Position - panel.Eye).Magnitude() > OUTLINE_REACH) {
					return;
				}

				const auto tint = static_cast<uint32_t>(part.Id);
				const CFrame &frame = placement.Frame;

				const engine::collision::TriangleMesh *soup =
					shapes != nullptr ? shapes->FindMesh(collider.Geometry) : nullptr;
				const engine::collision::ConvexHull *hull =
					shapes != nullptr ? shapes->FindHull(collider.Geometry) : nullptr;

				// **Which of the three this part has, rather than only the
				// one in force.** A `MeshPart` carries a bound and its
				// geometry resolves to both a hull and a soup, and the
				// question "why does this collide like that" is usually
				// answered by seeing the two that are not selected. See
				// `Editor::ColliderShapeView`.
				//
				// A mode with nothing baked to show falls back to what the
				// collider actually is rather than to an empty screen: the
				// absence *is* the answer for a part somebody expected a
				// hull on.
				engine::scene::ShapeKind drawing = collider.Shape;
				switch (ColliderShapes) {
				case ColliderShapeView::Chosen:
					break;
				case ColliderShapeView::Precise:
					drawing = soup != nullptr ? engine::scene::ShapeKind::Mesh : collider.Shape;
					break;
				case ColliderShapeView::Hull:
					drawing = hull != nullptr ? engine::scene::ShapeKind::Hull : collider.Shape;
					break;
				case ColliderShapeView::Bounds:
					// The box a baked shape falls back to when its name does
					// not resolve - `physics::ShapeInstance` demotes to
					// exactly this - so it is the one shape every part has.
					drawing = engine::scene::ShapeKind::Box;
					break;
				}

				// The bound, in the warning colour, for a baked kind with
				// nothing baked. A name that resolves to nothing collides as
				// the part's extent - `scene::Collider::Geometry` states it -
				// and drawing nothing here would hide exactly the case an
				// author most needs to see.
				const auto unresolved = [&] {
					Vector3 corners[8];
					BoxCorners(frame, collider.Extent, corners);
					const ImU32 colour = IM_COL32(230, 90, 90, 200);
					for (const auto &loop : BOX_FACES) {
						const Vector3 quad[4]{
							corners[loop[0]], corners[loop[1]], corners[loop[2]], corners[loop[3]]
						};
						face(quad, 4, colour);
						for (int edge = 0; edge < 4; edge++) {
							segment(quad[edge], quad[(edge + 1) % 4], colour);
						}
					}
				};

				switch (drawing) {
				case engine::scene::ShapeKind::Box: {
					Vector3 corners[8];
					BoxCorners(frame, collider.Extent, corners);
					for (int at = 0; at < 6; at++) {
						const ImU32 colour = ColourOf(tint, static_cast<uint32_t>(at));
						const Vector3 quad[4]{
							corners[BOX_FACES[at][0]],
							corners[BOX_FACES[at][1]],
							corners[BOX_FACES[at][2]],
							corners[BOX_FACES[at][3]],
						};
						face(quad, 4, colour);
						for (int edge = 0; edge < 4; edge++) {
							segment(quad[edge], quad[(edge + 1) % 4], colour);
						}
					}
					break;
				}

				case engine::scene::ShapeKind::Sphere: {
					// **Three great circles, which is what a sphere is
					// legible as.** A wireframe globe of latitude and
					// longitude is far more lines for no more information:
					// the question this view answers is where the surface is
					// and how big it is, and three rings answer it.
					const float radius = collider.Extent.X;
					constexpr int STEPS = 24;
					for (int ring = 0; ring < 3; ring++) {
						const ImU32 colour = ColourOf(tint, static_cast<uint32_t>(ring));
						for (int step = 0; step < STEPS; step++) {
							const auto angle = [](int at) {
								return static_cast<float>(at) * 6.2831853f / static_cast<float>(STEPS);
							};
							const float from = angle(step);
							const float to = angle(step + 1);

							const auto on = [&](float at) {
								const float x = std::cos(at) * radius;
								const float y = std::sin(at) * radius;
								const Vector3 local = ring == 0	  ? Vector3{x, y, 0.0f}
													  : ring == 1 ? Vector3{x, 0.0f, y}
																  : Vector3{0.0f, x, y};
								return frame.Position + frame.VectorToWorldSpace(local);
							};
							segment(on(from), on(to), colour);
						}
					}
					break;
				}

				case engine::scene::ShapeKind::Cylinder: {
					// Two caps and four barrel lines, which is what says
					// which way the barrel points - the thing a cylinder
					// collider is most often wrong about.
					const float radius = collider.Extent.X;
					const float half = collider.Extent.Y;
					constexpr int STEPS = 24;

					for (int cap = 0; cap < 2; cap++) {
						const ImU32 colour = ColourOf(tint, static_cast<uint32_t>(cap));
						const float height = cap == 0 ? -half : half;
						for (int step = 0; step < STEPS; step++) {
							const auto on = [&](int at) {
								const float angle =
									static_cast<float>(at) * 6.2831853f / static_cast<float>(STEPS);
								const Vector3 local{
									std::cos(angle) * radius, height, std::sin(angle) * radius
								};
								return frame.Position + frame.VectorToWorldSpace(local);
							};
							segment(on(step), on(step + 1), colour);
						}
					}

					const ImU32 barrel = ColourOf(tint, 2);
					for (int side = 0; side < 4; side++) {
						const float angle = static_cast<float>(side) * 6.2831853f / 4.0f;
						const Vector3 across{std::cos(angle) * radius, 0.0f, std::sin(angle) * radius};
						const Vector3 low{across.X, -half, across.Z};
						const Vector3 high{across.X, half, across.Z};
						segment(
							frame.Position + frame.VectorToWorldSpace(low),
							frame.Position + frame.VectorToWorldSpace(high),
							barrel
						);
					}
					break;
				}

				case engine::scene::ShapeKind::Hull: {
					if (hull == nullptr) {
						unresolved();
						break;
					}

					for (size_t at = 0; at < hull->Faces.size(); at++) {
						const engine::collision::HullFace &loop = hull->Faces[at];
						const ImU32 colour = ColourOf(tint, static_cast<uint32_t>(at));

						Vector3 polygon[8];
						const size_t corners = std::min<size_t>(loop.IndexCount, 8);
						for (size_t corner = 0; corner < corners; corner++) {
							polygon[corner] =
								frame.Position +
								frame.VectorToWorldSpace(hull->Points[hull->Loops[loop.FirstIndex + corner]]);
						}
						if (corners == loop.IndexCount) {
							face(polygon, corners, colour);
						}

						for (uint32_t corner = 0; corner < loop.IndexCount; corner++) {
							const Vector3 &from = hull->Points[hull->Loops[loop.FirstIndex + corner]];
							const Vector3 &to =
								hull->Points[hull->Loops[loop.FirstIndex + (corner + 1) % loop.IndexCount]];
							segment(
								frame.Position + frame.VectorToWorldSpace(from),
								frame.Position + frame.VectorToWorldSpace(to),
								colour
							);
						}
					}

					// **A hull with no faces still shows its points**, which
					// is the flat, straight and single-point output
					// `collision::BuildConvexHull` produces deliberately.
					// Drawing nothing for one would read as a collider that
					// is not there.
					if (!hull->Solid()) {
						const ImU32 colour = ColourOf(tint, 0);
						for (size_t at = 0; at + 1 < hull->Points.size(); at++) {
							segment(
								frame.Position + frame.VectorToWorldSpace(hull->Points[at]),
								frame.Position + frame.VectorToWorldSpace(hull->Points[at + 1]),
								colour
							);
						}
					}
					break;
				}

				case engine::scene::ShapeKind::Mesh: {
					if (soup == nullptr) {
						unresolved();
						break;
					}

					const size_t triangles = std::min(soup->TriangleCount(), OUTLINE_TRIANGLES);
					for (size_t triangle = 0; triangle < triangles; triangle++) {
						const engine::collision::Triangle corners = soup->TriangleAt(triangle);
						const ImU32 colour = ColourOf(tint, static_cast<uint32_t>(triangle));

						const Vector3 points[3]{
							frame.Position + frame.VectorToWorldSpace(corners.A),
							frame.Position + frame.VectorToWorldSpace(corners.B),
							frame.Position + frame.VectorToWorldSpace(corners.C),
						};
						face(points, 3, colour);
						segment(points[0], points[1], colour);
						segment(points[1], points[2], colour);
						segment(points[2], points[0], colour);
					}
					break;
				}
				}
			});
		});
	}
}
