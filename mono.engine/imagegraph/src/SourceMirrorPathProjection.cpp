#include "SourceMirrorPathProjection.hpp"

#include "nodes/Path.hpp"

#include <engine/core/Profiling.hpp>

#include <cmath>
#include <new>

namespace engine::imagegraph::detail {
	namespace {
		const Value *RawValue(const Node &node, std::string_view port) {
			for (const auto &value : node.Values)
				if (value.Port == port) return &value.Data;
			return nullptr;
		}
		std::optional<double> RawRatio(const Value &value) {
			if (const auto *vector = std::get_if<Vector2>(&value)) return vector->X;
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array || !array->Nested.empty() || !array->Items.empty() || array->Elements.empty())
				return std::nullopt;
			if (const auto *scalar = std::get_if<double>(&array->Elements.front())) return *scalar;
			if (const auto *integer = std::get_if<int64_t>(&array->Elements.front())) return double(*integer);
			return std::nullopt;
		}
	}
	SourceMirrorPathProjection::~SourceMirrorPathProjection() {
		if (!Installed) return;
		Context.ValueViews = std::move(OriginalViews);
		Context.MirrorPathSamples = OriginalFlags;
	}
	bool SourceMirrorPathProjection::Prepare() try {
		if (Context.Authored.Type != "pc.mirror_polar") return true;
		ENGINE_PROFILE("imagegraph.mirror_path_getter");
		std::array<const Path2D *, 5> paths{};
		size_t count = 0;
		for (size_t i = 0; i < paths.size(); ++i) {
			const auto *value = Context.Find(SourceMirrorVectorPorts[i]);
			paths[i] = value ? std::get_if<Path2D>(value) : nullptr;
			count += paths[i] != nullptr;
		}
		if (!count) return true;
		// The previous view buffer remains live until destruction restores it.
		auto reservation = Context.ReserveWorkspace(
			(Context.ValueViews.size() + count) * sizeof(OriginalViews[0]), "source_path_getter"
		);
		if (!reservation) return false;
		Charge = std::move(*reservation);
		for (size_t i = 0; i < paths.size(); ++i) {
			if (!paths[i]) continue;
			const auto port = SourceMirrorVectorPorts[i];
			const Value *raw = Context.MirrorRawAnimators[i] ? Context.MirrorRawAnimators[i]
															 : RawValue(Context.Authored, port);
			std::optional<double> ratio;
			if (raw)
				ratio = RawRatio(*raw);
			else {
				// Exact constructor defaults, including the project's raw Constant Dimension.
				constexpr std::array<double, 5> defaults{1, 0, 0, .5, 1};
				ratio = i == 1 ? double(Context.Project.SurfaceWidth) : defaults[i];
			}
			if (!ratio || !std::isfinite(*ratio))
				return Context.Fail(
					Status::UnsupportedExecution, "Polar Mirror local path ratio is undefined", port
				);
			PathRuntime runtime;
			if (!runtime.Init(Context, *paths[i])) return false;
			const auto point = runtime.PointRatio(*ratio, 0);
			if (Context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(point.X) || !std::isfinite(point.Y))
				return Context.Fail(
					Status::InvalidValue, "Polar Mirror sampled path point is nonfinite", port
				);
			Samples[i] = Vector2{point.X, point.Y};
		}
		OriginalFlags = Context.MirrorPathSamples;
		OriginalViews = std::move(Context.ValueViews);
		Installed = true;
		Context.ValueViews.reserve(OriginalViews.size() + count);
		if (!Charge.Resize(Context.ValueViews.capacity() * sizeof(OriginalViews[0])))
			return Context.Fail(
				Status::LimitExceeded, "Polar Mirror path getter view capacity exceeds budget"
			);
		Context.ValueViews.insert(Context.ValueViews.end(), OriginalViews.begin(), OriginalViews.end());
		for (size_t i = 0; i < paths.size(); ++i)
			if (paths[i]) {
				Context.ValueViews.emplace_back(SourceMirrorVectorPorts[i], &Samples[i]);
				Context.MirrorPathSamples[i] = true;
			}
		return true;
	} catch (const std::bad_alloc &) {
		return Context.Fail(Status::LimitExceeded, "Polar Mirror path getter allocation failed");
	}
}
