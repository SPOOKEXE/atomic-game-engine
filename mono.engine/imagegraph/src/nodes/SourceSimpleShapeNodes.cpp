#include "Families.hpp"
#include "Processor.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		bool SimpleShapeReference(NodeContext &context, uint32_t &width, uint32_t &height) {
			// Whole arrays remain borrowed in the original views or values after row selections are appended.
			const Value *original = nullptr;
			for (auto view = context.ProcessorOriginalValues.rbegin();
				 view != context.ProcessorOriginalValues.rend();
				 ++view)
				if (view->first == "dimension") {
					original = view->second;
					break;
				}
			if (!original)
				for (const auto &[port, value] : context.Values)
					if (port == "dimension") {
						original = &value;
						break;
					}
			if (!original)
				for (const auto &[port, value] : context.ValueViews)
					if (port == "dimension") {
						original = value;
						break;
					}
			Vector2 size{1, 1};
			const auto numeric = [](const auto &value, double &out) {
				if (const auto *v = std::get_if<double>(&value)) {
					out = *v;
					return true;
				}
				if (const auto *v = std::get_if<int64_t>(&value)) {
					out = double(*v);
					return true;
				}
				return false;
			};
			if (original) {
				if (const auto *v = std::get_if<Vector2>(original))
					size = *v;
				else if (const auto *v = std::get_if<double>(original))
					size = {*v, *v};
				else if (const auto *v = std::get_if<int64_t>(original))
					size = {double(*v), double(*v)};
				else if (const auto *array = std::get_if<ArrayValue>(original)) {
					if (!array->Items.empty()) {
						const auto &first = array->Items.front().Data;
						const auto *leaf = std::get_if<ElementValue>(&first);
						if (const auto *v = leaf ? std::get_if<Vector2>(leaf) : nullptr)
							size = *v;
						else {
							const auto *children = std::get_if<std::vector<SourceArrayItem>>(&first);
							const auto &row = children ? *children : array->Items;
							const auto read = [&](const SourceArrayItem &item, double &value) {
								const auto *number = std::get_if<ElementValue>(&item.Data);
								return number && numeric(*number, value);
							};
							if (row.size() < 2 || !read(row[0], size.X) || !read(row[1], size.Y))
								return context.Fail(
									Status::UnsupportedExecution,
									"first source dimension getter needs observed numeric components",
									"dimension"
								);
						}
					} else {
						const std::vector<ElementValue> &row =
							array->Nested.empty() ? array->Elements : array->Nested.front();
						if (!row.empty() && std::holds_alternative<Vector2>(row.front()))
							size = std::get<Vector2>(row.front());
						else if (row.size() < 2 || !numeric(row[0], size.X) || !numeric(row[1], size.Y))
							return context.Fail(
								Status::UnsupportedExecution,
								"first source dimension getter needs observed numeric components",
								"dimension"
							);
					}
				} else
					return context.Fail(
						Status::UnsupportedExecution,
						"first source dimension getter needs observed numeric components",
						"dimension"
					);
			}
			const int64_t unit = context.Integer("dimension_unit", 1);
			if (unit < 0 || unit > 1)
				return context.Fail(Status::InvalidValue, "dimension unit is invalid", "dimension");
			if (unit == 1 && !context.IsLinked("dimension")) {
				size.X *= context.Project.SurfaceWidth;
				size.Y *= context.Project.SurfaceHeight;
			}
			const auto rounded = [](double value) {
				const double floor = std::floor(value), fraction = value - floor;
				return std::max(1., floor + (fraction > .5 || (fraction == .5 && std::fmod(floor, 2.) != 0)));
			};
			if (!std::isfinite(size.X) || !std::isfinite(size.Y))
				return context.Fail(
					Status::InvalidValue, "source reference dimension is nonfinite", "dimension"
				);
			size = {rounded(size.X), rounded(size.Y)};
			if (size.X > Limits::MaximumDimension || size.Y > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "source reference dimension exceeds native limits", "dimension"
				);
			width = uint32_t(size.X);
			height = uint32_t(size.Y);
			return true;
		}
		bool SimpleShapeVector(
			NodeContext &context, std::string_view id, uint32_t width, uint32_t height, Vector2 &value
		) {
			if (const Image *image = context.Input(id)) {
				value = {double(image->Width), double(image->Height)};
				return true;
			}
			const Value *raw = context.Find(id);
			if (raw && (std::holds_alternative<Path2D>(*raw) || std::holds_alternative<PathValue3D>(*raw)))
				return context.Fail(
					Status::UnsupportedExecution,
					"source linked path vector requires its unobserved local animation ratio",
					id
				);
			const int64_t unit = context.Integer(std::string(id) + "_unit", 1);
			if (unit < 0 || unit > 1)
				return context.Fail(Status::InvalidValue, "shape vector unit is invalid", id);
			value = context.Vec2(id, {.5, .5});
			if (raw && !std::holds_alternative<Vector2>(*raw) && !std::holds_alternative<double>(*raw) &&
				!std::holds_alternative<int64_t>(*raw))
				return context.Fail(
					Status::UnsupportedExecution, "shape vector requires selected numeric components", id
				);
			// Numeric source Vec2 getters apply Reference units even when the producer is linked.
			if (unit == 1) {
				if (!SimpleShapeReference(context, width, height)) return false;
				value.X *= width;
				value.Y *= height;
			}
			if (!std::isfinite(float(value.X)) || !std::isfinite(float(value.Y)))
				return context.Fail(
					Status::UnsupportedExecution, "shape vector exceeds finite shader uniforms", id
				);
			return true;
		}
		bool SimpleShape(NodeContext &context) {
			uint32_t width = 0, height = 0;
			if (!ResolveDimension(context, "dimension", width, height)) return false;
			const uint64_t pixels = uint64_t(width) * height,
						   limit = 64'000'000 / std::max<size_t>(1, context.ProcessorCount);
			if (pixels > limit / 8)
				return context.Fail(
					Status::LimitExceeded, "shape samples exceed bounded aggregate work", "dimension"
				);
			const Image *background = context.Input("bg"), *mask = context.Input("mask");
			// The pinned shader's mask branch reads bgSurf, not its declared mask sampler.
			if (mask && !background)
				return context.Fail(
					Status::UnsupportedExecution,
					"source shape mask reads an unuploaded background sampler",
					"mask"
				);
			Vector2 center, scale;
			if (!SimpleShapeVector(context, "center", width, height, center) ||
				!SimpleShapeVector(context, "half_size", width, height, scale))
				return false;
			if (float(scale.X) == 0 || float(scale.Y) == 0)
				return context.Fail(
					Status::UnsupportedExecution, "source shape divides by zero half size", "half_size"
				);
			const bool half = context.Authored.Type == "pc.shape_half",
					   rectangle = context.Authored.Type == "pc.shape_rectangle";
			const float angle = float(context.Scalar("rotation")) * float(std::numbers::pi) / 180,
						corner = rectangle ? float(context.Scalar("corner")) : 0;
			if (!std::isfinite(angle) || !std::isfinite(corner))
				return context.Fail(
					Status::UnsupportedExecution,
					"shape controls exceed finite shader uniforms",
					!std::isfinite(angle) ? "rotation" : "corner"
				);
			const float c = std::cos(angle), s = std::sin(angle), px = float(center.X) / width,
						py = float(center.Y) / height, sx = float(scale.X) / width,
						sy = float(scale.Y) / height;
			if (sx == 0 || sy == 0)
				return context.Fail(
					Status::UnsupportedExecution,
					"source shape scale collapses in shader uniforms",
					"half_size"
				);
			const auto rgba = [](Colour col) {
				return Rgba{col.Red / 255., col.Green / 255., col.Blue / 255., col.Alpha / 255.};
			};
			const Rgba bg = rgba(context.Get<Colour>("bg_color", {})),
					   fill = rgba(context.Get<Colour>("color", {255, 255, 255, 255}));
			// surface_verify in the source base forces RGBA8. BLEND_ALPHA on its cleared target copies RGBA.
			Image *output = context.NewImage("surface_out", width, height, SurfaceFormat::RGBA8Unorm);
			if (!output) return false;
			for (uint32_t y = 0; y < height; y++)
				for (uint32_t x = 0; x < width; x++) {
					const float u = float((x + .5) / width), v = float((y + .5) / height);
					Rgba pixel = background ? SampleNearest(*background, u, v) : bg;
					bool gated = false;
					if (mask)
						gated =
							(float(pixel[0]) + float(pixel[1]) + float(pixel[2])) / 3 * float(pixel[3]) == 0;
					if (!gated) {
						float dx = u - px, dy = v - py;
						if (half) {
							dx /= sx;
							dy /= sy;
						}
						const float rx = dx * c - dy * s, ry = dx * s + dy * c;
						const float tx = half ? rx : rx / sx, ty = half ? ry : ry / sy;
						float distance = 0;
						if (half)
							distance = -ty;
						else if (rectangle) {
							const float a = std::abs(tx) - (float(width) / height - corner),
										b = std::abs(ty) - (1 - corner);
							const float mx = std::max(a, 0.f), my = std::max(b, 0.f);
							distance = std::sqrt(mx * mx + my * my) + std::min(std::max(a, b), 0.f) - corner;
						} else
							distance = std::sqrt(tx * tx + ty * ty) - 1;
						if (!std::isfinite(distance))
							return context.Fail(
								Status::UnsupportedExecution,
								"source shape produces nonfinite distance",
								"half_size"
							);
						if (distance <= 0) pixel = fill;
					}
					if (!WritePixel(*output, x, y, pixel))
						return context.Fail(
							Status::UnsupportedExecution, "shape sample exceeds finite surface storage", "bg"
						);
				}
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceSimpleShapeExecutors() {
		static const ExecutorEntry entries[] = {
			{"pc.shape_ellipse", SimpleShape, true},
			{"pc.shape_rectangle", SimpleShape, true},
			{"pc.shape_half", SimpleShape, true}
		};
		return entries;
	}
}
