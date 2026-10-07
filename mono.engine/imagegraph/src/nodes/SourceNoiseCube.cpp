#include "SourceNoiseCube.hpp"

#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <variant>
namespace engine::imagegraph::detail {
	namespace {
		namespace source_noise_cube {
			constexpr uint64_t WORK_LIMIT = 64000000, BASE_WORK = 512, MARCH_WORK = 256 * 96,
							   PERLIN_OCTAVE_WORK = 2048, CELLULAR_OCTAVE_WORK = 8192;
			using F3 = std::array<float, 3>;
			struct Matrix {
				float Row[3][3]{};
			};
			struct Inputs {
				source2d::ComplexCanvas Canvas;
				SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
				Matrix Camera{}, Object{};
				F3 ObjectScale{1, 1, 1}, Position{};
				float Width = 0, Height = 0, Ortho = 1, NoiseScale = 8, Seed = 0, Cross = 0, Low = 0,
					  High = 1;
				int32_t Iteration = 4, Shape = 0, Axis = 0, CrossAxis = 0;
				bool Covered = false, Cellular = false;
			};
			bool Finite(NodeContext &c, std::string_view port, float value) {
				return std::isfinite(value) ||
					   c.Fail(
						   Status::InvalidValue, "Noise Cube shader arithmetic exceeds finite range", port
					   );
			}
			bool Float(NodeContext &c, std::string_view port, double value, float &out) {
				if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
					return c.Fail(Status::InvalidValue, "Noise Cube value exceeds shader float range", port);
				out = float(value);
				return true;
			}
			bool VectorFinite(NodeContext &c, std::string_view port, F3 v) {
				for (float n : v)
					if (!Finite(c, port, n)) return false;
				return true;
			}
			bool Tuple(NodeContext &c, std::string_view port, Vector3 fallback, F3 &out) {
				const auto *value = c.Find(port);
				std::array<double, 3> lanes{fallback.X, fallback.Y, fallback.Z};
				if (value) {
					if (const auto *v = std::get_if<Vector3>(value))
						lanes = {v->X, v->Y, v->Z};
					else if (const auto *v = std::get_if<Vector2>(value))
						lanes = {v->X, v->Y, 0};
					else if (const auto *v = std::get_if<Vector4>(value))
						lanes = {v->X, v->Y, v->Z};
					else if (const auto *n = std::get_if<double>(value))
						lanes = {*n, *n, *n};
					else if (const auto *n = std::get_if<int64_t>(value))
						lanes = {double(*n), double(*n), double(*n)};
					else if (const auto *a = std::get_if<ArrayValue>(value)) {
						lanes = {0, 0, 0};
						if (!a->Nested.empty() || !a->Items.empty())
							return c.Fail(
								Status::UnsupportedExecution, "Noise Cube needs a flat numeric tuple", port
							);
						for (size_t i = 0; i < std::min<size_t>(3, a->Elements.size()); ++i) {
							if (const auto *n = std::get_if<double>(&a->Elements[i]))
								lanes[i] = *n;
							else if (const auto *n = std::get_if<int64_t>(&a->Elements[i]))
								lanes[i] = double(*n);
							else
								return c.Fail(
									Status::UnsupportedExecution,
									"Noise Cube tuple needs numeric components",
									port
								);
						}
					} else
						return c.Fail(
							Status::UnsupportedExecution, "Noise Cube needs a resolved numeric tuple", port
						);
				}
				for (size_t i = 0; i < 3; ++i)
					if (!Float(c, port, lanes[i], out[i])) return false;
				return true;
			}
			Matrix Multiply(const Matrix &a, const Matrix &b) {
				Matrix o;
				for (size_t i = 0; i < 3; ++i)
					for (size_t j = 0; j < 3; ++j)
						o.Row[i][j] =
							a.Row[i][0] * b.Row[0][j] + a.Row[i][1] * b.Row[1][j] + a.Row[i][2] * b.Row[2][j];
				return o;
			}
			F3 Transform(const Matrix &m, F3 p) {
				F3 o;
				for (size_t i = 0; i < 3; ++i)
					o[i] = m.Row[i][0] * p[0] + m.Row[i][1] * p[1] + m.Row[i][2] * p[2];
				return o;
			}
			Matrix Rotation(F3 a) {
				for (auto &v : a)
					v *= .017453292519943295f;
				const float cx = std::cos(a[0]), sx = std::sin(a[0]), cy = std::cos(a[1]),
							sy = std::sin(a[1]), cz = std::cos(a[2]), sz = std::sin(a[2]);
				const Matrix x{{{1, 0, 0}, {0, cx, sx}, {0, -sx, cx}}},
					y{{{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}}}, z{{{cz, sz, 0}, {-sz, cz, 0}, {0, 0, 1}}};
				return Multiply(Multiply(x, y), z);
			}
			bool Inverse(NodeContext &c, const Matrix &m, Matrix &o) {
				// grug source indexes columns. keep source cofactor inverse, not transpose.
				const float a00 = m.Row[0][0], a01 = m.Row[1][0], a02 = m.Row[2][0], a10 = m.Row[0][1],
							a11 = m.Row[1][1], a12 = m.Row[2][1], a20 = m.Row[0][2], a21 = m.Row[1][2],
							a22 = m.Row[2][2];
				const float b01 = a22 * a11 - a12 * a21, b11 = -a22 * a10 + a12 * a20,
							b21 = a21 * a10 - a11 * a20, det = a00 * b01 + a01 * b11 + a02 * b21;
				if (det == 0 || !std::isfinite(det))
					return c.Fail(
						Status::UnsupportedExecution, "Noise Cube camera inverse is undefined", "rotation"
					);
				o = {
					{{b01, b11, b21},
					 {-a22 * a01 + a02 * a21, a22 * a00 - a02 * a20, -a21 * a00 + a01 * a20},
					 {a12 * a01 - a02 * a11, -a12 * a00 + a02 * a10, a11 * a00 - a01 * a10}}
				};
				for (auto &row : o.Row)
					for (auto &v : row) {
						v /= det;
						if (!Finite(c, "rotation", v)) return false;
					}
				return true;
			}
			bool Integer(NodeContext &c, std::string_view port, int64_t value, int32_t &out) {
				if (value < std::numeric_limits<int32_t>::min() ||
					value > std::numeric_limits<int32_t>::max())
					return c.Fail(
						Status::UnsupportedExecution, "Noise Cube value exceeds shader integer range", port
					);
				out = int32_t(value);
				return true;
			}
			bool Choice(NodeContext &c, std::string_view port, int32_t &out) {
				const double value = c.SourceChoice(port, 0);
				if (!std::isfinite(value) || value < double(std::numeric_limits<int32_t>::min()) ||
					value > double(std::numeric_limits<int32_t>::max()))
					return c.Fail(
						Status::UnsupportedExecution, "Noise Cube choice exceeds shader integer range", port
					);
				out = int32_t(value);
				return c.FailureCode == Status::Ok;
			}

			bool Prepare(NodeContext &c, Inputs &in) {
				in.Cellular = c.Entry.Type == "pc.cellular_cube";
				if (!source2d::ResolveGeneratorDimensions(c, nullptr, in.Canvas.Width, in.Canvas.Height))
					return false;
				if (in.Canvas.Width > c.Request.MaximumImageDimension ||
					in.Canvas.Height > c.Request.MaximumImageDimension)
					return c.Fail(
						Status::LimitExceeded, "Noise Cube exceeds request dimensions", "dimension"
					);
				in.Canvas.Raw = c.Vec2("dimension", {1, 1});
				if (!c.IsLinked("dimension")) {
					const auto unit = c.Integer("dimension_unit", 1);
					if (unit == 1) {
						in.Canvas.Raw.X *= c.Project.SurfaceWidth;
						in.Canvas.Raw.Y *= c.Project.SurfaceHeight;
					} else if (unit == 2)
						return c.Fail(
							Status::UnsupportedExecution, "Noise Cube has no mask reference", "dimension_unit"
						);
				}
				if (!Float(c, "dimension", in.Canvas.Raw.X, in.Width) ||
					!Float(c, "dimension", in.Canvas.Raw.Y, in.Height))
					return false;
				const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
				if (!format) return false;
				in.Format = *format;
				in.Covered = in.Width > .5f && in.Height > .5f;
				if (!in.Covered) return c.FailureCode == Status::Ok;
				if (!Integer(c, "iteration", c.Integer("iteration", in.Cellular ? 1 : 4), in.Iteration) ||
					!Choice(c, "shape", in.Shape) || !Choice(c, "axis", in.Axis) ||
					!Choice(c, "axis_2", in.CrossAxis))
					return false;
				const auto level = c.Vec2("level", {0, 1});
				if (!Float(c, "level", level.X, in.Low) || !Float(c, "level", level.Y, in.High)) return false;
				if (in.Low == in.High)
					return c.Fail(
						Status::UnsupportedExecution, "Noise Cube equal levels divide by zero", "level"
					);
				if (!Finite(c, "level", in.High - in.Low)) return false;
				F3 camera{}, object{};
				if (!Tuple(c, "rotation", {30, 45, 0}, camera) ||
					!Float(c, "scale", c.Scalar("scale", 1), in.Ortho))
					return false;
				if (!Inverse(c, Rotation(camera), in.Camera)) return false;
				if (in.Iteration > 0) {
					if (!c.Find("seed"))
						return c.Fail(
							Status::UnsupportedExecution, "Noise Cube requires resolved source Seed", "seed"
						);
					if (!Float(c, "seed", c.Scalar("seed"), in.Seed) ||
						!Float(c, "noise_scale", c.Scalar("noise_scale", 8), in.NoiseScale) ||
						!Tuple(c, "position", {0, 0, 0}, in.Position) ||
						!Tuple(c, "rotation_2", {0, 0, 0}, object) ||
						!Tuple(c, "scale_2", {1, 1, 1}, in.ObjectScale) ||
						!Float(c, "position_2", c.Scalar("position_2", 0), in.Cross))
						return false;
					in.Object = Rotation(object);
				}
				return c.FailureCode == Status::Ok;
			}
			float Fract(float v) {
				return v - std::floor(v);
			}
			float Mix(float a, float b, float t) {
				return a * (1.f - t) + b * t;
			}
			bool Hash(NodeContext &c, F3 p, float seed, float &out) {
				const float factor = seed / 1000.f + .3183099f;
				for (auto &v : p) {
					const float q = v * factor + .1f;
					if (!Finite(c, "seed", q)) return false;
					v = Fract(q) * 17.f;
				}
				out = Fract(p[0] * p[1] * p[2] * (p[0] + p[1] + p[2]));
				return Finite(c, "seed", out);
			}
			bool Noise(NodeContext &c, F3 p, float seed, float &out) {
				if (!VectorFinite(c, "noise_scale", p)) return false;
				F3 cell{}, f{};
				for (size_t i = 0; i < 3; ++i) {
					cell[i] = std::floor(p[i]);
					const float a = Fract(p[i]);
					f[i] = a * a * (3.f - 2.f * a);
				}
				std::array<float, 8> values{};
				for (size_t z = 0; z < 2; ++z)
					for (size_t y = 0; y < 2; ++y)
						for (size_t x = 0; x < 2; ++x)
							if (!Hash(
									c,
									{cell[0] + float(x), cell[1] + float(y), cell[2] + float(z)},
									seed,
									values[z * 4 + y * 2 + x]
								))
								return false;
				out =
					Mix(Mix(Mix(values[0], values[1], f[0]), Mix(values[2], values[3], f[0]), f[1]),
						Mix(Mix(values[4], values[5], f[0]), Mix(values[6], values[7], f[0]), f[1]),
						f[2]);
				return Finite(c, "seed", out);
			}
			bool Hash3(NodeContext &c, F3 p, float seed, F3 &out) {
				const float factor = seed / 1000.f + .3183099f;
				for (auto &value : p) {
					const float input = value * factor + .1f;
					if (!Finite(c, "seed", input)) return false;
					value = Fract(input) * 17.f;
				}
				const float product = p[0] * p[1] * p[2], sum = p[0] + p[1] + p[2];
				out = {Fract(product * sum), Fract(product * (sum + 1.f)), Fract(product * (sum + 2.f))};
				return VectorFinite(c, "seed", out);
			}
			bool Cell(NodeContext &c, F3 position, float seed, float &out) {
				if (!VectorFinite(c, "noise_scale", position)) return false;
				F3 integer{}, fraction{};
				for (size_t lane = 0; lane < 3; ++lane) {
					integer[lane] = std::floor(position[lane]);
					fraction[lane] = Fract(position[lane]);
				}
				out = 1.f;
				for (int z = -1; z <= 1; ++z)
					for (int y = -1; y <= 1; ++y)
						for (int x = -1; x <= 1; ++x) {
							const F3 neighbor{float(x), float(y), float(z)};
							F3 point{}, difference{};
							if (!Hash3(
									c,
									{integer[0] + neighbor[0],
									 integer[1] + neighbor[1],
									 integer[2] + neighbor[2]},
									seed,
									point
								))
								return false;
							for (size_t lane = 0; lane < 3; ++lane)
								difference[lane] = neighbor[lane] + point[lane] - fraction[lane];
							const float distance = std::sqrt(
								difference[0] * difference[0] + difference[1] * difference[1] +
								difference[2] * difference[2]
							);
							if (!Finite(c, "seed", distance)) return false;
							out = std::min(out, distance);
						}
				return true;
			}
			bool Sample(NodeContext &c, const Inputs &in, F3 p, float &out) {
				out = 0;
				if (in.Iteration > 0) {
					p = Transform(in.Object, p);
					for (size_t i = 0; i < 3; ++i)
						p[i] = p[i] * in.ObjectScale[i] * in.NoiseScale + in.Position[i];
					if (!VectorFinite(c, "scale_2", p)) return false;
					const float count = float(in.Iteration);
					float amplitude = std::pow(2.f, count - 1.f) / (std::pow(2.f, count) - 1.f);
					if (!Finite(c, "iteration", amplitude)) return false;
					for (int32_t i = 0; i < in.Iteration; ++i) {
						float value = 0;
						if (!(in.Cellular ? Cell(c, p, in.Seed, value) : Noise(c, p, in.Seed, value)))
							return false;
						out += value * amplitude;
						if (!Finite(c, "iteration", out)) return false;
						// grug final next-octave coordinates are never consumed.
						if (i + 1 < in.Iteration) {
							amplitude *= .5f;
							for (auto &v : p)
								v *= 2.f;
							if (!VectorFinite(c, "noise_scale", p)) return false;
						}
					}
				}
				out = (out - in.Low) / (in.High - in.Low);
				return Finite(c, "level", out);
			}
			bool Shade(NodeContext &c, const Inputs &in, uint32_t x, uint32_t y, Rgba &surface, Rgba &cross) {
				surface = {};
				cross = {};
				if (float(x) + .5f >= in.Width || float(y) + .5f >= in.Height) return true;
				const float u = (float(x) + .5f) / in.Width, v = (float(y) + .5f) / in.Height;
				const F3 direction = Transform(in.Camera, {0, 0, -1}),
						 eye = Transform(
							 in.Camera, {(u - .5f) * 2.f * in.Ortho, (v - .5f) * 2.f * in.Ortho, 5}
						 );
				if (!VectorFinite(c, "scale", eye) || !VectorFinite(c, "rotation", direction)) return false;
				float depth = 0;
				for (size_t i = 0; i < 256; ++i) {
					F3 p{};
					for (size_t j = 0; j < 3; ++j)
						p[j] = eye[j] + depth * direction[j];
					if (!VectorFinite(c, "scale", p)) return false;
					float distance = 0;
					if (in.Shape == 0) {
						F3 q{std::abs(p[0]) - .5f, std::abs(p[1]) - .5f, std::abs(p[2]) - .5f};
						const float a = std::max(q[0], 0.f), b = std::max(q[1], 0.f), d = std::max(q[2], 0.f);
						distance = std::sqrt(a * a + b * b + d * d) +
								   std::min(std::max(q[0], std::max(q[1], q[2])), 0.f);
					} else if (in.Shape == 1)
						distance = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - .5f;
					if (!Finite(c, "scale", distance)) return false;
					if (distance < 1e-5f) break;
					depth += distance;
					if (!Finite(c, "scale", depth)) return false;
					if (depth >= 10.f) {
						depth = 10.f;
						break;
					}
					if (i == 255) depth = 10.f;
				}
				F3 hit{};
				for (size_t i = 0; i < 3; ++i)
					hit[i] = eye[i] + direction[i] * depth;
				if (in.Axis == 1)
					hit = {hit[1], hit[2], hit[0]};
				else if (in.Axis == 2)
					hit = {hit[2], hit[0], hit[1]};
				float a = 0, b = 0;
				if (!Sample(c, in, hit, a)) return false;
				F3 plane{};
				if (in.CrossAxis == 0)
					plane = {in.Cross, u, v};
				else if (in.CrossAxis == 1)
					plane = {u, in.Cross, v};
				else if (in.CrossAxis == 2)
					plane = {u, v, in.Cross};
				if (!Sample(c, in, plane, b)) return false;
				surface = {a, a, a, depth < 10.f ? 1. : 0.};
				cross = {b, b, b, 1};
				return true;
			}
			bool Storage(NodeContext &c, SurfaceFormat format, const Rgba &pixel, std::string_view port) {
				const auto info = *DescribeSurfaceFormat(format);
				for (size_t lane = 0; lane < info.Channels; ++lane)
					if (info.FloatingPoint && info.BitsPerChannel == 16 && std::abs(pixel[lane]) > 65504.)
						return c.Fail(
							Status::InvalidValue, "Noise Cube sample exceeds half-float storage range", port
						);
				return true;
			}
			bool Quote(NodeContext &c, const Inputs &in, uint64_t &work) {
				if (!source2d::ComplexBatchAdmission(c, 1, "surface_out", "surface_out", 2)) return false;
				const uint64_t pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height,
							   iterations = uint64_t(std::max(in.Iteration, 0));
				const uint64_t cost =
					BASE_WORK +
					(in.Covered ? MARCH_WORK + 2 * iterations *
												   (in.Cellular ? CELLULAR_OCTAVE_WORK : PERLIN_OCTAVE_WORK)
								: 0);
				if (cost > WORK_LIMIT || work > WORK_LIMIT || pixels > (WORK_LIMIT - work) / cost)
					return c.Fail(
						Status::LimitExceeded,
						"Noise Cube complete batch exceeds CPU work limit",
						"surface_out"
					);
				work += pixels * cost;
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
						Rgba a{}, b{};
						if (!Shade(c, in, x, y, a, b) || !Storage(c, in.Format, a, "surface_out") ||
							!Storage(c, in.Format, b, "cross_section"))
							return false;
					}
				return true;
			}
			bool Draw(NodeContext &c) {
				ENGINE_PROFILE_DYNAMIC_STABLE(
					"imagegraph.source",
					c.Entry.Type == "pc.cellular_cube" ? std::string_view{"imagegraph.source.cellular_cube"}
													   : std::string_view{"imagegraph.source.perlin_cube"},
					core::ProfileCategory::Engine
				);
				Inputs in;
				uint64_t work = 0;
				if (!Prepare(c, in) || !Quote(c, in, work)) return false;
				if (!c.NewImage("surface_out", in.Canvas.Width, in.Canvas.Height, in.Format) ||
					!c.NewImage("cross_section", in.Canvas.Width, in.Canvas.Height, in.Format))
					return false;
				// grug second output can move vector storage. take references after both allocations.
				auto &surface = c.OutputImages[c.OutputImages.size() - 2].second;
				auto &cross = c.OutputImages.back().second;
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
						Rgba a{}, b{};
						if (!Shade(c, in, x, y, a, b)) return false;
						if (!WritePixel(surface, x, y, a))
							return c.Fail(
								Status::InvalidValue, "Noise Cube exceeds output storage range", "surface_out"
							);
						if (!WritePixel(cross, x, y, b))
							return c.Fail(
								Status::InvalidValue,
								"Noise Cube exceeds output storage range",
								"cross_section"
							);
					}
				return c.FailureCode == Status::Ok;
			}
		}
	}
	bool AdmitSourceNoiseCube(NodeContext &c, uint64_t &work) {
		source_noise_cube::Inputs in;
		return source_noise_cube::Prepare(c, in) && source_noise_cube::Quote(c, in, work);
	}
	std::span<const ExecutorEntry> SourceNoiseCubeExecutors() {
		static constexpr ExecutorEntry entries[]{
			{"pc.perlin_cube", source_noise_cube::Draw, true},
			{"pc.cellular_cube", source_noise_cube::Draw, true}
		};
		return entries;
	}
}
