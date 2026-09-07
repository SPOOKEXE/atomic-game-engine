#include "SurfaceCapturePlan.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {
	namespace {
		bool Finite(const glm::mat4 &matrix) {
			for (int column = 0; column < 4; ++column) {
				for (int row = 0; row < 4; ++row) {
					if (!std::isfinite(matrix[column][row])) {
						return false;
					}
				}
			}
			return true;
		}
		struct Source {
			uint16_t Index = NO_SURFACE_CAPTURE;
			SurfaceCaptureKind Kind = SurfaceCaptureKind::Mirror;
		};
	}

	bool OrderCaptureTransparency(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> packed,
		uint32_t first,
		uint32_t count,
		const core::Vector3 &eye,
		std::vector<CaptureBlendSlot> &output
	) {
		ENGINE_PROFILE("order capture transparency");
		output.clear();
		if (uint64_t(first) + count > packed.size()) {
			return false;
		}
		for (uint32_t offset = 0; offset < count; ++offset) {
			const uint32_t slot = first + offset;
			const uint32_t source = packed[slot];
			if (source >= instances.size()) {
				output.clear();
				return false;
			}
			const float distance = (instances[source].Frame.Position - eye).MagnitudeSquared();
			if (!std::isfinite(distance)) {
				output.clear();
				return false;
			}
			output.push_back({slot, source, distance});
		}
		std::sort(output.begin(), output.end(), [](const auto &left, const auto &right) {
			if (left.DistanceSquared != right.DistanceSquared) {
				return left.DistanceSquared > right.DistanceSquared;
			}
			if (left.Source != right.Source) {
				return left.Source < right.Source;
			}
			return left.Slot < right.Slot;
		});
		return true;
	}

	SurfaceCaptureStatus PlanSurfaceCaptures(const SurfaceCaptureRequest &request, SurfaceCapturePlan &plan) {
		ENGINE_PROFILE("plan surface captures");
		plan.Entries.clear();
		plan.Postorder.clear();
		plan.Roots.fill(NO_SURFACE_CAPTURE);
		plan.Pixels = 0;
		if (request.Width == 0 || request.Height == 0 || request.Depth > 32 || !Finite(request.Projection) ||
			!Finite(request.Frame.ToMatrix())) {
			return SurfaceCaptureStatus::Invalid;
		}
		std::array<Source, scene::MAX_SURFACES> sources;
		const auto put = [&](int16_t slot, size_t index, SurfaceCaptureKind kind) {
			if (slot < 0 || size_t(slot) >= sources.size() || index >= NO_SURFACE_CAPTURE ||
				sources[slot].Index != NO_SURFACE_CAPTURE) {
				return false;
			}
			sources[slot] = {uint16_t(index), kind};
			return true;
		};
		for (size_t index = 0; index < request.Mirrors.size(); ++index) {
			if (!put(request.Mirrors[index].Index, index, SurfaceCaptureKind::Mirror)) {
				return SurfaceCaptureStatus::Invalid;
			}
		}
		for (size_t index = 0; index < request.Portals.size(); ++index) {
			if (!put(request.Portals[index].Index, index, SurfaceCaptureKind::Portal)) {
				return SurfaceCaptureStatus::Invalid;
			}
		}
		SurfaceCaptureStatus status = SurfaceCaptureStatus::Ok;
		const auto visit = [&](auto &&self,
							   const core::CFrame &frame,
							   const glm::mat4 &projection,
							   uint32_t width,
							   uint32_t height,
							   uint32_t depth,
							   int16_t arrival,
							   uint16_t parent) -> void {
			const glm::mat4 viewProjection = projection * glm::inverse(frame.ToMatrix());
			for (size_t slot = 0; slot < sources.size() && status == SurfaceCaptureStatus::Ok; ++slot) {
				const Source source = sources[slot];
				if (source.Index == NO_SURFACE_CAPTURE || int16_t(slot) == arrival) {
					continue;
				}
				SurfaceCaptureEntry entry;
				entry.Children.fill(NO_SURFACE_CAPTURE);
				entry.Kind = source.Kind;
				entry.Source = source.Index;
				entry.Slot = int16_t(slot);
				entry.RootSlot = parent == NO_SURFACE_CAPTURE ? int16_t(slot) : plan.Entries[parent].RootSlot;
				entry.Depth = request.Depth - depth + 1;
				entry.Width = width;
				entry.Height = height;
				if (source.Kind == SurfaceCaptureKind::Portal) {
					const auto &portal = request.Portals[source.Index];
					if (portal.ExternalImage ||
						!graph::VisiblePane(viewProjection, portal.Centre, portal.First, portal.Second)) {
						continue;
					}
					entry.Frame = portal.Warp.Place(frame);
					const float side = (frame.Position - portal.Centre).Dot(portal.Normal);
					const auto outward = portal.Normal * (side >= 0 ? 1.0f : -1.0f);
					const auto normal = portal.Warp.Rotate(outward) * -1.0f;
					const auto point = portal.Warp.Point(portal.Centre) -
									   normal * scene::PortalClipBias(portal.Warp.Length(std::abs(side)));
					entry.Matrices = scene::ResolveSurfaceCamera(
						entry.Frame,
						scene::ObliqueProjection(projection, entry.Frame, normal, normal.Dot(point))
					);
					entry.Arrival = portal.Partner;
				} else {
					const auto &mirror = request.Mirrors[source.Index];
					if (!graph::VisiblePane(
							viewProjection, mirror.PaneCentre, mirror.PaneFirst, mirror.PaneSecond
						)) {
						continue;
					}
					scene::SurfacePane pane;
					pane.Centre = mirror.PaneCentre;
					pane.Normal = mirror.PaneNormal;
					pane.First = mirror.PaneFirst;
					pane.Second = mirror.PaneSecond;
					pane.NearPlane = mirror.PaneNear;
					pane.FarPlane = mirror.PaneFar;
					const auto reflected = scene::ReflectCamera(pane, frame, {});
					if (!reflected.Renders) {
						continue;
					}
					entry.Frame = reflected.Frame;
					entry.Matrices = scene::ResolveSurfaceCamera(
						entry.Frame, scene::SurfaceProjection(reflected.Lens, entry.Frame)
					);
					entry.Width = mirror.Width;
					entry.Height = mirror.Height;
					if (entry.Width > width || entry.Height > height) {
						if (uint64_t(entry.Width) * height > uint64_t(entry.Height) * width) {
							entry.Height =
								std::max(1u, uint32_t(uint64_t(entry.Height) * width / entry.Width));
							entry.Width = width;
						} else {
							entry.Width =
								std::max(1u, uint32_t(uint64_t(entry.Width) * height / entry.Height));
							entry.Height = height;
						}
					}
					entry.Arrival = int16_t(slot);
				}
				if (depth == 0) {
					if (parent == NO_SURFACE_CAPTURE) {
						status = SurfaceCaptureStatus::BudgetExceeded;
					}
					continue;
				}
				if (!Finite(entry.Matrices.ViewProjection) || entry.Width == 0 || entry.Height == 0) {
					status = SurfaceCaptureStatus::Invalid;
					break;
				}
				const uint64_t pixels = uint64_t(entry.Width) * entry.Height;
				if (plan.Entries.size() == MAX_SURFACE_CAPTURES ||
					pixels > request.PixelBudget - plan.Pixels) {
					status = SurfaceCaptureStatus::BudgetExceeded;
					break;
				}
				plan.Pixels += pixels;
				const uint16_t index = uint16_t(plan.Entries.size());
				plan.Entries.push_back(entry);
				if (parent == NO_SURFACE_CAPTURE) {
					plan.Roots[slot] = index;
				} else {
					plan.Entries[parent].Children[slot] = index;
				}
				self(
					self,
					entry.Frame,
					entry.Matrices.Projection,
					entry.Width,
					entry.Height,
					depth - 1,
					entry.Arrival,
					index
				);
				plan.Postorder.push_back(index);
			}
		};
		visit(
			visit,
			request.Frame,
			request.Projection,
			request.Width,
			request.Height,
			request.Depth,
			-1,
			NO_SURFACE_CAPTURE
		);
		if (status != SurfaceCaptureStatus::Ok) {
			plan.Entries.clear();
			plan.Postorder.clear();
			plan.Roots.fill(NO_SURFACE_CAPTURE);
			plan.Pixels = 0;
		}
		return status;
	}
}
