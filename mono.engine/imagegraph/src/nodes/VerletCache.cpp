#include "Curve.hpp"
#include "VerletNodes.hpp"

namespace engine::imagegraph::detail {
	namespace {
		std::optional<double> Coordinate(const ElementValue &value) {
			return std::visit(
				[](const auto &item) -> std::optional<double> {
					using T = std::decay_t<decltype(item)>;
					if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
								  std::is_same_v<T, bool>)
						return double(item);
					return std::nullopt;
				},
				value
			);
		}
		std::optional<Vector2> CachePoint(const ArrayValue &array, size_t index) {
			if (!array.Nested.empty()) {
				const auto &row = array.Nested[index];
				if (row.size() < 2) return std::nullopt;
				const auto x = Coordinate(row[0]), y = Coordinate(row[1]);
				if (x && y) return Vector2{*x, *y};
			} else if (!array.Items.empty()) {
				const auto *row = std::get_if<std::vector<SourceArrayItem>>(&array.Items[index].Data);
				if (!row || row->size() < 2) return std::nullopt;
				const auto *first = std::get_if<ElementValue>(&(*row)[0].Data),
						   *second = std::get_if<ElementValue>(&(*row)[1].Data);
				if (!first || !second) return std::nullopt;
				const auto x = Coordinate(*first), y = Coordinate(*second);
				if (x && y) return Vector2{*x, *y};
			}
			return std::nullopt;
		}
	}
	bool VerletCaptureCache(NodeContext &context) {
		const SimulationReplayEntry *prior = nullptr;
		if (context.CurrentSimulation)
			for (const auto &entry : context.CurrentSimulation->Entries)
				if (entry.NodeId == context.Authored.Id && entry.ProcessorRow == context.ProcessorRow) {
					prior = &entry;
					break;
				}
		if (prior &&
			(!prior->Cache || prior->State.AuthoringRevision != context.Request.SimulationAuthoringRevision))
			return context.Fail(
				Status::InvalidValue, "mesh cache revision or kind requires reset", "cached_data"
			);
		std::span<const Vector2> cached =
			prior ? std::span<const Vector2>(*prior->Cache) : std::span<const Vector2>{};
		bool present = prior != nullptr;
		const int64_t frame = context.Integer("frame", 1);
		const bool manual = std::find(
								context.Request.SimulationCacheCaptures.begin(),
								context.Request.SimulationCacheCaptures.end(),
								context.Authored.Id
							) != context.Request.SimulationCacheCaptures.end();
		const bool capture = manual || (context.Boolean("autocache") && frame >= 0 &&
										uint64_t(frame) == context.Request.Tick &&
										!context.Request.NegativeFrame && context.Request.Subframe == 0);
		if (context.FailureCode != Status::Ok) return false;
		if (capture) {
			const Value *input = context.Find("mesh");
			const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
			if (mesh && mesh->Data && mesh->Data->Verlet) {
				if (!ValidMesh2DPayload(*mesh))
					return context.Fail(Status::InvalidValue, "cached mesh is invalid", "mesh");
				const uint64_t bytes = sizeof(SimulationReplayEntry) +
									   std::max(context.Authored.Id.size(), std::string{}.capacity()) +
									   mesh->Data->Simulation.Points.size() * sizeof(Vector2);
				if (!context.ReserveOutput(bytes, "cached_data")) return false;
				auto overlap = context.ReserveWorkspace(
					context.SimulationUpdates.capacity() * sizeof(SimulationReplayEntry), "cached_data"
				);
				if (!overlap) return false;
				context.SimulationUpdates.reserve(context.SimulationUpdates.size() + 1);
				SimulationReplayEntry update;
				update.NodeId = context.Authored.Id;
				update.ProcessorRow = context.ProcessorRow;
				update.State.Tick = context.Request.Tick;
				update.State.AuthoringRevision = context.Request.SimulationAuthoringRevision;
				update.State.Initialized = true;
				update.Cache.emplace().reserve(mesh->Data->Simulation.Points.size());
				for (const auto &point : mesh->Data->Simulation.Points)
					update.Cache->push_back(point.Position);
				context.SimulationUpdates.push_back(std::move(update));
				cached = *context.SimulationUpdates.back().Cache;
				present = true;
			}
		}
		if (!present) {
			context.SetValue("cached_data", UndefinedValue{});
			return context.FailureCode == Status::Ok;
		}
		if (cached.size() > (Limits::MaximumArrayElements - 2) / 2)
			return context.Fail(
				Status::LimitExceeded, "cached coordinates exceed bounded struct element count", "cached_data"
			);
		const uint64_t bytes = sizeof(StructData) + sizeof(std::pair<std::string, Value>) +
							   std::string{}.capacity() +
							   cached.size() * (sizeof(std::vector<ElementValue>) + 2 * sizeof(ElementValue));
		if (!context.ReserveOutput(bytes, "cached_data")) return false;
		StructValue output;
		ArrayValue positions;
		positions.ElementType = ValueType::Scalar;
		positions.Nested.reserve(cached.size());
		for (const auto point : cached)
			positions.Nested.push_back({point.X, point.Y});
		output.Data.emplace().Fields.emplace_back("points", std::move(positions));
		context.SetValue("cached_data", std::move(output));
		return context.FailureCode == Status::Ok;
	}
	bool VerletCacheMix(NodeContext &context) {
		const Value *input = context.Find("mesh"), *cacheInput = context.Find("cache_mesh");
		const auto *source = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!source || !source->Data || !source->Data->Verlet) return true;
		if (const auto preserved = PreserveCapturedVerlet(context, *source)) return *preserved;
		if (!ValidMesh2DPayload(*source))
			return context.Fail(Status::InvalidValue, "cache mix input mesh is invalid", "mesh");
		const auto *cache = cacheInput ? std::get_if<StructValue>(cacheInput) : nullptr;
		const ArrayValue *positions = nullptr;
		if (cache && cache->Data)
			for (const auto &[name, field] : cache->Data->Fields)
				if (name == "points") {
					positions = std::get_if<ArrayValue>(&field);
					break;
				}
		if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*source), "mesh")) return false;
		MeshValue2D output = *source;
		if (!cache || !cache->Data) return PublishVerletMesh(context, std::move(output));
		bool hasPoints = false;
		for (const auto &[name, field] : cache->Data->Fields)
			if (name == "points") hasPoints = true;
		if (!hasPoints) return PublishVerletMesh(context, std::move(output));
		if (!positions)
			return context.Fail(Status::InvalidValue, "source cached points must be an array", "cache_mesh");
		const size_t count = !positions->Nested.empty()	 ? positions->Nested.size()
							 : !positions->Items.empty() ? positions->Items.size()
														 : positions->Elements.size();
		const double amount = context.Scalar("amount", .5);
		if (context.FailureCode != Status::Ok) return false;
		auto &points = output.Data->Simulation.Points;
		for (size_t index = 0; index < std::min(count, points.size()); ++index) {
			const auto cached = CachePoint(*positions, index);
			if (!cached || !MeshFinite(*cached))
				return context.Fail(
					Status::InvalidValue, "source cached point needs two finite coordinates", "cache_mesh"
				);
			points[index].DrawPosition = Vector2{
				CurveLerp(points[index].Position.X, cached->X, amount),
				CurveLerp(points[index].Position.Y, cached->Y, amount)
			};
		}
		return PublishVerletMesh(context, std::move(output));
	}
}
