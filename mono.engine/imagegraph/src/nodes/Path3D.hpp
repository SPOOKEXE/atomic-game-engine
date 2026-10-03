#pragma once
#include "../SourceMeshTransform.hpp"
#include "../ValuePayload.hpp"
#include "Path.hpp"
namespace engine::imagegraph::detail {
	struct PathPoint3D {
		Vector3 Position{};
		double Weight = 1;
	};
	class PathRuntime3D {
		const PathData3D &Data;
		AllocationReservation ChildCharge;
		std::vector<PathRuntime3D> Children;
		std::array<double, Limits::MaximumPathAnchors> Lengths{};
		size_t SegmentCount = 0;
		double Total = 0;
		std::optional<PathRuntime> Runtime2D;
		bool Ready = true;
		std::optional<Vector4> SourceBounds;
		void AddSourceBounds(const Vector3 &p) {
			if (!SourceBounds)
				SourceBounds = Vector4{p.X, p.Y, p.X, p.Y};
			else {
				SourceBounds->X = std::min(SourceBounds->X, p.X);
				SourceBounds->Y = std::min(SourceBounds->Y, p.Y);
				SourceBounds->Z = std::max(SourceBounds->Z, p.X);
				SourceBounds->W = std::max(SourceBounds->W, p.Y);
			}
		}
		PathPoint3D Apply(PathPoint3D point) const {
			for (const auto &transform : Data.Transforms) {
				if (transform.Projective) {
					auto multiply = [](const std::array<double, 16> &matrix,
									   const std::array<double, 4> &value) {
						std::array<double, 4> result{};
						for (size_t row = 0; row < 4; ++row)
							for (size_t column = 0; column < 4; ++column)
								result[row] += matrix[column * 4 + row] * value[column];
						return result;
					};
					const auto view = multiply(
						transform.CameraView, {point.Position.X, point.Position.Y, point.Position.Z, 1}
					);
					const auto clip = multiply(transform.CameraProjection, view);
					point.Position = {
						(1 + clip[0] / clip[3]) * transform.ProjectionScale.X,
						(1 + clip[1] / clip[3]) * transform.ProjectionScale.Y,
						0
					};
					if (transform.DepthWeight) point.Weight = clip[2] / clip[3];
					continue;
				}
				const auto &p = point.Position;
				const Vector3 rotated = SourceRotate(
					transform.Rotation,
					{p.X - transform.Anchor.X, p.Y - transform.Anchor.Y, p.Z - transform.Anchor.Z}
				);
				point.Position = {
					transform.Anchor.X + rotated.X * transform.Scale.X + transform.Position.X,
					transform.Anchor.Y + rotated.Y * transform.Scale.Y + transform.Position.Y,
					transform.Anchor.Z + rotated.Z * transform.Scale.Z + transform.Position.Z
				};
			}
			return point;
		}
		const PathRuntime3D *Child(size_t &line) const {
			if (Children.empty()) return nullptr;
			if (Data.SourceOperation->Kind != SourcePathOperationKind::Combine) return &Children.front();
			for (const auto &child : Children) {
				const size_t count = child.LineCount();
				if (line < count) return &child;
				line -= count;
			}
			return nullptr;
		}
		Vector3 Segment(size_t index, double ratio) const {
			const auto &a = Data.Anchors[index].Controls;
			const auto &b = Data.Anchors[(index + 1) % Data.Anchors.size()].Controls;
			Vector3 point;
			const bool linear = a[6] == 0 && a[7] == 0 && a[8] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0;
			double *coordinates[]{&point.X, &point.Y, &point.Z};
			for (size_t axis = 0; axis < 3; ++axis)
				*coordinates[axis] =
					linear ? a[axis] + (b[axis] - a[axis]) * ratio
						   : BezierComponent(
								 ratio, a[axis], b[axis], a[axis] + a[axis + 6], b[axis] + b[axis + 3]
							 );
			return point;
		}

	  public:
		explicit PathRuntime3D(const PathData3D &data, NodeContext *context = nullptr) : Data(data) {
			if (!data.SourcePresent) return;
			if (data.SourceOperation) {
				if (!context) {
					Ready = false;
					return;
				}
				const auto &op = *data.SourceOperation;
				auto charge = context->ReserveWorkspace(op.Inputs.size() * sizeof(PathRuntime3D), "path");
				if (!charge) {
					Ready = false;
					return;
				}
				ChildCharge = std::move(*charge);
				Children.reserve(op.Inputs.size());
				for (const auto &child : op.Inputs) {
					if (!child.Data) {
						Ready = false;
						return;
					}
					Children.emplace_back(*child.Data, context);
					if (!Children.back().Valid()) {
						Ready = false;
						return;
					}
				}
				Total = Length();
				return;
			}
			if (data.Source2D) {
				if (!context) {
					Ready = false;
					return;
				}
				Runtime2D.emplace();
				Ready = Runtime2D->Init(*context, *data.Source2D);
				if (Ready) Total = Runtime2D->LengthTotal;
				return;
			}
			if (data.SourcePolyline) {
				if (data.Anchors.size() > Limits::MaximumPathAnchors) {
					Ready = false;
					return;
				}
				if (data.SourceEmptyCache) {
					Total = data.SourceEmptyCache->Length;
					SegmentCount = data.SourceEmptyCache->SegmentCount;
					return;
				}
				SegmentCount = data.Anchors.size();
				for (size_t i = 1; i < SegmentCount; ++i) {
					const auto &a = data.Anchors[i - 1].Controls, &b = data.Anchors[i].Controls;
					const double dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
					Lengths[i - 1] = std::sqrt(dx * dx + dy * dy + dz * dz);
					Total += Lengths[i - 1];
				}
				return;
			}
			if (data.Anchors.size() < 2 || data.Anchors.size() > Limits::MaximumPathAnchors ||
				!data.Resolution || data.Resolution > Limits::MaximumArrayElements)
				return;
			SegmentCount = data.Loop ? data.Anchors.size() : data.Anchors.size() - 1;
			for (size_t i = 0; i < SegmentCount; ++i) {
				Vector3 previous{};
				double length = 0;
				for (uint32_t sample = 0; sample <= data.Resolution; ++sample) {
					const Vector3 point = Segment(i, double(sample) / data.Resolution);
					AddSourceBounds(point);
					if (sample) length += std::hypot(point.X - previous.X, point.Y - previous.Y, point.Z);
					// Source updateLength never updates its
					// previous Z coordinate.
					previous.X = point.X;
					previous.Y = point.Y;
				}
				Lengths[i] = length;
				Total += length;
			}
		}
		bool Valid() const {
			return Ready && std::isfinite(Total);
		}
		size_t LineCount() const {
			if (Data.SourceOperation) {
				if (Data.SourceOperation->Kind != SourcePathOperationKind::Combine)
					return Children.empty() ? 1 : Children.front().LineCount();
				size_t count = 0;
				for (const auto &child : Children)
					count += child.LineCount();
				return count;
			}
			return Runtime2D ? Runtime2D->LineCount() : 1;
		}
		double Length(size_t line = 0) const {
			if (Data.SourceOperation) {
				const auto *child = Child(line);
				return child
						   ? child->Length(line) *
								 (Data.SourceOperation->Kind == SourcePathOperationKind::Trim
									  ? Data.SourceOperation->TrimRange.Y - Data.SourceOperation->TrimRange.X
									  : 1)
						   : 0;
			}
			return Runtime2D ? Runtime2D->Length(line) : Total;
		}
		size_t SourceSegmentCount(size_t line = 0) const {
			if (Data.SourceOperation) {
				const auto *child = Child(line);
				return child ? child->SourceSegmentCount(line) : 0;
			}
			return Runtime2D ? Runtime2D->SegmentCount(line) : SegmentCount;
		}
		size_t SourceAccumulatedCount(size_t line = 0) const {
			if (Data.SourceOperation) {
				const auto *child = Child(line);
				return child ? child->SourceAccumulatedCount(line) : 0;
			}
			return Runtime2D ? Runtime2D->AccumulatedCount(line) : SegmentCount;
		}
		double SourceAccumulatedAt(size_t index, size_t line = 0) const {
			if (Data.SourceOperation) {
				const auto *child = Child(line);
				if (!child) return 0;
				if (Data.SourceOperation->Kind == SourcePathOperationKind::Reverse) {
					const size_t count = child->SourceAccumulatedCount(line);
					return index < count ? child->SourceAccumulatedAt(count - index - 1, line) : 0;
				}
				return child->SourceAccumulatedAt(index, line);
			}
			if (Runtime2D) return Runtime2D->AccumulatedAt(index, line);
			if (Data.SourceEmptyCache)
				return index < Data.SourceEmptyCache->Accumulated.size()
						   ? Data.SourceEmptyCache->Accumulated[index]
						   : NAN;
			if (index >= SegmentCount) return 0;
			double length = 0;
			for (size_t i = 0; i <= index; ++i)
				length += Lengths[i];
			return length;
		}
		std::optional<Vector4> SourceBoundary(size_t line = 0) const {
			if (!Data.SourcePresent) return Vector4{0, 0, 1, 1};
			std::optional<Vector4> bounds;
			if (Data.SourceOperation) {
				const auto *child = Child(line);
				const auto kind = Data.SourceOperation->Kind;
				bounds = child ? child->SourceBoundary(line)
						 : kind == SourcePathOperationKind::Reverse || kind == SourcePathOperationKind::Trim
							 ? Vector4{0, 0, 1, 1}
							 : Vector4{-4, -4, -4, -4};
			} else if (Runtime2D) {
				bounds = Runtime2D->HasBoundary
							 ? Vector4{Runtime2D->MinX, Runtime2D->MinY, Runtime2D->MaxX, Runtime2D->MaxY}
							 : Vector4{-4, -4, -4, -4};
			} else if (Data.SourcePolyline)
				bounds = Data.SourceBounds2D;
			else
				bounds = SourceBounds.value_or(Vector4{-4, -4, -4, -4});
			if (!bounds) return std::nullopt;
			for (const auto &t : Data.Transforms) {
				// Source 3D Transform's bbox ignores quaternion rotation;
				// camera leaves bbox unchanged.
				if (t.Projective) continue;
				const double x0 = t.Anchor.X + (bounds->X - t.Anchor.X) * t.Scale.X + t.Position.X,
							 y0 = t.Anchor.Y + (bounds->Y - t.Anchor.Y) * t.Scale.Y + t.Position.Y,
							 x1 = t.Anchor.X + (bounds->Z - t.Anchor.X) * t.Scale.X + t.Position.X,
							 y1 = t.Anchor.Y + (bounds->W - t.Anchor.Y) * t.Scale.Y + t.Position.Y;
				bounds = Vector4{std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
			}
			return bounds;
		}
		bool SpatialLine(size_t line = 0) const {
			if (!Data.Transforms.empty()) return !Data.Transforms.back().Projective;
			if (Data.SourceOperation) {
				const auto *child = Child(line);
				return child && child->SpatialLine(line);
			}
			return !Data.Source2D;
		}
		PathPoint3D Ratio(double ratio, size_t line = 0) const {
			if (Data.SourceOperation) {
				const auto &op = *Data.SourceOperation;
				const auto *child = Child(line);
				if (op.Kind == SourcePathOperationKind::Reverse)
					ratio = 1 - ratio;
				else if (op.Kind == SourcePathOperationKind::Trim)
					ratio = op.TrimRange.X + (op.TrimRange.Y - op.TrimRange.X) * ratio;
				return Apply(child ? child->Ratio(ratio, line) : PathPoint3D{});
			}
			if (Runtime2D) {
				const auto point = Runtime2D->PointRatio(ratio, line);
				return Apply({{point.X, point.Y, 0}, point.Weight});
			}
			if (Data.SourceEmptyCache) return {{NAN, NAN, NAN}, 1};
			if (!SegmentCount) return Data.SourcePresent ? Apply({}) : PathPoint3D{};
			if (Data.SourcePolyline) {
				const double fraction = ratio - std::trunc(ratio);
				double remaining = Total == 0 ? 0 : std::fmod(fraction * Total, Total);
				for (size_t i = 0; i < SegmentCount; ++i) {
					if (remaining > Lengths[i]) {
						remaining -= Lengths[i];
						continue;
					}
					return Apply({Segment(i, remaining / Lengths[i]), 1});
				}
				return Apply({});
			}
			if (ratio < 0) ratio = 1 + std::fmod(ratio, 1);
			const double distance = (Data.Loop ? std::fmod(ratio, 1) : std::clamp(ratio, 0.0, .99)) * Total;
			double remaining = distance;
			for (size_t i = 0; i < SegmentCount; ++i) {
				if (remaining > Lengths[i]) {
					remaining -= Lengths[i];
					continue;
				}
				return Apply({Segment(i, remaining / Lengths[i]), 1});
			}
			return {};
		}
		PathPoint3D BySegment(double ratio) const {
			if (Data.SourcePolyline || Data.SourceOperation || !Data.Transforms.empty() ||
				(Data.Source2D && Data.Source2D->SourceOperation))
				return {{NAN, NAN, NAN}, 1};
			if (Runtime2D) {
				const auto point = Runtime2D->PointSegment(ratio);
				return {{point.X, point.Y, 0}, point.Weight};
			}
			if (!SegmentCount) return Data.SourcePresent ? Apply({}) : PathPoint3D{};
			if (ratio < 0) {
				const auto &a = Data.Anchors.front().Controls;
				return {{a[0], a[1], a[2]}, 1};
			}
			ratio = std::fmod(ratio, double(Data.Anchors.size()));
			return {Segment(size_t(std::floor(ratio)), ratio - std::floor(ratio)), 1};
		}
	};
} // namespace engine::imagegraph::detail
