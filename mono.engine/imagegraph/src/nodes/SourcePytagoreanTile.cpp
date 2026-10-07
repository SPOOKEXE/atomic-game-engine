#include "SourcePytagoreanTile.hpp"

#include "../SourceMappedInputs.hpp"
#include "Families.hpp"
#include "Sampler.hpp"
#include "Source2DComplexGenerator.hpp"
#include "SourceShaderGradient.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <variant>
#include <vector>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t PYTAGOREAN_WORK_LIMIT = 64000000, PYTAGOREAN_PIXEL_WORK = 8192;
		using Float2 = std::array<float, 2>;
		using Float4 = std::array<float, 4>;
		struct PytagoreanInputs {
			source2d::ComplexCanvas Canvas;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			Float2 Dimension{}, Position{}, Scale{}, Rotation{}, Gap{}, LevelIn{0, 1}, LevelOut{0, 1},
				TextureAngle{};
			Float4 TexturePosition{}, TextureScale{1, 1, 1, 1};
			const Image *Uv = nullptr, *Mask = nullptr, *Texture = nullptr, *ScaleMap = nullptr,
						*RotationMap = nullptr, *GapMap = nullptr;
			float UvMix = 1, Phase = 90, Seed = 0, TextureSeed = 0, Flip = .5f;
			int32_t Mode = 0;
			int64_t Oversample = 4;
			bool Covered = false, AntiAlias = false, Truchet = false;
		};
		bool PytagoreanFinite(NodeContext &c, std::string_view port, float value) {
			return std::isfinite(value) ||
				   c.Fail(Status::InvalidValue, "Pytagorean shader arithmetic exceeds finite range", port);
		}
		bool PytagoreanFloat(NodeContext &c, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return c.Fail(Status::InvalidValue, "Pytagorean value exceeds shader float range", port);
			out = float(value);
			return true;
		}
		bool PytagoreanPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
			return PytagoreanFloat(c, port, value.X, out[0]) && PytagoreanFloat(c, port, value.Y, out[1]);
		}
		bool PytagoreanSurface(NodeContext &c, std::string_view port, const Image *image) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Pytagorean raw sampler binding rejects Atlas", port
				);
			return !image ||
				   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Pytagorean sampler layout is invalid", port);
		}
		bool PreparePytagorean(NodeContext &c, PytagoreanInputs &in) {
			in.Uv = c.Input("uv_map");
			in.Mask = c.Input("mask");
			if (!PytagoreanSurface(c, "uv_map", in.Uv) || !PytagoreanSurface(c, "mask", in.Mask) ||
				!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
				return false;
			if (in.Canvas.Width > c.Request.MaximumImageDimension ||
				in.Canvas.Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Pytagorean output exceeds request dimensions", "dimension"
				);
			in.Canvas.Raw = c.Vec2("dimension", {1, 1});
			if (!c.IsLinked("dimension")) {
				const auto unit = c.Integer("dimension_unit", 1);
				if (unit == 1) {
					in.Canvas.Raw.X *= c.Project.SurfaceWidth;
					in.Canvas.Raw.Y *= c.Project.SurfaceHeight;
				} else if (unit == 2) {
					in.Canvas.Raw.X *= in.Mask->Width;
					in.Canvas.Raw.Y *= in.Mask->Height;
				}
			}
			if (!PytagoreanPair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
			const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
			// grug clear target and absent fragments do not consume shader uniforms.
			if (!in.Covered) return c.FailureCode == Status::Ok;
			if (in.Uv && !PytagoreanFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
			Vector2 position;
			if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position)) return false;
			// grug host divides Position in double before uploading its two float lanes.
			if (!PytagoreanPair(
					c, "position", {position.X / in.Canvas.Raw.X, position.Y / in.Canvas.Raw.Y}, in.Position
				))
				return false;
			for (const std::string_view port :
				 {std::string_view("scale"), std::string_view("rotation"), std::string_view("gap")}) {
				Vector2 range;
				if (!ReadSourceMappedRange(c, port, range)) return false;
				if (port == "scale") {
					const auto unit = c.Integer("scale_unit", 1);
					if (unit < 0 || unit > 1)
						return c.Fail(Status::InvalidValue, "Pytagorean Scale unit is invalid", "scale_unit");
					const auto domain = c.InputDomain("scale");
					const bool surface = domain && domain->Kind == SourceSocketKind::Surface;
					if (unit == 1 && !c.Input("scale") && !surface) {
						Vector2 reference;
						if (!source2d::ResolveReferenceDimension(c, in.Canvas.Raw, reference)) return false;
						range.X *= reference.X;
						range.Y *= reference.Y;
					}
				}
				Float2 *target = port == "scale" ? &in.Scale : port == "rotation" ? &in.Rotation : &in.Gap;
				if (!PytagoreanPair(c, port, range, *target)) return false;
			}
			const auto *original = source2d::GeneratorOriginal(c, "scale");
			const auto *array = original ? std::get_if<ArrayValue>(original) : nullptr;
			if (SourceRangeMapped(c, "scale") && array &&
				(!array->Nested.empty() || !array->Items.empty() ||
				 std::any_of(array->Elements.begin(), array->Elements.end(), [](const auto &v) {
					 return !std::holds_alternative<double>(v) && !std::holds_alternative<int64_t>(v);
				 })))
				return c.Fail(
					Status::UnsupportedExecution,
					"Pytagorean mapped Scale nested upload is not a defined vec2 uniform",
					"scale"
				);
			in.ScaleMap = c.Boolean("scale_mapped") ? c.Input("scale_map") : nullptr;
			in.RotationMap = c.Boolean("rotation_mapped") ? c.Input("rotation_map") : nullptr;
			in.GapMap = c.Boolean("gap_mapped") ? c.Input("gap_map") : nullptr;
			if ((c.Boolean("scale_mapped") && !PytagoreanSurface(c, "scale_map", in.ScaleMap)) ||
				(c.Boolean("rotation_mapped") && !PytagoreanSurface(c, "rotation_map", in.RotationMap)) ||
				(c.Boolean("gap_mapped") && !PytagoreanSurface(c, "gap_map", in.GapMap)))
				return false;
			const auto mode = c.Integer("render_type", 0);
			if (mode < 0 || mode > 2)
				return c.Fail(
					Status::UnsupportedExecution,
					"Pytagorean render type leaves source color uninitialized",
					"render_type"
				);
			in.Mode = int32_t(mode);
			if (!c.Find("seed"))
				return c.Fail(
					Status::UnsupportedExecution, "Pytagorean requires resolved source Seed", "seed"
				);
			if (!PytagoreanFloat(c, "seed", c.Scalar("seed"), in.Seed) ||
				!PytagoreanFloat(c, "phase", c.Scalar("phase", 90), in.Phase))
				return false;
			in.AntiAlias = c.Boolean("anti_aliasing");
			const auto sampler = ReadSampler(c);
			in.Oversample = sampler.Oversample;
			in.Texture = c.Input("texture");
			if (in.Texture &&
				!ValidSurfaceLayout(*in.Texture, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return c.Fail(Status::InvalidValue, "Pytagorean Texture layout is invalid", "texture");
			if (const auto *value = c.Find("texture"))
				if (const auto *atlas = std::get_if<AtlasValue>(value); atlas && !ValidAtlasPayload(*atlas))
					return c.Fail(Status::InvalidValue, "Pytagorean Texture Atlas is malformed", "texture");
			if (in.Mode == 1) {
				if (!PytagoreanPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
					!PytagoreanPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
					return false;
				if (in.LevelIn[0] == in.LevelIn[1])
					return c.Fail(
						Status::UnsupportedExecution, "Pytagorean equal levels divide by zero", "level_in"
					);
			}
			in.Truchet = in.Mode == 2 && c.Boolean("truchet");
			if (in.Truchet) {
				if (!c.Find("texture_seed"))
					return c.Fail(
						Status::UnsupportedExecution,
						"Pytagorean requires resolved Texture Seed",
						"texture_seed"
					);
				if (!PytagoreanFloat(c, "texture_seed", double(c.Integer("texture_seed")), in.TextureSeed) ||
					!PytagoreanFloat(c, "flip_threshold", c.Scalar("flip_threshold", .5), in.Flip) ||
					!PytagoreanPair(c, "random_angle", c.Vec2("random_angle", {0, 0}), in.TextureAngle))
					return false;
				const auto position = c.Get<Vector4>("random_position", {0, 0, 0, 0}),
						   scale = c.Get<Vector4>("random_scale", {1, 1, 1, 1});
				const std::array<double, 4> p{position.X, position.Y, position.Z, position.W},
					s{scale.X, scale.Y, scale.Z, scale.W};
				for (size_t lane = 0; lane < 4; ++lane)
					if (!PytagoreanFloat(c, "random_position", p[lane], in.TexturePosition[lane]) ||
						!PytagoreanFloat(c, "random_scale", s[lane], in.TextureScale[lane]))
						return false;
			}
			return c.FailureCode == Status::Ok;
		}

		float PytagoreanFract(float value) {
			return value - std::floor(value);
		}
		float PytagoreanMix(float low, float high, float weight) {
			return low * (1.f - weight) + high * weight;
		}
		float PytagoreanMod(float x, float y) {
			return x - y * std::floor(x / y);
		}
		Float4 PytagoreanColor(NodeContext &c, std::string_view port, Colour fallback) {
			const auto color = c.Get<Colour>(port, fallback);
			return {
				float(color.Red) / 255.f,
				float(color.Green) / 255.f,
				float(color.Blue) / 255.f,
				float(color.Alpha) / 255.f
			};
		}
		bool PytagoreanGradient(NodeContext &c, float progress, Float4 &out) {
			if (c.Boolean("tile_color_mapped")) {
				const auto *map = c.Input("tile_color_map");
				if (!PytagoreanSurface(c, "tile_color_map", map)) return false;
				if (map) {
					const auto range = c.Get<Vector4>("tile_color_map_range", {0, 0, 1, 0});
					std::array<float, 4> lanes{};
					const std::array<double, 4> values{range.X, range.Y, range.Z, range.W};
					for (size_t lane = 0; lane < 4; ++lane)
						if (!PytagoreanFloat(c, "tile_color_map_range", values[lane], lanes[lane]))
							return false;
					const float u = PytagoreanMix(lanes[0], lanes[2], progress),
								v = PytagoreanMix(lanes[1], lanes[3], progress);
					if (!PytagoreanFinite(c, "tile_color_map_range", u) ||
						!PytagoreanFinite(c, "tile_color_map_range", v))
						return false;
					const auto pixel = BilinearClamp(*map, u, v);
					for (size_t lane = 0; lane < 4; ++lane) {
						out[lane] = float(pixel[lane]);
						if (!PytagoreanFinite(c, "tile_color_map", out[lane])) return false;
					}
					return true;
				}
			}
			const auto *value = c.Find("tile_color");
			const auto *gradient = value ? std::get_if<Gradient>(value) : nullptr;
			if (!gradient || gradient->Keys.empty() || gradient->Keys.size() > 64)
				return c.Fail(
					Status::UnsupportedExecution,
					"Pytagorean gradient exceeds defined GLSL key layout",
					"tile_color"
				);
			const auto color = [&](size_t i) {
				const auto &v = gradient->Keys[i].Color;
				return Float4{
					float(v.Red) / 255.f,
					float(v.Green) / 255.f,
					float(v.Blue) / 255.f,
					float(v.Alpha) / 255.f
				};
			};
			for (size_t i = 0; i < gradient->Keys.size(); ++i) {
				float time = 0;
				if (!PytagoreanFloat(c, "tile_color", gradient->Keys[i].Time, time)) return false;
				if (time == progress || (time > progress && i == 0)) {
					out = color(i);
					return true;
				}
				if (time > progress) {
					float before = 0;
					if (!PytagoreanFloat(c, "tile_color", gradient->Keys[i - 1].Time, before)) return false;
					const float t = (progress - before) / (time - before);
					const auto a = color(i - 1), b = color(i);
					if (gradient->Mode == 1) {
						out = a;
						return true;
					}
					if (gradient->Mode > 6) break;
					const auto rgb =
						ShaderGradientMix({a[0], a[1], a[2]}, {b[0], b[1], b[2]}, t, gradient->Mode);
					out = {rgb[0], rgb[1], rgb[2], PytagoreanMix(a[3], b[3], t)};
					for (float lane : out)
						if (!PytagoreanFinite(c, "tile_color", lane)) return false;
					return true;
				}
			}
			out = color(gradient->Keys.size() - 1);
			return true;
		}
		Float2 PytagoreanRotate(Float2 p, float angle) {
			const float c = std::cos(angle), s = std::sin(angle);
			return {p[0] * c - p[1] * s, p[0] * s + p[1] * c};
		}
		float PytagoreanSign(float x) {
			return float(x > 0) - float(x < 0);
		}
		bool PytagoreanRandom(NodeContext &c, const PytagoreanInputs &in, Float2 p, float &value) {
			const float angle = (p[0] + 85.456034f) * 12.9898f + (p[1] + 64.54065f) * 78.233f;
			const float factor = PytagoreanMod(43758.5453123f + in.Seed, 100000.f);
			if (!PytagoreanFinite(c, "seed", angle) || !PytagoreanFinite(c, "seed", factor)) return false;
			value = PytagoreanFract(std::sin(angle) * factor / 10.f);
			return PytagoreanFinite(c, "seed", value);
		}
		bool PytagoreanCoordinates(NodeContext &c, const PytagoreanInputs &in, Float2 uv, Float4 &hc) {
			constexpr float pi = 3.14159265359f, tau = 6.28318530718f;
			const float a = (in.Phase * .017453292519943295f) / 4.f;
			if (!PytagoreanFinite(c, "phase", a)) return false;
			const float quarter = a / (tau / 4.f),
						rounded = PytagoreanFract(quarter) >= .5f ? std::ceil(quarter) : std::floor(quarter),
						q = PytagoreanMod(rounded, 2.f);
			Float2 p = PytagoreanRotate({uv[0] * 2.f, uv[1] * 2.f}, a), p1{}, p2{}, sign{}, id{};
			for (size_t lane = 0; lane < 2; ++lane) {
				if (!PytagoreanFinite(c, "scale", p[lane])) return false;
				p1[lane] = PytagoreanFract(p[lane]) - .5f;
				sign[lane] = PytagoreanSign(p1[lane]);
				p2[lane] = (std::abs(p1[lane]) - .5f) * sign[lane];
				id[lane] = (p[lane] - p1[lane]) * 2.f;
			}
			p1 = PytagoreanRotate(p1, -a);
			p2 = PytagoreanRotate(p2, -a);
			const Float2 sizeAxes{std::abs(std::cos(a)) * .5f, std::abs(std::sin(a)) * .5f};
			const Float2 d1{std::abs(p1[0]) - sizeAxes[0], std::abs(p1[1]) - sizeAxes[0]},
				d2{std::abs(p2[0]) - sizeAxes[1], std::abs(p2[1]) - sizeAxes[1]};
			const float box1 = std::max(d1[0], d1[1]), box2 = std::max(d2[0], d2[1]);
			const float m = q > .5f ? (box1 < 0 ? 0.f : 1.f) : (0.f < box2 ? 0.f : 1.f);
			const float s1 = PytagoreanMod(std::ceil(std::abs((a - pi) / (pi / 4.f))), 2.f) * 2.f - 1.f,
						s2 = q * 2.f - 1.f, ss = s1 * s2 * 2.f;
			const float m2 = std::clamp(PytagoreanSign(box1) + PytagoreanSign(box2), 0.f, 1.f);
			for (size_t lane = 0; lane < 2; ++lane) {
				id[lane] += sign[lane] * m;
				id[lane] += (d1[lane] < 0 ? 0.f : 1.f) * PytagoreanSign(p1[lane]) * ss * m2;
			}
			const float size = m < .5f ? sizeAxes[0] : sizeAxes[1];
			if (size == 0)
				return c.Fail(
					Status::UnsupportedExecution, "Pytagorean cell coordinate divides by zero size", "phase"
				);
			Float2 puv = PytagoreanRotate({(p[0] - id[0] * .5f) / size, (p[1] - id[1] * .5f) / size}, -a);
			for (auto &lane : puv)
				lane *= .5f;
			float random = 0;
			if (!PytagoreanRandom(c, in, id, random)) return false;
			hc = {
				random,
				std::min((.5f - std::abs(puv[0])) * size, (.5f - std::abs(puv[1])) * size),
				puv[0],
				puv[1]
			};
			for (float lane : hc)
				if (!PytagoreanFinite(c, "phase", lane)) return false;
			return true;
		}
		bool PytagoreanMapped(
			NodeContext &c,
			std::string_view port,
			Float2 range,
			const Image *map,
			float u,
			float v,
			float &value
		) {
			value = range[0];
			if (map) {
				const auto pixel = SampleNearest(*map, u, v);
				value = PytagoreanMix(
					range[0], range[1], (float(pixel[0]) + float(pixel[1]) + float(pixel[2])) / 3.f
				);
			}
			return PytagoreanFinite(c, port, value);
		}
		bool PytagoreanTexture(NodeContext &c, const PytagoreanInputs &in, Float2 uv, Float4 &color) {
			for (float lane : uv)
				if (!PytagoreanFinite(c, "texture", lane)) return false;
			// grug source sampleTexture reads UV again, though this overload uses mapBlend zero.
			if (in.Uv) {
				const auto sample = SampleNearest(*in.Uv, uv[0], uv[1]);
				const float weight = 0.f * in.UvMix;
				uv = {
					PytagoreanMix(uv[0], float(sample[0]), weight),
					PytagoreanMix(uv[1], 1.f - float(sample[1]), weight)
				};
			}
			for (float lane : uv)
				if (!PytagoreanFinite(c, "texture", lane)) return false;
			if (!(uv[0] >= 0 && uv[0] <= 1 && uv[1] >= 0 && uv[1] <= 1)) {
				const auto mode = in.Oversample;
				if (mode <= 1) {
					color = {0, 0, 0, 0};
					return true;
				}
				if (mode == 2) {
					color = {0, 0, 0, 1};
					return true;
				}
				if (mode == 3) {
					for (auto &lane : uv)
						lane = std::clamp(lane, 0.f, 1.f);
				} else if (mode == 4) {
					for (auto &lane : uv)
						lane = PytagoreanFract(lane);
				} else if (mode >= 6 && mode <= 8) {
					uv[0] = PytagoreanFract(uv[0]);
					if (mode == 8)
						uv[1] = std::clamp(uv[1], 0.f, 1.f);
					else if (uv[1] < 0 || uv[1] > 1) {
						color = {0, 0, 0, mode == 7 ? 1.f : 0.f};
						return true;
					}
				} else if (mode >= 10 && mode <= 12) {
					uv[1] = PytagoreanFract(uv[1]);
					if (mode == 12)
						uv[0] = std::clamp(uv[0], 0.f, 1.f);
					else if (uv[0] < 0 || uv[0] > 1) {
						color = {0, 0, 0, mode == 11 ? 1.f : 0.f};
						return true;
					}
				} else {
					color = {0, 0, 0, 0};
					return true;
				}
			}
			if (!in.Texture) {
				color = {1, 1, 1, 1};
				return true;
			}
			const auto sample = SampleNearest(*in.Texture, uv[0], uv[1]);
			for (size_t lane = 0; lane < 4; ++lane) {
				color[lane] = float(sample[lane]);
				if (!PytagoreanFinite(c, "texture", color[lane])) return false;
			}
			return true;
		}
		bool
		ShadePytagorean(NodeContext &c, const PytagoreanInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
			if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1]) {
				pixel = {0, 0, 0, 0};
				return true;
			}
			const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
			Float2 scale = in.Scale;
			if (in.ScaleMap) {
				float value = 0;
				if (!PytagoreanMapped(c, "scale", in.Scale, in.ScaleMap, u, v, value)) return false;
				scale = {value, value};
			}
			float rotation = 0, gap = 0;
			if (!PytagoreanMapped(c, "rotation", in.Rotation, in.RotationMap, u, v, rotation) ||
				!PytagoreanMapped(c, "gap", in.Gap, in.GapMap, u, v, gap))
				return false;
			for (size_t lane = 0; lane < 2; ++lane) {
				if (scale[lane] == 0)
					return c.Fail(Status::UnsupportedExecution, "Pytagorean Scale divides by zero", "scale");
				scale[lane] = in.Dimension[lane] / scale[lane] / 4.f;
				if (!PytagoreanFinite(c, "scale", scale[lane])) return false;
			}
			rotation *= .017453292519943295f;
			if (!PytagoreanFinite(c, "rotation", rotation)) return false;
			gap = std::pow(std::clamp(gap, 0.f, 1.f), 3.f);
			Float2 uv{u, v};
			float alpha = 1;
			if (in.Uv) {
				const auto sample = SampleNearest(*in.Uv, u, v);
				uv = {
					PytagoreanMix(u, float(sample[0]), in.UvMix),
					PytagoreanMix(v, 1.f - float(sample[1]), in.UvMix)
				};
				alpha = float(sample[3]);
			}
			for (float lane : uv)
				if (!PytagoreanFinite(c, "uv_map", lane)) return false;
			if (!PytagoreanFinite(c, "uv_map", alpha)) return false;
			Float2 position = PytagoreanRotate(
				{(uv[0] - in.Position[0]) * (in.Dimension[0] / in.Dimension[1]), uv[1] - in.Position[1]},
				rotation
			);
			for (size_t lane = 0; lane < 2; ++lane)
				position[lane] *= scale[lane];
			Float4 hc{};
			if (!PytagoreanCoordinates(c, in, position, hc)) return false;
			if (in.Mode == 1) {
				const float value = PytagoreanMix(
					in.LevelOut[0],
					in.LevelOut[1],
					(hc[1] * 2.f - in.LevelIn[0]) / (in.LevelIn[1] - in.LevelIn[0])
				);
				if (!PytagoreanFinite(c, "level_out", value)) return false;
				pixel = {value, value, value, alpha};
				return true;
			}
			Float4 color{};
			if (in.Mode == 0) {
				float shift = 0;
				if (!PytagoreanFloat(c, "shift", c.Scalar("shift"), shift)) return false;
				const float sum = std::abs(hc[0]) + shift;
				if (!PytagoreanFinite(c, "shift", sum)) return false;
				if (!PytagoreanGradient(c, PytagoreanFract(PytagoreanFract(sum) + 1.f), color)) return false;
			} else {
				Float2 sample{hc[2] + .5f, hc[3] + .5f};
				if (in.Truchet) {
					const auto random = [&](Float2 offset, float &out) {
						return PytagoreanRandom(
							c,
							in,
							{hc[0] + in.TextureSeed / 100.f + offset[0],
							 hc[0] + in.TextureSeed / 100.f + offset[1]},
							out
						);
					};
					float rx = 0, ry = 0, tseed = 0;
					if (!random({0, 0}, rx) || !random({.4864f, .6879f}, ry) ||
						!random({.9843f, .1636f}, tseed))
						return false;
					if (rx > in.Flip) sample[0] = 1.f - sample[0];
					if (ry > in.Flip) sample[1] = 1.f - sample[1];
					std::array<float, 5> choices{};
					for (size_t i = 0; i < 5; ++i)
						if (!PytagoreanRandom(c, in, {tseed + float(i), tseed + float(i)}, choices[i]))
							return false;
					const float angle =
						(in.TextureAngle[0] + (in.TextureAngle[1] - in.TextureAngle[0]) * choices[0]) *
						.017453292519943295f;
					if (!PytagoreanFinite(c, "random_angle", angle)) return false;
					const Float2 offset{
						PytagoreanMix(in.TexturePosition[0], in.TexturePosition[2], choices[1]),
						PytagoreanMix(in.TexturePosition[1], in.TexturePosition[3], choices[2])
					},
						sizing{
							PytagoreanMix(in.TextureScale[0], in.TextureScale[1], choices[3]),
							PytagoreanMix(in.TextureScale[2], in.TextureScale[3], choices[4])
						};
					sample = PytagoreanRotate({sample[0] - .5f, sample[1] - .5f}, angle);
					for (size_t lane = 0; lane < 2; ++lane) {
						if (sizing[lane] == 0)
							return c.Fail(
								Status::UnsupportedExecution,
								"Pytagorean Texture Scale divides by zero",
								"random_scale"
							);
						if (!PytagoreanFinite(c, "random_scale", sizing[lane]) ||
							!PytagoreanFinite(c, "random_position", offset[lane]))
							return false;
						sample[lane] /= sizing[lane];
						sample[lane] += .5f;
						sample[lane] -= offset[lane];
					}
				}
				for (float lane : sample)
					if (!PytagoreanFinite(c, "texture", lane)) return false;
				if (!PytagoreanTexture(c, in, sample, color)) return false;
			}
			float weight = hc[1] < gap ? 0.f : 1.f;
			if (in.AntiAlias) {
				const float aa = 3.f / std::max(in.Dimension[0], in.Dimension[1]), edge = gap - aa;
				if (!(edge < gap))
					return c.Fail(
						Status::UnsupportedExecution,
						"Pytagorean anti-aliasing smoothstep edges collapse",
						"anti_aliasing"
					);
				const float t = std::clamp((hc[1] - edge) / (gap - edge), 0.f, 1.f);
				weight = t * t * (3.f - 2.f * t);
			}
			const auto background = PytagoreanColor(c, "gap_color", {0, 0, 0, 255});
			for (size_t lane = 0; lane < 4; ++lane)
				color[lane] = PytagoreanMix(background[lane], color[lane], weight);
			color[3] *= alpha;
			for (float lane : color)
				if (!PytagoreanFinite(c, "surface_out", lane)) return false;
			pixel = {color[0], color[1], color[2], color[3]};
			return true;
		}

		bool QuotePytagorean(NodeContext &c, const PytagoreanInputs &in, uint64_t &work) {
			const bool mask =
				in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &input) {
					return input.first == "mask" && input.second && !input.second->Images.empty();
				});
			if (!source2d::ComplexBatchAdmission(
					c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
				))
				return false;
			const uint64_t perPixel = in.Covered ? PYTAGOREAN_PIXEL_WORK : 512,
						   pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height;
			if (work > PYTAGOREAN_WORK_LIMIT || pixels > (PYTAGOREAN_WORK_LIMIT - work) / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Pytagorean complete batch exceeds work limit", "surface_out"
				);
			work += pixels * perPixel;
			// grug source curve and sampler state must be proved before any output is made.
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					Rgba pixel{};
					if (!ShadePytagorean(c, in, x, y, pixel)) return false;
				}
			return true;
		}
		bool DrawPytagorean(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.pytagorean_tile");
			PytagoreanInputs in;
			uint64_t work = 0;
			if (!PreparePytagorean(c, in) || !QuotePytagorean(c, in, work)) return false;
			const uint64_t bytes = uint64_t(in.Canvas.Width) * in.Canvas.Height * 4;
			auto charge = c.ReserveWorkspace(in.Mask ? bytes : 0, "surface_out");
			if (!charge) return false;
			Image scratch;
			if (in.Mask)
				scratch = {
					in.Canvas.Width,
					in.Canvas.Height,
					std::vector<uint8_t>(size_t(bytes)),
					0,
					SurfaceFormat::RGBA8Unorm
				};
			auto *out = c.NewImage("surface_out", in.Canvas.Width, in.Canvas.Height, in.Format);
			if (!out) return false;
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					Rgba pixel{};
					if (!ShadePytagorean(c, in, x, y, pixel)) return false;
					if (!WritePixel(*out, x, y, pixel))
						return c.Fail(
							Status::InvalidValue,
							"Pytagorean sample exceeds output storage range",
							"surface_out"
						);
					if (in.Mask) {
						pixel = ReadPixel(*out, x, y);
						const auto mask = SampleNearest(
							*in.Mask, (float(x) + .5f) / in.Canvas.Width, (float(y) + .5f) / in.Canvas.Height
						);
						const float brightness =
							(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
						pixel[3] = float(pixel[3]) * brightness;
						if (!PytagoreanFinite(c, "mask", brightness) || !WritePixel(scratch, x, y, pixel))
							return c.Fail(
								Status::InvalidValue, "Pytagorean mask exceeds finite storage range", "mask"
							);
					}
				}
			if (in.Mask)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x)
						if (!WritePixel(*out, x, y, ReadPixel(scratch, x, y)))
							return c.Fail(
								Status::InvalidValue,
								"Pytagorean mask copy exceeds storage range",
								"surface_out"
							);
			return c.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourcePytagoreanTile(NodeContext &context, uint64_t &work) {
		PytagoreanInputs in;
		return PreparePytagorean(context, in) && QuotePytagorean(context, in, work);
	}
	std::span<const ExecutorEntry> SourcePytagoreanTileExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.pytagorean_tile", DrawPytagorean, true}};
		return entries;
	}
}
