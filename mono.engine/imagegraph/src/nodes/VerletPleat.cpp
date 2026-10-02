#include "Curve.hpp"
#include "VerletNodes.hpp"

namespace engine::imagegraph::detail {
	bool VerletPleatMesh(NodeContext &context) {
		const Value *input = context.Find("mesh");
		const auto *source = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!source || !source->Data || !source->Data->Verlet) return true;
		if (const auto preserved = PreserveVerletForCacheAction(context, *source)) return *preserved;
		if (!ValidMesh2DPayload(*source))
			return context.Fail(Status::InvalidValue, "pleat mesh input is invalid", "mesh");
		const int64_t target = context.Integer("source", 1);
		if (target < 0 || target > 1)
			return context.Fail(Status::InvalidValue, "pleat target is invalid", "source");
		const double strength = context.Scalar("strength", .5),
					 amount = std::max(1.0, context.Scalar("amount", 4)),
					 contract = context.Scalar("contract", 1), offset = context.Scalar("offset", 2);
		const Value *stretchInput = context.Find("stretch_falloff"),
					*offsetInput = context.Find("offset_falloff");
		const auto *stretch = stretchInput ? std::get_if<Curve>(stretchInput) : nullptr,
				   *offsetFalloff = offsetInput ? std::get_if<Curve>(offsetInput) : nullptr;
		if (!stretch || !offsetFalloff)
			return context.Fail(Status::InvalidValue, "pleat falloff must be a curve", "stretch_falloff");
		if (context.FailureCode != Status::Ok) return false;
		if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*source), "mesh")) return false;
		MeshValue2D output = *source;
		auto &points = output.Data->Simulation.Points;
		const auto &edges = output.Data->Simulation.Edges;
		if (target == 0) {
			const Area area = VerletMeshArea(context);
			const double x0 = area.CenterX - area.HalfWidth, y0 = area.CenterY - area.HalfHeight,
						 x1 = area.CenterX + area.HalfWidth, y1 = area.CenterY + area.HalfHeight,
						 width = area.HalfWidth * 2 / amount;
			if (width == 0)
				return context.Fail(
					Status::InvalidValue, "source pleat area division has zero width", "area"
				);
			for (auto &point : points) {
				const Vector2 original = point.Original;
				if (!point.Pin || original.X < x0 || original.X > x1 || original.Y < y0 || original.Y > y1)
					continue;
				const double segment = std::floor((original.X - x0) / width);
				const double center = x0 + (segment + .5) * width;
				const double progress = std::clamp(std::abs(original.X - center) / width * 2, 0.0, 1.0);
				const double stretchValue = EvalCurveX(*stretch, progress),
							 offsetValue = EvalCurveX(*offsetFalloff, progress);
				const double px =
					area.CenterX +
					(center + (original.X - center) * 2 * stretchValue - area.CenterX) * contract;
				const double py = original.Y + (std::fmod(segment, 2) == 0 ? 1 : -1) * offset * offsetValue;
				point.Position = {CurveLerp(original.X, px, strength), CurveLerp(original.Y, py, strength)};
			}
		} else {
			const int64_t selected = context.Integer("edge_index");
			if (selected < 0 || uint64_t(selected) >= edges.size())
				return PublishVerletMesh(context, std::move(output));
			auto temporary =
				context.ReserveWorkspace((edges.size() * 2 + 1) * sizeof(uint32_t), "edge_index");
			if (!temporary) return false;
			std::vector<uint32_t> chain;
			chain.reserve(edges.size());
			int64_t edgeIndex = edges[size_t(selected)].PreviousEdge;
			while (edgeIndex >= 0) {
				if (chain.size() >= edges.size())
					return context.Fail(
						Status::InvalidValue, "pleat edge chain contains a cycle", "edge_index"
					);
				chain.push_back(uint32_t(edgeIndex));
				edgeIndex = edges[size_t(edgeIndex)].PreviousEdge;
			}
			if (chain.size() >= edges.size())
				return context.Fail(Status::InvalidValue, "pleat edge chain contains a cycle", "edge_index");
			std::reverse(chain.begin(), chain.end());
			chain.push_back(uint32_t(selected));
			edgeIndex = edges[size_t(selected)].NextEdge;
			while (edgeIndex >= 0) {
				if (chain.size() >= edges.size())
					return context.Fail(
						Status::InvalidValue, "pleat edge chain contains a cycle", "edge_index"
					);
				chain.push_back(uint32_t(edgeIndex));
				edgeIndex = edges[size_t(edgeIndex)].NextEdge;
			}
			for (size_t index = 0; index <= chain.size(); ++index) {
				auto &point = points[index == 0 ? edges[chain[0]].First : edges[chain[index - 1]].Second];
				const double progress = double(index) / chain.size() * amount;
				const double fraction = progress - std::floor(progress);
				const double phase = std::clamp(std::abs(fraction * 2 - 1), 0.0, 1.0);
				const double se = EvalCurveX(*stretch, phase), of = EvalCurveX(*offsetFalloff, phase);
				point.Position = {
					point.Original.X + se * strength, point.Original.Y + offset * of * strength
				};
			}
		}
		return PublishVerletMesh(context, std::move(output));
	}
}
