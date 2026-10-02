#include "Families.hpp"
#include "Gradient.hpp"
#include "Sampler.hpp"

#include <numbers>

// Source Pixel Bevel uses three RGBA8 temporary surfaces and two RGBA8 outputs.
namespace engine::imagegraph::detail {
	namespace {
		float PixelBevelValue(const Rgba &c) {
			return (float(c[0]) + float(c[1]) + float(c[2])) / 3 * float(c[3]);
		}
		Rgba PixelBevelGradient(const Gradient &gradient, double progress) {
			const auto colour = [&](size_t n) {
				const auto &c = gradient.Keys[n].Color;
				return Rgba{c.Red / 255., c.Green / 255., c.Blue / 255., c.Alpha / 255.};
			};
			for (size_t i = 0; i < gradient.Keys.size(); i++) {
				const float time = float(gradient.Keys[i].Time);
				if (time == float(progress)) return colour(i);
				if (time > float(progress)) {
					if (i == 0) return colour(0);
					const float t = (float(progress) - float(gradient.Keys[i - 1].Time)) /
									(time - float(gradient.Keys[i - 1].Time));
					const auto a = colour(i - 1), b = colour(i);
					if (gradient.Mode == 1) return a;
					auto rgb = GradientMix({a[0], a[1], a[2]}, {b[0], b[1], b[2]}, t, gradient.Mode);
					if (gradient.Mode == 0)
						for (size_t c = 0; c < 3; c++)
							rgb[c] = float(a[c]) + (float(b[c]) - float(a[c])) * t;
					if (gradient.Mode == 4)
						for (size_t c = 0; c < 3; c++) {
							const float x = std::pow(float(a[c]), 2.2f), y = std::pow(float(b[c]), 2.2f);
							rgb[c] = std::pow(x + (y - x) * t, 1 / 2.2f);
						}
					if (gradient.Mode == 3) {
						constexpr float into[3][3] = {
							{.4121656120f, .5362752080f, .0514575653f},
							{.2118591070f, .6807189584f, .1074065790f},
							{.0883097947f, .2818474174f, .6302613616f}
						};
						constexpr float out[3][3] = {
							{4.0767245293f, -3.3072168827f, .2307590544f},
							{-1.2681437731f, 2.6093323231f, -.3411344290f},
							{-.0041119885f, -.7034763098f, 1.7068625689f}
						};
						std::array<float, 3> aa{}, bb{}, mix{};
						for (size_t r = 0; r < 3; r++) {
							for (size_t c = 0; c < 3; c++) {
								aa[r] += into[r][c] * std::pow(float(a[c]), 2.2f);
								bb[r] += into[r][c] * std::pow(float(b[c]), 2.2f);
							}
							aa[r] = std::pow(aa[r], 1 / 3.f);
							bb[r] = std::pow(bb[r], 1 / 3.f);
							mix[r] = aa[r] + (bb[r] - aa[r]) * t;
						}
						for (size_t r = 0; r < 3; r++) {
							float value = 0;
							for (size_t c = 0; c < 3; c++)
								value += out[r][c] * mix[c] * mix[c] * mix[c];
							rgb[r] = std::pow(value, 1 / 2.2f);
						}
					}

					return {rgb[0], rgb[1], rgb[2], float(a[3]) + (float(b[3]) - float(a[3])) * t};
				}
			}
			return colour(gradient.Keys.size() - 1);
		}
		bool PixelBevel(NodeContext &context) {
			const auto *source = context.Input("surface");
			if (!source)
				return context.Fail(Status::TypeMismatch, "pixel bevel requires a surface", "surface");
			const int64_t height = context.Integer("height", 2);
			const uint64_t pixels = uint64_t(source->Width) * source->Height,
						   limit = 64'000'000 / std::max<size_t>(1, context.ProcessorCount);
			if (height > int64_t(limit / 4) ||
				pixels > limit / (32 + 256 + 4 * uint64_t(std::max<int64_t>(height, 0))))
				return context.Fail(
					Status::LimitExceeded, "pixel bevel exceeds bounded aggregate sample work", "height"
				);
			const auto *gh = context.Find("color_over_height")
								 ? std::get_if<Gradient>(context.Find("color_over_height"))
								 : nullptr;
			const auto *ga = context.Find("color_over_angle")
								 ? std::get_if<Gradient>(context.Find("color_over_angle"))
								 : nullptr;
			if (!gh || !ga || gh->Keys.empty() || ga->Keys.empty() || gh->Keys.size() > 128 ||
				ga->Keys.size() > 128 || gh->Mode > 4 || ga->Mode > 4)
				return context.Fail(
					Status::UnsupportedExecution,
					"pixel bevel requires defined source gradient uploads",
					"color_over_height"
				);
			for (const auto id : {"shift_angle", "shift", "shift_2", "highlight_direction"})
				if (!std::isfinite(float(context.Scalar(id))))
					return context.Fail(
						Status::UnsupportedExecution, "pixel bevel controls exceed finite shader uniforms", id
					);
			auto charge = context.ReserveWorkspace(pixels * 12, "surface_out");
			if (!charge) return false;
			Image edge, angle, colour;
			for (auto *image : {&edge, &angle, &colour}) {
				image->Width = source->Width;
				image->Height = source->Height;
				image->Format = SurfaceFormat::RGBA8Unorm;
				image->Pixels.resize(pixels * 4);
			}
			Image *inner =
				context.NewImage("inner_area", source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
			Image *output =
				context.NewImage("surface_out", source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
			if (!inner || !output) return false;
			const bool filtered = false;
			const float tx = 1.f / source->Width, ty = 1.f / source->Height;
			const auto sample = [&](const Image &im, float u, float v) {
				return Texture(im, u, v, filtered);
			};
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const float u = float((x + .5) / source->Width), v = float((y + .5) / source->Height);
					bool boundary = false;
					if (PixelBevelValue(sample(*source, u, v)) == 0)
						for (const auto &d :
							 {Vector2{tx, 0}, Vector2{-tx, 0}, Vector2{0, ty}, Vector2{0, -ty}})
							if (PixelBevelValue(sample(*source, u + float(d.X), v + float(d.Y))) == 1) {
								boundary = true;
								break;
							}
					WritePixel(edge, x, y, boundary ? Rgba{1, 1, 1, 1} : Rgba{});
				}
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const float u = float((x + .5) / source->Width), v = float((y + .5) / source->Height);
					Rgba result{};
					if (PixelBevelValue(sample(edge, u, v)) != 0) {
						const auto exists = [&](float dx, float dy) {
							return PixelBevelValue(sample(edge, u + dx, v + dy)) != 0;
						};
						const bool l = exists(-tx, 0), r = exists(tx, 0), up = exists(0, -ty),
								   down = exists(0, ty);
						if (l && r)
							result = {0, 0, 0, 1};
						else if (up && down)
							result = {.5, 0, 0, 1};
						else if (exists(tx, -ty) || exists(-tx, ty))
							result = {.25, 0, 0, 1};
						else if (exists(-tx, -ty) || exists(tx, ty))
							result = {.75, 0, 0, 1};
						else if (l || r)
							result = {0, 0, 0, 1};
						else if (up || down)
							result = {.5, 0, 0, 1};
					}
					WritePixel(angle, x, y, result);
				}
			const float shiftAngle = float(context.Scalar("shift_angle") / 360),
						shiftH = float(context.Scalar("shift")), shiftA = float(context.Scalar("shift_2"));
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const float u = float((x + .5) / source->Width), v = float((y + .5) / source->Height);
					auto result = sample(*source, u, v);
					bool inside = true, inverse = false;
					float eu = 0, ev = 0, dist = 0;
					if (PixelBevelValue(result) != 0) {
						for (int64_t i = 1; i <= height && inside; i++)
							for (int j = 0; j < 4; j++) {
								const float a = float(j) * .25f * float(2 * std::numbers::pi),
											su = u + std::cos(a) * float(i) * tx,
											sv = v + std::sin(a) * float(i) * ty;
								if (PixelBevelValue(sample(*source, su, sv)) == 0) {
									inside = false;
									eu = su;
									ev = sv;
									dist = float(i);
									inverse = j >= 2;
									break;
								}
							}
						if (inside)
							WritePixel(*inner, x, y, {1, 1, 1, 1});
						else {
							float a = float(sample(angle, eu, ev)[0]) / 2;
							if (inverse) a += .5f;
							a = float(Fract(a - shiftAngle));
							const auto h =
								PixelBevelGradient(*gh, Fract(Fract(dist / float(height + 1) + shiftH) + 1));
							const auto r = PixelBevelGradient(*ga, Fract(Fract(a + shiftA) + 1));
							for (size_t c = 0; c < 4; c++)
								result[c] = float(h[c]) * float(r[c]);
						}
					}
					// Fixed-point MRT clamps source colour before the restored bm_normal blend.
					for (double &component : result) {
						if (!std::isfinite(component))
							return context.Fail(
								Status::UnsupportedExecution,
								"pixel bevel gradient produced nonfinite shader math",
								"surface_out"
							);
						component = std::clamp(component, 0., 1.);
					}
					for (double &component : result)
						component *= result[3];
					if (!WritePixel(colour, x, y, result))
						return context.Fail(
							Status::UnsupportedExecution,
							"pixel bevel gradient produced nonfinite shader math",
							"surface_out"
						);
				}
			const bool highlight = context.Boolean("highlight"), all = context.Boolean("highlight_all");
			const float direction = float(context.Scalar("highlight_direction") * std::numbers::pi / 180);
			const auto hcolour = context.Get<Colour>("highlight_color", {255, 255, 255, 255});
			const Rgba highlightColour{
				hcolour.Red / 255., hcolour.Green / 255., hcolour.Blue / 255., hcolour.Alpha / 255.
			};
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const float u = float((x + .5) / source->Width), v = float((y + .5) / source->Height);
					auto result = sample(colour, u, v);
					if (highlight && PixelBevelValue(sample(*inner, u, v)) != 0) {
						bool hit = false;
						if (!all)
							hit =
								PixelBevelValue(
									sample(*inner, u + std::cos(direction) * tx, v - std::sin(direction) * ty)
								) == 0;
						else
							for (const auto &d :
								 {Vector2{tx, 0}, Vector2{-tx, 0}, Vector2{0, ty}, Vector2{0, -ty}})
								hit = hit ||
									  PixelBevelValue(sample(*inner, u + float(d.X), v + float(d.Y))) == 0;
						if (hit) result = highlightColour;
					}
					if (!WritePixel(*output, x, y, result)) return false;
				}
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourcePixelBevelExecutors() {
		static const ExecutorEntry entries[] = {{"pc.pb_fx_bevel", PixelBevel, true}};
		return entries;
	}
}
