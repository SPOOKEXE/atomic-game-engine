#include "SourceParticle3DRecipe.hpp"

#include "../NodeExecutors.hpp"
#include "../SourcePathPayload3D.hpp"

#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	namespace source_particle3d_recipe {
		constexpr uint64_t MAXIMUM_WORK = 64000000;
		template <class T> bool Read(NodeContext &c, std::string_view id, T &output) {
			const Value *value = c.Find(id);
			if (!value) return true;
			const auto *typed = std::get_if<T>(value);
			if (!typed) return c.Fail(Status::TypeMismatch, "Particle 3D control has wrong type", id);
			output = *typed;
			return true;
		}
		bool Choice(NodeContext &context, std::string_view id, uint32_t maximum, int &result) {
			const double choice = context.SourceChoice(id);
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(choice) || choice < 0 || choice > maximum || std::trunc(choice) != choice)
				return context.Fail(Status::InvalidValue, "Particle 3D selector is invalid", id);
			result = static_cast<int>(choice);
			return true;
		}
		bool Range3(NodeContext &c, std::string_view id, std::array<double, 6> &range) {
			const Value *value = c.Find(id);
			if (!value) return true;
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array || array->Elements.size() != 6 || !array->Nested.empty() || !array->Items.empty())
				return c.Fail(Status::TypeMismatch, "Particle 3D Range3 requires six scalar endpoints", id);
			for (size_t i = 0; i < 6; ++i) {
				const auto *number = std::get_if<double>(&array->Elements[i]);
				const auto *integer = std::get_if<int64_t>(&array->Elements[i]);
				if (!number && !integer)
					return c.Fail(Status::TypeMismatch, "Particle 3D range endpoint must be numeric", id);
				range[i] = number ? *number : double(*integer);
				if (!std::isfinite(range[i]))
					return c.Fail(Status::InvalidValue, "Particle 3D range endpoint must be finite", id);
			}
			return true;
		}
		bool PointItem(NodeContext &context, const SourceArrayItem &item, Vector3 &point) {
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
				const auto *vector = std::get_if<Vector3>(leaf);
				if (!vector)
					return context.Fail(
						Status::TypeMismatch, "Particle 3D spawn data requires vector3 leaves", "spawn_data"
					);
				point = *vector;
			} else {
				const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
				if (!row || row->size() != 3)
					return context.Fail(
						Status::TypeMismatch, "Particle 3D spawn row requires three coordinates", "spawn_data"
					);
				double *axes[]{&point.X, &point.Y, &point.Z};
				for (size_t axis = 0; axis < 3; ++axis) {
					const auto *coordinate = std::get_if<ElementValue>(&(*row)[axis].Data);
					const auto *number = coordinate ? std::get_if<double>(coordinate) : nullptr;
					const auto *integer = coordinate ? std::get_if<int64_t>(coordinate) : nullptr;
					if (!number && !integer)
						return context.Fail(
							Status::TypeMismatch, "Particle 3D spawn coordinate must be numeric", "spawn_data"
						);
					*axes[axis] = number ? *number : double(*integer);
				}
			}
			return MeshFinite(point) ||
				   context.Fail(Status::InvalidValue, "Particle 3D spawn point must be finite", "spawn_data");
		}
		bool AddCharge(NodeContext &c, SourceParticle3DPrepared &p, uint64_t bytes, std::string_view id) {
			auto charge = c.ReserveWorkspace(bytes, id);
			return charge && p.Charge.Merge(std::move(*charge));
		}
		bool CurveControl(NodeContext &c, std::string_view id, const Curve *&curve) {
			const Value *value = c.Find(id);
			if (!value) return true;
			curve = std::get_if<Curve>(value);
			return curve || c.Fail(Status::TypeMismatch, "Particle 3D curve control requires a curve", id);
		}
		bool GradientControl(NodeContext &c, std::string_view id, const Gradient *&gradient) {
			const Value *value = c.Find(id);
			if (!value) return true;
			gradient = std::get_if<Gradient>(value);
			return gradient ||
				   c.Fail(Status::TypeMismatch, "Particle 3D colour control requires a gradient", id);
		}
		bool Work3D(const PathData3D &path, uint64_t &work, size_t depth);
		bool TableWork(size_t count, uint64_t &work, uint64_t perItem = 1) {
			if (count > Limits::MaximumArrayElements || count > (MAXIMUM_WORK - work) / perItem) return false;
			work += count * perItem;
			return true;
		}
		bool Work2D(const Path2D &path, uint64_t &work, size_t depth = 0) {
			if (depth > Limits::MaximumArrayDepth || path.Anchors.size() > Limits::MaximumPathAnchors ||
				path.Weights.size() > Limits::MaximumArrayElements)
				return false;
			const uint64_t add = 1 + path.Anchors.size() * 128 + path.Weights.size();
			if (add > MAXIMUM_WORK - work) return false;
			work += add;
			if (!path.SourceOperation) return true;
			const auto &op = *path.SourceOperation;
			if (op.Inputs.size() > Limits::MaximumArrayElements) return false;
			if (!TableWork(op.WeightCurve.size(), work) || !TableWork(op.CachedLengths.size(), work) ||
				!TableWork(op.Reversed.size(), work) || !TableWork(op.BlendLengths.size(), work) ||
				!TableWork(op.BlendAccumulated.size(), work))
				return false;
			for (const auto &table : op.BlendAccumulated)
				if (!TableWork(table.size(), work)) return false;
			uint64_t childRepeats = 1;
			if (op.Sequential) {
				const auto &sequential = *op.Sequential;
				if (!TableWork(sequential.Accumulated.size(), work) ||
					!TableWork(sequential.FlattenLengths.size(), work) ||
					!TableWork(sequential.FlattenOwners.size(), work) ||
					!TableWork(sequential.Cache.size(), work, 4096))
					return false;
				if (op.Kind == SourcePathOperationKind::Smoothen) {
					if (sequential.SmoothSteps > int64_t(Limits::MaximumArrayElements)) return false;
					childRepeats += 2 * uint64_t(std::max<int64_t>(sequential.SmoothSteps, 0));
				}
			}
			if (op.WeightInput3D) {
				uint64_t childWork = 0;
				if (!Work3D(*op.WeightInput3D, childWork, depth + 1) ||
					childWork > (MAXIMUM_WORK - work) / childRepeats)
					return false;
				work += childWork * childRepeats;
			}
			const uint64_t extra =
				(op.Shape ? op.Shape->Points.size() : 0) + (op.Baked ? op.Baked->Lines.size() : 0);
			if (extra > MAXIMUM_WORK - work) return false;
			work += extra;
			if (op.Baked) {
				if (4096 > MAXIMUM_WORK - work) return false;
				work += 4096;
			}
			if (op.Baked)
				for (const auto &line : op.Baked->Lines) {
					if (line.size() > MAXIMUM_WORK - work) return false;
					work += line.size();
				}
			if (op.Mesh) {
				const uint64_t edges = op.Mesh->Simulation.Edges.size();
				if (edges > MAXIMUM_WORK - work) return false;
				work += edges;
			}
			for (const auto &child : op.Inputs) {
				uint64_t childWork = 0;
				if (!Work2D(child, childWork, depth + 1) || childWork > (MAXIMUM_WORK - work) / childRepeats)
					return false;
				work += childWork * childRepeats;
			}
			return true;
		}
		bool Work3D(const PathData3D &path, uint64_t &work, size_t depth = 0) {
			if (depth > Limits::MaximumArrayDepth || path.Anchors.size() > Limits::MaximumPathAnchors ||
				path.Resolution > Limits::MaximumArrayElements ||
				path.Transforms.size() > Limits::MaximumArrayDepth)
				return false;
			const uint64_t add =
				1 + path.Anchors.size() * (uint64_t(path.Resolution) + 1) * 32 + path.Transforms.size() * 64;
			if (add > MAXIMUM_WORK - work) return false;
			work += add;
			if (path.Source2D && !Work2D(*path.Source2D, work, depth + 1)) return false;
			if (path.SourceOperation) {
				if (path.SourceOperation->Inputs.size() > Limits::MaximumArrayElements) return false;
				for (const auto &child : path.SourceOperation->Inputs)
					if (!child.Data || !Work3D(*child.Data, work, depth + 1)) return false;
			}
			return true;
		}
		bool Path(
			NodeContext &c,
			SourceParticle3DPrepared &p,
			std::string_view id,
			std::unique_ptr<PathData3D> &owned,
			std::unique_ptr<PathRuntime3D> &runtime,
			const PathRuntime3D *&borrowed
		) {
			const Value *value = c.Find(id);
			if (!value || c.IsCatalogueDefault(id) == true) return true;
			const auto *spatial = std::get_if<PathValue3D>(value);
			const auto *planar = std::get_if<Path2D>(value);
			if (!spatial && !planar)
				return c.Fail(Status::TypeMismatch, "Particle 3D path requires a path", id);
			if (spatial && (!spatial->Data || !spatial->Data->SourcePresent)) return true;
			uint64_t work = 0;
			if (!(planar ? Work2D(*planar, work) : Work3D(*spatial->Data, work)) ||
				work > MAXIMUM_WORK / std::max<size_t>(c.ProcessorCount, 1))
				return c.Fail(Status::LimitExceeded, "Particle 3D path preparation exceeds work bounds", id);
			if (!(planar ? ValidSourcePath2D(*planar) : ValidSourcePath3D(*spatial->Data)))
				return c.Fail(Status::InvalidValue, "Particle 3D source path is invalid", id);
			uint64_t bytes = sizeof(PathRuntime3D);
			if (planar)
				bytes =
					MeshAddBytes(bytes, MeshAddBytes(sizeof(PathData3D), SourcePath2DBytes<false>(*planar)));
			if (!AddCharge(c, p, bytes, id)) return false;
			const PathData3D *data = spatial ? spatial->Data.operator->() : nullptr;
			if (planar) {
				owned = std::make_unique<PathData3D>();
				owned->Source2D = *planar;
				data = owned.get();
			}
			runtime = std::make_unique<PathRuntime3D>(*data, &c);
			if (!runtime->Valid())
				return c.FailureCode != Status::Ok
						   ? false
						   : c.Fail(Status::InvalidValue, "Particle 3D path runtime is invalid", id);
			borrowed = runtime.get();
			p.Controls.PathSampleWork = MeshAddBytes(p.Controls.PathSampleWork, work);
			return true;
		}
	}
	bool PrepareSourceParticle3DControls(NodeContext &context, SourceParticle3DPrepared &prepared) {
		using namespace source_particle3d_recipe;
		ENGINE_PROFILE("imagegraph.particle3d.prepare");
		auto &c = prepared.Controls;
		const double seed = context.Scalar("seed", 0);
		if (!std::isfinite(seed) || seed < 0 || seed > UINT32_MAX || std::trunc(seed) != seed)
			return context.Fail(Status::InvalidValue, "Particle 3D seed must be a uint32", "seed");
		c.Seed = static_cast<uint32_t>(seed);
		c.Spawn = context.Boolean("spawn", true);
		c.Trigger = context.Boolean("spawn_trigger");
		c.Billboard = context.Boolean("billboard");
		c.FollowVelocity = context.Boolean("follow_velocity");
		int type = 0, source = 0, shape = 0;
		if (!Choice(context, "spawn_type", 2, type) || !Choice(context, "spawn_source", 3, source) ||
			!Choice(context, "spawn_shape", 2, shape))
			return false;
		c.SpawnType = static_cast<SourceParticle3DSpawnType>(type);
		c.SpawnSource = static_cast<SourceParticle3DSpawnSource>(source);
		c.SpawnShape = static_cast<SourceParticle3DSpawnShape>(shape);
		c.SpawnDelay = context.Integer("spawn_delay", 4);
		c.BurstDuration = context.Integer("burst_duration", 1);
		c.PreRender = context.Integer("pre_render", -1);
		c.Physics = context.Boolean("use_physics");
		c.Ground = context.Boolean("collide_ground");
		c.Follow = context.Boolean("follow_path");
		c.Loop = context.Boolean("loop", true);
		c.BounceAmount = context.Scalar("bounce_amount", .5);
		c.BounceFriction = context.Scalar("bounce_friction", .1);
		const double selection = context.SourceChoice("attribute_array_select_color_by_index", 0);
		if (!std::isfinite(selection) || selection < 0 || selection > 2 || std::trunc(selection) != selection)
			return context.Fail(
				Status::InvalidValue, "Particle 3D palette selection is invalid", "color_by_index"
			);
		c.PaletteSelection = static_cast<uint32_t>(selection);
		const uint64_t frames = context.Timeline ? context.Timeline->Frames : 1;
		if (!frames || frames > UINT32_MAX)
			return context.Fail(
				Status::LimitExceeded, "Particle 3D timeline exceeds frame bounds", "pre_render"
			);
		c.TotalFrames = static_cast<uint32_t>(frames);
		if (context.FailureCode != Status::Ok) return false;
		if (!Read(context, "spawn_amount", c.SpawnAmount) || !Read(context, "lifespan", c.Lifespan) ||
			!Read(context, "spawn_origin", c.SpawnOrigin) || !Read(context, "spawn_span", c.SpawnSpan) ||
			!Read(context, "spawn_rotation", c.SpawnRotation) ||
			!Read(context, "follow_spawn_shape", c.ShapeVelocity) || !Read(context, "size", c.Size) ||
			!Read(context, "alpha", c.Alpha) || !Read(context, "gravity", c.Gravity) ||
			!Read(context, "ground_offset", c.GroundOffset) || !Read(context, "path_range", c.PathRange))
			return false;
		if (!Range3(context, "velocity", c.Velocity) || !Range3(context, "acceleration", c.Acceleration) ||
			!Range3(context, "rotation", c.Rotation) ||
			!Range3(context, "rotational_speed", c.RotationSpeed) || !Range3(context, "scale", c.Scale))
			return false;
		if (!CurveControl(context, "velocity_over_lifespan", c.SpeedCurve) ||
			!CurveControl(context, "rotational_speed_over_lifespan", c.RotationCurve) ||
			!CurveControl(context, "size_over_lifespan", c.SizeCurve) ||
			!CurveControl(context, "alpha_over_lifespan", c.AlphaCurve) ||
			!CurveControl(context, "path_deviation", c.PathCurve) ||
			!GradientControl(context, "color_over_lifetime", c.LifetimeColour) ||
			!GradientControl(context, "random_blend", c.RandomColour))
			return false;
		if (const Value *value = context.Find("color_by_index")) {
			const auto *palette = std::get_if<ArrayValue>(value);
			if (!palette || !palette->Nested.empty() || !palette->Items.empty())
				return context.Fail(
					Status::TypeMismatch, "Particle 3D palette requires colour leaves", "color_by_index"
				);
			if (palette->Elements.size() > Limits::MaximumArrayElements ||
				palette->Elements.size() > MAXIMUM_WORK / std::max<size_t>(context.ProcessorCount, 1))
				return context.Fail(
					Status::LimitExceeded, "Particle 3D palette exceeds bounds", "color_by_index"
				);
			if (!AddCharge(context, prepared, palette->Elements.size() * sizeof(Colour), "color_by_index"))
				return false;
			prepared.Palette.reserve(palette->Elements.size());
			for (const auto &leaf : palette->Elements) {
				const auto *colour = std::get_if<Colour>(&leaf);
				if (!colour)
					return context.Fail(
						Status::TypeMismatch, "Particle 3D palette requires colours", "color_by_index"
					);
				prepared.Palette.push_back(*colour);
			}
		}
		c.Palette = prepared.Palette;
		if (c.SpawnSource == SourceParticle3DSpawnSource::DirectData) {
			if (const Value *value = context.Find("spawn_data")) {
				const auto *data = std::get_if<ArrayValue>(value);
				if (!data)
					return context.Fail(
						Status::TypeMismatch,
						"Particle 3D spawn data requires vector3 leaves or triples",
						"spawn_data"
					);
				const size_t count = !data->Items.empty()	? data->Items.size()
									 : data->Nested.empty() ? data->Elements.size()
															: data->Nested.size();
				if ((!data->Items.empty() && (!data->Nested.empty() || !data->Elements.empty())) ||
					(!data->Nested.empty() && !data->Elements.empty()) ||
					count > Limits::MaximumArrayElements ||
					count > MAXIMUM_WORK / 3 / std::max<size_t>(context.ProcessorCount, 1))
					return context.Fail(
						Status::LimitExceeded, "Particle 3D spawn data exceeds bounds", "spawn_data"
					);
				if (!AddCharge(context, prepared, count * sizeof(Vector3), "spawn_data")) return false;
				prepared.SpawnData.reserve(count);
				if (!data->Items.empty()) {
					for (const auto &item : data->Items) {
						Vector3 point;
						if (!PointItem(context, item, point)) return false;
						prepared.SpawnData.push_back(point);
					}
				} else if (data->Nested.empty())
					for (const auto &leaf : data->Elements) {
						const auto *point = std::get_if<Vector3>(&leaf);
						if (!point || !MeshFinite(*point))
							return context.Fail(
								Status::TypeMismatch,
								"Particle 3D spawn data requires finite vector3 points",
								"spawn_data"
							);
						prepared.SpawnData.push_back(*point);
					}
				else
					for (const auto &row : data->Nested) {
						if (row.size() != 3)
							return context.Fail(
								Status::TypeMismatch,
								"Particle 3D spawn row requires three coordinates",
								"spawn_data"
							);
						Vector3 point;
						double *axes[]{&point.X, &point.Y, &point.Z};
						for (size_t axis = 0; axis < 3; ++axis) {
							const auto *number = std::get_if<double>(&row[axis]);
							const auto *integer = std::get_if<int64_t>(&row[axis]);
							if (!number && !integer)
								return context.Fail(
									Status::TypeMismatch,
									"Particle 3D spawn coordinate must be numeric",
									"spawn_data"
								);
							*axes[axis] = number ? *number : double(*integer);
						}
						if (!MeshFinite(point))
							return context.Fail(
								Status::InvalidValue, "Particle 3D spawn point must be finite", "spawn_data"
							);
						prepared.SpawnData.push_back(point);
					}
			}
			c.SpawnData = prepared.SpawnData;
		}
		if (c.SpawnSource == SourceParticle3DSpawnSource::MeshVertices) {
			if (const Value *value = context.Find("spawn_mesh")) {
				const auto *mesh = std::get_if<MeshValue3D>(value);
				if (!mesh)
					return context.Fail(
						Status::TypeMismatch, "Particle 3D spawn mesh requires an object mesh", "spawn_mesh"
					);
				if (mesh->Data) {
					if (mesh->Data->Instanced || !mesh->Data->CpuVerticesPresent)
						return context.Fail(
							Status::UnsupportedExecution,
							"Particle 3D spawn mesh requires raw object vertices",
							"spawn_mesh"
						);
					const auto &parts = mesh->Data->Parts;
					if (parts.size() > Limits::MaximumArrayElements)
						return context.Fail(
							Status::LimitExceeded, "Particle 3D spawn mesh exceeds group bounds", "spawn_mesh"
						);
					uint64_t count = 0;
					for (const auto &part : parts) {
						if (part.Vertices.size() > Limits::MaximumArrayElements - count)
							return context.Fail(
								Status::LimitExceeded,
								"Particle 3D spawn mesh exceeds vertex bounds",
								"spawn_mesh"
							);
						count += part.Vertices.size();
					}
					if (count > MAXIMUM_WORK / 3 / std::max<size_t>(context.ProcessorCount, 1))
						return context.Fail(
							Status::LimitExceeded,
							"Particle 3D spawn mesh scan exceeds work bounds",
							"spawn_mesh"
						);
					if (!AddCharge(
							context,
							prepared,
							count * sizeof(Vector3) + parts.size() * (sizeof(std::vector<Vector3>) +
																	  sizeof(std::span<const Vector3>)),
							"spawn_mesh"
						))
						return false;
					prepared.SpawnMesh.reserve(parts.size());
					prepared.SpawnMeshViews.reserve(parts.size());
					for (const auto &part : parts) {
						auto &points = prepared.SpawnMesh.emplace_back();
						points.reserve(part.Vertices.size());
						for (const auto &vertex : part.Vertices) {
							if (!MeshFinite(vertex.Position))
								return context.Fail(
									Status::InvalidValue,
									"Particle 3D spawn vertex must be finite",
									"spawn_mesh"
								);
							points.push_back(vertex.Position);
						}
						prepared.SpawnMeshViews.emplace_back(points);
					}
				}
			}
			c.SpawnMeshVertices = prepared.SpawnMeshViews;
		}
		if (c.SpawnSource == SourceParticle3DSpawnSource::Path &&
			!Path(context, prepared, "spawn_path", prepared.SpawnPlanar, prepared.SpawnRuntime, c.SpawnPath))
			return false;
		if (c.Follow &&
			!Path(context, prepared, "path", prepared.FollowPlanar, prepared.FollowRuntime, c.FollowPath))
			return false;
		if (!c.FollowPath) c.Follow = false;
		return context.FailureCode == Status::Ok;
	}
}
