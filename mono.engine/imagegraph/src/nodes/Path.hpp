#pragma once

// Runtime path evaluation from node_path.gml _pathObject. Paths travel between nodes as Path2D values:
// each anchor is (x, y, in-handle x, in-handle y, out-handle x, out-handle y) with its mirror flag as Index,
// and weights are (position 0..100, weight) pairs. owned source wrappers preserve independent lines
// and delegate samples to their inputs instead of flattening operations into anchors.

#include "../NodeExecutors.hpp"
#include "../SourcePathPayload.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace engine::imagegraph::detail {
	// PREFERENCES.path_resolution default.
	inline constexpr int PATH_RESOLUTION = 32;

	struct PathPoint {
		double X = 0.0, Y = 0.0, Weight = 1.0;
	};

	inline double BezierComponent(double t, double p0, double p1, double c0, double c1) {
		return std::pow(1 - t, 3) * p0 + 3 * std::pow(1 - t, 2) * t * c0 + 3 * std::pow(t, 2) * (1 - t) * c1 +
			   std::pow(t, 3) * p1;
	}

	// GML x % y keeps the dividend's sign.
	inline double GmlMod(double value, double divisor) {
		return std::fmod(value, divisor);
	}

	class PathRuntime {
		// Storage is freed before its lease, including the old buffers displaced by Init.
		AllocationReservation StorageCharge;
		std::optional<SourcePathOperationKind> Operation;
		Vector2 TrimRange{0, 1};
		std::vector<PathRuntime> Inputs;
		const SourcePathData2D *SourceMesh = nullptr;
		const SourcePathShapeData2D *Shape = nullptr;
		NodeContext *EvaluationContext = nullptr;

	  public:
		PathRuntime() = default;

		static std::optional<uint64_t> StorageBytes(const Path2D &path) {
			if (path.Anchors.size() > Limits::MaximumPathAnchors ||
				path.Weights.size() > Limits::MaximumPathWeights)
				return std::nullopt;
			const uint64_t count = path.Anchors.size();
			const uint64_t segments = count < 2 ? 0 : (path.Loop ? count : count - 1);
			return count * sizeof(std::array<double, 6>) +
				   path.Weights.size() * sizeof(std::array<double, 2>) +
				   (segments * 2 + (count < 2 ? 0 : count + 1 + 101)) * sizeof(double);
		}

		// Admit the full replacement while old storage is still live; refusal preserves this runtime.
		bool Init(NodeContext &context, const Path2D &path) {
			const auto bytes = StorageBytes(path);
			if (!bytes)
				return context.Fail(Status::LimitExceeded, "path exceeds the anchor or weight limit", "path");
			if (!ValidSourcePath2D(path))
				return context.Fail(Status::InvalidValue, "source path operation is invalid", "path");
			if (path.SourceOperation) {
				const auto &operation = *path.SourceOperation;
				const uint64_t shapeSlots =
					operation.Shape ? operation.Shape->Points.size() + size_t(operation.Shape->Loop) : 0;
				auto charge = context.ReserveWorkspace(
					operation.Inputs.size() * sizeof(PathRuntime) + shapeSlots * 2 * sizeof(double), "path"
				);
				if (!charge) return false;
				PathRuntime replacement;
				replacement.StorageCharge = std::move(*charge);
				replacement.Operation = operation.Kind;
				replacement.TrimRange = operation.TrimRange;
				replacement.EvaluationContext = &context;
				if (operation.Kind == SourcePathOperationKind::VerletMesh)
					replacement.SourceMesh = &operation;
				if (operation.Shape) {
					replacement.Shape = &*operation.Shape;
					replacement.Loop = operation.Shape->Loop;
					replacement.Lengths.resize(shapeSlots);
					replacement.LengthAccumulated.resize(shapeSlots);
					const auto &points = operation.Shape->Points;
					for (size_t index = 1; index < points.size(); ++index)
						replacement.Lengths[index - 1] = std::hypot(
							points[index].X - points[index - 1].X, points[index].Y - points[index - 1].Y
						);
					if (!points.empty() && operation.Shape->Loop)
						replacement.Lengths[points.size() - 1] = std::hypot(
							points.back().X - points.front().X, points.back().Y - points.front().Y
						);
					for (size_t index = 0; index < replacement.Lengths.size(); ++index) {
						replacement.LengthTotal += replacement.Lengths[index];
						replacement.LengthAccumulated[index] = replacement.LengthTotal;
					}
					if (!std::isfinite(replacement.LengthTotal))
						return context.Fail(
							Status::InvalidValue, "source shape chord length is nonfinite", "path"
						);
					replacement.MinX = operation.Shape->Position.X - operation.Shape->HalfSize.X;
					replacement.MinY = operation.Shape->Position.Y - operation.Shape->HalfSize.Y;
					replacement.MaxX = operation.Shape->Position.X + operation.Shape->HalfSize.X;
					replacement.MaxY = operation.Shape->Position.Y + operation.Shape->HalfSize.Y;
					replacement.HasBoundary = true;
				}
				replacement.Inputs.reserve(operation.Inputs.size());
				for (const auto &child : operation.Inputs) {
					PathRuntime runtime;
					if (!runtime.Init(context, child)) return false;
					replacement.Inputs.push_back(std::move(runtime));
				}
				if (!replacement.Shape) replacement.LengthTotal = replacement.Length();
				if (!replacement.Inputs.empty()) {
					const auto &first = replacement.Inputs[0];
					replacement.MinX = first.MinX;
					replacement.MinY = first.MinY;
					replacement.MaxX = first.MaxX;
					replacement.MaxY = first.MaxY;
					replacement.HasBoundary = first.HasBoundary;
				}
				Swap(replacement);
				return true;
			}

			auto charge = context.ReserveWorkspace(*bytes, "path");
			if (!charge) return false;
			PathRuntime replacement;
			replacement.StorageCharge = std::move(*charge);
			replacement.Loop = path.Loop;
			replacement.Segmented = path.Segmented;
			replacement.Anchors.reserve(path.Anchors.size());
			replacement.Weights.reserve(path.Weights.size());
			if (path.Anchors.size() >= 2) {
				const size_t segments = path.Loop ? path.Anchors.size() : path.Anchors.size() - 1;
				replacement.Lengths.reserve(segments);
				replacement.LengthAccumulated.reserve(segments);
				replacement.LengthRatio.reserve(path.Anchors.size() + 1);
				replacement.WeightRatio.reserve(101);
			}
			for (const PathAnchor &anchor : path.Anchors)
				replacement.Anchors.push_back(anchor.Controls);
			for (const PathWeight &weight : path.Weights)
				replacement.Weights.push_back({weight.Position, weight.Weight});
			replacement.UpdateLength();
			Swap(replacement);
			return true;
		}

		void Swap(PathRuntime &other) {
			using std::swap;
			swap(StorageCharge, other.StorageCharge);
			swap(Operation, other.Operation);
			swap(TrimRange, other.TrimRange);
			Inputs.swap(other.Inputs);
			swap(SourceMesh, other.SourceMesh);
			swap(Shape, other.Shape);
			swap(EvaluationContext, other.EvaluationContext);
			swap(Loop, other.Loop);
			swap(Segmented, other.Segmented);
			Anchors.swap(other.Anchors);
			Weights.swap(other.Weights);
			Lengths.swap(other.Lengths);
			LengthAccumulated.swap(other.LengthAccumulated);
			LengthRatio.swap(other.LengthRatio);
			WeightRatio.swap(other.WeightRatio);
			swap(LengthTotal, other.LengthTotal);
			swap(MinX, other.MinX);
			swap(MinY, other.MinY);
			swap(MaxX, other.MaxX);
			swap(MaxY, other.MaxY);
			swap(HasBoundary, other.HasBoundary);
		}

		bool Loop = false;
		bool Segmented = false;
		std::vector<std::array<double, 6>> Anchors;
		std::vector<std::array<double, 2>> Weights;
		std::vector<double> Lengths, LengthAccumulated, LengthRatio, WeightRatio;
		double LengthTotal = 0.0;
		double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
		bool HasBoundary = false;

		PathPoint ShapePoint(const std::optional<Vector2> &point) const {
			if (point) return {point->X, point->Y, 1};
			if (EvaluationContext)
				EvaluationContext->Fail(
					Status::InvalidValue, "source shape path sample is undefined or nonfinite", "path"
				);
			return {NAN, NAN, NAN};
		}
		PathPoint MeshPoint(const std::optional<std::array<double, 3>> &point) const {
			if (point) return {(*point)[0], (*point)[1], (*point)[2]};
			if (EvaluationContext)
				EvaluationContext->Fail(
					Status::InvalidValue, "source mesh path geometry has an undefined cached edge", "path"
				);
			return {NAN, NAN, NAN};
		}
		const PathRuntime *SelectLine(size_t &line) const {
			for (const auto &child : Inputs) {
				const size_t count = child.LineCount();
				if (line < count) return &child;
				line -= count;
			}
			return nullptr;
		}
		size_t LineCount() const {
			if (SourceMesh) return LineCountSourceVerletPath(*SourceMesh);
			if (!Operation) return 1;
			if (*Operation != SourcePathOperationKind::Combine)
				return Inputs.empty() ? 1 : Inputs[0].LineCount();
			size_t count = 0;
			for (const auto &child : Inputs)
				count += child.LineCount();
			return count;
		}
		double Length(size_t line = 0) const {
			if (Shape) return LengthTotal;
			if (SourceMesh) return LengthSourceVerletPath(*SourceMesh, line);
			if (!Operation) return LengthTotal;
			const auto *child = SelectLine(line);
			return child ? child->Length(line) *
							   (*Operation == SourcePathOperationKind::Trim ? TrimRange.Y - TrimRange.X : 1)
						 : 0;
		}
		size_t SegmentCount(size_t line = 0) const {
			if (Shape) return Lengths.size();
			if (SourceMesh) return SourceMesh->CachedLengths.size();
			if (Operation) {
				const auto *child = SelectLine(line);
				return child ? child->SegmentCount(line) : 0;
			}
			return line == 0 ? (Segmented ? Anchors.size() : Lengths.size()) : 0;
		}

		PathPoint SegmentPoint(size_t index, double t) const {
			const auto &a0 = Anchors[index % Anchors.size()];
			const auto &a1 = Anchors[(index + 1) % Anchors.size()];
			if (a0[4] == 0 && a0[5] == 0 && a1[2] == 0 && a1[3] == 0)
				return {a0[0] + (a1[0] - a0[0]) * t, a0[1] + (a1[1] - a0[1]) * t, 1.0};
			return {
				BezierComponent(t, a0[0], a1[0], a0[0] + a0[4], a1[0] + a1[2]),
				BezierComponent(t, a0[1], a1[1], a0[1] + a0[5], a1[1] + a1[3]),
				1.0
			};
		}

		// updateLength: each segment is sampled at PATH_RESOLUTION + 1 points and measured as a polyline.
		void UpdateLength() {
			const size_t count = Anchors.size();
			if (count < 2) return;
			const size_t segments = Loop ? count : count - 1;
			for (size_t index = 0; index < segments; index++) {
				double length = 0.0, previousX = 0.0, previousY = 0.0;
				for (int step = 0; step <= PATH_RESOLUTION; step++) {
					const PathPoint point = SegmentPoint(index, double(step) / PATH_RESOLUTION);
					AddBoundary(point.X, point.Y);
					if (step) length += std::hypot(point.X - previousX, point.Y - previousY);
					previousX = point.X;
					previousY = point.Y;
				}
				Lengths.push_back(length);
				LengthTotal += length;
				LengthAccumulated.push_back(LengthTotal);
			}
			LengthRatio.assign(count + 1, 0.0);
			for (size_t index = 0; index < segments; index++)
				LengthRatio[index + 1] = LengthAccumulated[index] / LengthTotal;
			// The source builds weightRatio only for two or more weights; fewer read an unset variable, so a
			// constant table stands in for that error path.
			if (Weights.size() < 2) {
				WeightRatio.assign(101, Weights.empty() ? 1.0 : Weights[0][1]);
				return;
			}
			WeightRatio.assign(101, 0.0);
			size_t cursor = 0;
			std::array<double, 2> from = Weights[0], to = Weights[1];
			for (int index = 0; index <= 100; index++) {
				if (index < 100 && double(index) == to[0]) {
					cursor++;
					from = Weights[cursor % Weights.size()];
					to = Weights[(cursor + 1) % Weights.size()];
				}
				const double x = (index - from[0]) / (to[0] - from[0]);
				WeightRatio[size_t(index)] = from[1] + (to[1] - from[1]) * (x * x * (3.0 - 2.0 * x));
			}
		}

		void AddBoundary(double x, double y) {
			if (!HasBoundary) {
				MinX = MaxX = x;
				MinY = MaxY = y;
				HasBoundary = true;
				return;
			}
			MinX = std::min(MinX, x);
			MaxX = std::max(MaxX, x);
			MinY = std::min(MinY, y);
			MaxY = std::max(MaxY, y);
		}

		double WeightAt(double index) const {
			const auto at = [&](double position) {
				if (position < 0 || position >= WeightRatio.size()) return 0.0;
				return WeightRatio[size_t(position)];
			};
			const double whole = std::floor(index);
			if (index == whole) return at(whole);
			return at(whole) + (at(whole + 1) - at(whole)) * (index - whole);
		}

		PathPoint PointDistance(double distance, size_t line = 0) const {
			if (Shape) return ShapePoint(SourceShapeDistance(*Shape, Lengths, LengthTotal, distance));
			if (SourceMesh) return MeshPoint(DistanceSourceVerletPath(*SourceMesh, distance, line));
			if (Operation) {
				if (*Operation == SourcePathOperationKind::Reverse ||
					*Operation == SourcePathOperationKind::Trim)
					return PointRatio(distance / Length(), line);
				const auto *child = SelectLine(line);
				return child ? child->PointDistance(distance, line) : PathPoint{};
			}
			PathPoint out{0.0, 0.0, 1.0};
			if (Lengths.empty()) return out;
			if (distance < 0) distance = LengthTotal + GmlMod(distance, LengthTotal);
			if (Loop)
				distance =
					LengthTotal == 0 ? 0 : GmlMod(distance, LengthTotal) + (distance < 0 ? LengthTotal : 0);
			size_t index = 0;
			for (size_t repeat = 0; repeat < Anchors.size(); repeat++) {
				const double length = Lengths[std::min(index, Lengths.size() - 1)];
				if (distance > length) {
					distance -= length;
					index++;
					continue;
				}
				const double t = length == 0 ? 0 : distance / length;
				const double ratio =
					(LengthRatio[index] + (LengthRatio[index + 1] - LengthRatio[index]) * t) * 100.0;
				out = SegmentPoint(index, t);
				out.Weight = WeightAt(ratio);
				return out;
			}
			return out;
		}

		PathPoint PointRatio(double ratio, size_t line = 0) const {
			if (Shape) return ShapePoint(SourceShapeRatio(*Shape, Lengths, LengthTotal, ratio));
			if (SourceMesh) return MeshPoint(SampleSourceVerletPath(*SourceMesh, ratio, line));
			if (Operation) {
				const auto *child = SelectLine(line);
				return child ? child->PointRatio(
								   *Operation == SourcePathOperationKind::Reverse ? 1 - ratio
								   : *Operation == SourcePathOperationKind::Trim
									   ? TrimRange.X + (TrimRange.Y - TrimRange.X) * ratio
									   : ratio,
								   line
							   )
							 : PathPoint{};
			}
			if (ratio < 0) ratio = 1 + (ratio - std::trunc(ratio));
			const double position =
				(Loop || Segmented) ? ratio - std::trunc(ratio) : std::clamp(ratio, 0.0, 0.9999);
			return PointDistance(position * LengthTotal);
		}

		PathPoint PointSegment(double ratio) const {
			if (Operation) return {NAN, NAN, NAN};
			if (Lengths.empty()) return {};
			const size_t count = Anchors.size();
			if (ratio < 0) return {Anchors[0][0], Anchors[0][1], 1.0};
			ratio = std::fmod(ratio, double(count));
			const size_t from = std::min(size_t(std::floor(ratio)), count - 1);
			const size_t to = (from + 1) % count;
			const double t = ratio - std::trunc(ratio);
			if (to >= count && !Loop) return {Anchors[count - 1][0], Anchors[count - 1][1], 1.0};
			PathPoint point = SegmentPoint(from, t);
			point.Weight = 1.0;
			return point;
		}
	};
}
