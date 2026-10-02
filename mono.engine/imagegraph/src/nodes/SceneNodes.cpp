#include "../SourceGradient.hpp"
#include "../SourceMeshTransform.hpp"
#include "../SourceQuaternion.hpp"
#include "../SourceRandom.hpp"
#include "Curve.hpp"
#include "Families.hpp"
#include "Path3D.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	bool EvaluateSourcePath3D(NodeContext &context);
	bool TransformSourcePath3D(NodeContext &context);
	bool SourceMeshPathRevolve(NodeContext &context);
	bool SourceMeshPathExtrude(NodeContext &context);
	bool ProjectSourcePath3D(NodeContext &context);
	bool SourceMeshInstancer(NodeContext &context);
	bool SourceMeshWall(NodeContext &context);
	bool SourceMeshPlanarExtrude(NodeContext &context);
	bool SourceMeshSurfaceExtrude(NodeContext &context);
	bool SourceMeshSliceStack(NodeContext &context);
	bool SourceMeshFromSdf(NodeContext &context);
	namespace {
		bool ReadVector(NodeContext &context, std::string_view port, Vector3 &output) {
			const Value *value = context.Find(port);
			if (!value) return true;
			if (const auto *vector = std::get_if<Vector3>(value))
				output = *vector;
			else if (const auto *number = std::get_if<double>(value))
				output = {*number, *number, *number};
			else if (const auto *number = std::get_if<int64_t>(value))
				output = {double(*number), double(*number), double(*number)};
			else
				return context.Fail(Status::TypeMismatch, "3D transform requires Vector3", port);
			return MeshFinite(output) ||
				   context.Fail(Status::InvalidValue, "3D transform must be finite", port);
		}

		bool Light(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.scene.light");
			LightValue3D output;
			if (!context.Boolean("active", true)) {
				context.SetValue("light", std::move(output));
				return context.FailureCode == Status::Ok;
			}
			if (!context.ReserveOutput(sizeof(LightData3D) + 32, "light")) return false;
			auto &light = output.Data.emplace();
			light.Kind =
				context.Authored.Type == "pc.3_d_light_point" ? LightKind3D::Point : LightKind3D::Directional;
			if (!ReadVector(context, "position", light.Transform.Position) ||
				!ReadVector(context, "anchor", light.Transform.Anchor) ||
				!ReadVector(context, "scale", light.Transform.Scale))
				return false;
			if (const Value *value = context.Find("rotation")) {
				const auto *rotation = std::get_if<Quaternion>(value);
				if (!rotation)
					return context.Fail(
						Status::TypeMismatch, "light rotation requires Quaternion", "rotation"
					);
				light.Transform.Rotation = *rotation;
			}
			if (const Value *value = context.Find("color")) {
				const auto *color = std::get_if<Colour>(value);
				if (!color) return context.Fail(Status::TypeMismatch, "light color requires Colour", "color");
				light.Color = *color;
			}
			light.Intensity = context.Scalar("intensity", 1);
			light.Radius = context.Scalar("radius", 4);
			light.CastShadow = context.Boolean("cast_shadow");
			light.ShadowMapSize = context.Integer("shadow_map_size", 1024);
			light.ShadowMapScale = context.Integer("shadow_map_scale", 16);
			light.ShadowBias = context.Scalar("shadow_bias", .01);
			if (light.Kind == LightKind3D::Point)
				light.ShadowBias *= 100;
			else {
				Vector3 direction{
					0.0 - light.Transform.Position.X,
					0.0 - light.Transform.Position.Y,
					0.0 - light.Transform.Position.Z
				};
				const double length = std::hypot(direction.X, direction.Y, direction.Z);
				if (length) direction = {direction.X / length, direction.Y / length, direction.Z / length};
				const double horizontal = std::hypot(direction.X, direction.Y),
							 pitch = std::asin(direction.Z) * 180 / std::numbers::pi,
							 yaw = std::atan2(direction.Y, direction.X) * 180 / std::numbers::pi;
				light.Transform.Rotation = SourceQuaternionFromEuler(horizontal ? 180 : 0, -pitch, -yaw);
			}
			if (context.FailureCode != Status::Ok) return false;
			if (!ValidLightPayload(output))
				return context.Fail(Status::InvalidValue, "light parameters must be finite", "light");
			context.SetValue("light", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool PointAffector(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.scene.point_affector");
			Vector3 position{}, scale{1, 1, 1}, point{}, initial{}, final{};
			if (!ReadVector(context, "position", position) || !ReadVector(context, "scale", scale) ||
				!ReadVector(context, "points", point) || !ReadVector(context, "initial_value", initial) ||
				!ReadVector(context, "final_value", final))
				return false;
			const double shape = context.SourceChoice("shape"),
						 falloff = context.Scalar("falloff_distance", .5);
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(falloff))
				return context.Fail(
					Status::InvalidValue, "affector falloff must be finite", "falloff_distance"
				);
			double distance = 0, inner = 0, outer = 1;
			if (shape == 0) {
				distance = std::hypot(point.X - position.X, point.Y - position.Y, point.Z - position.Z);
				const double maximum = std::max({scale.X, scale.Y, scale.Z});
				inner = (maximum - falloff) / 2;
				outer = (maximum + falloff) / 2;
			} else if (shape == 1) {
				Quaternion rotation{};
				if (const Value *value = context.Find("rotation")) {
					const auto *typed = std::get_if<Quaternion>(value);
					if (!typed)
						return context.Fail(
							Status::TypeMismatch, "affector rotation requires Quaternion", "rotation"
						);
					rotation = *typed;
				}
				const Vector3 normal = SourceRotate(rotation, {0, 0, 1});
				distance = ((point.X - position.X) * normal.X + (point.Y - position.Y) * normal.Y +
							(point.Z - position.Z) * normal.Z) /
						   std::hypot(normal.X, normal.Y, normal.Z);
				inner = -falloff / 2;
				outer = falloff / 2;
			} else
				return context.Fail(
					Status::UnsupportedExecution, "affector shape has no defined source case", "shape"
				);
			Vector3 output;
			if (distance >= outer)
				output = initial;
			else if (distance <= inner)
				output = final;
			else {
				double influence = (distance - inner) / falloff;
				if (const Value *value = context.Find("falloff_curve")) {
					const auto *curve = std::get_if<Curve>(value);
					if (!curve)
						return context.Fail(
							Status::TypeMismatch, "affector falloff requires Curve", "falloff_curve"
						);
					if (!ValidRuntimeValue(*value))
						return context.Fail(
							Status::InvalidValue, "affector falloff curve is invalid", "falloff_curve"
						);
					const double index = std::clamp(influence, 0.0, 1.0) * 100;
					const double lower = std::floor(index), upper = std::ceil(index);
					influence = CurveLerp(
						EvalCurveX(*curve, lower * .01, .00001),
						EvalCurveX(*curve, upper * .01, .00001),
						index - lower
					);
				}
				output = {
					CurveLerp(final.X, initial.X, influence),
					CurveLerp(final.Y, initial.Y, influence),
					CurveLerp(final.Z, initial.Z, influence)
				};
			}
			if (!MeshFinite(output))
				return context.Fail(Status::InvalidValue, "affector produced nonfinite values", "output");
			context.SetValue("output", output);
			return context.FailureCode == Status::Ok;
		}

		bool Mirror(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.scene.mirror");
			const Value *value = context.Find("mesh");
			const auto *mesh = value ? std::get_if<MeshValue3D>(value) : nullptr;
			const auto *scene = value ? std::get_if<SceneValue3D>(value) : nullptr;
			if ((!mesh || !mesh->Data) && (!scene || !scene->Data)) {
				context.SetValue("mesh", MeshValue3D{});
				return context.FailureCode == Status::Ok;
			}
			MeshTransform3D mirror;
			Vector3 position{};
			if (!ReadVector(context, "position", position)) return false;
			const int64_t axis = context.Integer("axis");
			if (axis < 0 || axis > 2)
				return context.Fail(Status::InvalidValue, "mirror axis has no source case", "axis");
			mirror.Mirror = true;
			mirror.ShowOriginal = context.Boolean("show_original");
			if (axis == 0) {
				mirror.Position.X = position.X * 2;
				mirror.Scale.X = -1;
			} else if (axis == 1) {
				mirror.Position.Y = position.Y * 2;
				mirror.Scale.Y = -1;
			} else {
				mirror.Position.Z = position.Z * 2;
				mirror.Scale.Z = -1;
			}
			if (context.FailureCode != Status::Ok) return false;
			if (!MeshFinite(mirror.Position))
				return context.Fail(Status::InvalidValue, "mirror position overflows", "position");
			if (mesh) {
				if (!ValidMeshPayload(*mesh) ||
					mesh->Data->LocalTransforms.size() >= Limits::MaximumArrayDepth)
					return context.Fail(Status::LimitExceeded, "mirror mesh exceeds transform depth", "mesh");
				if (!context.ReserveOutput(
						MeshStorageBytes<true>(*mesh) + sizeof(MeshTransform3D) * 2 + 32, "mesh"
					))
					return false;
				MeshValue3D output = *mesh;
				output.Data->LocalTransforms.insert(output.Data->LocalTransforms.begin(), mirror);
				context.SetValue("mesh", std::move(output));
			} else {
				if (!ValidScenePayload(*scene))
					return context.Fail(Status::InvalidValue, "mirror scene payload is invalid", "mesh");
				const uint64_t bytes = MeshAddBytes(
					SceneStorageBytes<true>(*scene), sizeof(SceneData3D) + sizeof(SceneObject3D)
				);
				if (!context.ReserveOutput(bytes + 32, "mesh")) return false;
				SceneValue3D output;
				auto &wrapper = output.Data.emplace();
				wrapper.Transform = mirror;
				wrapper.Objects.push_back({scene->Data});
				if (!ValidScenePayload(output))
					return context.Fail(Status::LimitExceeded, "mirror scene exceeds nesting depth", "mesh");
				context.SetValue("mesh", std::move(output));
			}
			return context.FailureCode == Status::Ok;
		}

		struct SceneCost {
			uint64_t Objects = 0, Bytes = sizeof(SceneData3D);
			size_t TotalObjects = 0;
		};
		template <class T> bool CountObject(const T &value, SceneCost &cost) {
			if constexpr (std::is_same_v<T, MeshValue3D>) {
				if (!value.Data) return true;
				cost.Bytes = MeshAddBytes(cost.Bytes, MeshStorageBytes<true>(value));
			} else if constexpr (std::is_same_v<T, LightValue3D>) {
				if (!value.Data) return true;
				cost.Bytes = MeshAddBytes(cost.Bytes, sizeof(LightData3D));
			} else if constexpr (std::is_same_v<T, SceneValue3D>) {
				if (!value.Data) return true;
				if (!ValidSceneData(*value.Data, cost.TotalObjects, 1)) return false;
				cost.Bytes = MeshAddBytes(cost.Bytes, SceneStorageBytes<true>(*value.Data, 1));
			} else
				return true;
			++cost.Objects;
			++cost.TotalObjects;
			cost.Bytes = MeshAddBytes(cost.Bytes, sizeof(SceneObject3D));
			return cost.TotalObjects <= Limits::MaximumArrayElements &&
				   cost.Bytes <= Limits::MaximumArrayBytes;
		}
		template <class T> void AppendObject(const T &value, SceneData3D &scene) {
			if constexpr (std::is_same_v<T, MeshValue3D> || std::is_same_v<T, LightValue3D>) {
				if (value.Data) scene.Objects.push_back({value});
			} else if constexpr (std::is_same_v<T, SceneValue3D>) {
				if (value.Data) scene.Objects.push_back({value.Data});
			}
		}
		template <class Fn> bool EachObject(const Value &value, Fn &&fn) {
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				for (const auto &element : array->Elements)
					if (!std::visit(fn, element)) return false;
				for (const auto &item : array->Items)
					if (const auto *element = std::get_if<ElementValue>(&item.Data))
						if (!std::visit(fn, *element)) return false;
				return true;
			}
			return std::visit(fn, value);
		}
		bool Scene(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.scene.collect");
			SceneCost cost;
			for (const auto &port : context.Authored.DynamicInputs) {
				const Value *value = context.Find(port.Id);
				if (!value) continue;
				if (!ValidRuntimeValue(*value))
					return context.Fail(Status::InvalidValue, "scene object is invalid", port.Id);
				if (!EachObject(*value, [&](const auto &item) { return CountObject(item, cost); }))
					return context.Fail(
						Status::LimitExceeded, "scene collection exceeds payload caps", port.Id
					);
			}
			if (!context.ReserveOutput(cost.Bytes + 32, "scene")) return false;
			SceneValue3D output;
			auto &scene = output.Data.emplace();
			scene.Objects.reserve(size_t(cost.Objects));
			for (const auto &port : context.Authored.DynamicInputs)
				if (const Value *value = context.Find(port.Id))
					EachObject(*value, [&](const auto &item) {
						AppendObject(item, scene);
						return true;
					});
			if (!ValidScenePayload(output))
				return context.Fail(Status::LimitExceeded, "nested scene exceeds payload caps", "scene");
			context.SetValue("scene", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		std::optional<double> Number(const ElementValue &value) {
			if (const auto *number = std::get_if<double>(&value)) return *number;
			if (const auto *number = std::get_if<int64_t>(&value)) return double(*number);
			return std::nullopt;
		}
		std::optional<Vector3> NumericRow(const std::vector<ElementValue> &row) {
			if (row.size() < 3) return std::nullopt;
			const auto x = Number(row[0]), y = Number(row[1]), z = Number(row[2]);
			if (!x || !y || !z) return std::nullopt;
			return Vector3{*x, *y, *z};
		}
		std::optional<Vector3> TransformRow(const Value *value, size_t index) {
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (!array) return std::nullopt;
			if (index < array->Nested.size()) return NumericRow(array->Nested[index]);
			if (index < array->Elements.size()) {
				if (const auto *row = std::get_if<Vector3>(&array->Elements[index])) return *row;
			}
			if (index < array->Items.size()) {
				const auto &item = array->Items[index];
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (const auto *row = std::get_if<Vector3>(leaf)) return *row;
				}
				if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
					if (children->size() < 3) return std::nullopt;
					std::array<double, 3> numbers{};
					for (size_t i = 0; i < 3; ++i) {
						const auto *leaf = std::get_if<ElementValue>(&(*children)[i].Data);
						const auto number = leaf ? Number(*leaf) : std::nullopt;
						if (!number) return std::nullopt;
						numbers[i] = *number;
					}
					return Vector3{numbers[0], numbers[1], numbers[2]};
				}
			}
			return std::nullopt;
		}

		bool RepeatScene(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.scene.repeat");
			const Value *source = context.Find("objects");
			const double mode = context.SourceChoice("object_type"),
						 pattern = context.SourceChoice("pattern");
			Vector3 start{}, scale{1, 1, 1}, shift{1, 0, 0}, shiftY{0, 1, 0}, shiftZ{0, 0, 1}, scaleShift{},
				grid{2, 2, 1};
			if (!ReadVector(context, "starting_position", start) ||
				!ReadVector(context, "starting_scale", scale) ||
				!ReadVector(context, "shift_position", shift) ||
				!ReadVector(context, "shift_position_y", shiftY) ||
				!ReadVector(context, "shift_position_z", shiftZ) ||
				!ReadVector(context, "shift_scale", scaleShift) || !ReadVector(context, "grid", grid))
				return false;
			Quaternion initial{}, rotationShift{};
			for (auto [id, target] :
				 {std::pair<std::string_view, Quaternion *>{"starting_rotation", &initial},
				  {"shift_rotation", &rotationShift}}) {
				if (const Value *value = context.Find(id)) {
					const auto *q = std::get_if<Quaternion>(value);
					if (!q || !MeshFinite(*q))
						return context.Fail(
							Status::TypeMismatch, "repeat rotation requires a finite quaternion", id
						);
					*target = *q;
				}
			}
			const auto *array = source ? std::get_if<ArrayValue>(source) : nullptr;
			int64_t amount = context.Integer("amount", 2);
			if (mode == 1)
				amount =
					array ? int64_t(!array->Items.empty() ? array->Items.size() : array->Elements.size()) : 0;
			else if (pattern == 1) {
				const double count = grid.X * grid.Y * grid.Z;
				if (!std::isfinite(count) || count > Limits::MaximumArrayElements)
					return context.Fail(Status::LimitExceeded, "repeat grid exceeds object caps", "grid");
				amount = int64_t(count);
			}
			const double seedValue = context.Scalar("seed", double(uint32_t(context.Request.Seed))),
						 radius = context.Scalar("radius", 1), look = context.Scalar("look_at_center"),
						 follow = context.Scalar("follow_path");
			if (context.FailureCode != Status::Ok) return false;
			if (mode != 0 && mode != 1)
				return context.Fail(
					Status::UnsupportedExecution, "repeat object mode has no source case", "object_type"
				);
			if (pattern != 0 && pattern != 1 && pattern != 2)
				return context.Fail(
					Status::UnsupportedExecution, "repeat pattern has no source case", "pattern"
				);
			if (!std::isfinite(seedValue) || seedValue < INT32_MIN || seedValue > UINT32_MAX ||
				!std::isfinite(radius) || !std::isfinite(look) || !std::isfinite(follow))
				return context.Fail(
					Status::InvalidValue, "repeat controls must be finite and seed bounded", "seed"
				);
			if (amount > int64_t(Limits::MaximumArrayElements))
				return context.Fail(Status::LimitExceeded, "repeat amount exceeds object caps", "amount");
			if (pattern == 1 && (grid.X <= 0 || grid.Y <= 0 || grid.Z <= 0)) amount = 0;
			std::array<double, 6> posRange{}, rotRange{}, scaleRange{};
			auto range = [&](std::string_view port, std::array<double, 6> &data) {
				const Value *value = context.Find(port);
				if (!value) return true;
				const auto *row = std::get_if<ArrayValue>(value);
				if (!row || !row->Nested.empty() || !row->Items.empty() || row->Elements.size() < 6)
					return context.Fail(Status::TypeMismatch, "scatter requires six scalar fields", port);
				for (size_t i = 0; i < 6; ++i) {
					auto n = Number(row->Elements[i]);
					if (!n || !std::isfinite(*n))
						return context.Fail(Status::InvalidValue, "scatter fields must be finite", port);
					data[i] = *n;
				}
				return true;
			};
			if (!range("position_scatter", posRange) || !range("rotation_scatter", rotRange) ||
				!range("scale_scatter", scaleRange))
				return false;
			uint64_t bytes = sizeof(SceneData3D);
			size_t totalObjects = 0, copies = 0;
			auto each = [&](size_t i, auto &&fn) {
				if (!source) return true;
				if (mode == 0) return std::visit(fn, *source);
				if (!array) return true;
				if (!array->Items.empty()) {
					if (const auto *leaf = std::get_if<ElementValue>(&array->Items[i].Data))
						return std::visit(fn, *leaf);
					return true;
				}
				return std::visit(fn, array->Elements[i]);
			};
			for (int64_t i = 0; i < amount; ++i) {
				SceneCost cost;
				if (!each(size_t(i), [&](const auto &value) { return CountObject(value, cost); }))
					return context.Fail(Status::LimitExceeded, "repeat source exceeds scene caps", "objects");
				if (!cost.Objects) continue;
				totalObjects += cost.TotalObjects + 1;
				++copies;
				bytes = MeshAddBytes(bytes, MeshAddBytes(cost.Bytes, sizeof(SceneObject3D)));
				if (totalObjects > Limits::MaximumArrayElements || bytes > Limits::MaximumArrayBytes)
					return context.Fail(Status::LimitExceeded, "repeat output exceeds scene caps", "scene");
			}
			if (source && !ValidRuntimeValue(*source))
				return context.Fail(Status::InvalidValue, "repeat objects are invalid", "objects");
			if (!context.ReserveOutput(bytes + 64, "scene")) return false;
			const Value *path = context.Find("shift_path");
			const auto *spatial = path ? std::get_if<PathValue3D>(path) : nullptr;
			const auto *planar = path ? std::get_if<Path2D>(path) : nullptr;
			std::optional<PathRuntime3D> space;
			std::optional<PathRuntime> flat;
			if (pattern == 0 && spatial && spatial->Data && spatial->Data->SourcePresent) {
				space.emplace(*spatial->Data, &context);
				if (!space->Valid())
					return context.Fail(Status::InvalidValue, "repeat path length is invalid", "shift_path");
			} else if (pattern == 0 && planar &&
					   (!planar->Anchors.empty() || context.IsLinked("shift_path"))) {
				flat.emplace();
				if (!flat->Init(context, *planar)) return false;
			}
			auto pathPoint = [&](double ratio) {
				if (space) return space->Ratio(ratio).Position;
				auto point = flat->PointRatio(ratio);
				return Vector3{point.X, point.Y, 0};
			};
			SceneValue3D result;
			auto &scene = result.Data.emplace();
			scene.Objects.reserve(copies);
			const Vector3 startEuler = SourceQuaternionToEuler(initial),
						  shiftEuler = SourceQuaternionToEuler(rotationShift);
			const double ratioStep = amount >= 1 ? 1.0 / (amount - 1) : 0;
			const bool uniform = context.Boolean("scale_uniform", true);
			for (int64_t i = 0; i < amount; ++i) {
				SceneCost cost;
				each(size_t(i), [&](const auto &value) { return CountObject(value, cost); });
				if (!cost.Objects) continue;
				SourceRandom random(uint32_t(int64_t(seedValue)) + uint32_t(i + 1) * 78);
				MeshTransform3D transform;
				transform.Position = {
					start.X + random.Range(posRange[0], posRange[1]),
					start.Y + random.Range(posRange[2], posRange[3]),
					start.Z + random.Range(posRange[4], posRange[5])
				};
				Vector3 angles{
					startEuler.X + shiftEuler.X * i + random.Range(rotRange[0], rotRange[1]),
					startEuler.Y + shiftEuler.Y * i + random.Range(rotRange[2], rotRange[3]),
					startEuler.Z + shiftEuler.Z * i + random.Range(rotRange[4], rotRange[5])
				};
				auto add = [](Vector3 &a, Vector3 b) { a = {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; };
				if (pattern == 0) {
					add(transform.Position, {shift.X * i, shift.Y * i, shift.Z * i});
					if (space || flat) {
						const double ratio = i * ratioStep;
						add(transform.Position, pathPoint(std::clamp(ratio, 0.0, .9999)));
						if (follow > 0) {
							const auto a = pathPoint(std::clamp(ratio - ratioStep / 2, 0.0, .9999)),
									   b = pathPoint(std::clamp(ratio + ratioStep / 2, 0.0, .9999));
							const Vector3 forward{b.Y - a.Y, b.X - a.X, b.Z - a.Z};
							if (forward != Vector3{}) {
								const double h = std::sqrt(.5);
								auto e = SourceQuaternionToEuler(SourceQuaternionMultiply(
									SourceQuaternionLook(forward), Quaternion{h, 0, 0, h}
								));
								add(angles, {e.X * follow, e.Y * follow, e.Z * follow});
							}
						}
					}
				} else if (pattern == 1) {
					double xy = grid.X * grid.Y, z = std::floor(i / xy),
						   y = std::floor((i - z * xy) / grid.X), x = std::fmod(i - z * xy, grid.X);
					add(transform.Position,
						{shift.X * x + shiftY.X * y + shiftZ.X * z,
						 shift.Y * x + shiftY.Y * y + shiftZ.Y * z,
						 shift.Z * x + shiftY.Z * y + shiftZ.Z * z});
				} else {
					const double degrees = 360.0 / amount * i, radians = degrees * std::numbers::pi / 180;
					add(transform.Position,
						{shift.X * i + radius * std::cos(radians),
						 shift.Y * i - radius * std::sin(radians),
						 shift.Z * i});
					if (look > 0) angles.Z += degrees * look;
				}
				transform.Scale = {
					scale.X + scaleShift.X * i + random.Range(scaleRange[0], scaleRange[1]),
					scale.Y + scaleShift.Y * i + random.Range(scaleRange[2], scaleRange[3]),
					scale.Z + scaleShift.Z * i + random.Range(scaleRange[4], scaleRange[5])
				};
				if (uniform) transform.Scale.Y = transform.Scale.Z = transform.Scale.X;
				for (auto [port, target] :
					 {std::pair<std::string_view, Vector3 *>{"positions", &transform.Position},
					  {"rotations", &angles},
					  {"scales", &transform.Scale}}) {
					if (auto row = TransformRow(context.Find(port), size_t(i)))
						add(*target, *row);
					else if (const auto *a =
								 context.Find(port) ? std::get_if<ArrayValue>(context.Find(port)) : nullptr;
							 a && (!a->Nested.empty() || !a->Items.empty() ||
								   a->ElementType == ValueType::Vector3))
						return context.Fail(
							Status::InvalidValue, "repeat transform row is missing or incomplete", port
						);
				}
				transform.Rotation = SourceQuaternionFromEuler(angles.X, angles.Y, angles.Z);
				OwnedPayload3D<SceneData3D> wrapper;
				auto &group = wrapper.emplace();
				group.Transform = transform;
				group.Objects.reserve(1);
				each(size_t(i), [&](const auto &value) {
					AppendObject(value, group);
					return true;
				});
				scene.Objects.push_back({std::move(wrapper)});
			}
			if (!ValidScenePayload(result))
				return context.Fail(Status::InvalidValue, "repeat produced an invalid scene", "scene");
			context.SetValue("scene", std::move(result));
			return context.FailureCode == Status::Ok;
		}

		bool TransformScene(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.scene.transform");
			const Value *value = context.Find("scene");
			const auto *source = value ? std::get_if<SceneValue3D>(value) : nullptr;
			if (!source || !source->Data) {
				context.SetValue("scene", SceneValue3D{});
				return context.FailureCode == Status::Ok;
			}
			if (!ValidScenePayload(*source))
				return context.Fail(Status::InvalidValue, "scene input is invalid", "scene");
			const double positionMode = context.SourceChoice("positioning_type"),
						 rotationMode = context.SourceChoice("rotating_type"),
						 scaleMode = context.SourceChoice("scaling_type");
			if (context.FailureCode != Status::Ok) return false;
			if (!context.ReserveOutput(SceneStorageBytes<true>(*source) + 32, "scene")) return false;
			SceneValue3D output = *source;
			for (size_t i = 0; i < output.Data->Objects.size(); ++i) {
				MeshTransform3D *transform = std::visit(
					[](auto &object) -> MeshTransform3D * {
						using T = std::decay_t<decltype(object)>;
						if constexpr (std::is_same_v<T, MeshValue3D>)
							return object.Data ? &object.Data->LocalTransforms.front() : nullptr;
						else if constexpr (std::is_same_v<T, LightValue3D>)
							return object.Data ? &object.Data->Transform : nullptr;
						else
							return object ? &object->Transform : nullptr;
					},
					output.Data->Objects[i].Data
				);
				if (!transform) continue;
				if (const auto position = TransformRow(context.Find("position"), i)) {
					if (positionMode == 0)
						transform->Position = {
							transform->Position.X + position->X,
							transform->Position.Y + position->Y,
							transform->Position.Z + position->Z
						};
					else if (positionMode == 1)
						transform->Position = *position;
				}
				if (const auto angles = TransformRow(context.Find("rotation"), i)) {
					const Quaternion rotation = SourceQuaternionFromEuler(angles->X, angles->Y, angles->Z);
					if (rotationMode == 0)
						transform->Rotation = SourceQuaternionMultiply(transform->Rotation, rotation);
					else if (rotationMode == 1)
						transform->Rotation = rotation;
				}
				if (const auto scale = TransformRow(context.Find("scale"), i)) {
					if (scaleMode == 0)
						transform->Scale = {
							transform->Scale.X + scale->X,
							transform->Scale.Y + scale->Y,
							transform->Scale.Z + scale->Z
						};
					else if (scaleMode == 1)
						transform->Scale = {
							transform->Scale.X * scale->X,
							transform->Scale.Y * scale->Y,
							transform->Scale.Z * scale->Z
						};
					else if (scaleMode == 2)
						transform->Scale = *scale;
				}
			}
			if (!ValidScenePayload(output))
				return context.Fail(Status::InvalidValue, "scene transform produced nonfinite data", "scene");
			context.SetValue("scene", std::move(output));
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SceneExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.3_d_scene", Scene, true},
			ExecutorEntry{"pc.3_d_repeat", RepeatScene, true},
			ExecutorEntry{"pc.3_d_instancer", SourceMeshInstancer, true},
			ExecutorEntry{"pc.3_d_mesh_wall_builder", SourceMeshWall, true},
			ExecutorEntry{"pc.3_d_mesh_extrude_mesh", SourceMeshPlanarExtrude, true},
			ExecutorEntry{"pc.3_d_mesh_extrude", SourceMeshSurfaceExtrude, true},
			ExecutorEntry{"pc.3_d_mesh_stack_slice", SourceMeshSliceStack, true},
			ExecutorEntry{"pc.rm_to_voxel", SourceMeshFromSdf, true},
			ExecutorEntry{"pc.rm_to_mesh", SourceMeshFromSdf, true},
			ExecutorEntry{"pc.3_d_mesh_path_revolve", SourceMeshPathRevolve, true},
			ExecutorEntry{"pc.3_d_mesh_path_extrude", SourceMeshPathExtrude, true},
			ExecutorEntry{"pc.path_3_d", EvaluateSourcePath3D, true},
			ExecutorEntry{"pc.path_3_d_transform", TransformSourcePath3D, true},
			ExecutorEntry{"pc.path_3_d_camera", ProjectSourcePath3D, true},
			ExecutorEntry{"pc.3_d_point_affector", PointAffector, true},
			ExecutorEntry{"pc.3_d_mirror", Mirror, true},
			ExecutorEntry{"pc.3_d_transform_scene", TransformScene, true},
			ExecutorEntry{"pc.3_d_light_point", Light, true},
			ExecutorEntry{"pc.3_d_light_directional", Light, true}
		};
		return ENTRIES;
	}
}
