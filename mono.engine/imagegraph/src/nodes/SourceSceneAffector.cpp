#include "NodeExecutors.hpp"
#include "ScenePayload.hpp"
#include "SourceMeshAvailability.hpp"
#include "SourceMeshTransform.hpp"
#include "SourceQuaternion.hpp"
#include "TimelineDrivers.hpp"
#include "nodes/Curve.hpp"

#include <engine/core/Metrics.hpp>

namespace engine::imagegraph::detail {
	namespace {
		Vector3 Add(Vector3 a, Vector3 b) {
			return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
		}
		Vector3 Center(const SceneObject3D &object) {
			return std::visit(
				[](const auto &value) -> Vector3 {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, MeshValue3D>) {
						Vector3 center{};
						if (value.Data)
							for (const auto &transform : value.Data->LocalTransforms)
								center = Add(center, transform.Position);
						return center;
					} else if constexpr (std::is_same_v<T, LightValue3D>)
						return value.Data ? value.Data->Transform.Position : Vector3{};
					else {
						if (!value) return {};
						Vector3 center{};
						for (const auto &child : value->Objects)
							center = Add(center, Center(child));
						const double count = double(value->Objects.size());
						if (count) center = {center.X / count, center.Y / count, center.Z / count};
						return Add(center, value->Transform.Position);
					}
				},
				object.Data
			);
		}
		bool CloneSupported(const SceneData3D &scene) {
			for (const auto &object : scene.Objects)
				if (!std::visit(
						[](const auto &value) {
							using T = std::decay_t<decltype(value)>;
							if constexpr (std::is_same_v<T, MeshValue3D>)
								return !value.Data || !value.Data->Instanced;
							else if constexpr (std::is_same_v<T, LightValue3D>)
								return true;
							else
								return !value || CloneSupported(*value);
						},
						object.Data
					))
					return false;
			return true;
		}
		void CloneSourceSceneObjects(SceneData3D &scene) {
			for (auto &object : scene.Objects) {
				if (auto *mesh = std::get_if<MeshValue3D>(&object.Data)) {
					if (mesh->Data) ApplySourceObjectCloneMetadata(*mesh->Data, false);
				} else if (auto *light = std::get_if<LightValue3D>(&object.Data)) {
					// __3dLight inherits __3dObject.clone: the clone has no light subclass or
					// drawable VB.
					MeshValue3D mesh;
					if (light->Data) {
						auto &data = mesh.Data.emplace();
						data.LocalTransforms.push_back(light->Data->Transform);
						data.CpuVerticesPresent = data.CpuEdgesPresent = false;
					}
					object.Data = std::move(mesh);
				} else if (auto *group = std::get_if<OwnedPayload3D<SceneData3D>>(&object.Data);
						   group && *group)
					CloneSourceSceneObjects(**group);
			}
		}
		MeshTransform3D *Transform(SceneObject3D &object) {
			return std::visit(
				[](auto &value) -> MeshTransform3D * {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, MeshValue3D>)
						return value.Data && !value.Data->LocalTransforms.empty()
								   ? &value.Data->LocalTransforms.front()
								   : nullptr;
					else if constexpr (std::is_same_v<T, LightValue3D>)
						return value.Data ? &value.Data->Transform : nullptr;
					else
						return value ? &value->Transform : nullptr;
				},
				object.Data
			);
		}
	} // namespace
	// node_3d_affector.gml affects immediate cloned scene children, using a
	// first-frame curveMap.
	bool SourceSceneAffector(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.scene.affector");
		const Value *input = c.Find("scene");
		const auto *scene = input ? std::get_if<SceneValue3D>(input) : nullptr;
		if (!scene || !scene->Data) {
			c.SetValue("scene", SceneValue3D{});
			return c.FailureCode == Status::Ok;
		}
		if (!ValidScenePayload(*scene))
			return c.Fail(Status::InvalidValue, "scene affector input is invalid", "scene");
		if (!CloneSupported(*scene->Data))
			return c.Fail(
				Status::UnsupportedExecution, "source instancer clone has no defined scene object", "scene"
			);
		const double shape = c.SourceChoice("shape"), falloff = c.Scalar("falloff_distance", .5);
		if (c.FailureCode != Status::Ok) return false;
		if (shape != 0 && shape != 1)
			return c.Fail(Status::UnsupportedExecution, "affector shape has no defined source case", "shape");
		const Vector3 position = c.Get<Vector3>("position"), scale = c.Get<Vector3>("scale", {1, 1, 1}),
					  affectPosition = c.Get<Vector3>("affect_position"),
					  affectScale = c.Get<Vector3>("affect_scale", {1, 1, 1});
		const Quaternion rotation = c.Get<Quaternion>("rotation"),
						 affectRotation = c.Get<Quaternion>("affect_rotation");
		if (!std::isfinite(falloff) || !MeshFinite(position) || !MeshFinite(scale) ||
			!MeshFinite(affectPosition) || !MeshFinite(affectScale) || !MeshFinite(rotation) ||
			!MeshFinite(affectRotation))
			return c.Fail(Status::InvalidValue, "affector controls must be finite", "scene");
		if (c.Request.NegativeFrame || c.Request.Subframe != 0)
			return c.Fail(
				Status::UnsupportedExecution,
				"scene affector replay needs integer nonnegative source frames",
				"falloff_curve"
			);
		const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
		const DataReplayEntry *prior = nullptr;
		if (owner) {
			Diagnostic diagnostic;
			if (ValidateDataReplay(*owner, c.ByteBudget, diagnostic) != Status::Ok)
				return c.Fail(diagnostic.Code, diagnostic.Message, "falloff_curve");
			for (const auto &entry : owner->Entries)
				if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow) prior = &entry;
		}
		const ArrayValue *map = nullptr;
		DataReplayEntry update;
		if (c.Request.Tick == 0) {
			const Value *value = c.Find("falloff_curve");
			const auto *curve = value ? std::get_if<Curve>(value) : nullptr;
			if (!curve || !ValidRuntimeValue(*value))
				return c.Fail(Status::InvalidValue, "affector falloff curve is invalid", "falloff_curve");
			const uint64_t bytes = sizeof(DataReplayEntry) +
								   std::max(c.Authored.Id.size(), std::string{}.capacity()) +
								   sizeof(DataReplayValueFrame) + 101 * sizeof(ElementValue);
			if (!c.ReserveOutput(bytes, "falloff_curve")) return false;
			ArrayValue sampled{ValueType::Scalar, {}};
			sampled.Elements.reserve(101);
			for (size_t i = 0; i <= 100; ++i) {
				const double sample = EvalCurveX(*curve, double(i) * .01, .00001);
				if (!std::isfinite(sample))
					return c.Fail(
						Status::InvalidValue, "affector curve map sample is nonfinite", "falloff_curve"
					);
				sampled.Elements.emplace_back(sample);
			}
			update.NodeId = c.Authored.Id;
			update.ProcessorRow = c.ProcessorRow;
			update.Initialized = true;
			update.Values.push_back({0, std::move(sampled)});
			map = std::get_if<ArrayValue>(&update.Values[0].Data);
		} else {
			if (!prior || prior->Values.size() != 1 || prior->Values[0].Frame != 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"scene affector needs captured first-frame curve replay",
					"falloff_curve"
				);
			map = std::get_if<ArrayValue>(&prior->Values[0].Data);
		}
		if (!map || map->Elements.size() != 101 || !map->Nested.empty() || !map->Items.empty())
			return c.Fail(
				Status::InvalidValue, "scene affector curve replay has wrong shape", "falloff_curve"
			);
		for (const auto &sample : map->Elements)
			if (!std::holds_alternative<double>(sample) || !std::isfinite(std::get<double>(sample)))
				return c.Fail(
					Status::InvalidValue, "scene affector curve replay sample is invalid", "falloff_curve"
				);
		if (!c.ReserveOutput(SceneStorageBytes<false>(*scene) + sizeof(AuthoredValue) + 5, "scene"))
			return false;
		SceneValue3D output = *scene;
		CloneSourceSceneObjects(*output.Data);
		core::Metrics::Count("imagegraph.scene.cloned_payload_bytes", SceneStorageBytes<true>(output));
		core::Metrics::Count("imagegraph.scene.clones", 1);
		const double maximum = std::max({scale.X, scale.Y, scale.Z}),
					 inner = shape == 0 ? (maximum - falloff) / 2 : -falloff / 2,
					 outer = shape == 0 ? (maximum + falloff) / 2 : falloff / 2;
		const Vector3 normal = SourceRotate(rotation, {0, 0, 1});
		for (auto &object : output.Data->Objects) {
			auto *transform = Transform(object);
			if (!transform) continue;
			const Vector3 center = Center(object);
			const double distance =
				shape == 0 ? std::hypot(center.X - position.X, center.Y - position.Y, center.Z - position.Z)
						   : ((center.X - position.X) * normal.X + (center.Y - position.Y) * normal.Y +
							  (center.Z - position.Z) * normal.Z) /
								 std::hypot(normal.X, normal.Y, normal.Z);
			if (!std::isfinite(distance))
				return c.Fail(Status::InvalidValue, "affector distance is nonfinite", "rotation");
			double influence = 0;
			if (distance >= outer)
				influence = 0;
			else if (distance <= inner)
				influence = 1;
			else {
				const double index = std::clamp(1 - (distance - inner) / falloff, 0.0, 1.0) * 100,
							 lower = std::floor(index), upper = std::ceil(index);
				influence = CurveLerp(
					std::get<double>(map->Elements[size_t(lower)]),
					std::get<double>(map->Elements[size_t(upper)]),
					index - lower
				);
			}
			if (influence == 0) continue;
			transform->Position = Add(
				transform->Position,
				{affectPosition.X * influence, affectPosition.Y * influence, affectPosition.Z * influence}
			);
			Quaternion result;
			if (!SourceRawQuaternionSlerp(
					transform->Rotation,
					SourceQuaternionMultiply(transform->Rotation, affectRotation),
					influence,
					result
				))
				return c.Fail(
					Status::InvalidValue, "scene affector rotation is nonfinite", "affect_rotation"
				);
			transform->Rotation = result;
			transform->Scale = {
				transform->Scale.X * (1 + (affectScale.X - 1) * influence),
				transform->Scale.Y * (1 + (affectScale.Y - 1) * influence),
				transform->Scale.Z * (1 + (affectScale.Z - 1) * influence)
			};
		}
		if (!ValidScenePayload(output))
			return c.Fail(Status::InvalidValue, "scene affector produced nonfinite data", "scene");
		c.SetValue("scene", std::move(output));
		if (c.FailureCode != Status::Ok) return false;
		if (c.Request.Tick == 0) c.DataUpdates.push_back(std::move(update));
		return true;
	}
} // namespace engine::imagegraph::detail
