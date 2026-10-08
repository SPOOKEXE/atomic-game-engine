#include "../SourceMappedInputs.hpp"
#include "Families.hpp"
#include "Source2DGenerator.hpp"

#include <engine/imagegraph/GroupRenderSession.hpp>

// Source algorithms: Copyright (c) 2023 Tanasart, MIT License.
// See docs/pixel-composer-m0/PixelComposer-LICENSE.txt for the source license.
// Native GLSL-sized CPU profile: nearest maps, float stages, normal blend and white draw colour.
// The source HLSL 1024-point variant and matched GPU parity are separate profiles.
namespace engine::imagegraph::detail {
	namespace {
		constexpr size_t POINT_LIMIT = 256;
		constexpr uint64_t WORK_LIMIT = 64000000;
		struct PointInputs {
			uint32_t Width = 0, Height = 0;
			float DimensionX = 0, DimensionY = 0, Low = 16, High = 16;
			bool Inverted = false;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			std::array<std::array<float, 2>, POINT_LIMIT> Points{};
			size_t Count = 0;
			const Image *Map = nullptr, *Held = nullptr;
		};
		bool Float(NodeContext &c, double value, float &out, std::string_view port) {
			if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
				return c.Fail(Status::InvalidValue, "Point SDF value exceeds finite shader range", port);
			out = float(value);
			return true;
		}
		bool Number(const ElementValue &v, double &out) {
			if (auto p = std::get_if<double>(&v))
				out = *p;
			else if (auto p = std::get_if<int64_t>(&v))
				out = double(*p);
			else if (auto p = std::get_if<bool>(&v))
				out = *p ? 1 : 0;
			else if (auto p = std::get_if<EnumValue>(&v))
				out = double(p->Value);
			else
				return false;
			return true;
		}
		bool Point(NodeContext &c, PointInputs &in, const ElementValue &v) {
			double x = 0, y = 0;
			if (auto p = std::get_if<Vector2>(&v)) {
				x = p->X;
				y = p->Y;
			} else if (auto p = std::get_if<Vector3>(&v)) {
				x = p->X;
				y = p->Y;
			} else if (auto p = std::get_if<Vector4>(&v)) {
				x = p->X;
				y = p->Y;
			} else if (auto p = std::get_if<Quaternion>(&v)) {
				x = p->X;
				y = p->Y;
			} else
				return c.Fail(Status::UnsupportedExecution, "Point SDF requires coordinate pairs", "points");
			return Float(c, x, in.Points[in.Count][0], "points") &&
				   Float(c, y, in.Points[in.Count++][1], "points");
		}
		bool Row(NodeContext &c, PointInputs &in, const std::vector<ElementValue> &row) {
			double x = 0, y = 0;
			if ((!row.empty() && !Number(row[0], x)) || (row.size() > 1 && !Number(row[1], y)))
				return c.Fail(
					Status::UnsupportedExecution, "Point SDF coordinate row is not numeric", "points"
				);
			return Float(c, x, in.Points[in.Count][0], "points") &&
				   Float(c, y, in.Points[in.Count++][1], "points");
		}
		bool ReadPoints(NodeContext &c, PointInputs &in) {
			auto v = c.Find("points");
			auto a = v ? std::get_if<ArrayValue>(v) : nullptr;
			if (!a) return c.Fail(Status::UnsupportedExecution, "Point SDF requires a point array", "points");
			if (unsigned(!a->Elements.empty()) + unsigned(!a->Nested.empty()) + unsigned(!a->Items.empty()) >
				1)
				return c.Fail(Status::InvalidValue, "Point SDF array representations conflict", "points");
			if (std::max({a->Elements.size(), a->Nested.size(), a->Items.size()}) > POINT_LIMIT)
				return c.Fail(
					Status::LimitExceeded, "Point SDF GLSL profile supports at most 256 points", "points"
				);
			for (auto &p : a->Elements)
				if (!Point(c, in, p)) return false;
			for (auto &p : a->Nested)
				if (!Row(c, in, p)) return false;
			for (auto &item : a->Items) {
				if (auto p = std::get_if<ElementValue>(&item.Data)) {
					if (!Point(c, in, *p)) return false;
				} else if (auto row = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
					double xy[2] = {0, 0};
					for (size_t i = 0; i < std::min(size_t(2), row->size()); ++i) {
						auto leaf = std::get_if<ElementValue>(&(*row)[i].Data);
						if (!leaf || !Number(*leaf, xy[i]))
							return c.Fail(
								Status::UnsupportedExecution,
								"Point SDF coordinate row is not numeric",
								"points"
							);
					}
					if (!Float(c, xy[0], in.Points[in.Count][0], "points") ||
						!Float(c, xy[1], in.Points[in.Count][1], "points"))
						return false;
					++in.Count;
				} else
					return c.Fail(
						Status::UnsupportedExecution,
						"Point SDF point surface is not a coordinate pair",
						"points"
					);
			}
			return true;
		}
		bool HeldItems(
			NodeContext &c,
			const std::vector<SourceArrayItem> &items,
			size_t depth,
			size_t &count,
			const Image *&held
		) {
			if (depth > Limits::MaximumArrayDepth)
				return c.Fail(Status::LimitExceeded, "Point SDF retained array is too deep", "surface_out");
			for (auto &item : items) {
				const Image *image = std::get_if<Image>(&item.Data);
				if (auto leaf = std::get_if<ElementValue>(&item.Data)) {
					auto surface = std::get_if<SurfaceValue>(leaf);
					if (!surface)
						return c.Fail(
							Status::UnsupportedExecution,
							"Point SDF retained array requires surfaces",
							"surface_out"
						);
					image = &surface->Data;
				}
				if (auto children = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
					if (!HeldItems(c, *children, depth + 1, count, held)) return false;
				} else {
					if (!image ||
						!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
						return c.Fail(
							Status::InvalidValue,
							"Point SDF retained surface layout is invalid",
							"surface_out"
						);
					if (image->Width > c.Request.MaximumImageDimension ||
						image->Height > c.Request.MaximumImageDimension)
						return c.Fail(
							Status::LimitExceeded,
							"Point SDF retained surface exceeds request dimensions",
							"surface_out"
						);
					if (count == c.ProcessorRow) held = image;
					if (++count > Limits::MaximumArrayElements)
						return c.Fail(
							Status::LimitExceeded, "Point SDF retained array is too large", "surface_out"
						);
				}
			}
			return true;
		}
		bool FindHeld(NodeContext &c, PointInputs &in) {
			const auto *state = c.Request.GroupRender ? &c.Request.GroupRender->Outputs
													  : c.Request.SourceTunnelPreviousOutputs;
			if (state)
				for (auto &node : state->Nodes)
					if (node.NodeId == c.Authored.Id && node.NodeType == c.Authored.Type)
						for (auto &output : node.Outputs)
							if (output.Port == "surface_out" && output.Data && !output.Refusal) {
								if (auto surface = std::get_if<SurfaceValue>(&*output.Data);
									surface && c.ProcessorCount == 1)
									in.Held = &surface->Data;
								else if (auto array = std::get_if<ArrayValue>(&*output.Data);
										 array && output.ImageArrayPayload) {
									size_t count = 0;
									if (!array->Elements.empty() || !array->Nested.empty())
										return c.Fail(
											Status::UnsupportedExecution,
											"Point SDF retained image array requires source items",
											"surface_out"
										);
									if (!HeldItems(c, array->Items, 0, count, in.Held)) return false;
									if (count != c.ProcessorCount) in.Held = nullptr;
								}
							}
			if (!in.Held)
				return c.Fail(
					Status::UnsupportedExecution,
					"Point SDF empty points require a captured prior output surface",
					"points"
				);
			if (!ValidSurfaceLayout(*in.Held, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return c.Fail(
					Status::InvalidValue, "Point SDF retained surface layout is invalid", "surface_out"
				);
			if (in.Held->Width != in.Width || in.Held->Height != in.Height || in.Held->Format != in.Format)
				return c.Fail(
					Status::UnsupportedExecution,
					"Point SDF empty resized surface needs a source observation",
					"surface_out"
				);
			return true;
		}
		bool Prepare(NodeContext &c, PointInputs &in) {
			if (c.Request.MaximumImageDimension == 0 ||
				c.Request.MaximumImageDimension > Limits::MaximumDimension)
				return c.Fail(
					Status::InvalidValue, "Point SDF request dimension limit is invalid", "dimension"
				);
			if (auto v = c.Find("dimension"); v && std::holds_alternative<AtlasValue>(*v))
				return c.Fail(
					Status::UnsupportedExecution, "Point SDF requires physical dimension binding", "dimension"
				);
			auto dimension = c.Vec2("dimension", {1, 1});
			auto unit = c.Integer("dimension_unit", 1);
			if (unit < 0 || unit > 2)
				return c.Fail(Status::InvalidValue, "Point SDF dimension unit is invalid", "dimension_unit");
			if (c.IsLinked("dimension")) {
				if (auto image = c.Input("dimension"))
					dimension = {double(image->Width), double(image->Height)};
			} else if (unit == 1) {
				dimension.X *= c.Project.SurfaceWidth;
				dimension.Y *= c.Project.SurfaceHeight;
			} else if (unit == 2)
				return c.Fail(
					Status::UnsupportedExecution,
					"Point SDF dimension has no source mask binding",
					"dimension_unit"
				);
			if (!Float(c, dimension.X, in.DimensionX, "dimension") ||
				!Float(c, dimension.Y, in.DimensionY, "dimension"))
				return false;
			double width = std::max(1., source2d::GeneratorRoundHalfEven(dimension.X)),
				   height = std::max(1., source2d::GeneratorRoundHalfEven(dimension.Y));
			if (width > c.Request.MaximumImageDimension || height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Point SDF dimensions exceed request limits", "dimension"
				);
			in.Width = uint32_t(width);
			in.Height = uint32_t(height);
			auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			if (!ReadPoints(c, in)) return false;
			if (!in.Count) return FindHeld(c, in);
			Vector2 range;
			if (!ReadSourceMappedRange(c, "max_distance", range) ||
				!Float(c, range.X, in.Low, "max_distance") || !Float(c, range.Y, in.High, "max_distance"))
				return false;
			auto flag = c.Find("inverted");
			if (flag) {
				if (auto p = std::get_if<bool>(flag))
					in.Inverted = *p;
				else if (auto p = SourceChoiceNumber(*flag))
					in.Inverted = *p > .5;
				else
					return c.Fail(
						Status::UnsupportedExecution,
						"Point SDF requires a finite boolean control",
						"inverted"
					);
			}
			if (SourceRangeMapped(c, "max_distance")) {
				if (auto v = c.Find("max_distance_map"); v && std::holds_alternative<AtlasValue>(*v))
					return c.Fail(
						Status::UnsupportedExecution,
						"Point SDF map requires a physical surface",
						"max_distance_map"
					);
				in.Map = c.Input("max_distance_map");
				if (in.Map &&
					!ValidSurfaceLayout(*in.Map, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
					return c.Fail(
						Status::InvalidValue, "Point SDF map layout is invalid", "max_distance_map"
					);
			}
			return true;
		}
		bool Pixel(NodeContext &c, const PointInputs &in, uint32_t x, uint32_t y, Rgba &out) {
			float u = (float(x) + .5f) / float(in.Width), v = (float(y) + .5f) / float(in.Height),
				  dst = in.Low;
			if (in.Map) {
				auto map = SampleNearest(*in.Map, u, v);
				float mean = (float(map[0]) + float(map[1]) + float(map[2])) / 3.f;
				dst = in.Low * (1.f - mean) + in.High * mean;
				if (!std::isfinite(mean) || !std::isfinite(dst))
					return c.Fail(
						Status::InvalidValue, "Point SDF mapped distance is nonfinite", "max_distance_map"
					);
			}
			if (dst == 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Point SDF zero distance has no finite native shader profile",
					"max_distance"
				);
			float px = u * in.DimensionX, py = v * in.DimensionY, nearest = 9999.f;
			for (size_t i = 0; i < in.Count; ++i) {
				float dx = px - in.Points[i][0], dy = py - in.Points[i][1], square = dx * dx + dy * dy;
				if (!std::isfinite(square))
					return c.Fail(
						Status::InvalidValue, "Point SDF distance exceeds finite shader range", "points"
					);
				nearest = std::min(nearest, std::sqrt(square));
			}
			float shade = nearest / dst;
			if (in.Inverted) shade = 1.f - shade;
			if (!std::isfinite(shade))
				return c.Fail(Status::InvalidValue, "Point SDF output is nonfinite", "max_distance");
			const auto storage = DescribeSurfaceFormat(in.Format);
			if (storage && storage->FloatingPoint && storage->BitsPerChannel == 16 &&
				std::abs(shade) > 65504.f)
				return c.Fail(
					Status::InvalidValue, "Point SDF output exceeds half storage range", "max_distance"
				);
			out = {shade, shade, shade, 1};
			return true;
		}
		bool Quote(NodeContext &c, const PointInputs &in, uint64_t &work, uint64_t &bytes) {
			const uint64_t pixels = uint64_t(in.Width) * in.Height;
			// Charges all admission, repeated draw validation and write passes, including every point
			// distance.
			const uint64_t cost = in.Held ? 16 : 64 + 48 * in.Count + (in.Map ? 64 : 0);
			if (work > WORK_LIMIT || pixels > (WORK_LIMIT - work) / cost)
				return c.Fail(Status::LimitExceeded, "Point SDF batch exceeds native work budget", "points");
			auto layout =
				CheckedSurfaceLayout(in.Width, in.Height, in.Format, Limits::MaximumEvaluationBytes);
			if (!layout || bytes > c.AvailableBytes() || layout->Bytes > c.AvailableBytes() - bytes)
				return c.Fail(
					Status::LimitExceeded, "Point SDF batch exceeds image byte budget", "surface_out"
				);
			if (!in.Held)
				for (uint32_t y = 0; y < in.Height; ++y)
					for (uint32_t x = 0; x < in.Width; ++x) {
						Rgba pixel;
						if (!Pixel(c, in, x, y, pixel)) return false;
					}
			work += pixels * cost;
			bytes += layout->Bytes;
			return true;
		}
	}
	bool AdmitSourcePointSdf(NodeContext &c, uint64_t &work, uint64_t &bytes) {
		PointInputs in;
		return Prepare(c, in) && Quote(c, in, work, bytes);
	}
	bool SourcePointSdf(NodeContext &c) {
		ENGINE_PROFILE("imagegraph Point SDF");
		PointInputs in;
		uint64_t work = 0, bytes = 0;
		if (!Prepare(c, in) || !Quote(c, in, work, bytes)) return false;
		auto out = c.NewImage("surface_out", in.Width, in.Height, in.Format);
		if (!out) return false;
		if (in.Held) {
			out->Pixels = in.Held->Pixels;
			out->Hash = in.Held->Hash;
			return true;
		}
		for (uint32_t y = 0; y < in.Height; ++y)
			for (uint32_t x = 0; x < in.Width; ++x) {
				Rgba pixel;
				if (!Pixel(c, in, x, y, pixel) || !WritePixel(*out, x, y, pixel))
					return c.Fail(Status::InvalidValue, "Point SDF pixel cannot be stored", "surface_out");
			}
		return true;
	}
}
