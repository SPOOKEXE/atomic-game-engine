#include "Families.hpp"
#include "Path.hpp"

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t COMPOSE_WORK_LIMIT = 1u << 24;
		bool ExplicitPath(NodeContext &c, std::string_view id, const Path2D *path) {
			const auto isDefault = c.IsCatalogueDefault(id);
			if (!isDefault)
				return c.Fail(
					Status::UnsupportedExecution,
					"Path composition requires resolved source default provenance",
					id
				);
			return path && !*isDefault;
		}

		bool PathWork(const Path2D &path, uint64_t &work) {
			const uint64_t add = path.Anchors.size() * 33 + path.Weights.size() + 1;
			if (add > COMPOSE_WORK_LIMIT - work) return false;
			work += add;
			if (path.SourceOperation) {
				const auto &op = *path.SourceOperation;
				if (op.Shape) {
					const uint64_t points = op.Shape->Points.size();
					if (points > COMPOSE_WORK_LIMIT - work) return false;
					work += points;
				}
				if (op.Mesh) {
					const uint64_t edges = op.Mesh->Simulation.Edges.size();
					if (edges > COMPOSE_WORK_LIMIT - work) return false;
					work += edges;
				}
				for (const auto &child : op.Inputs)
					if (!PathWork(child, work)) return false;
			}
			return true;
		}
		bool PublishCompose(
			NodeContext &c,
			SourcePathOperationKind kind,
			const std::vector<const Path2D *> &paths,
			const std::vector<uint8_t> &reversals = {},
			std::array<bool, 2> present = {false, false}
		) {
			size_t nodes = 1;
			uint64_t bytes = sizeof(SourcePathData2D) + paths.size() * sizeof(Path2D) + reversals.size(),
					 work = 0;
			for (const auto *path : paths) {
				if (!ValidSourcePath2D(*path, 1, &nodes))
					return c.Fail(Status::LimitExceeded, "Composed path tree exceeds bounds", "path");
				const uint64_t child = SourcePath2DBytes<false>(*path);
				if (child > Limits::MaximumEvaluationBytes - bytes)
					return c.Fail(Status::LimitExceeded, "Composed path exceeds byte bounds", "path");
				bytes += child;
				if (!PathWork(*path, work))
					return c.Fail(Status::LimitExceeded, "Composed path exceeds bounded work", "path");
			}
			// Join initializes two endpoint samples per input, and normal path consumers sample position and
			// tangent.
			if (work > COMPOSE_WORK_LIMIT / std::max<size_t>(1, c.ProcessorCount) / 8)
				return c.Fail(
					Status::LimitExceeded, "Composed path processor batch exceeds bounded work", "path"
				);
			if (!c.ReserveOutput(bytes, "path")) return false;
			Path2D output;
			auto &op = output.SourceOperation.emplace();
			op.Kind = kind;
			op.Inputs.reserve(paths.size());
			for (const auto *path : paths)
				op.Inputs.push_back(*path);
			if (kind == SourcePathOperationKind::Offset) {
				op.Offset = c.Scalar("offset");
				op.ClampOffset = c.Boolean("clamp");
			}
			if (kind == SourcePathOperationKind::Blend) {
				const double mode = c.SourceChoice("mode");
				if (mode < 0 || mode > 3 || std::floor(mode) != mode)
					return c.Fail(
						Status::InvalidValue, "Path blend mode must be a source choice 0 through 3", "mode"
					);
				op.BlendMode = uint8_t(mode);
				op.BlendAmount = c.Scalar("amount");
				op.BlendInputsValid = present;
			}

			if (kind == SourcePathOperationKind::Blend) {
				PathRuntime first, second;
				const SourcePathData2D *previous = nullptr;
				const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
				if (owner) {
					Diagnostic diagnostic;
					if (ValidateDataReplay(*owner, c.ByteBudget, diagnostic) != Status::Ok)
						return c.Fail(diagnostic.Code, diagnostic.Message);
					for (const auto &entry : owner->Entries)
						if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow &&
							!entry.Values.empty()) {
							const auto *path = std::get_if<Path2D>(&entry.Values.back().Data);
							if (path && path->SourceOperation &&
								path->SourceOperation->Kind == SourcePathOperationKind::Blend)
								previous = &*path->SourceOperation;
						}
				}
				size_t lines = 0, items = 0;
				if (present[0] && present[1]) {
					if (!first.Init(c, *paths[0]) || !second.Init(c, *paths[1])) return false;
					lines = first.LineCount();
					items = lines;
					if (lines > Limits::MaximumArrayElements)
						return c.Fail(
							Status::LimitExceeded, "Blend cached line count exceeds bounds", "path"
						);
					for (size_t line = 0; line < lines; ++line) {
						const size_t count =
							std::max(first.AccumulatedCount(line), second.AccumulatedCount(line));
						if (count > Limits::MaximumArrayElements - items)
							return c.Fail(
								Status::LimitExceeded, "Blend accumulated cache exceeds bounds", "path"
							);
						items += count;
					}
					if (items > COMPOSE_WORK_LIMIT / std::max<size_t>(1, c.ProcessorCount) / 8 /
									std::max<uint64_t>(1, work))
						return c.Fail(
							Status::LimitExceeded,
							"Blend accumulated processor batch exceeds bounded work",
							"path"
						);
				} else if (previous) {
					lines = previous->BlendLengths.size();
					items = lines;
					for (const auto &row : previous->BlendAccumulated)
						items += row.size();
				}
				const uint64_t cache = lines * sizeof(std::vector<double>) + items * sizeof(double);
				if (!c.ReserveOutput(
						bytes + cache * 2 + sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
							std::max(c.Authored.Id.size(), std::string{}.capacity()),
						"path"
					))
					return false;
				if (present[0] && present[1]) {
					op.BlendLengths.reserve(lines);
					op.BlendAccumulated.reserve(lines);
					for (size_t line = 0; line < lines; ++line) {
						const double a = first.Length(line), b = second.Length(line);
						op.BlendLengths.push_back(op.BlendMode == 0 ? a + (b - a) * op.BlendAmount : a + b);
						const size_t count =
							std::max(first.AccumulatedCount(line), second.AccumulatedCount(line));
						auto &row = op.BlendAccumulated.emplace_back();
						row.reserve(count);
						for (size_t index = 0; index < count; ++index) {
							const double x = first.AccumulatedAt(index, line),
										 y = second.AccumulatedAt(index, line);
							row.push_back(op.BlendMode == 0 ? x + (y - x) * op.BlendAmount : std::max(x, y));
						}
					}
				} else if (previous) {
					op.BlendLengths = previous->BlendLengths;
					op.BlendAccumulated = previous->BlendAccumulated;
				}
			}
			if (kind == SourcePathOperationKind::Join) op.Reversed = reversals;
			if (!ValidSourcePath2D(output))
				return c.Fail(Status::InvalidValue, "Composed path controls are invalid", "path");
			PathRuntime check;
			if (!check.Init(c, output)) return false;
			if (!std::isfinite(check.Length()))
				return c.Fail(Status::InvalidValue, "Composed path length is nonfinite", "path");

			DataReplayEntry state;
			if (kind == SourcePathOperationKind::Blend) {
				state.NodeId = c.Authored.Id;
				state.ProcessorRow = c.ProcessorRow;
				state.Tick = c.Request.Tick;
				state.Subframe = c.Request.Subframe;
				state.NegativeFrame = c.Request.NegativeFrame;
				state.Initialized = true;
				state.PreviousFrame =
					double(FrameTimeToReal({state.Tick, state.Subframe, state.NegativeFrame}));
				state.Values.push_back({state.Tick, Value{output}});
			}
			c.SetValue(kind == SourcePathOperationKind::Join ? "joined_path" : "path", std::move(output));
			if (c.FailureCode != Status::Ok) return false;
			if (kind == SourcePathOperationKind::Blend) c.DataUpdates.push_back(std::move(state));
			return true;
		}
		bool PathOffset(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.path_offset");
			const auto *value = c.Find("path");
			const auto *path = value ? std::get_if<Path2D>(value) : nullptr;
			if (value && !path)
				return c.Fail(
					Status::UnsupportedExecution,
					"Path Offset requires the planar source path profile",
					"path"
				);
			std::vector<const Path2D *> paths;
			auto scratch = c.ReserveWorkspace(sizeof(const Path2D *), "path");
			if (!scratch) return false;
			if (ExplicitPath(c, "path", path)) paths.push_back(path);
			if (c.FailureCode != Status::Ok) return false;
			return PublishCompose(c, SourcePathOperationKind::Offset, paths);
		}
		bool PathBlend(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.path_blend");
			const auto *a = c.Find("path_1"), *b = c.Find("path_2");
			const auto *first = a ? std::get_if<Path2D>(a) : nullptr,
					   *second = b ? std::get_if<Path2D>(b) : nullptr;
			if ((a && !first) || (b && !second))
				return c.Fail(
					Status::UnsupportedExecution, "Path Blend requires planar source paths", "path"
				);
			const Path2D missing;
			auto scratch = c.ReserveWorkspace(2 * sizeof(const Path2D *), "path");
			if (!scratch) return false;
			std::vector<const Path2D *> paths{first ? first : &missing, second ? second : &missing};
			const std::array<bool, 2> present{
				ExplicitPath(c, "path_1", first), ExplicitPath(c, "path_2", second)
			};
			if (c.FailureCode != Status::Ok) return false;
			return PublishCompose(c, SourcePathOperationKind::Blend, paths, {}, present);
		}
		bool PathJoin(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.path_join");
			std::vector<const Path2D *> paths;
			std::vector<uint8_t> reversed;
			size_t count = 0;
			bool materialize = false;
			const auto append = [&](const auto &value, bool reverse, bool allowMissing) -> bool {
				const auto *path = std::get_if<Path2D>(&value);
				if (!path)
					return c.Fail(
						Status::UnsupportedExecution, "Joined path array must contain planar paths", "path"
					);
				if (allowMissing && !path->SourceOperation && path->Anchors.empty()) return true;
				if (count >= Limits::MaximumArrayElements)
					return c.Fail(Status::LimitExceeded, "Joined path input count exceeds bounds", "path");
				++count;
				if (materialize) {
					paths.push_back(path);
					reversed.push_back(reverse ? 1 : 0);
				}
				return true;
			};
			const auto collect = [&]() -> bool {
				for (const auto &input : c.Authored.DynamicInputs) {
					size_t group = 0;
					const auto *slot = FindDynamicTemplate(c.Entry, input.Id, group);
					if (!slot || slot->Id != "path") continue;
					bool reverse = false;
					for (const auto &sibling : c.Authored.DynamicInputs) {
						size_t other = 0;
						const auto *s = FindDynamicTemplate(c.Entry, sibling.Id, other);
						if (s && s->Id == "reverse" && other == group) reverse = c.Boolean(sibling.Id);
					}
					const auto *value = c.Find(input.Id);
					if (!value) continue;
					if (!ValidRuntimeValue(*value))
						return c.Fail(Status::InvalidValue, "Joined input payload is invalid", input.Id);
					if (const auto *array = std::get_if<ArrayValue>(value)) {
						if (!array->Nested.empty())
							return c.Fail(
								Status::InvalidValue, "Source joined path expects a flat array", input.Id
							);
						if (!array->Items.empty()) {
							for (const auto &item : array->Items) {
								const auto *leaf = std::get_if<ElementValue>(&item.Data);
								if (!leaf || !append(*leaf, reverse, false))
									return c.Fail(
										Status::InvalidValue,
										"Source joined path array contains a non-path",
										input.Id
									);
							}
						} else
							for (const auto &leaf : array->Elements)
								if (!append(leaf, reverse, false)) return false;
					} else {
						const auto isDefault = c.IsCatalogueDefault(input.Id);
						if (!isDefault)
							return c.Fail(
								Status::UnsupportedExecution,
								"Path Join requires resolved source default provenance",
								input.Id
							);
						if (!append(*value, reverse, *isDefault)) return false;
					}
				}
				return true;
			};
			if (!collect()) return false;
			auto scratch = c.ReserveWorkspace(count * (sizeof(const Path2D *) + sizeof(uint8_t)), "path");
			if (!scratch) return false;
			paths.reserve(count);
			reversed.reserve(count);
			count = 0;
			materialize = true;
			if (!collect()) return false;
			return PublishCompose(c, SourcePathOperationKind::Join, paths, reversed);
		}
	}
	std::span<const ExecutorEntry> SourcePathComposeExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.path_offset", PathOffset}, {"pc.path_blend", PathBlend}, {"pc.path_join", PathJoin}
		};
		return entries;
	}
}
