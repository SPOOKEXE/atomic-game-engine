#include "../SourceMeshAvailability.hpp"
#include "../SourceMeshTransform.hpp"
#include "../SourceRandom.hpp"
#include "Families.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace engine::imagegraph::detail {
	namespace {
		bool ReadSourceMesh(NodeContext &context, const MeshValue3D *&mesh) {
			const Value *value = context.Find("mesh");
			if (!value) return true;
			mesh = std::get_if<MeshValue3D>(value);
			if (!mesh) return context.Fail(Status::TypeMismatch, "modifier requires a mesh", "mesh");
			return ValidMeshPayload(*mesh) ||
				   context.Fail(Status::InvalidValue, "modifier mesh payload is invalid", "mesh");
		}
		double SourceRound(double value) {
			const double floor = std::floor(value), fraction = value - floor;
			if (fraction < .5) return floor;
			if (fraction > .5) return floor + 1;
			return std::fmod(floor, 2) == 0 ? floor : floor + 1;
		}
		bool Publish(NodeContext &context, MeshValue3D output, std::string_view port = "mesh") {
			if (!ValidMeshPayload(output))
				return context.Fail(Status::InvalidValue, "modified mesh contains nonfinite geometry", port);
			context.SetValue(port, std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool RoundVertex(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.round_vertex");
			const MeshValue3D *mesh = nullptr;
			if (!ReadSourceMesh(context, mesh)) return false;
			if (!mesh || !mesh->Data || mesh->Data->Instanced || mesh->Data->LocalTransforms.size() != 1)
				return Publish(context, {});
			const double step = context.Scalar("step", .1);
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(step))
				return context.Fail(Status::InvalidValue, "snap step must be finite", "step");
			if (!context.ReserveOutput(MeshStorageBytes<true>(*mesh) + 32, "mesh")) return false;
			MeshValue3D output = CloneSourceObjectMesh(*mesh, true);
			RebuildSourceObjectMesh(*output.Data);
			for (auto &part : output.Data->Parts)
				for (auto &vertex : part.Vertices) {
					if (step == 0) continue;
					vertex.Position = {
						SourceRound(vertex.Position.X / step) * step,
						SourceRound(vertex.Position.Y / step) * step,
						SourceRound(vertex.Position.Z / step) * step
					};
				}
			return Publish(context, std::move(output));
		}
		MeshVertex3D Midpoint(const MeshVertex3D &a, const MeshVertex3D &b) {
			return {
				{(a.Position.X + b.Position.X) / 2,
				 (a.Position.Y + b.Position.Y) / 2,
				 (a.Position.Z + b.Position.Z) / 2},
				{(a.Normal.X + b.Normal.X) / 2, (a.Normal.Y + b.Normal.Y) / 2, (a.Normal.Z + b.Normal.Z) / 2},
				{(a.UV.X + b.UV.X) / 2, (a.UV.Y + b.UV.Y) / 2}
			};
		}
		bool Subdivide(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.subdivide");
			const MeshValue3D *mesh = nullptr;
			if (!ReadSourceMesh(context, mesh)) return false;
			if (!mesh || !mesh->Data || mesh->Data->Instanced || mesh->Data->LocalTransforms.size() != 1)
				return Publish(context, {});
			const double target = context.Scalar("subobjects", -1);
			const int64_t level = context.Integer("level", 1);
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(target) || target != std::trunc(target))
				return context.Fail(Status::InvalidValue, "subobject index must be an integer", "subobjects");
			const auto &source = *mesh->Data;
			const size_t selected =
				SourceCpuMeshParts(source).empty()
					? 0
					: size_t(std::clamp(target, 0.0, double(SourceCpuMeshParts(source).size() - 1)));
			uint64_t multiplier = 1;
			for (int64_t i = 0; i < level; ++i) {
				if (multiplier > Limits::MaximumArrayElements / 4)
					return context.Fail(Status::LimitExceeded, "subdivision exceeds geometry caps", "level");
				multiplier *= 4;
			}
			uint64_t total = 0, growth = 0, largest = 0;
			for (size_t i = 0; i < SourceCpuMeshParts(source).size(); ++i) {
				const uint64_t count = SourceCpuMeshParts(source)[i].Vertices.size();
				const uint64_t next = count * (target == -1 || selected == i ? multiplier : 1);
				if (next > Limits::MaximumArrayElements - total)
					return context.Fail(Status::LimitExceeded, "subdivision exceeds geometry caps", "level");
				total += next;
				growth += (next - count) * sizeof(MeshVertex3D);
				largest = std::max(largest, next);
			}
			const uint64_t bytes = MeshAddBytes(MeshStorageBytes<true>(*mesh), growth);
			if (bytes > Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "subdivision exceeds mesh payload bytes", "level");
			if (!context.ReserveOutput(bytes + 32, "mesh")) return false;
			auto scratch = context.ReserveWorkspace(largest * sizeof(MeshVertex3D), "mesh");
			if (!scratch) return false;
			MeshValue3D output = CloneSourceObjectMesh(*mesh, true);
			RebuildSourceObjectMesh(*output.Data);
			for (size_t i = 0; i < SourceCpuMeshParts(source).size(); ++i) {
				if (target != -1 && selected != i) continue;
				auto &vertices = output.Data->Parts[i].Vertices;
				for (int64_t iteration = 0; iteration < level; ++iteration) {
					std::vector<MeshVertex3D> next;
					next.reserve(vertices.size() * 4);
					for (size_t j = 0; j < vertices.size(); j += 3) {
						const auto &a = vertices[j], &b = vertices[j + 1], &c = vertices[j + 2];
						const auto ab = Midpoint(a, b), ac = Midpoint(a, c), bc = Midpoint(b, c);
						for (const auto &vertex :
							 std::array<MeshVertex3D, 12>{a, ab, ac, ab, b, bc, ac, bc, c, ab, bc, ac})
							next.push_back(vertex);
					}
					vertices.swap(next);
				}
			}
			return Publish(context, std::move(output));
		}
		bool SetOrigin(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.set_origin");
			const MeshValue3D *mesh = nullptr;
			if (!ReadSourceMesh(context, mesh)) return false;
			if (!mesh || !mesh->Data) {
				context.SetValue("origin", Vector3{});
				return Publish(context, {}, "scene");
			}
			if (mesh->Data->LocalTransforms.size() == MAXIMUM_MESH_TRANSFORMS)
				return context.Fail(
					Status::LimitExceeded, "origin wrapper exceeds mesh transform caps", "scene"
				);
			const double mode = context.SourceChoice("type", 1);
			Vector3 origin{};
			if (mode == 0) {
				if (const Value *value = context.Find("point")) {
					const auto *point = std::get_if<Vector3>(value);
					if (!point)
						return context.Fail(Status::TypeMismatch, "origin point requires Vector3", "point");
					origin = *point;
				}
			} else if (mode == 1) {
				uint64_t count = 0;
				for (const auto &part : mesh->Data->Parts)
					for (const auto &vertex : part.Vertices) {
						origin.X += float(vertex.Position.X);
						origin.Y += float(vertex.Position.Y);
						origin.Z += float(vertex.Position.Z);
						++count;
					}
				if (count) origin = {origin.X / count, origin.Y / count, origin.Z / count};
			}
			if (context.FailureCode != Status::Ok) return false;
			if (!context.ReserveOutput(MeshStorageBytes<true>(*mesh) + sizeof(MeshTransform3D) + 64, "scene"))
				return false;
			MeshValue3D output = *mesh;
			const Vector3 position = mesh->Data->LocalTransforms.front().Position;
			MeshTransform3D wrapper;
			wrapper.Anchor = origin;
			wrapper.Position = {origin.X - position.X, origin.Y - position.Y, origin.Z - position.Z};
			output.Data->LocalTransforms.insert(output.Data->LocalTransforms.begin(), wrapper);
			context.SetValue("origin", origin);
			return Publish(context, std::move(output), "scene");
		}
	}
	namespace {
		bool SetGroupMaterial(NodeContext &context, const SceneValue3D &scene) {
			if (!scene.Data) {
				context.SetValue("mesh", SceneValue3D{});
				return context.FailureCode == Status::Ok;
			}
			if (!ValidScenePayload(scene) || scene.Data->Transform.Mirror)
				return context.Fail(
					Status::UnsupportedExecution,
					"group material replacement requires a plain source group",
					"mesh"
				);
			for (const auto &object : scene.Data->Objects)
				if (!std::holds_alternative<MeshValue3D>(object.Data))
					return context.Fail(
						Status::UnsupportedExecution,
						"source nested groups and lights have no material getter",
						"mesh"
					);
			for (const auto &object : scene.Data->Objects) {
				const auto &mesh = std::get<MeshValue3D>(object.Data);
				if (mesh.Data && mesh.Data->Instanced)
					return context.Fail(
						Status::UnsupportedExecution,
						"source instancer clone has no defined material object",
						"mesh"
					);
			}
			const Value *value = context.Find("materials");
			const auto *single = value ? std::get_if<MaterialValue3D>(value) : nullptr;
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (value && !single && !array)
				return context.Fail(Status::TypeMismatch, "replacement requires materials", "materials");
			MaterialValue3D defaults;
			struct Row {
				const MaterialValue3D *Single = nullptr;
				std::span<const ElementValue> Elements;
				std::span<const SourceArrayItem> Items;
				bool Defined = false;
				const MaterialValue3D *At(size_t index) const {
					if (Single) return Single;
					if (index < Elements.size()) return std::get_if<MaterialValue3D>(&Elements[index]);
					if (index < Items.size())
						if (const auto *element = std::get_if<ElementValue>(&Items[index].Data))
							return std::get_if<MaterialValue3D>(element);
					return nullptr;
				}
			};
			const size_t count =
				array ? (array->Items.empty()
							 ? (array->Nested.empty() ? array->Elements.size() : array->Nested.size())
							 : array->Items.size())
					  : 1;
			const auto rowAt = [&](size_t index) -> Row {
				if (!array) return {single ? single : &defaults, {}, {}, true};
				if (!array->Items.empty()) {
					const auto &item = array->Items[index];
					if (const auto *element = std::get_if<ElementValue>(&item.Data))
						return {
							std::get_if<MaterialValue3D>(element),
							{},
							{},
							std::holds_alternative<MaterialValue3D>(*element)
						};
					if (const auto *items = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
						return {nullptr, {}, *items, true};
					return {};
				}
				if (!array->Nested.empty()) return {nullptr, array->Nested[index], {}, true};
				return {
					std::get_if<MaterialValue3D>(&array->Elements[index]),
					{},
					{},
					std::holds_alternative<MaterialValue3D>(array->Elements[index])
				};
			};
			for (size_t i = 0; i < count; ++i) {
				const Row row = rowAt(i);
				if (row.Single) {
					if (!ValidMaterialPayload(*row.Single))
						return context.Fail(
							Status::InvalidValue, "group replacement material is invalid", "materials"
						);
				} else {
					if (!row.Defined)
						return context.Fail(
							Status::TypeMismatch, "group replacement requires material rows", "materials"
						);
					for (size_t j = 0; j < std::max(row.Elements.size(), row.Items.size()); ++j) {
						const auto *material = row.At(j);
						if (!material || !ValidMaterialPayload(*material))
							return context.Fail(
								Status::TypeMismatch, "group replacement requires material rows", "materials"
							);
					}
				}
			}
			const double selection = context.SourceChoice("select"),
						 overflow = context.SourceChoice("overflow");
			if ((selection != 0 && selection != 1) || (overflow != 0 && overflow != 1))
				return context.Fail(
					Status::UnsupportedExecution, "material selection has no defined source case", "select"
				);
			const double seedValue = context.Scalar("seed", double(uint32_t(context.Request.Seed)));
			if (!std::isfinite(seedValue) || seedValue < INT32_MIN || seedValue > UINT32_MAX)
				return context.Fail(Status::InvalidValue, "material seed exceeds native32bit range", "seed");
			if (!count && (selection == 1 || overflow == 1))
				return context.Fail(
					Status::InvalidValue, "material selection cannot loop an empty array", "materials"
				);
			if (context.FailureCode != Status::Ok) return false;
			const uint32_t seed = uint32_t(int64_t(seedValue));
			SourceRandom random(seed);
			const auto select = [&](size_t index, const MeshValue3D &mesh) -> Row {
				if (selection == 1) {
					const size_t index = random.Index(uint32_t(count));
					if (index >= count) {
						context.Fail(
							Status::InvalidValue,
							"source random material index exceeds the array",
							"materials"
						);
						return {};
					}
					return rowAt(index);
				}
				if (overflow == 1) return rowAt(index % count);
				if (overflow == 0 && index < count) return rowAt(index);
				return {mesh.Data->Materials.empty() ? nullptr : &mesh.Data->Materials.front(), {}, {}, true};
			};
			uint64_t bytes = SceneStorageBytes<true>(scene);
			for (size_t i = 0; i < scene.Data->Objects.size(); ++i) {
				const auto &mesh = std::get<MeshValue3D>(scene.Data->Objects[i].Data);
				if (!mesh.Data || mesh.Data->Materials.empty()) continue;
				const Row row = select(i, mesh);
				for (size_t j = 0; j < mesh.Data->Materials.size(); ++j) {
					const auto *material = row.At(j);
					if (material) bytes = MeshAddBytes(bytes, MaterialStorageBytes<true>(*material));
				}
			}
			if (bytes > Limits::MaximumArrayBytes || !context.ReserveOutput(bytes + 32, "mesh"))
				return context.Fail(
					Status::LimitExceeded, "group replacement exceeds material payload cap", "materials"
				);
			if (context.FailureCode != Status::Ok) return false;
			random = SourceRandom(seed);
			SceneValue3D output = scene;
			for (auto &object : output.Data->Objects) {
				auto &mesh = std::get<MeshValue3D>(object.Data);
				if (mesh.Data) ApplySourceObjectCloneMetadata(*mesh.Data, false);
			}
			for (size_t i = 0; i < output.Data->Objects.size(); ++i) {
				auto &mesh = std::get<MeshValue3D>(output.Data->Objects[i].Data);
				if (!mesh.Data || mesh.Data->Materials.empty()) continue;
				const Row row = select(i, mesh);
				for (size_t j = 0; j < mesh.Data->Materials.size(); ++j) {
					const auto *material = row.At(j);
					if (material) mesh.Data->Materials[j] = *material;
				}
			}
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}

		bool SetMaterial(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.set_material");
			if (const Value *input = context.Find("mesh"))
				if (const auto *group = std::get_if<SceneValue3D>(input))
					return SetGroupMaterial(context, *group);
			const MeshValue3D *mesh = nullptr;
			if (!ReadSourceMesh(context, mesh)) return false;
			if (!mesh || !mesh->Data) return Publish(context, {});
			if (mesh->Data->Instanced)
				return context.Fail(
					Status::UnsupportedExecution,
					"source instancer clone has no defined material object",
					"mesh"
				);

			const double selection = context.SourceChoice("select"),
						 overflow = context.SourceChoice("overflow");
			if (context.FailureCode != Status::Ok) return false;
			const Value *value = context.Find("materials");
			const auto *single = value ? std::get_if<MaterialValue3D>(value) : nullptr;
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (value && !single && !array)
				return context.Fail(Status::TypeMismatch, "replacement requires materials", "materials");
			if (array && (!array->Nested.empty() || !array->Items.empty()))
				return context.Fail(
					Status::TypeMismatch, "mesh replacement requires a flat material array", "materials"
				);
			const size_t count = array ? array->Elements.size() : 1;
			MaterialValue3D defaults;
			const auto materialAt = [&](size_t i) -> const MaterialValue3D * {
				if (!array) return single ? single : &defaults;
				return std::get_if<MaterialValue3D>(&array->Elements[i]);
			};
			for (size_t i = 0; i < count; ++i) {
				const auto *material = materialAt(i);
				if (!material || !ValidMaterialPayload(*material))
					return context.Fail(
						Status::TypeMismatch, "replacement array requires finite materials", "materials"
					);
			}
			if (!count && (selection == 1 || overflow == 1))
				return context.Fail(
					Status::InvalidValue, "material selection cannot loop an empty array", "materials"
				);
			const double seedValue = context.Scalar("seed", double(uint32_t(context.Request.Seed)));
			if (!std::isfinite(seedValue) || seedValue < INT32_MIN || seedValue > UINT32_MAX)
				return context.Fail(
					Status::InvalidValue, "material seed exceeds the native 32 bit range", "seed"
				);
			const uint32_t seed = uint32_t(int64_t(seedValue));
			SourceRandom random(seed);
			const auto selected = [&](size_t i) -> const MaterialValue3D * {
				if (selection == 1) {
					const size_t index = random.Index(uint32_t(count));
					if (index >= count) {
						context.Fail(
							Status::InvalidValue,
							"source random material index exceeds the array",
							"materials"
						);
						return nullptr;
					}
					return materialAt(index);
				}
				if (overflow == 1) return materialAt(i % count);
				if (overflow == 0 && i < count) return materialAt(i);
				return nullptr;
			};
			uint64_t bytes = MeshStorageBytes<true>(*mesh);
			for (size_t i = 0; i < mesh->Data->Materials.size(); ++i)
				if (const auto *material = selected(i))
					bytes = MeshAddBytes(bytes, MaterialStorageBytes<true>(*material));
			if (bytes > Limits::MaximumArrayBytes)
				return context.Fail(
					Status::LimitExceeded, "replacement materials exceed mesh payload caps", "materials"
				);
			if (!context.ReserveOutput(bytes + 32, "mesh")) return false;
			if (context.FailureCode != Status::Ok) return false;
			random = SourceRandom(seed);
			MeshValue3D output = CloneSourceObjectMesh(*mesh, true);
			for (size_t i = 0; i < output.Data->Materials.size(); ++i)
				if (const auto *material = selected(i)) output.Data->Materials[i] = *material;
			return Publish(context, std::move(output));
		}
		bool RemapVector(NodeContext &context, std::string_view port, Vector3 &vector) {
			if (const Value *value = context.Find(port)) {
				if (const auto *typed = std::get_if<Vector3>(value))
					vector = *typed;
				else if (const auto *number = std::get_if<double>(value))
					vector = {*number, *number, *number};
				else if (const auto *number = std::get_if<int64_t>(value))
					vector = {double(*number), double(*number), double(*number)};
				else
					return context.Fail(Status::TypeMismatch, "UV transform requires Vector3", port);
			}
			return MeshFinite(vector) ||
				   context.Fail(Status::InvalidValue, "UV transform must be finite", port);
		}
		bool UVRemap(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.uv_remap");
			const Value *source = context.Find("mesh");
			const auto *mesh = source ? std::get_if<MeshValue3D>(source) : nullptr;
			const auto *scene = source ? std::get_if<SceneValue3D>(source) : nullptr;
			if ((!mesh || !mesh->Data) && (!scene || !scene->Data)) return Publish(context, {});
			if (mesh && mesh->Data && mesh->Data->Instanced)
				return context.Fail(
					Status::UnsupportedExecution, "source instancer clone has no defined UV object", "mesh"
				);
			if ((mesh && !ValidMeshPayload(*mesh)) || (scene && !ValidScenePayload(*scene)))
				return context.Fail(Status::InvalidValue, "UV source payload is invalid", "mesh");
			Vector3 position{}, scale{1, 1, 1};
			if (!RemapVector(context, "position", position) || !RemapVector(context, "scale", scale))
				return false;
			Quaternion rotation{};
			if (const Value *value = context.Find("rotation")) {
				const auto *typed = std::get_if<Quaternion>(value);
				if (!typed)
					return context.Fail(Status::TypeMismatch, "UV rotation requires Quaternion", "rotation");
				rotation = *typed;
			}
			if (scale.X == 0 || scale.Y == 0)
				return context.Fail(Status::InvalidValue, "UV projection scale is zero", "scale");
			const Vector3 normal = SourceRotate(rotation, {0, 0, 1}),
						  xAxis = SourceRotate(rotation, {1, 0, 0}),
						  yAxis = SourceRotate(rotation, {0, -1, 0});
			if (!MeshFinite(normal) || !MeshFinite(xAxis) || !MeshFinite(yAxis))
				return context.Fail(Status::InvalidValue, "UV rotation must be nonzero", "rotation");
			const Value *filter = context.Find("target_subobject");
			const auto matches = [&](size_t index) {
				if (!filter) return true;
				if (const auto *number = std::get_if<int64_t>(filter))
					return *number == -1 || *number == int64_t(index);
				if (const auto *array = std::get_if<ArrayValue>(filter)) {
					for (const auto &element : array->Elements) {
						if (const auto *number = std::get_if<int64_t>(&element);
							number && *number == int64_t(index))
							return true;
						if (const auto *number = std::get_if<double>(&element);
							number && *number == double(index))
							return true;
					}
				}
				return false;
			};
			const auto modifyMesh = [&](MeshValue3D &output, std::span<const MeshTransform3D> parents) {
				if (!output.Data || output.Data->LocalTransforms.size() != 1) {
					output = {};
					return;
				}
				output.Data->CpuVerticesPresent = false;
				output.Data->CpuEdgesPresent = false;
				for (auto &part : output.Data->Parts)
					part.LocalMatrix.reset();
				for (size_t i = 0; i < output.Data->Parts.size(); ++i) {
					if (!matches(i)) continue;
					for (auto &vertex : output.Data->Parts[i].Vertices) {
						Vector3 point{
							float(vertex.Position.X), float(vertex.Position.Y), float(vertex.Position.Z)
						};
						point = SourceMeshPoint(output.Data->LocalTransforms.front(), point);
						for (size_t parent = parents.size(); parent > 0; --parent)
							point = SourceMeshPoint(parents[parent - 1], point);
						const Vector3 relative{
							point.X - position.X, point.Y - position.Y, point.Z - position.Z
						};
						const double distance =
							relative.X * normal.X + relative.Y * normal.Y + relative.Z * normal.Z;
						const Vector3 projected{
							relative.X - normal.X * distance,
							relative.Y - normal.Y * distance,
							relative.Z - normal.Z * distance
						};
						const double u = (projected.X * xAxis.X + projected.Y * xAxis.Y +
										  projected.Z * xAxis.Z) /
											 scale.X +
										 .5,
									 v = (projected.X * yAxis.X + projected.Y * yAxis.Y +
										  projected.Z * yAxis.Z) /
											 scale.Y +
										 .5;
						vertex.UV = {float(u), float(v)};
					}
				}
			};
			if (mesh) {
				if (!context.ReserveOutput(MeshStorageBytes<true>(*mesh) + 32, "mesh")) return false;
				MeshValue3D output = *mesh;
				modifyMesh(output, {});
				return Publish(context, std::move(output));
			}
			if (!context.ReserveOutput(SceneStorageBytes<true>(*scene) + 32, "mesh")) return false;
			SceneValue3D output = *scene;
			std::array<MeshTransform3D, Limits::MaximumArrayDepth + 1> parents{};
			const auto modifyScene = [&](auto &&self, SceneData3D &group, size_t depth) -> void {
				parents[depth] = group.Transform;
				for (auto &object : group.Objects)
					std::visit(
						[&](auto &item) {
							using T = std::decay_t<decltype(item)>;
							if constexpr (std::is_same_v<T, MeshValue3D>)
								modifyMesh(item, std::span(parents).first(depth + 1));
							else if constexpr (std::is_same_v<T, LightValue3D>)
								item = {};
							else if (item) {
								if (item->Transform.Mirror)
									item = {};
								else
									self(self, *item, depth + 1);
							}
						},
						object.Data
					);
			};
			if (output.Data->Transform.Mirror) return Publish(context, {});
			modifyScene(modifyScene, *output.Data, 0);
			if (!ValidScenePayload(output))
				return context.Fail(
					Status::InvalidValue, "UV scene projection produced nonfinite geometry", "mesh"
				);
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool Displace(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.displace");
			const MeshValue3D *mesh = nullptr;
			if (!ReadSourceMesh(context, mesh)) return false;
			if (!mesh || !mesh->Data || mesh->Data->Instanced || mesh->Data->LocalTransforms.size() != 1)
				return Publish(context, {});
			const MaterialData3D *material = nullptr;
			const Image *surface = context.Input("displace_texture");
			if (const Value *value = context.Find("displace_texture")) {
				const auto *typed = std::get_if<MaterialValue3D>(value);
				if (!typed || !ValidMaterialPayload(*typed))
					return context.Fail(
						Status::TypeMismatch, "displacement requires a material", "displace_texture"
					);
				material = &typed->Get();
				if (material->Surface) surface = &*material->Surface;
			}
			if (!context.ReserveOutput(MeshStorageBytes<true>(*mesh) + 32, "mesh")) return false;
			if (!surface) return Publish(context, *mesh);
			if (!ValidMaterialSurface(*surface))
				return context.Fail(
					Status::InvalidValue, "displacement surface is invalid", "displace_texture"
				);
			if (surface->Format != SurfaceFormat::RGBA8Unorm)
				return context.Fail(
					Status::UnsupportedExecution,
					"source displacement byte sampler requires RGBA8",
					"displace_texture"
				);
			const double height = context.Scalar("height", .1), target = context.Scalar("subobjects", -1);
			const bool normal = context.Boolean("recalculate_normal", true);
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(target) || target != std::trunc(target))
				return context.Fail(
					Status::InvalidValue, "displacement subobject must be an integer", "subobjects"
				);
			const Vector2 scale = material ? material->TextureScale : Vector2{1, 1},
						  shift = material ? material->TextureShift : Vector2{};
			MeshValue3D output = CloneSourceObjectMesh(*mesh, true);
			RebuildSourceObjectMesh(*output.Data);
			const auto coordinate = [&](double uv, double tiling, double offset, uint32_t extent) {
				double value = std::fmod(std::clamp(uv, 0.0, .9999) * tiling + offset, 1);
				if (value < 0) value += 1;
				return uint32_t(SourceRound(value * double(extent - 1)));
			};
			const size_t selected =
				output.Data->Parts.empty()
					? 0
					: size_t(std::clamp(target, 0.0, double(output.Data->Parts.size() - 1)));
			for (size_t i = 0; i < output.Data->Parts.size(); ++i) {
				if (target != -1 && i != selected) continue;
				auto &vertices = output.Data->Parts[i].Vertices;
				for (auto &vertex : vertices) {
					const uint32_t x = coordinate(vertex.UV.X, scale.X, shift.X, surface->Width),
								   y = coordinate(vertex.UV.Y, scale.Y, shift.Y, surface->Height);
					std::array<double, 4> pixel{};
					LoadSurfacePixel(*surface, x, y, pixel);
					const double amount = height * (.299 * pixel[0] + .587 * pixel[1] + .114 * pixel[2]);
					vertex.Position = {
						vertex.Position.X + vertex.Normal.X * amount,
						vertex.Position.Y + vertex.Normal.Y * amount,
						vertex.Position.Z + vertex.Normal.Z * amount
					};
				}
				if (!normal) continue;
				for (size_t j = 0; j < vertices.size(); j += 3) {
					const Vector3 a = vertices[j].Position, b = vertices[j + 1].Position,
								  c = vertices[j + 2].Position;
					const Vector3 d{b.X - a.X, b.Y - a.Y, b.Z - a.Z}, e{a.X - c.X, a.Y - c.Y, a.Z - c.Z};
					Vector3 n{d.Y * e.Z - d.Z * e.Y, d.Z * e.X - d.X * e.Z, d.X * e.Y - d.Y * e.X};
					const double length = std::hypot(n.X, n.Y, n.Z);
					if (length) n = {n.X / length, n.Y / length, n.Z / length};
					vertices[j].Normal = vertices[j + 1].Normal = vertices[j + 2].Normal = n;
				}
			}
			return Publish(context, std::move(output));
		}
		bool VertexPoints(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.vertex_points");
			const MeshValue3D *mesh = nullptr;
			if (!ReadSourceMesh(context, mesh)) return false;
			ArrayValue positions, normals;
			positions.ElementType = normals.ElementType = ValueType::Scalar;
			if (!mesh || !mesh->Data || mesh->Data->Instanced || mesh->Data->LocalTransforms.size() != 1) {
				context.SetValue("positions", std::move(positions));
				context.SetValue("normals", std::move(normals));
				return context.FailureCode == Status::Ok;
			}
			const bool remove = context.Boolean("remove_overlap"), apply = context.Boolean("apply_transform");
			if (context.FailureCode != Status::Ok) return false;
			size_t count = 0;
			for (const auto &part : mesh->Data->Parts)
				count += part.Vertices.size();
			const uint64_t rowBytes = 2 * (sizeof(std::vector<ElementValue>) + 3 * sizeof(ElementValue));
			if (!context.ReserveOutput(count * rowBytes + 64, "positions")) return false;
			auto scratch =
				context.ReserveWorkspace(count * (sizeof(std::array<double, 6>) + 64), "positions");
			if (!scratch) return false;
			positions.Nested.reserve(count);
			normals.Nested.reserve(count);
			std::set<std::array<double, 6>> seen;
			for (const auto &part : mesh->Data->Parts)
				for (const auto &vertex : part.Vertices) {
					const Vector3 p = mesh->Data->CpuVerticesPresent ? vertex.Position
						: Vector3{float(vertex.Position.X), float(vertex.Position.Y), float(vertex.Position.Z)},
						n = mesh->Data->CpuVerticesPresent ? vertex.Normal
						: Vector3{float(vertex.Normal.X), float(vertex.Normal.Y), float(vertex.Normal.Z)};
					const std::array<double, 6> key{
						p.X, p.Y, p.Z, remove ? 0 : n.X, remove ? 0 : n.Y, remove ? 0 : n.Z
					};
					if (!seen.insert(key).second) continue;
					const Vector3 position =
						apply ? SourceMeshPoint(mesh->Data->LocalTransforms.front(), p) : p;
					const Vector3 normal =
						apply ? SourceMeshPoint(mesh->Data->LocalTransforms.front(), n, true) : n;
					if (!MeshFinite(position) || !MeshFinite(normal))
						return context.Fail(
							Status::InvalidValue, "vertex transform produced nonfinite points", "positions"
						);
					positions.Nested.push_back({position.X, position.Y, position.Z});
					normals.Nested.push_back({normal.X, normal.Y, normal.Z});
				}
			context.SetValue("positions", std::move(positions));
			context.SetValue("normals", std::move(normals));
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> MeshModifyExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.3_d_round_vertex", RoundVertex, true},
			ExecutorEntry{"pc.3_d_displace", Displace, true},
			ExecutorEntry{"pc.3_d_uv_remap", UVRemap, true},
			ExecutorEntry{"pc.3_d_set_material", SetMaterial, true},
			ExecutorEntry{"pc.3_d_subdivide", Subdivide, true},
			ExecutorEntry{"pc.3_d_set_origin", SetOrigin, true},
			ExecutorEntry{"pc.3_d_mesh_vertex_points", VertexPoints, true}
		};
		return ENTRIES;
	}
}
