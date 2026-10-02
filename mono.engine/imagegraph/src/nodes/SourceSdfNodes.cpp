#include "../SdfPayload.hpp"
#include "Families.hpp"
#include "Source2DGenerator.hpp"

#include <array>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		template <class T> T Read(NodeContext &c, std::string_view port, T fallback) {
			if (const Value *v = c.Find(port)) {
				if (const T *t = std::get_if<T>(v)) return *t;
				c.Fail(Status::TypeMismatch, "SDF control has an incompatible type", port);
			}
			return fallback;
		}
		bool PrimitiveNode(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.sdf.primitive");
			static constexpr std::array<int32_t, 23> shapes{100, 101, 102, 103, -1,	 200, 201, 202,
															203, 204, 205, -1,	300, 306, 301, 302,
															303, 304, 305, 307, -1,	 400, 401};
			const auto selector =
				c.Find("shape") ? SourceChoiceNumber(*c.Find("shape")) : std::optional<double>{1};
			const double selected = selector.value_or(-1);
			if (!std::isfinite(selected) || selected < 0 || selected >= shapes.size() ||
				selected != std::floor(selected) || shapes[size_t(selected)] < 0)
				return c.Fail(
					Status::UnsupportedExecution, "SDF shape selector has no source primitive", "shape"
				);
			const Image *texture = c.Input("texture");
			const uint64_t charge = sizeof(SdfData) + sizeof(SourceSdfShape) + sizeof(SourceSdfOperation) +
									c.Authored.Id.size() + 32 + (texture ? texture->Pixels.size() : 0);
			if (!c.ReserveOutput(charge, "sdf_object")) return false;
			SdfValue output;
			auto &data = output.Data.emplace();
			SourceSdfShape s;
			s.Identity = c.Authored.Id + ":" + std::to_string(c.ProcessorRow);
			s.Shape = shapes[size_t(selected)];
			s.Size = Read(c, "size", s.Size);
			s.Radius = c.Scalar("radius", .7);
			s.Thickness = c.Scalar("thickness", .2);
			s.Crop = c.Scalar("crop");
			if (s.Shape == 203) s.Crop = s.Crop / std::numbers::pi * 2.15;
			s.Angle = c.Scalar("angle", 30) * std::numbers::pi / 180;
			s.Height = c.Scalar("height", .5);
			s.RadiusRange = Read(c, "radius_range", s.RadiusRange);
			s.UniformSize = c.Scalar("uniform_size", 1);
			s.Elongate = Read(c, "elongate", s.Elongate);
			s.Rounded = c.Scalar("rounded");
			s.Corner = Read(c, "corner", s.Corner);
			s.Size2D = Read(c, "2_d_size", s.Size2D);
			s.Sides = int32_t(c.Integer("side", 3));
			s.WaveAmplitude = Read(c, "wave_amplitude", s.WaveAmplitude);
			s.WaveIntensity = Read(c, "wave_intensity", s.WaveIntensity);
			s.WavePhase = Read(c, "wave_phase", s.WavePhase);
			s.TwistAxis = int32_t(c.SourceChoice("twist_axis"));
			s.TwistAmount = c.Scalar("twist_amount");
			s.Position = Read(c, "position", s.Position);
			s.Rotation = Read(c, "rotation", s.Rotation);
			s.Scale = c.Scalar("scale", 1);
			s.Tile = c.Boolean("tile");
			s.TileDistance = Read(c, "tile_distance", s.TileDistance);
			s.TileAmount = Read(c, "tile_amount", s.TileAmount);
			s.Diffuse = Read(c, "base_color", s.Diffuse);
			s.Specular = c.Scalar("specular");
			s.Reflective = c.Scalar("reflective");
			s.Volumetric = c.Boolean("volumetric");
			s.Density = c.Scalar("density", .3);
			if (texture) s.Texture = *texture;
			s.TextureInterpolation = c.Boolean("texture_interpolation");
			s.TextureScale = c.Scalar("texture_scale", 1);
			s.Triplanar = c.Scalar("triplanar_smoothing", 1);
			data.Shapes.push_back(std::move(s));
			data.Operations.push_back({0, 0});
			if (c.FailureCode != Status::Ok) return false;
			if (!ValidSdfPayload(output))
				return c.Fail(
					Status::InvalidValue,
					"SDF primitive parameters exceed its finite bounded domain",
					"sdf_object"
				);
			// Surface rendering belongs to the host GPU adapter; this executor publishes the owned scene
			// resource.
			c.SetValue("sdf_object", std::move(output));
			return c.FailureCode == Status::Ok;
		}
		bool CombineNode(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.sdf.combine");
			const auto *left = c.Find("shape_1"), *right = c.Find("shape_2");
			const auto *a = left ? std::get_if<SdfValue>(left) : nullptr,
					   *b = right ? std::get_if<SdfValue>(right) : nullptr;
			SdfValue output;
			if (!a || !b || !a->Data || !b->Data || a->Data->Shapes.empty() || b->Data->Shapes.empty()) {
				c.SetValue("sdf_object", std::move(output));
				return c.FailureCode == Status::Ok;
			}
			if (!ValidSdfPayload(*a) || !ValidSdfPayload(*b))
				return c.Fail(Status::InvalidValue, "SDF combine requires valid bounded scene operands");
			const double type = c.SourceChoice("type"), merge = c.Scalar("merge", .1);
			if (type < 0 || type > 3 || type != std::floor(type) || !std::isfinite(type) ||
				!std::isfinite(merge))
				return c.Fail(
					Status::InvalidValue, "SDF operation and merge must be finite source controls", "type"
				);
			size_t count = a->Data->Shapes.size();
			uint64_t bytes = SdfStorageBytes(*a, true) + sizeof(SourceSdfOperation);
			for (const auto &shape : b->Data->Shapes) {
				const auto match =
					std::find_if(a->Data->Shapes.begin(), a->Data->Shapes.end(), [&](const auto &existing) {
						return existing.Identity == shape.Identity;
					});
				if (match == a->Data->Shapes.end()) {
					++count;
					bytes += sizeof(SourceSdfShape) + shape.Identity.capacity() +
							 (shape.Texture ? shape.Texture->Pixels.capacity() : 0);
				} else if (*match != shape)
					return c.Fail(
						Status::InvalidValue,
						"SDF shared primitive identity has conflicting captured parameters",
						"sdf_object"
					);
			}
			const size_t operations = a->Data->Operations.size() + b->Data->Operations.size() + 1;
			if (count > SOURCE_SDF_MAXIMUM_SHAPES || operations > SOURCE_SDF_MAXIMUM_OPERATIONS)
				return c.Fail(
					Status::LimitExceeded,
					"SDF composition exceeds source 16-shape or 32-operation limits",
					"sdf_object"
				);
			bytes += b->Data->Operations.size() * sizeof(SourceSdfOperation);
			if (!c.ReserveOutput(bytes, "sdf_object")) return false;
			auto &data = output.Data.emplace();
			data.Shapes.reserve(count);
			data.Operations.reserve(operations);
			data.Shapes = a->Data->Shapes;
			data.Operations = a->Data->Operations;
			std::array<int32_t, SOURCE_SDF_MAXIMUM_SHAPES> remap{};
			for (size_t index = 0; index < b->Data->Shapes.size(); ++index) {
				const auto &shape = b->Data->Shapes[index];
				const auto found =
					std::find_if(data.Shapes.begin(), data.Shapes.end(), [&](const auto &existing) {
						return existing.Identity == shape.Identity;
					});
				remap[index] = int32_t(found - data.Shapes.begin());
				if (found == data.Shapes.end()) data.Shapes.push_back(shape);
			}
			for (auto op : b->Data->Operations) {
				if (op.Code < 100) op.Code = remap[size_t(op.Code)];
				data.Operations.push_back(op);
			}
			data.Operations.push_back({100 + int32_t(type), merge});
			c.SetValue("sdf_object", std::move(output));
			return c.FailureCode == Status::Ok;
		}
		bool CrossSection(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.sdf.cross_section");
			const Value *value = c.Find("sdf_object");
			const auto *sdf = value ? std::get_if<SdfValue>(value) : nullptr;
			if (!sdf || !sdf->Data || sdf->Data->Shapes.empty())
				return c.Fail(
					Status::InvalidValue, "cross section requires a populated SDF resource", "sdf_object"
				);
			if (!ValidSdfPayload(*sdf))
				return c.Fail(Status::InvalidValue, "cross section SDF resource is invalid", "sdf_object");
			const int64_t axis = c.Integer("axis");
			const Vector2 middle = c.Vec2("midpoint"), span = c.Vec2("span", {1, 1}),
						  depth = c.Vec2("range", {0, 1});
			const double offset = c.Scalar("offset");
			if (axis < 0 || axis > 2 || !std::isfinite(offset) || !std::isfinite(depth.X) ||
				!std::isfinite(depth.Y) || depth.X == depth.Y)
				return c.Fail(Status::InvalidValue, "cross section controls have an undefined source domain");
			return source2d::RunGenerator(c, [&](uint32_t, uint32_t, uint32_t, uint32_t, double u, double v) {
				const float x = float(span.X) * (float(u) - .5f) * 2.f,
							y = float(span.Y) * (float(v) - .5f) * 2.f;
				Vector3 point;
				if (axis == 0)
					point = {float(offset), float(middle.X) + x, float(middle.Y) + y};
				else if (axis == 1)
					point = {float(middle.X) + x, float(offset), float(middle.Y) + y};
				else
					point = {float(middle.X) + x, float(middle.Y) + y, float(offset)};
				Diagnostic diagnostic;
				double distance = 0;
				if (SampleSourceSdf(*sdf, point, distance, diagnostic) != Status::Ok) {
					c.Fail(diagnostic.Code, diagnostic.Message, "sdf_object");
					return Rgba{};
				}
				const float shade =
					1.f - (float(distance) - float(depth.X)) / (float(depth.Y) - float(depth.X));
				return Rgba{shade, shade, shade, 1};
			});
		}

	}
	std::span<const ExecutorEntry> SourceSdfExecutors() {
		static constexpr std::array entries{
			ExecutorEntry{"pc.rm_primitive", PrimitiveNode, true},
			ExecutorEntry{"pc.rm_combine", CombineNode, true},
			ExecutorEntry{"pc.rm_render_cross", CrossSection, true}
		};
		return entries;
	}
}

namespace engine::imagegraph {
	Status BuildSourceSdfObject(
		const Node &node,
		const EvaluationSnapshot &snapshot,
		SdfValue &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		if (node.Type != "pc.rm_primitive" && node.Type != "pc.rm_combine") {
			diagnostic = {
				Status::UnsupportedExecution,
				node.Id,
				{},
				"SDF snapshot builder requires a source primitive or combine node"
			};
			return diagnostic.Code;
		}
		if (maximumBytes == 0 || maximumBytes > Limits::MaximumEvaluationBytes ||
			snapshot.RetainedBytes() > maximumBytes) {
			diagnostic = {
				Status::LimitExceeded,
				node.Id,
				{},
				"SDF snapshot and publication exceed the requested byte budget"
			};
			return diagnostic.Code;
		}
		const auto *entry = FindCatalogueEntry(node.Type);
		if (!entry) {
			diagnostic = {Status::InvalidValue, node.Id, {}, "SDF source catalogue entry is absent"};
			return diagnostic.Code;
		}
		EvaluationRequest request;
		detail::EvaluationBudget budget(maximumBytes);
		auto inputCharge = budget.Reserve(snapshot.RetainedBytes());
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = maximumBytes;
		const uint64_t views =
			snapshot.Values().size() *
				(sizeof(std::pair<std::string_view, const Value *>) + sizeof(std::string_view)) +
			snapshot.Images().size() * sizeof(std::pair<std::string_view, const Image *>);
		auto viewCharge = context.ReserveWorkspace(views);
		if (!viewCharge) {
			diagnostic = {context.FailureCode, node.Id, context.FailurePort, context.FailureMessage};
			return diagnostic.Code;
		}
		context.ValueViews.reserve(snapshot.Values().size());
		context.LinkedValues.reserve(snapshot.Values().size());
		context.Images.reserve(snapshot.Images().size());
		for (const auto &value : snapshot.Values()) {
			if (std::holds_alternative<ArrayValue>(value.Data)) {
				diagnostic = {
					Status::UnsupportedExecution,
					node.Id,
					value.Port,
					"SDF snapshot builder requires a selected processor row"
				};
				return diagnostic.Code;
			}
			context.ValueViews.emplace_back(value.Port, &value.Data);
			if (value.Linked) context.LinkedValues.push_back(value.Port);
		}
		if (!snapshot.ImageArrays().empty()) {
			diagnostic = {
				Status::UnsupportedExecution,
				node.Id,
				{},
				"SDF snapshot builder requires selected image-array inputs"
			};
			return diagnostic.Code;
		}
		for (const auto &image : snapshot.Images())
			context.Images.emplace_back(image.Port, &image.Data);
		const bool ok =
			node.Type == "pc.rm_primitive" ? detail::PrimitiveNode(context) : detail::CombineNode(context);
		if (!ok || context.FailureCode != Status::Ok) {
			diagnostic = {context.FailureCode, node.Id, context.FailurePort, context.FailureMessage};
			return diagnostic.Code;
		}
		for (auto &output : context.OutputValues)
			if (output.Port == "sdf_object") {
				if (auto *sdf = std::get_if<SdfValue>(&output.Data)) {
					result = std::move(*sdf);
					diagnostic = {};
					return Status::Ok;
				}
			}
		diagnostic = {
			Status::InvalidOutput,
			node.Id,
			"sdf_object",
			"SDF snapshot builder did not publish its typed resource"
		};
		return diagnostic.Code;
	}
}
