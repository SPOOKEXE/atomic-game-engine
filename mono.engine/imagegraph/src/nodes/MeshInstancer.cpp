#include "../SourceGradient.hpp"
#include "../SourceMeshTransform.hpp"
#include "../SourceQuaternion.hpp"
#include "../SourceRandom.hpp"
#include "Path3D.hpp"
namespace engine::imagegraph::detail {
	namespace {
		bool Vector(NodeContext &context, std::string_view id, Vector3 &result) {
			const Value *value = context.Find(id);
			if (!value) return true;
			if (const auto *v = std::get_if<Vector3>(value))
				result = *v;
			else if (const auto *n = std::get_if<double>(value))
				result = {*n, *n, *n};
			else
				return context.Fail(Status::TypeMismatch, "instance control requires Vector3", id);
			return MeshFinite(result) ||
				   context.Fail(Status::InvalidValue, "instance control must be finite", id);
		}
		std::optional<double> Numeric(const ElementValue &value) {
			if (const auto *n = std::get_if<double>(&value)) return *n;
			if (const auto *n = std::get_if<int64_t>(&value)) return double(*n);
			return {};
		}
		bool Scatter(NodeContext &context, std::string_view id, std::array<double, 6> &values) {
			const Value *value = context.Find(id);
			if (!value) return true;
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array || !array->Items.empty() || !array->Nested.empty() || array->Elements.size() < 6)
				return context.Fail(Status::TypeMismatch, "instance scatter requires six scalar fields", id);
			for (size_t i = 0; i < 6; ++i) {
				auto n = Numeric(array->Elements[i]);
				if (!n || !std::isfinite(*n))
					return context.Fail(Status::InvalidValue, "instance scatter must be finite", id);
				values[i] = *n;
			}
			return true;
		}
		bool Row(NodeContext &context, std::string_view id, size_t index, Vector3 &result) {
			const Value *value = context.Find(id);
			if (!value) return true;
			if (const auto *vector = std::get_if<Vector3>(value)) {
				result = *vector;
				return true;
			}
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array) return context.Fail(Status::TypeMismatch, "instance rows require vector data", id);
			const size_t count = !array->Nested.empty()
									 ? array->Nested.size()
									 : (!array->Items.empty() ? array->Items.size() : array->Elements.size());
			if (!count) return true;
			index %= count;
			if (!array->Nested.empty()) {
				const auto &row = array->Nested[index];
				if (row.size() < 3)
					return context.Fail(Status::InvalidValue, "instance vector row is incomplete", id);
				auto a = Numeric(row[0]), b = Numeric(row[1]), c = Numeric(row[2]);
				if (!a || !b || !c)
					return context.Fail(Status::TypeMismatch, "instance row requires numbers", id);
				result = {*a, *b, *c};
				return true;
			}
			const ElementValue *leaf = array->Items.empty()
										   ? &array->Elements[index]
										   : std::get_if<ElementValue>(&array->Items[index].Data);
			if (leaf) {
				if (const auto *vector = std::get_if<Vector3>(leaf)) {
					result = *vector;
					return true;
				}
			}
			if (!array->Items.empty()) {
				const auto *row = std::get_if<std::vector<SourceArrayItem>>(&array->Items[index].Data);
				if (row && row->size() >= 3) {
					std::array<double, 3> xyz{};
					for (size_t i = 0; i < 3; ++i) {
						const auto *item = std::get_if<ElementValue>(&(*row)[i].Data);
						auto n = item ? Numeric(*item) : std::nullopt;
						if (!n)
							return context.Fail(Status::TypeMismatch, "instance row requires numbers", id);
						xyz[i] = *n;
					}
					result = {xyz[0], xyz[1], xyz[2]};
					return true;
				}
			}
			return context.Fail(Status::TypeMismatch, "instance rows require triples", id);
		}
		bool FitsFloat(double value) {
			return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
		}
		struct FlatCost {
			uint64_t Bytes = sizeof(MeshData3D) + sizeof(MeshTransform3D);
			size_t Parts = 0, Vertices = 0;
		};
		template <class T>
		bool Flatten(
			const T &value,
			std::array<const MeshTransform3D *, 128> &chain,
			size_t depth,
			FlatCost &cost,
			MeshData3D *output
		) {
			if constexpr (std::is_same_v<T, MeshValue3D>) {
				if (!value.Data) return true;
				const auto &mesh = *value.Data;
				if (depth + mesh.LocalTransforms.size() > chain.size()) return false;
				for (const auto &transform : mesh.LocalTransforms)
					if (transform.Mirror) return true;
				size_t count = depth;
				for (size_t i = 0; i + 1 < mesh.LocalTransforms.size(); ++i)
					chain[count++] = &mesh.LocalTransforms[i];
				for (const auto &part : mesh.Parts) {
					cost.Parts++;
					cost.Vertices += part.Vertices.size();
					if (cost.Parts > Limits::MaximumArrayElements ||
						cost.Vertices > Limits::MaximumArrayElements)
						return false;
					const auto &material = mesh.Materials[part.MaterialIndex];
					cost.Bytes = MeshAddBytes(
						cost.Bytes,
						sizeof(MeshPart3D) + sizeof(MaterialValue3D) +
							part.Vertices.size() * sizeof(MeshVertex3D) + MaterialStorageBytes<true>(material)
					);
					if (cost.Bytes > Limits::MaximumArrayBytes) return false;
					MeshPart3D *target = nullptr;
					if (output) {
						output->Materials.push_back(material);
						output->Parts.emplace_back();
						target = &output->Parts.back();
						target->MaterialIndex = uint32_t(output->Materials.size() - 1);
						target->Vertices.reserve(part.Vertices.size());
					}
					for (auto vertex : part.Vertices) {
						for (double component :
							 {vertex.Position.X,
							  vertex.Position.Y,
							  vertex.Position.Z,
							  vertex.Normal.X,
							  vertex.Normal.Y,
							  vertex.Normal.Z,
							  vertex.UV.X,
							  vertex.UV.Y})
							if (!FitsFloat(component)) return false;
						vertex.Position = {
							float(vertex.Position.X), float(vertex.Position.Y), float(vertex.Position.Z)
						};
						for (size_t i = count; i > 0; --i)
							vertex.Position = SourceMeshPoint(*chain[i - 1], vertex.Position);
						for (double component : {vertex.Position.X, vertex.Position.Y, vertex.Position.Z})
							if (!FitsFloat(component)) return false;
						vertex.Position = {
							float(vertex.Position.X), float(vertex.Position.Y), float(vertex.Position.Z)
						};
						vertex.Normal = {
							float(vertex.Normal.X), float(vertex.Normal.Y), float(vertex.Normal.Z)
						};
						vertex.UV = {float(vertex.UV.X), float(vertex.UV.Y)};
						if (target) target->Vertices.push_back(vertex);
					}
				}
				return true;
			} else if constexpr (std::is_same_v<T, SceneValue3D> ||
								 std::is_same_v<T, OwnedPayload3D<SceneData3D>>) {
				const auto *data = [&]() {
					if constexpr (std::is_same_v<T, SceneValue3D>)
						return value.Data ? &*value.Data : nullptr;
					else
						return value ? &*value : nullptr;
				}();
				if (!data) return true;
				if (depth >= chain.size()) return false;
				if (data->Transform.Mirror) return true;
				chain[depth] = &data->Transform;
				for (const auto &child : data->Objects)
					if (!std::visit(
							[&](const auto &item) { return Flatten(item, chain, depth + 1, cost, output); },
							child.Data
						))
						return false;
				return true;
			} else
				return true;
		}
	}
	bool SourceMeshInstancer(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.mesh.instancer");
		const Value *source = context.Find("mesh");
		const auto *mesh = source ? std::get_if<MeshValue3D>(source) : nullptr;
		const auto *scene = source ? std::get_if<SceneValue3D>(source) : nullptr;
		if ((!mesh || !mesh->Data) && (!scene || !scene->Data)) {
			context.SetValue("mesh", MeshValue3D{});
			return context.FailureCode == Status::Ok;
		}
		if (!ValidRuntimeValue(*source))
			return context.Fail(Status::InvalidValue, "instance mesh source is invalid", "mesh");
		const double pattern = context.SourceChoice("pattern"),
					 select = context.SourceChoice("colors_per_index_select");
		Vector3 start{}, scale{1, 1, 1}, shift{1, 0, 0}, shiftY{0, 1, 0}, shiftZ{0, 0, 1}, scaleShift{},
			grid{2, 2, 1};
		if (!Vector(context, "starting_position", start) || !Vector(context, "starting_scale", scale) ||
			!Vector(context, "shift_position", shift) || !Vector(context, "shift_position_y", shiftY) ||
			!Vector(context, "shift_position_z", shiftZ) || !Vector(context, "shift_scale", scaleShift) ||
			!Vector(context, "grid", grid))
			return false;
		for (double *component : {&grid.X, &grid.Y, &grid.Z})
			*component = SourceRoundEven(*component);
		Quaternion rotation{}, rotationShift{};
		for (auto [id, target] :
			 {std::pair<std::string_view, Quaternion *>{"starting_rotation", &rotation},
			  {"shift_rotation", &rotationShift}})
			if (const Value *value = context.Find(id)) {
				const auto *q = std::get_if<Quaternion>(value);
				if (!q || !MeshFinite(*q))
					return context.Fail(
						Status::TypeMismatch, "instance rotation requires finite Quaternion", id
					);
				*target = *q;
			}
		int64_t amount = context.Integer("amounts", 1);
		if (pattern == 1) {
			double n = grid.X * grid.Y * grid.Z;
			if (!std::isfinite(n) || n > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "instance grid exceeds count caps", "grid");
			amount = int64_t(n);
		}
		if (amount <= 0) {
			context.SetValue("mesh", MeshValue3D{});
			return context.FailureCode == Status::Ok;
		}
		if (amount > int64_t(Limits::MaximumArrayElements))
			return context.Fail(Status::LimitExceeded, "instance count exceeds caps", "amounts");
		const double radius = context.Scalar("radius", 1),
					 seedValue = context.Scalar("seed", double(uint32_t(context.Request.Seed)));
		const bool uniform = context.Boolean("scale_uniform", true), look = context.Boolean("look_at_center"),
				   follow = context.Boolean("follow_path");
		Vector2 pathRange{0, 1};
		if (const Value *v = context.Find("path_range")) {
			const auto *p = std::get_if<Vector2>(v);
			if (!p || !MeshFinite(*p))
				return context.Fail(
					Status::TypeMismatch, "instance path range requires finite Vector2", "path_range"
				);
			pathRange = *p;
		}
		if (context.FailureCode != Status::Ok) return false;
		if (pattern != 0 && pattern != 1 && pattern != 2)
			return context.Fail(
				Status::UnsupportedExecution, "instance pattern has no source case", "pattern"
			);
		if (select != 0 && select != 1 && select != 2)
			return context.Fail(
				Status::UnsupportedExecution,
				"instance palette selection has no source case",
				"colors_per_index_select"
			);
		if (!std::isfinite(radius) || !std::isfinite(seedValue) || seedValue < INT32_MIN ||
			seedValue > UINT32_MAX)
			return context.Fail(Status::InvalidValue, "instance controls and seed must be bounded", "seed");
		std::array<double, 6> posRange{}, rotRange{}, scaleRange{};
		if (!Scatter(context, "position_scatter", posRange) ||
			!Scatter(context, "rotation_scatter", rotRange) || !Scatter(context, "scale_scatter", scaleRange))
			return false;
		for (double &value : rotRange)
			value *= std::numbers::pi / 180;
		const Value *colors = context.Find("colors_per_index");
		const auto *palette = colors ? std::get_if<ArrayValue>(colors) : nullptr;
		if (colors && (!palette || !palette->Nested.empty() || !palette->Items.empty()))
			return context.Fail(
				Status::TypeMismatch, "instance palette requires a flat color array", "colors_per_index"
			);
		if (palette)
			for (const auto &color : palette->Elements)
				if (!std::holds_alternative<Colour>(color))
					return context.Fail(
						Status::TypeMismatch, "instance palette requires colors", "colors_per_index"
					);
		const Value *randomColors = context.Find("random_colors");
		const auto *gradient = randomColors ? std::get_if<Gradient>(randomColors) : nullptr;
		if (randomColors && (!gradient || !ValidRuntimeValue(*randomColors)))
			return context.Fail(
				Status::TypeMismatch, "instance random colors require a valid gradient", "random_colors"
			);
		FlatCost cost;
		std::array<const MeshTransform3D *, 128> chain{};
		if (!std::visit([&](const auto &item) { return Flatten(item, chain, 0, cost, nullptr); }, *source))
			return context.Fail(Status::LimitExceeded, "instance geometry flatten exceeds caps", "mesh");
		if (!context.ReserveOutput(
				MeshAddBytes(cost.Bytes, uint64_t(amount) * sizeof(MeshInstance3D)) + 64, "mesh"
			))
			return false;
		const Value *path = context.Find("shift_path");
		const auto *spatial = path ? std::get_if<PathValue3D>(path) : nullptr;
		const auto *planar = path ? std::get_if<Path2D>(path) : nullptr;
		std::optional<PathRuntime3D> space;
		std::optional<PathRuntime> flat;
		if (pattern == 0 && spatial && spatial->Data && spatial->Data->SourcePresent) {
			space.emplace(*spatial->Data, &context);
			if (!space->Valid())
				return context.Fail(Status::InvalidValue, "instance path length is invalid", "shift_path");
		} else if (pattern == 0 && planar && (!planar->Anchors.empty() || context.IsLinked("shift_path"))) {
			flat.emplace();
			if (!flat->Init(context, *planar)) return false;
		}
		auto point = [&](double ratio) {
			if (space) return space->Ratio(ratio).Position;
			auto p = flat->PointRatio(ratio);
			return Vector3{p.X, p.Y, 0};
		};
		MeshValue3D result;
		auto &data = result.Data.emplace();
		data.Instanced = true;
		data.InstanceObjectTransform = mesh ? mesh->Data->LocalTransforms.front() : scene->Data->Transform;
		data.LocalTransforms.emplace_back();
		data.Parts.reserve(cost.Parts);
		data.Materials.reserve(cost.Parts);
		data.Instances.reserve(size_t(amount));
		FlatCost emitting;
		if (!std::visit([&](const auto &item) { return Flatten(item, chain, 0, emitting, &data); }, *source))
			return context.Fail(Status::LimitExceeded, "instance geometry flatten exceeds caps", "mesh");
		const Vector3 startEuler = SourceQuaternionToEuler(rotation),
					  shiftEuler = SourceQuaternionToEuler(rotationShift);
		const double ratioStep = amount >= 1 ? 1.0 / (amount - 1) : 0;
		for (int64_t i = 0; i < amount; ++i) {
			SourceRandom random(uint32_t(int64_t(seedValue)) + uint32_t(i) * 78);
			Vector3 p{}, r{}, s{1, 1, 1}, normal{};
			if (!Row(context, "positions", size_t(i), p) || !Row(context, "rotations", size_t(i), r) ||
				!Row(context, "scales", size_t(i), s) || !Row(context, "normal", size_t(i), normal))
				return false;
			p = {
				start.X + random.Range(posRange[0], posRange[1]) + p.X,
				start.Y + random.Range(posRange[2], posRange[3]) + p.Y,
				start.Z + random.Range(posRange[4], posRange[5]) + p.Z
			};
			r = {
				startEuler.X + shiftEuler.X * i + random.Range(rotRange[0], rotRange[1]) + r.X,
				startEuler.Y + shiftEuler.Y * i + random.Range(rotRange[2], rotRange[3]) + r.Y,
				startEuler.Z + shiftEuler.Z * i + random.Range(rotRange[4], rotRange[5]) + r.Z
			};
			s = {
				(scale.X - 1) + scaleShift.X * i + random.Range(scaleRange[0], scaleRange[1]) + s.X,
				(scale.Y - 1) + scaleShift.Y * i + random.Range(scaleRange[2], scaleRange[3]) + s.Y,
				(scale.Z - 1) + scaleShift.Z * i + random.Range(scaleRange[4], scaleRange[5]) + s.Z
			};
			if (uniform) s.Y = s.Z = s.X;
			auto add = [](Vector3 &a, Vector3 b) { a = {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; };
			if (pattern == 0) {
				add(p, {shift.X * i, shift.Y * i, shift.Z * i});
				if (space || flat) {
					const double ratio = pathRange.X + (pathRange.Y - pathRange.X) * (i * ratioStep);
					add(p, point(ratio));
					if (follow) {
						const auto a = point(std::clamp(ratio - ratioStep / 2, 0.0, .999)),
								   b = point(std::clamp(ratio + ratioStep / 2, 0.0, .999));
						add(normal, {b.X - a.X, b.Y - a.Y, b.Z - a.Z});
					}
				}
			} else if (pattern == 1) {
				const double xy = grid.X * grid.Y, z = std::floor(i / xy),
							 y = std::floor((i - z * xy) / grid.X), x = std::fmod(i - z * xy, grid.X);
				add(p,
					{shift.X * x + shiftY.X * y + shiftZ.X * z,
					 shift.Y * x + shiftY.Y * y + shiftZ.Y * z,
					 shift.Z * x + shiftY.Z * y + shiftZ.Z * z});
			} else {
				const double angle = (360.0 / amount * i) * std::numbers::pi / 180;
				add(p,
					{shift.X * i + radius * std::cos(angle),
					 shift.Y * i - radius * std::sin(angle),
					 shift.Z * i});
				if (look) add(normal, {p.X - start.X, p.Y - start.Y, p.Z - start.Z});
			}
			Colour color{255, 255, 255, 255};
			const size_t length = palette ? palette->Elements.size() : 0;
			size_t colorIndex = size_t(i);
			if (select == 0 && length)
				colorIndex %= length;
			else if (select == 1 && length) {
				const size_t period = length * 2 - 1;
				const size_t v = colorIndex % period;
				colorIndex = v >= length ? period - v : v;
			} else if (select == 2) {
				if (length)
					colorIndex = random.Index(uint32_t(length));
				else {
					random.Unit();
					random.Unit();
				}
			}
			if (length && colorIndex < length) color = std::get<Colour>(palette->Elements[colorIndex]);
			const double randomRatio = random.Unit();
			const auto blend = gradient ? SourceCachedGradient(*gradient, randomRatio)
										: std::optional<Colour>(Colour{255, 255, 255, 255});
			if (!blend)
				return context.Fail(
					Status::InvalidValue, "instance gradient cached color is nonfinite", "random_colors"
				);
			color.Red = uint8_t(unsigned(color.Red) * blend->Red / 255);
			color.Green = uint8_t(unsigned(color.Green) * blend->Green / 255);
			color.Blue = uint8_t(unsigned(color.Blue) * blend->Blue / 255);
			const std::array<double, 16> fields = {
				p.X,
				p.Y,
				p.Z,
				color.Red / 255.f,
				r.X,
				r.Y,
				r.Z,
				color.Green / 255.f,
				s.X,
				s.Y,
				s.Z,
				color.Blue / 255.f,
				normal.X,
				normal.Y,
				normal.Z,
				0
			};
			MeshInstance3D record;
			for (size_t component = 0; component < fields.size(); ++component) {
				if (!FitsFloat(fields[component]))
					return context.Fail(
						Status::InvalidValue, "instance stream exceeds finite f32 range", "mesh"
					);
				record.Fields[component] = float(fields[component]);
			}
			data.Instances.push_back(record);
		}
		if (!ValidMeshPayload(result))
			return context.Fail(Status::InvalidValue, "instance output is invalid", "mesh");
		context.SetValue("mesh", std::move(result));
		return context.FailureCode == Status::Ok;
	}
}
