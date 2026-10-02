// Owned source mesh descriptions preserve geometry order and local wrapper transforms.

#include "../SourceMaterialInputs.hpp"
#include "Curve.hpp"
#include "Families.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		uint64_t PortBytes(std::string_view port) {
			return std::max(port.size(), std::string{}.capacity());
		}
		bool ReadTransform(NodeContext &context, MeshTransform3D &transform) {
			const auto vector = [&](std::string_view port, Vector3 &target) {
				const Value *value = context.Find(port);
				if (!value) return true;
				if (const auto *typed = std::get_if<Vector3>(value))
					target = *typed;
				else if (const auto *scalar = std::get_if<double>(value))
					target = {*scalar, *scalar, *scalar};
				else if (const auto *integer = std::get_if<int64_t>(value))
					target = {double(*integer), double(*integer), double(*integer)};
				else
					return context.Fail(
						Status::TypeMismatch, "mesh transform requires a numeric Vector3", port
					);
				return MeshFinite(target) ||
					   context.Fail(Status::InvalidValue, "mesh transform must be finite", port);
			};
			if (!vector("position", transform.Position) || !vector("anchor", transform.Anchor) ||
				!vector("scale", transform.Scale))
				return false;
			if (const Value *value = context.Find("rotation")) {
				const auto *rotation = std::get_if<Quaternion>(value);
				if (!rotation)
					return context.Fail(
						Status::TypeMismatch, "mesh rotation requires a Quaternion", "rotation"
					);
				transform.Rotation = *rotation;
			}
			return MeshFinite(transform.Rotation) ||
				   context.Fail(Status::InvalidValue, "mesh rotation must be finite", "rotation");
		}
		struct MaterialInput {
			const MaterialValue3D *Material = nullptr;
			const Image *Surface = nullptr;
			uint64_t Bytes = 0, LogicalBytes = 0;
		};
		bool ReadMaterial(NodeContext &context, std::string_view port, MaterialInput &input) {
			if (const Value *value = context.Find(port)) {
				input.Material = std::get_if<MaterialValue3D>(value);
				if (!input.Material)
					return context.Fail(
						Status::TypeMismatch, "mesh material requires a typed material", port
					);
				if (!ValidMaterialPayload(*input.Material))
					return context.Fail(Status::InvalidValue, "mesh material payload is invalid", port);
				input.Bytes = MaterialStorageBytes<true>(*input.Material);
				input.LogicalBytes = MaterialStorageBytes<false>(*input.Material);
				return true;
			}
			input.Surface = context.Input(port);
			if (!input.Surface) return true;
			const Image &surface = *input.Surface;
			if (!ValidMaterialSurface(surface))
				return context.Fail(
					Status::InvalidValue, "material surface requires a bounded finite layout", port
				);
			input.Bytes = MeshAddBytes(sizeof(MaterialData3D), surface.Pixels.capacity());
			input.LogicalBytes = MeshAddBytes(sizeof(MaterialData3D), surface.Pixels.size());
			return input.Bytes <= Limits::MaximumArrayBytes ||
				   context.Fail(Status::LimitExceeded, "material surface exceeds mesh payload bytes", port);
		}
		MaterialValue3D CloneMaterial(const MaterialInput &input) {
			if (input.Material) return *input.Material;
			MaterialValue3D result;
			if (input.Surface) result.Edit().Surface = *input.Surface;
			return result;
		}
		bool Material(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.material");
			const Image *texture = context.Input("texture"), *normal = context.Input("normal_map");
			const Image *metallic = context.Input("metalic_map"), *roughness = context.Input("roughness_map");
			uint32_t width = 32, height = 32;
			uint64_t retained = sizeof(MaterialData3D), logical = sizeof(MaterialData3D);
			for (const auto &[port, image] : std::array{
					 std::pair{"texture", texture},
					 std::pair{"normal_map", normal},
					 std::pair{"metalic_map", metallic},
					 std::pair{"roughness_map", roughness}
				 }) {
				if (!image) continue;
				if (!ValidMaterialSurface(*image))
					return context.Fail(
						Status::InvalidValue, "material surface requires a bounded finite layout", port
					);
				if (std::string_view(port) == "metalic_map" || std::string_view(port) == "roughness_map") {
					width = std::max(width, image->Width);
					height = std::max(height, image->Height);
				} else {
					retained = MeshAddBytes(retained, image->Pixels.capacity());
					logical = MeshAddBytes(logical, image->Pixels.size());
				}
			}
			const auto layout =
				CheckedSurfaceLayout(width, height, SurfaceFormat::RGBA8Unorm, Limits::MaximumArrayBytes);
			if (!layout)
				return context.Fail(
					Status::LimitExceeded, "material property map exceeds the byte cap", "material"
				);
			retained = MeshAddBytes(retained, layout->Bytes + (texture ? 0 : 4));
			logical = MeshAddBytes(logical, layout->Bytes + (texture ? 0 : 4));
			if (logical > Limits::MaximumArrayBytes)
				return context.Fail(
					Status::LimitExceeded, "material descriptor exceeds the byte cap", "material"
				);
			MaterialData3D descriptor;
			descriptor.TextureScale = context.Get<Vector2>("scale", {1, 1});
			descriptor.TextureShift = context.Get<Vector2>("shift");
			descriptor.TextureFilter = context.SourceChoice("interpolation");
			// Shader selects source control visibility; it is not stored in
			// __d3dMaterial.
			(void)context.SourceChoice("shader");
			descriptor.Diffuse = context.Scalar("diffuse", 1);
			descriptor.Specular = context.Scalar("specular");
			descriptor.Shininess = context.Scalar("shininess", 1);
			descriptor.Metal = context.Boolean("metal");
			descriptor.Reflectance = context.Scalar("reflectance");
			descriptor.NormalStrength = context.Scalar("normal_strength", 1);
			descriptor.MetallicMapped = context.Boolean("metalic_mapped");
			descriptor.RoughnessMapped = context.Boolean("roughness_mapped");
			if (descriptor.MetallicMapped) {
				if (!ReadSourceMaterialRange(context, "metalic", descriptor.MetallicRange)) return false;
			} else {
				const double value = context.Scalar("metalic");
				descriptor.MetallicRange = {value, value};
			}
			if (descriptor.RoughnessMapped) {
				if (!ReadSourceMaterialRange(context, "roughness", descriptor.RoughnessRange)) return false;
			} else {
				const double value = context.Scalar("roughness", 1);
				descriptor.RoughnessRange = {value, value};
			}
			if (context.FailureCode != Status::Ok) return false;
			if (!MeshFinite(descriptor.TextureScale) || !MeshFinite(descriptor.TextureShift) ||
				!MeshFinite(descriptor.MetallicRange) || !MeshFinite(descriptor.RoughnessRange))
				return context.Fail(Status::InvalidValue, "material vectors must be finite", "material");
			for (double number :
				 {descriptor.Diffuse,
				  descriptor.Specular,
				  descriptor.Shininess,
				  descriptor.Reflectance,
				  descriptor.NormalStrength})
				if (!std::isfinite(number))
					return context.Fail(Status::InvalidValue, "material numbers must be finite", "material");
			if (!context.ReserveOutput(MeshAddBytes(retained, PortBytes("material")), "material"))
				return false;
			MaterialValue3D output;
			auto &data = output.Data.emplace();
			data = std::move(descriptor);
			if (texture)
				data.Surface = *texture;
			else {
				data.Surface = Image{1, 1, std::vector<uint8_t>(4, 0)};
				data.Surface->Hash = SurfaceHash(*data.Surface);
			}
			if (normal) data.Normal = *normal;
			data.PropertiesMap = Image{width, height, std::vector<uint8_t>(size_t(layout->Bytes), 0)};
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					SurfacePixel pixel{0, 0, 0, 1};
					if (!StoreSurfacePixel(*data.PropertiesMap, x, y, pixel))
						return context.Fail(
							Status::InvalidValue, "property map initialization failed", "material"
						);
					for (const auto &[port, map] : std::array{
							 std::pair{"metalic_map", metallic}, std::pair{"roughness_map", roughness}
						 }) {
						if (!map) continue;
						const uint32_t sx =
							uint32_t((uint64_t(x) * 2 + 1) * map->Width / (uint64_t(width) * 2));
						const uint32_t sy =
							uint32_t((uint64_t(y) * 2 + 1) * map->Height / (uint64_t(height) * 2));
						SurfacePixel sample;
						if (!LoadSurfacePixel(*map, sx, sy, sample) ||
							!LoadSurfacePixel(*data.PropertiesMap, x, y, pixel))
							return context.Fail(Status::InvalidValue, "property map sample is invalid", port);
						const bool red = std::string_view(port) == "metalic_map";
						const auto format = DescribeSurfaceFormat(map->Format);
						if (format->Channels == 1) sample = {sample[0], sample[0], sample[0], 1};
						pixel[red ? 0 : 1] = sample[red ? 0 : 1];
						pixel[3] += sample[3];
						if (!StoreSurfacePixel(*data.PropertiesMap, x, y, pixel))
							return context.Fail(Status::InvalidValue, "property map sample is invalid", port);
					}
				}
			data.PropertiesMap->Hash = SurfaceHash(*data.PropertiesMap);
			context.SetValue("material", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool Plane(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.plane");
			MeshTransform3D transform;
			if (!ReadTransform(context, transform)) return false;
			const double normal = context.SourceChoice("normal", 2);
			if (context.FailureCode != Status::Ok) return false;
			if (normal != std::trunc(normal))
				return context.Fail(
					Status::UnsupportedExecution,
					"fractional Normal produces unmatched source geometry; native support is unverified",
					"normal"
				);
			if (normal < 0 || normal > 2)
				return context.Fail(
					Status::UnsupportedExecution, "source Normal array row has no geometry case", "normal"
				);
			if (const Value *value = context.Find("both_side");
				value && std::holds_alternative<ArrayValue>(*value))
				return context.Fail(
					Status::UnsupportedExecution, "source Both Side rejects arrays", "both_side"
				);
			const bool both = context.Boolean("both_side");
			if (context.FailureCode != Status::Ok) return false;
			MaterialInput front, back;
			if (!ReadMaterial(context, "material", front) ||
				(both && !ReadMaterial(context, "back_material", back)))
				return false;
			const size_t parts = both ? 2 : 1;
			uint64_t bytes =
				parts * (sizeof(MeshPart3D) + 6 * sizeof(MeshVertex3D) + sizeof(MaterialValue3D));
			bytes =
				MeshAddBytes(bytes, sizeof(MeshData3D) + 4 * sizeof(MeshEdge3D) + sizeof(MeshTransform3D));
			uint64_t logicalBytes = MeshAddBytes(bytes, front.LogicalBytes);
			if (both) logicalBytes = MeshAddBytes(logicalBytes, back.LogicalBytes);
			bytes = MeshAddBytes(bytes, front.Bytes);
			if (both) bytes = MeshAddBytes(bytes, back.Bytes);
			if (logicalBytes > Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "plane mesh exceeds payload bytes", "mesh");
			if (!context.ReserveOutput(MeshAddBytes(bytes, PortBytes("mesh")), "mesh")) return false;
			const std::array<Vector3, 3> normals{Vector3{1, 0, 0}, Vector3{0, 1, 0}, Vector3{0, 0, 1}};
			const Vector3 axis = normals[size_t(normal)];
			std::array<MeshVertex3D, 6> vertices{};
			if (normal == 0)
				vertices = {
					{{{0, -.5, -.5}, axis, {0, 1}},
					 {{0, .5, .5}, axis, {1, 0}},
					 {{0, .5, -.5}, axis, {1, 1}},
					 {{0, -.5, -.5}, axis, {0, 1}},
					 {{0, -.5, .5}, axis, {0, 0}},
					 {{0, .5, .5}, axis, {1, 0}}}
				};
			else if (normal == 1)
				vertices = {
					{{{-.5, 0, -.5}, axis, {1, 1}},
					 {{.5, 0, -.5}, axis, {0, 1}},
					 {{.5, 0, .5}, axis, {0, 0}},
					 {{-.5, 0, -.5}, axis, {1, 1}},
					 {{.5, 0, .5}, axis, {0, 0}},
					 {{-.5, 0, .5}, axis, {1, 0}}}
				};
			else
				vertices = {
					{{{-.5, -.5, 0}, axis, {0, 0}},
					 {{.5, .5, 0}, axis, {1, 1}},
					 {{.5, -.5, 0}, axis, {0, 1}},
					 {{-.5, -.5, 0}, axis, {0, 0}},
					 {{-.5, .5, 0}, axis, {1, 0}},
					 {{.5, .5, 0}, axis, {1, 1}}}
				};
			MeshValue3D output;
			auto &data = output.Data.emplace();
			data.Parts.reserve(parts);
			data.Materials.reserve(parts);
			data.LocalTransforms.reserve(1);
			data.LocalTransforms.push_back(transform);
			data.Edges.reserve(4);
			const std::array<size_t, 4> corners =
				normal == 1 ? std::array<size_t, 4>{0, 1, 2, 5} : std::array<size_t, 4>{0, 2, 1, 4};
			for (size_t i = 0; i < corners.size(); ++i)
				data.Edges.push_back(
					{vertices[corners[i]].Position, vertices[corners[(i + 1) % 4]].Position}
				);
			for (size_t part = 0; part < parts; ++part) {
				data.Materials.push_back(CloneMaterial(part == 0 ? front : back));
				auto &meshPart = data.Parts.emplace_back();
				meshPart.MaterialIndex = uint32_t(part);
				meshPart.Vertices.reserve(6);
				for (size_t i = 0; i < vertices.size(); ++i) {
					auto vertex = vertices[part == 0 ? i : 5 - i];
					if (part != 0) vertex.Normal = {-vertex.Normal.X, -vertex.Normal.Y, -vertex.Normal.Z};
					meshPart.Vertices.push_back(vertex);
				}
			}
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool Cube(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.cube");
			MeshTransform3D transform;
			if (!ReadTransform(context, transform)) return false;
			const double taper = context.Scalar("taper");
			if (!std::isfinite(taper))
				return context.Fail(Status::InvalidValue, "cube taper must be finite", "taper");
			const double mode = context.SourceChoice("material_mode");
			if (context.FailureCode != Status::Ok) return false;
			if (mode < 0 || mode > 2)
				return context.Fail(
					Status::UnsupportedExecution,
					"source material mode is outside its defined cases",
					"material_mode"
				);
			double axis = 0;
			if (taper != 0) {
				axis = context.SourceChoice("taper_axis");
				if (context.FailureCode != Status::Ok) return false;
				if (axis != std::trunc(axis) || axis < 0 || axis > 2)
					return context.Fail(
						Status::UnsupportedExecution,
						"fractional cube taper axis indexing remains unverified",
						"taper_axis"
					);
			}
			Vector3 subdivisions{1, 1, 1};
			if (const auto *value = context.Find("subdivision")) {
				if (const auto *v = std::get_if<Vector3>(value))
					subdivisions = *v;
				else if (const auto *v = std::get_if<ArrayValue>(value);
						 v && v->Nested.empty() && v->Elements.size() == 3) {
					std::array<double, 3> components{};
					for (size_t i = 0; i < 3; ++i) {
						if (const auto *n = std::get_if<double>(&v->Elements[i]))
							components[i] = *n;
						else if (const auto *n = std::get_if<int64_t>(&v->Elements[i]))
							components[i] = double(*n);
						else
							return context.Fail(
								Status::TypeMismatch,
								"cube subdivision needs numeric components",
								"subdivision"
							);
					}
					subdivisions = {components[0], components[1], components[2]};
				} else
					return context.Fail(
						Status::TypeMismatch, "cube subdivision needs three numeric components", "subdivision"
					);
			}
			std::array<uint64_t, 3> count{};
			std::array<double, 3> denominator{};
			const std::array<double, 3> raw{subdivisions.X, subdivisions.Y, subdivisions.Z};
			for (size_t i = 0; i < count.size(); ++i) {
				if (!std::isfinite(raw[i]))
					return context.Fail(
						Status::InvalidValue, "cube subdivision must be finite", "subdivision"
					);
				denominator[i] = std::max(1.0, raw[i]);
				const double rounded = std::ceil(denominator[i]);
				if (rounded > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "cube subdivision exceeds the vertex cap", "subdivision"
					);
				count[i] = uint64_t(rounded);
			}
			const uint64_t vertices = 12 * (count[0] * count[1] + count[1] * count[2] + count[2] * count[0]);
			if (vertices > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "cube exceeds the vertex cap", "subdivision");
			const std::array<std::string_view, 6> ports{
				"material",
				"material_bottom",
				"material_left",
				"material_right",
				"material_back",
				"material_front"
			};
			std::array<MaterialInput, 6> materials{};
			for (size_t i = 0; i < materials.size(); ++i)
				if (!ReadMaterial(context, ports[i], materials[i])) return false;
			const size_t parts = mode > .5 ? 6 : 1;
			uint64_t bytes = sizeof(MeshData3D) + parts * sizeof(MeshPart3D) +
							 vertices * sizeof(MeshVertex3D) + 12 * sizeof(MeshEdge3D) +
							 6 * sizeof(MaterialValue3D) + sizeof(MeshTransform3D);
			uint64_t logical = bytes;
			for (size_t i = 0; i < 6; ++i) {
				const size_t source = mode == 2 ? (i < 2 ? 0 : 1) : i;
				bytes = MeshAddBytes(bytes, materials[source].Bytes);
				logical = MeshAddBytes(logical, materials[source].LogicalBytes);
			}
			if (logical > Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "cube exceeds payload bytes", "mesh");
			if (!context.ReserveOutput(MeshAddBytes(bytes, PortBytes("mesh")), "mesh")) return false;
			std::array<Vector3, 8> corners{};
			for (size_t i = 0; i < corners.size(); ++i) {
				std::array<double, 3> p{(i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0};
				if (taper != 0) {
					const size_t a = size_t(axis);
					for (size_t j : {(a + 1) % 3, (a + 2) % 3})
						p[j] += (p[j] > 0 ? 1 : -1) * p[a] * taper;
				}
				corners[i] = {p[0] / 2, p[1] / 2, p[2] / 2};
			}
			MeshValue3D output;
			auto &data = output.Data.emplace();
			data.Parts.reserve(parts);
			data.Materials.reserve(6);
			data.Edges.reserve(12);
			data.LocalTransforms.reserve(1);
			data.LocalTransforms.push_back(transform);
			for (size_t i = 0; i < 6; ++i)
				data.Materials.push_back(CloneMaterial(materials[mode == 2 ? (i < 2 ? 0 : 1) : i]));
			const std::array<std::array<size_t, 2>, 12> edges{
				{{0, 1},
				 {1, 3},
				 {3, 2},
				 {2, 0},
				 {4, 5},
				 {5, 7},
				 {7, 6},
				 {6, 4},
				 {0, 4},
				 {1, 5},
				 {2, 6},
				 {3, 7}}
			};
			for (auto edge : edges)
				data.Edges.push_back({corners[edge[0]], corners[edge[1]]});
			struct Face {
				std::array<size_t, 4> Corners;
				size_t U, V;
				Vector3 Normal;
				std::array<size_t, 6> Order;
			};
			// Face ids and uniform concatenation order are distinct in the source.
			const std::array<Face, 6> faces{
				{{{7, 5, 6, 4}, 0, 1, {0, 0, 1}, {3, 0, 1, 3, 2, 0}},
				 {{3, 1, 2, 0}, 0, 1, {0, 0, -1}, {3, 1, 0, 3, 0, 2}},
				 {{6, 2, 4, 0}, 1, 2, {-1, 0, 0}, {2, 1, 0, 2, 3, 1}},
				 {{5, 1, 7, 3}, 1, 2, {1, 0, 0}, {0, 2, 3, 0, 3, 1}},
				 {{7, 3, 6, 2}, 0, 2, {0, 1, 0}, {2, 1, 0, 2, 3, 1}},
				 {{4, 0, 5, 1}, 0, 2, {0, -1, 0}, {0, 2, 3, 0, 3, 1}}}
			};
			for (size_t i = 0; i < parts; ++i) {
				auto &part = data.Parts.emplace_back();
				part.MaterialIndex = uint32_t(i);
				part.Vertices.reserve(
					parts == 1 ? size_t(vertices) : size_t(6 * count[faces[i].U] * count[faces[i].V])
				);
			}
			const auto lerp = [](Vector3 a, Vector3 b, double t) {
				return Vector3{a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t};
			};
			for (size_t id : {2u, 3u, 4u, 5u, 0u, 1u}) {
				const auto &face = faces[id];
				auto &part = data.Parts[parts == 1 ? 0 : id];
				for (uint64_t i = 0; i < count[face.U]; ++i)
					for (uint64_t j = 0; j < count[face.V]; ++j) {
						const double u0 = double(i) / denominator[face.U],
									 u1 = double(i + 1) / denominator[face.U],
									 v0 = double(j) / denominator[face.V],
									 v1 = double(j + 1) / denominator[face.V];
						const auto a = lerp(corners[face.Corners[0]], corners[face.Corners[2]], u0),
								   b = lerp(corners[face.Corners[1]], corners[face.Corners[3]], u0),
								   c = lerp(corners[face.Corners[0]], corners[face.Corners[2]], u1),
								   d = lerp(corners[face.Corners[1]], corners[face.Corners[3]], u1);
						const std::array<Vector3, 4> p{
							lerp(a, b, v0), lerp(a, b, v1), lerp(c, d, v0), lerp(c, d, v1)
						};
						const std::array<Vector2, 4> uv{{{u0, v0}, {u0, v1}, {u1, v0}, {u1, v1}}};
						for (size_t vertex : face.Order)
							part.Vertices.push_back({p[vertex], face.Normal, uv[vertex]});
					}
			}
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool Cylinder(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.cylinder");
			MeshTransform3D transform;
			if (!ReadTransform(context, transform)) return false;
			const int64_t side = std::max(int64_t(3), context.Integer("side", 8));
			const int64_t segment = std::max(int64_t(1), context.Integer("segments", 1));
			const bool caps = context.Boolean("end_caps", true), smooth = context.Boolean("smooth_side");
			if (context.FailureCode != Status::Ok) return false;
			if (side > int64_t(Limits::MaximumArrayElements) ||
				segment > int64_t(Limits::MaximumArrayElements))
				return context.Fail(
					Status::LimitExceeded, "cylinder resolution exceeds the vertex cap", "segments"
				);
			const uint64_t sides = uint64_t(side), segments = uint64_t(segment);
			const uint64_t vertices = 6 * sides * segments + (caps ? 6 * sides : 0),
						   edges = 2 * sides * (segments + 1);
			if (vertices > Limits::MaximumArrayElements || edges > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "cylinder exceeds geometry count caps", "mesh");
			const Curve *profile = nullptr;
			if (const auto *value = context.Find("profile")) {
				profile = std::get_if<Curve>(value);
				if (!profile)
					return context.Fail(Status::TypeMismatch, "cylinder profile requires a Curve", "profile");
				if (!ValidPayload(*profile, false))
					return context.Fail(
						Status::InvalidValue, "cylinder profile must be finite and bounded", "profile"
					);
			}
			auto profilesCharge = context.ReserveWorkspace((segments + 1) * sizeof(double), "profile");
			if (!profilesCharge) return false;
			std::vector<double> profiles;
			profiles.reserve(size_t(segments + 1));
			for (uint64_t j = 0; j <= segments; ++j) {
				const double sample = profile ? EvalCurveX(*profile, double(j) / double(segments)) : 1;
				if (!std::isfinite(sample))
					return context.Fail(
						Status::InvalidValue, "cylinder profile sample is nonfinite", "profile"
					);
				profiles.push_back(sample);
			}
			std::array<MaterialInput, 3> materials{};
			const std::array<std::string_view, 3> ports{"material_side", "material_top", "material_bottom"};
			for (size_t i = 0; i < 3; ++i)
				if (!ReadMaterial(context, ports[i], materials[i])) return false;
			const uint64_t parts = caps ? 3 : 1;
			uint64_t bytes = sizeof(MeshData3D) + parts * sizeof(MeshPart3D) +
							 vertices * sizeof(MeshVertex3D) + edges * sizeof(MeshEdge3D) +
							 3 * sizeof(MaterialValue3D) + sizeof(MeshTransform3D);
			uint64_t logical = bytes;
			for (const auto &material : materials) {
				bytes = MeshAddBytes(bytes, material.Bytes);
				logical = MeshAddBytes(logical, material.LogicalBytes);
			}
			if (logical > Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "cylinder exceeds payload bytes", "mesh");
			if (!context.ReserveOutput(MeshAddBytes(bytes, PortBytes("mesh")), "mesh")) return false;
			MeshValue3D output;
			auto &data = output.Data.emplace();
			data.Parts.reserve(size_t(parts));
			data.Edges.reserve(size_t(edges));
			data.Materials.reserve(3);
			data.LocalTransforms.reserve(1);
			data.LocalTransforms.push_back(transform);
			for (const auto &material : materials)
				data.Materials.push_back(CloneMaterial(material));
			for (size_t i = 0; i < size_t(parts); ++i) {
				auto &part = data.Parts.emplace_back();
				part.MaterialIndex = uint32_t(i);
				part.Vertices.reserve(size_t(i == 0 ? 6 * sides * segments : 3 * sides));
			}
			const auto direction = [](double length, double turn) {
				const double angle = turn * 360 * (std::numbers::pi / 180);
				const auto snap = [](double value) {
					const double nearest = std::round(value);
					return std::abs(value - nearest) <= .0001 ? nearest : value;
				};
				return Vector2{snap(length * std::cos(angle)), snap(-length * std::sin(angle))};
			};
			// Source cap edges exist even when caps are hidden from the vertex parts.
			for (uint64_t i = 0; i < sides; ++i) {
				const auto a = direction(1, double(i) / double(sides)),
						   b = direction(1, double(i + 1) / double(sides));
				const auto ua = direction(.5, double(i) / double(sides)),
						   ub = direction(.5, double(i + 1) / double(sides));
				const double top = .5 * profiles.back(), bottom = .5 * profiles.front();
				const Vector3 at{a.X * top, a.Y * top, .5}, bt{b.X * top, b.Y * top, .5},
					ab{a.X * bottom, a.Y * bottom, -.5}, bb{b.X * bottom, b.Y * bottom, -.5};
				data.Edges.push_back({at, bt});
				data.Edges.push_back({ab, bb});
				if (caps) {
					auto &t = data.Parts[1].Vertices;
					t.push_back({{0, 0, .5}, {0, 0, 1}, {.5, .5}});
					t.push_back({at, {0, 0, 1}, {.5 + ua.X, .5 + ua.Y}});
					t.push_back({bt, {0, 0, 1}, {.5 + ub.X, .5 + ub.Y}});
					auto &v = data.Parts[2].Vertices;
					v.push_back({{0, 0, -.5}, {0, 0, -1}, {.5, .5}});
					v.push_back({bb, {0, 0, -1}, {.5 + ub.X, .5 + ub.Y}});
					v.push_back({ab, {0, 0, -1}, {.5 + ua.X, .5 + ua.Y}});
				}
			}
			for (uint64_t i = 0; i < sides; ++i) {
				const double u0 = double(i) / double(sides), u1 = double(i + 1) / double(sides);
				const auto a = direction(1, u0), b = direction(1, u1), middle = direction(1, (u0 + u1) / 2);
				const auto normalA = smooth ? a : middle, normalB = smooth ? b : middle;
				for (uint64_t j = 0; j < segments; ++j) {
					const double spacing = 1.0 / double(segments), v0 = double(j) * spacing,
								 v1 = v0 + spacing;
					const double r0 = .5 * profiles[size_t(j)], r1 = .5 * profiles[size_t(j + 1)];
					const Vector3 a0{a.X * r0, a.Y * r0, -.5 + v0}, b0{b.X * r0, b.Y * r0, -.5 + v0},
						a1{a.X * r1, a.Y * r1, -.5 + v1}, b1{b.X * r1, b.Y * r1, -.5 + v1};
					double z0 = (r1 - r0) / .5, z1 = z0;
					if (smooth) {
						const double previous = j > 0 ? (r0 - .5 * profiles[size_t(j - 1)]) / .5 : z0,
									 next = j < segments - 1 ? (.5 * profiles[size_t(j + 2)] - r1) / .5 : z1;
						z0 = (z0 + previous) / 2;
						z1 = (z1 + next) / 2;
					}
					const double length0 = std::sqrt(normalA.X * normalA.X + normalA.Y * normalA.Y + z0 * z0),
								 length1 = std::sqrt(normalB.X * normalB.X + normalB.Y * normalB.Y + z1 * z1);
					const Vector3 n0{normalA.X / length0, normalA.Y / length0, z0 / length0},
						n1{normalB.X / length1, normalB.Y / length1, z1 / length1};
					// The source mixes nz1 with n0's xy and nz0 with n1's xy on two vertices.
					auto &v = data.Parts[0].Vertices;
					v.push_back({a1, {n0.X, n0.Y, n1.Z}, {u0, v1}});
					v.push_back({a0, n0, {u0, v0}});
					v.push_back({b1, n1, {u1, v1}});
					v.push_back({a0, n0, {u0, v0}});
					v.push_back({b0, {n1.X, n1.Y, n0.Z}, {u1, v0}});
					v.push_back({b1, n1, {u1, v1}});
					data.Edges.push_back({a0, a1});
					data.Edges.push_back({b0, b1});
				}
			}
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}

		bool Cone(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.cone");
			MeshTransform3D transform;
			if (!ReadTransform(context, transform)) return false;
			// Ordinary Int projection owns validator/rounding. Raw deep source arrays bypass it.
			if (const Value *value = context.Find("side")) {
				if (const auto *raw = std::get_if<double>(value); raw && !std::isfinite(*raw))
					return context.Fail(Status::InvalidValue, "cone Side must be finite", "side");
				if (const auto *raw = std::get_if<double>(value); raw && std::trunc(*raw) != *raw)
					return context.Fail(
						Status::UnsupportedExecution,
						"fractional raw Cone Side geometry is unverified",
						"side"
					);
			}
			const int64_t side = context.Integer("side", 8);
			const bool smooth = context.Boolean("smooth_side");
			if (context.FailureCode != Status::Ok) return false;
			if (side < 0)
				return context.Fail(Status::InvalidValue, "cone side count must be nonnegative", "side");
			if (side > int64_t(Limits::MaximumArrayElements))
				return context.Fail(Status::LimitExceeded, "cone side count exceeds geometry caps", "side");
			const uint64_t sides = uint64_t(side), vertices = 6 * sides, edges = 2 * sides;
			if (vertices > Limits::MaximumArrayElements || edges > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "cone exceeds geometry count caps", "mesh");
			std::array<MaterialInput, 2> materials{};
			const std::array<std::string_view, 2> ports{"material_bottom", "material_side"};
			for (size_t i = 0; i < materials.size(); ++i)
				if (!ReadMaterial(context, ports[i], materials[i])) return false;
			uint64_t bytes = sizeof(MeshData3D) + 2 * sizeof(MeshPart3D) + vertices * sizeof(MeshVertex3D) +
							 edges * sizeof(MeshEdge3D) + 2 * sizeof(MaterialValue3D) +
							 sizeof(MeshTransform3D);
			uint64_t logical = bytes;
			for (const auto &material : materials) {
				bytes = MeshAddBytes(bytes, material.Bytes);
				logical = MeshAddBytes(logical, material.LogicalBytes);
			}
			if (logical > Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "cone exceeds payload bytes", "mesh");
			if (!context.ReserveOutput(MeshAddBytes(bytes, PortBytes("mesh")), "mesh")) return false;
			MeshValue3D output;
			auto &data = output.Data.emplace();
			data.Parts.reserve(2);
			data.Edges.reserve(size_t(edges));
			data.Materials.reserve(2);
			data.LocalTransforms.reserve(1);
			data.LocalTransforms.push_back(transform);
			for (const auto &material : materials)
				data.Materials.push_back(CloneMaterial(material));
			for (size_t i = 0; i < 2; ++i) {
				auto &part = data.Parts.emplace_back();
				part.MaterialIndex = uint32_t(i);
				part.Vertices.reserve(size_t(3 * sides));
			}
			const auto direction = [](double length, double turn) {
				const double angle = turn * 360 * (std::numbers::pi / 180);
				const auto snap = [](double value) {
					const double nearest = std::round(value);
					return std::abs(value - nearest) <= .0001 ? nearest : value;
				};
				return Vector2{snap(length * std::cos(angle)), snap(-length * std::sin(angle))};
			};
			for (uint64_t i = 0; i < sides; ++i) {
				const double u0 = double(i) / double(sides), u1 = double(i + 1) / double(sides);
				const auto a = direction(.5, u0), b = direction(.5, u1);
				const Vector3 a0{a.X, a.Y, -.5}, b0{b.X, b.Y, -.5};
				auto &bottom = data.Parts[0].Vertices;
				bottom.push_back({{0, 0, -.5}, {0, 0, -1}, {.5, .5}});
				bottom.push_back({b0, {0, 0, -1}, {.5 + b.X, .5 + b.Y}});
				bottom.push_back({a0, {0, 0, -1}, {.5 + a.X, .5 + a.Y}});
				data.Edges.push_back({a0, b0});
			}
			for (uint64_t i = 0; i < sides; ++i) {
				const double u0 = double(i) / double(sides), u1 = double(i + 1) / double(sides);
				const auto a = direction(.5, u0), b = direction(.5, u1), middle = direction(1, (u0 + u1) / 2);
				const auto n0 = smooth ? direction(1, u0) : middle, n1 = smooth ? direction(1, u1) : middle;
				// Source nz is radius squared / (radius squared + height squared), not a unit normal.
				constexpr double NZ = .25 / (.25 + 1);
				auto &sideVertices = data.Parts[1].Vertices;
				sideVertices.push_back({{0, 0, .5}, {middle.X, middle.Y, NZ}, {(u0 + u1) / 2, 0}});
				sideVertices.push_back({{a.X, a.Y, -.5}, {n0.X, n0.Y, NZ}, {u0, 1}});
				sideVertices.push_back({{b.X, b.Y, -.5}, {n1.X, n1.Y, NZ}, {u1, 1}});
				data.Edges.push_back({{a.X, a.Y, -.5}, {0, 0, .5}});
			}
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}

		bool Torus(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.torus");
			MeshTransform3D transform;
			if (!ReadTransform(context, transform)) return false;
			std::array<double, 2> denominator{};
			std::array<uint64_t, 2> count{};
			const std::array<std::string_view, 2> slices{"toroidal_slices", "poloidal_slices"};
			for (size_t i = 0; i < 2; ++i) {
				denominator[i] = context.Scalar(slices[i], i == 0 ? 16 : 8);
				if (!std::isfinite(denominator[i]) || denominator[i] < 0)
					return context.Fail(
						Status::InvalidValue, "torus slices must be finite and nonnegative", slices[i]
					);
				if (denominator[i] > double(Limits::MaximumArrayElements))
					return context.Fail(
						Status::LimitExceeded, "torus slices exceed geometry caps", slices[i]
					);
				count[i] = uint64_t(std::ceil(denominator[i]));
			}
			const uint64_t cells = count[0] * count[1], vertices = 6 * cells, edges = 4 * cells;
			if (vertices > Limits::MaximumArrayElements || edges > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "torus exceeds geometry count caps", "mesh");
			const double major = context.Scalar("toroidal_radius", 1),
						 minor = context.Scalar("poloidal_radius", .2),
						 angleT = context.Scalar("toroidal_angle"), angleP = context.Scalar("poloidal_angle"),
						 twist = context.Scalar("twist");
			const std::array<std::string_view, 5> scalarPorts{
				"toroidal_radius", "poloidal_radius", "toroidal_angle", "poloidal_angle", "twist"
			};
			const std::array<double, 5> scalarValues{major, minor, angleT, angleP, twist};
			for (size_t i = 0; i < scalarPorts.size(); ++i)
				if (!std::isfinite(scalarValues[i]))
					return context.Fail(Status::InvalidValue, "torus control must be finite", scalarPorts[i]);
			const bool smooth = context.Boolean("smooth_normal");
			if (context.FailureCode != Status::Ok) return false;
			MaterialInput material;
			if (!ReadMaterial(context, "material", material)) return false;
			const uint64_t shape = sizeof(MeshData3D) + sizeof(MeshPart3D) + vertices * sizeof(MeshVertex3D) +
								   edges * sizeof(MeshEdge3D) + sizeof(MaterialValue3D) +
								   sizeof(MeshTransform3D);
			if (MeshAddBytes(shape, material.LogicalBytes) > Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "torus exceeds payload bytes", "mesh");
			if (!context.ReserveOutput(
					MeshAddBytes(MeshAddBytes(shape, material.Bytes), PortBytes("mesh")), "mesh"
				))
				return false;
			MeshValue3D output;
			auto &data = output.Data.emplace();
			data.Parts.reserve(1);
			data.Materials.reserve(1);
			data.Edges.reserve(size_t(edges));
			data.LocalTransforms.reserve(1);
			data.LocalTransforms.push_back(transform);
			data.Materials.push_back(CloneMaterial(material));
			auto &part = data.Parts.emplace_back();
			part.Vertices.reserve(size_t(vertices));
			const auto direction = [](double length, double degrees) {
				const double angle = degrees * (std::numbers::pi / 180);
				const auto snap = [](double value) {
					const double nearest = std::round(value);
					return std::abs(value - nearest) <= .0001 ? nearest : value;
				};
				return Vector2{snap(length * std::cos(angle)), snap(-length * std::sin(angle))};
			};
			for (uint64_t i = 0; i < count[0]; ++i) {
				const auto t0 = direction(1, double(i) / denominator[0] * 360 + angleT),
						   t1 = direction(1, double(i + 1) / denominator[0] * 360 + angleT);
				const Vector2 center0{t0.X * major, t0.Y * major}, center1{t1.X * major, t1.Y * major};
				for (uint64_t j = 0; j < count[1]; ++j) {
					// Twist offsets each right edge by the same poloidal slice amount, not i*twist.
					const auto p00 = direction(minor, double(j) / denominator[1] * 360 + angleP),
							   p01 = direction(minor, double(j + 1) / denominator[1] * 360 + angleP),
							   p10 = direction(minor, (double(j) + twist) / denominator[1] * 360 + angleP),
							   p11 =
								   direction(minor, (double(j + 1) + twist) / denominator[1] * 360 + angleP);
					const std::array<Vector3, 4> p{
						{{t0.X * (major + p00.X), t0.Y * (major + p00.X), p00.Y},
						 {t1.X * (major + p10.X), t1.Y * (major + p10.X), p10.Y},
						 {t1.X * (major + p11.X), t1.Y * (major + p11.X), p11.Y},
						 {t0.X * (major + p01.X), t0.Y * (major + p01.X), p01.Y}}
					};
					std::array<Vector3, 4> normal{};
					if (smooth) {
						for (size_t k = 0; k < 4; ++k) {
							const auto center = k == 0 || k == 3 ? center0 : center1;
							normal[k] = {p[k].X - center.X, p[k].Y - center.Y, p[k].Z};
						}
					} else {
						const Vector3 average{
							(p[0].X + p[1].X + p[2].X + p[3].X) / 4 - (center0.X + center1.X) / 2,
							(p[0].Y + p[1].Y + p[2].Y + p[3].Y) / 4 - (center0.Y + center1.Y) / 2,
							(p[0].Z + p[1].Z + p[2].Z + p[3].Z) / 4
						};
						normal.fill(average);
					}
					const double u0 = 1 - double(i) / denominator[0], u1 = 1 - double(i + 1) / denominator[0],
								 v0 = 1 - double(j) / denominator[1], v1 = 1 - double(j + 1) / denominator[1];
					const std::array<Vector2, 4> uv{{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
					for (size_t k = 0; k < 4; ++k)
						if (!MeshFinite(p[k]) || !MeshFinite(normal[k]))
							return context.Fail(
								Status::InvalidValue, "torus generated nonfinite geometry", "mesh"
							);
					for (size_t k : {0u, 2u, 1u, 0u, 3u, 2u})
						part.Vertices.push_back({p[k], normal[k], uv[k]});
					for (size_t k = 0; k < 4; ++k)
						data.Edges.push_back({p[k], p[(k + 1) % 4]});
				}
			}
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}

		bool UVSphere(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.sphere_uv");
			MeshTransform3D transform;
			if (!ReadTransform(context, transform)) return false;
			const double horizontal = context.Scalar("horizontal_slices", 8),
						 vertical = context.Scalar("vertical_slices", 16);
			const std::array<std::string_view, 2> ports{"horizontal_slices", "vertical_slices"};
			const std::array<double, 2> denominators{horizontal, vertical};
			for (size_t i = 0; i < 2; ++i) {
				if (!std::isfinite(denominators[i]) || denominators[i] < 0)
					return context.Fail(
						Status::InvalidValue, "sphere slices must be finite and nonnegative", ports[i]
					);
				if (denominators[i] > double(Limits::MaximumArrayElements))
					return context.Fail(
						Status::LimitExceeded, "sphere slices exceed geometry caps", ports[i]
					);
			}
			// The pinned array index uses i*horizontal. Real-index coercion is not documented.
			if (vertical != 0 && std::trunc(horizontal) != horizontal)
				return context.Fail(
					Status::UnsupportedExecution,
					"raw fractional horizontal sphere slices require unverified GML array-index coercion",
					"horizontal_slices"
				);
			const uint64_t h = uint64_t(std::ceil(horizontal)), v = uint64_t(std::ceil(vertical));
			const uint64_t cells = h * v, vertices = 6 * cells, edges = 2 * cells;
			if (vertices > Limits::MaximumArrayElements || edges > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "sphere exceeds geometry count caps", "mesh");
			const bool smooth = context.Boolean("smooth_normal");
			const int64_t projection = cells != 0 ? context.Integer("projection") : 0;
			if (context.FailureCode != Status::Ok) return false;
			if (projection < 0 || projection > 2)
				return context.Fail(
					Status::UnsupportedExecution,
					"source sphere projection leaves UV values undefined outside cases 0 to 2",
					"projection"
				);
			MaterialInput material;
			if (!ReadMaterial(context, "material", material)) return false;
			const uint64_t shape = sizeof(MeshData3D) + sizeof(MeshPart3D) + vertices * sizeof(MeshVertex3D) +
								   edges * sizeof(MeshEdge3D) + sizeof(MaterialValue3D) +
								   sizeof(MeshTransform3D);
			if (MeshAddBytes(shape, material.LogicalBytes) > Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "sphere exceeds payload bytes", "mesh");
			if (!context.ReserveOutput(
					MeshAddBytes(MeshAddBytes(shape, material.Bytes), PortBytes("mesh")), "mesh"
				))
				return false;
			MeshValue3D output;
			auto &data = output.Data.emplace();
			data.Parts.reserve(1);
			data.Materials.reserve(1);
			data.Edges.reserve(size_t(edges));
			data.LocalTransforms.reserve(1);
			data.LocalTransforms.push_back(transform);
			data.Materials.push_back(CloneMaterial(material));
			auto &part = data.Parts.emplace_back();
			part.Vertices.reserve(size_t(vertices));
			const auto latitudeV = [&](double latitude) {
				double value = 0;
				if (projection == 0)
					value = std::sin(latitude * std::numbers::pi / 180);
				else if (projection == 1)
					value = latitude / 90;
				else
					value =
						(2 * std::atan(std::exp(latitude * std::numbers::pi / 180)) - std::numbers::pi / 2) /
						(std::numbers::pi / 2);
				return .5 - .5 * value;
			};
			for (uint64_t i = 0; i < v; ++i)
				for (uint64_t j = 0; j < h; ++j) {
					const double longitude0 = double(i) / vertical * 360,
								 longitude1 = double(i + 1) / vertical * 360;
					const double latitude0 = 90 - double(j) / horizontal * 180,
								 latitude1 = 90 - double(j + 1) / horizontal * 180;
					const auto point = [](double longitude, double latitude) {
						const double a = longitude * std::numbers::pi / 180,
									 b = latitude * std::numbers::pi / 180;
						const double radius = .5 * std::cos(b);
						// Sphere uses degree sin/cos, not the negative-Y snapped lengthdir functions.
						return Vector3{std::cos(a) * radius, std::sin(a) * radius, .5 * std::sin(b)};
					};
					const std::array<Vector3, 4> points{
						point(longitude0, latitude0),
						point(longitude1, latitude0),
						point(longitude0, latitude1),
						point(longitude1, latitude1)
					};
					std::array<Vector3, 4> normals = points;
					if (!smooth) {
						const Vector3 a{
							points[2].X - points[0].X, points[2].Y - points[0].Y, points[2].Z - points[0].Z
						},
							b{points[1].X - points[0].X,
							  points[1].Y - points[0].Y,
							  points[1].Z - points[0].Z};
						Vector3 normal{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
						const double magnitude =
							std::sqrt(normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z);
						if (!std::isfinite(magnitude) || magnitude == 0)
							return context.Fail(
								Status::InvalidValue,
								"sphere flat normal normalization produces nonfinite geometry",
								"mesh"
							);
						normal = {normal.X / magnitude, normal.Y / magnitude, normal.Z / magnitude};
						normals.fill(normal);
					}
					const double u0 = longitude0 / 360;
					const double u1 = longitude1 / 360, v0 = latitudeV(latitude0), v1 = latitudeV(latitude1);
					const std::array<Vector2, 4> uv{{{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}}};
					for (size_t k = 0; k < 4; ++k)
						if (!MeshFinite(points[k]) || !MeshFinite(normals[k]) || !std::isfinite(uv[k].X) ||
							!std::isfinite(uv[k].Y))
							return context.Fail(
								Status::InvalidValue, "sphere generated nonfinite geometry", "mesh"
							);
					for (size_t k : {0u, 1u, 2u, 1u, 3u, 2u})
						part.Vertices.push_back({points[k], normals[k], uv[k]});
					data.Edges.push_back({points[0], points[1]});
					data.Edges.push_back({points[0], points[2]});
				}
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}

		bool Icosphere(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.sphere_ico");
			MeshTransform3D transform;
			if (!ReadTransform(context, transform)) return false;
			const double requested = context.Scalar("subdivision", 1);
			if (!std::isfinite(requested) || requested < 0)
				return context.Fail(
					Status::InvalidValue, "icosphere level must be finite and nonnegative", "subdivision"
				);
			if (std::trunc(requested) != requested)
				return context.Fail(
					Status::UnsupportedExecution,
					"raw fractional nested icosphere level requires unverified GML repeat coercion",
					"subdivision"
				);
			const uint32_t level = uint32_t(std::min(requested, 5.0));
			uint64_t faceCount = 20, poolCount = 13;
			for (uint32_t i = 0; i < level; ++i) {
				poolCount += faceCount * 3;
				faceCount *= 4;
			}
			const uint64_t verticesCount = faceCount * 3;
			if (verticesCount > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "icosphere exceeds geometry count caps", "mesh");
			const bool smooth = context.Boolean("smooth_normal");
			MaterialInput material;
			if (context.FailureCode != Status::Ok || !ReadMaterial(context, "material", material))
				return false;
			struct Point {
				Vector3 Position;
				uint32_t First = UINT32_MAX, Last = UINT32_MAX;
				bool Old = false;
			};
			struct Neighbor {
				uint32_t Point, Next = UINT32_MAX;
			};
			struct Midpoint {
				uint32_t A, B, Point;
			};
			using Face = std::array<uint32_t, 3>;
			const uint64_t priorFaces = level ? faceCount / 4 : 0;
			const uint64_t scratchBytes = poolCount * sizeof(Point) + faceCount * 2 * sizeof(Face) +
										  priorFaces * 6 * sizeof(Neighbor) +
										  priorFaces * 3 * sizeof(Midpoint);
			const uint64_t shape = sizeof(MeshData3D) + sizeof(MeshPart3D) +
								   verticesCount * (sizeof(MeshVertex3D) + sizeof(MeshEdge3D)) +
								   sizeof(MaterialValue3D) + sizeof(MeshTransform3D);
			if (MeshAddBytes(shape, material.LogicalBytes) > Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "icosphere exceeds payload bytes", "mesh");
			if (!context.ReserveOutput(
					MeshAddBytes(MeshAddBytes(shape, material.Bytes), PortBytes("mesh")), "mesh"
				))
				return false;
			auto scratch = context.ReserveWorkspace(scratchBytes, "mesh");
			if (!scratch) return false;
			std::vector<Point> points;
			points.reserve(size_t(poolCount));
			std::vector<Face> faces, next;
			faces.reserve(size_t(faceCount));
			next.reserve(size_t(faceCount));
			std::vector<Neighbor> neighbors;
			neighbors.reserve(size_t(priorFaces * 6));
			std::vector<Midpoint> midpoints;
			midpoints.reserve(size_t(priorFaces * 3));
			uint64_t actualScratch = MeshVectorBytes<true>(points);
			for (uint64_t bytes :
				 {MeshVectorBytes<true>(faces),
				  MeshVectorBytes<true>(next),
				  MeshVectorBytes<true>(neighbors),
				  MeshVectorBytes<true>(midpoints)})
				actualScratch = MeshAddBytes(actualScratch, bytes);
			// reserve may retain more than requested; reconcile before populating or reading scratch.
			if (actualScratch == std::numeric_limits<uint64_t>::max() ||
				(actualScratch > scratch->Bytes() &&
				 actualScratch - scratch->Bytes() > context.AvailableBytes()) ||
				!scratch->Resize(actualScratch))
				return context.Fail(
					Status::LimitExceeded,
					"icosphere retained workspace exceeds the evaluation byte budget",
					"mesh"
				);
			const auto normalized = [](Vector3 p) {
				const double length = std::sqrt(p.X * p.X + p.Y * p.Y + p.Z * p.Z);
				return length ? Vector3{p.X / length, p.Y / length, p.Z / length} : p;
			};
			const double phi = (1 + std::sqrt(5.0)) * .5, b = 1 / phi;
			for (Vector3 p : std::array<Vector3, 13>{
					 {{1, 1, 1},
					  {0, b, -1},
					  {b, 1, 0},
					  {-b, 1, 0},
					  {0, b, 1},
					  {0, -b, 1},
					  {-1, 0, b},
					  {0, -b, -1},
					  {1, 0, -b},
					  {1, 0, b},
					  {-1, 0, -b},
					  {b, -1, 0},
					  {-b, -1, 0}}
				 }) {
				p = normalized(p);
				points.push_back({{p.X * .5, p.Y * .5, p.Z * .5}, UINT32_MAX, UINT32_MAX, true});
			}
			constexpr std::array<Face, 20> seeds{
				{{3, 1, 2},	  {2, 4, 3},  {6, 4, 5},   {5, 4, 9},  {8, 1, 7},  {7, 1, 10}, {12, 5, 11},
				 {11, 7, 12}, {10, 3, 6}, {6, 12, 10}, {9, 2, 8},  {8, 11, 9}, {3, 4, 6},  {9, 4, 2},
				 {10, 1, 3},  {2, 1, 8},  {12, 7, 10}, {8, 7, 11}, {6, 5, 12}, {11, 5, 9}}
			};
			faces.insert(faces.end(), seeds.begin(), seeds.end());
			for (uint32_t iteration = 0; iteration < level; ++iteration) {
				next.clear();
				midpoints.clear();
				neighbors.clear();
				const auto midpoint = [&](uint32_t a, uint32_t b) {
					const uint32_t low = std::min(a, b), high = std::max(a, b);
					// Binary64 levels 0..3 matched source-key pooling under nine formatter hypotheses.
					// Keep coordinates in double through smoothing; normalized midpoint geometry differs.
					for (const auto &entry : midpoints)
						if (entry.A == low && entry.B == high) return entry.Point;
					const Vector3 p = points[a].Position, q = points[b].Position;
					const uint32_t index = uint32_t(points.size());
					points.push_back({{(p.X + q.X) / 2, (p.Y + q.Y) / 2, (p.Z + q.Z) / 2}});
					midpoints.push_back({low, high, index});
					return index;
				};
				const auto connect = [&](uint32_t point, uint32_t neighbor) {
					const uint32_t index = uint32_t(neighbors.size());
					neighbors.push_back({neighbor});
					if (points[point].Last != UINT32_MAX)
						neighbors[points[point].Last].Next = index;
					else
						points[point].First = index;
					points[point].Last = index;
				};
				for (const auto &[a, b, c] : faces) {
					const uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
					connect(a, ab);
					connect(a, ca);
					connect(b, ab);
					connect(b, bc);
					connect(c, bc);
					connect(c, ca);
					next.push_back({a, ab, ca});
					next.push_back({ab, b, bc});
					next.push_back({ca, bc, c});
					next.push_back({ab, bc, ca});
				}
				for (const auto &face : next)
					for (uint32_t id : face) {
						auto &point = points[id];
						if (point.Old && point.First != UINT32_MAX) {
							uint32_t count = 0;
							Vector3 sum{};
							for (uint32_t n = point.First; n != UINT32_MAX; n = neighbors[n].Next) {
								const auto p = points[neighbors[n].Point].Position;
								sum.X += p.X;
								sum.Y += p.Y;
								sum.Z += p.Z;
								++count;
							}
							const double k = double(count) / 2, beta = 3 / (5 * k);
							const auto p = point.Position;
							point.Position = {
								sum.X * .5 * beta + p.X * (1 - k * beta),
								sum.Y * .5 * beta + p.Y * (1 - k * beta),
								sum.Z * .5 * beta + p.Z * (1 - k * beta)
							};
						}
						point.Old = true;
						point.First = point.Last = UINT32_MAX;
					}
				faces.swap(next);
			}
			MeshValue3D output;
			auto &data = output.Data.emplace();
			data.Parts.reserve(1);
			data.Materials.reserve(1);
			data.LocalTransforms.reserve(1);
			data.Edges.reserve(size_t(verticesCount));
			data.Materials.push_back(CloneMaterial(material));
			data.LocalTransforms.push_back(transform);
			auto &part = data.Parts.emplace_back();
			part.Vertices.reserve(size_t(verticesCount));
			const auto angle = [](double x, double y) {
				double a = std::atan2(-y, x) * 180 / std::numbers::pi;
				return a < 0 ? a + 360 : a;
			};
			for (const auto &face : faces) {
				const std::array<Vector3, 3> p{
					points[face[0]].Position, points[face[1]].Position, points[face[2]].Position
				};
				const Vector3 a{p[2].X - p[0].X, p[2].Y - p[0].Y, p[2].Z - p[0].Z},
					b{p[1].X - p[0].X, p[1].Y - p[0].Y, p[1].Z - p[0].Z};
				const Vector3 flat{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
				for (const Vector3 &v : p) {
					double vertical = std::fmod(angle(v.X, v.Z) + 90, 360);
					if (vertical > 180) vertical = 360 - vertical;
					part.Vertices.push_back(
						{v, smooth ? normalized(v) : flat, {angle(v.X, v.Y) / 360, vertical / 180}}
					);
				}
				data.Edges.push_back({p[0], p[1]});
				data.Edges.push_back({p[0], p[2]});
				data.Edges.push_back({p[2], p[1]});
			}
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}

		bool ReadMesh(NodeContext &context, const MeshValue3D *&mesh) {
			const Value *value = context.Find("mesh");
			if (!value) return true;
			mesh = std::get_if<MeshValue3D>(value);
			if (!mesh) return context.Fail(Status::TypeMismatch, "mesh input requires an owned mesh", "mesh");
			return ValidMeshPayload(*mesh) ||
				   context.Fail(Status::InvalidValue, "mesh payload is invalid", "mesh");
		}
		bool Transform(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.transform");
			const MeshValue3D *mesh = nullptr;
			if (!ReadMesh(context, mesh)) return false;
			if (!mesh || !mesh->Data) {
				context.SetValue("mesh", MeshValue3D{});
				return context.FailureCode == Status::Ok;
			}
			MeshTransform3D transform;
			if (!ReadTransform(context, transform)) return false;
			const auto &source = *mesh->Data;
			if (source.LocalTransforms.size() == MAXIMUM_MESH_TRANSFORMS)
				return context.Fail(
					Status::LimitExceeded, "mesh transform chain exceeds 64 local records", "mesh"
				);
			const uint64_t bytes = MeshAddBytes(MeshStorageBytes<true>(*mesh), sizeof(MeshTransform3D));
			if (MeshAddBytes(MeshStorageBytes<false>(*mesh), sizeof(MeshTransform3D)) >
				Limits::MaximumArrayBytes)
				return context.Fail(Status::LimitExceeded, "transformed mesh exceeds payload bytes", "mesh");
			if (!context.ReserveOutput(MeshAddBytes(bytes, PortBytes("mesh")), "mesh")) return false;
			MeshValue3D output;
			auto &data = output.Data.emplace();
			data.Parts = source.Parts;
			data.Edges = source.Edges;
			data.Materials = source.Materials;
			data.LocalTransforms.reserve(source.LocalTransforms.size() + 1);
			data.LocalTransforms.push_back(transform);
			data.LocalTransforms.insert(
				data.LocalTransforms.end(), source.LocalTransforms.begin(), source.LocalTransforms.end()
			);
			context.SetValue("mesh", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool GetData(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.mesh.get_data");
			const MeshValue3D *mesh = nullptr;
			if (!ReadMesh(context, mesh)) return false;
			MeshTransform3D transform;
			transform.Scale = {};
			if (mesh && mesh->Data) transform = mesh->Data->LocalTransforms.front();
			const uint64_t bytes =
				PortBytes("origin") + PortBytes("position") + PortBytes("rotation") + PortBytes("scale");
			if (!context.ReserveOutput(bytes, "origin")) return false;
			context.SetValue("origin", transform.Anchor);
			context.SetValue("position", transform.Position);
			context.SetValue(
				"rotation",
				Vector4{
					transform.Rotation.X, transform.Rotation.Y, transform.Rotation.Z, transform.Rotation.W
				}
			);
			context.SetValue("scale", transform.Scale);
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> MeshExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.3_d_material", Material, true},
			ExecutorEntry{"pc.3_d_mesh_plane", Plane, true},
			ExecutorEntry{"pc.3_d_mesh_cube", Cube, true},
			ExecutorEntry{"pc.3_d_mesh_cylinder", Cylinder, true},
			ExecutorEntry{"pc.3_d_mesh_cone", Cone, true},
			ExecutorEntry{"pc.3_d_mesh_torus", Torus, true},
			ExecutorEntry{"pc.3_d_mesh_sphere_uv", UVSphere, true},
			ExecutorEntry{"pc.3_d_mesh_sphere_ico", Icosphere, true},
			ExecutorEntry{"pc.3_d_transform", Transform, true},
			ExecutorEntry{"pc.3_d_get_data", GetData, true}
		};
		return ENTRIES;
	}
}
