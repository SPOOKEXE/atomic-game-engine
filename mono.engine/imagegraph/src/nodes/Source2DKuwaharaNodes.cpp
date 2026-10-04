#include "../SourceMappedInputs.hpp"
#include "../SourceSafeDraw.hpp"
#include "../SurfaceScratch.hpp"
#include "Sampler.hpp"
#include "Source2DGenerator.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t KUWAHARA_WORK_LIMIT = 64000000;
		constexpr int KUWAHARA_NATIVE_RAD = 16;
		struct KuwaharaSettings {
			SamplerSettings Sampling;
			UvMap Uv;
			Vector2 Radius;
			const Image *RadiusMap = nullptr;
			int64_t Type = 0;
			double Alpha = 1, Crossing = .58, Hardness = 8, Sharpness = 8;
		};
		template <class V> double KuwaharaMaximum(const V &v) {
			if (const auto *n = std::get_if<double>(&v)) return std::max(0., *n);
			if (const auto *n = std::get_if<int64_t>(&v)) return std::max(0., double(*n));
			if (const auto *n = std::get_if<Vector2>(&v)) return std::max({0., n->X, n->Y});
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *a = std::get_if<ArrayValue>(&v)) {
					double maximum = 0;
					for (const auto &n : a->Elements)
						maximum = std::max(maximum, KuwaharaMaximum(n));
					for (const auto &row : a->Nested)
						for (const auto &n : row)
							maximum = std::max(maximum, KuwaharaMaximum(n));
					source2d::GeneratorVisitArrayItems(a->Items, 0, [&](const ElementValue &n) {
						maximum = std::max(maximum, KuwaharaMaximum(n));
					});
					return maximum;
				}
			}
			return 0;
		}
		template <class V> bool KuwaharaWideMode(const V &v, bool anisotropic = false) {
			if (const auto *n = std::get_if<bool>(&v)) return *n;
			if (const auto *n = std::get_if<EnumValue>(&v))
				return anisotropic ? n->Value == 1 : n->Value != 0;
			if (const auto *n = std::get_if<int64_t>(&v)) return anisotropic ? *n == 1 : *n != 0;
			if (const auto *n = std::get_if<double>(&v)) return anisotropic ? *n == 1 : *n != 0;
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *a = std::get_if<ArrayValue>(&v)) {
					for (const auto &n : a->Elements)
						if (KuwaharaWideMode(n, anisotropic)) return true;
					for (const auto &row : a->Nested)
						for (const auto &n : row)
							if (KuwaharaWideMode(n, anisotropic)) return true;
					bool wide = false;
					source2d::GeneratorVisitArrayItems(a->Items, 0, [&](const ElementValue &n) {
						wide = wide || KuwaharaWideMode(n, anisotropic);
					});
					return wide;
				}
			}
			return false;
		}
		uint64_t KuwaharaPixels(const NodeContext &c, std::string_view port) {
			const auto *image = c.Input(port);
			uint64_t largest = image ? uint64_t(image->Width) * image->Height : 0;
			for (const auto &[id, frames] : c.ImageArrays)
				if (id == port && frames)
					for (const auto &frame : frames->Images)
						largest = std::max(largest, uint64_t(frame.Width) * frame.Height);
			return largest;
		}
		bool KuwaharaAdmission(NodeContext &c) {
			if (c.ProcessorRow) return true;
			const auto maximum = [&](std::string_view port, double fallback) {
				const auto *value = source2d::GeneratorOriginal(c, port);
				return value ? KuwaharaMaximum(*value) : fallback;
			};
			const uint64_t rows = std::max<size_t>(1, c.ProcessorCount),
						   pixels = KuwaharaPixels(c, "surface_in"), mask = KuwaharaPixels(c, "mask");
			// Synthetic endpoints only stand in for an unlinked catalogue default. Physical
			// authored/linked Radius remains authoritative across all original processor rows.
			const bool synthetic = SourceRangeMapped(c, "radius") && !c.IsLinked("radius") &&
								   c.IsCatalogueDefault("radius").value_or(false);
			if (c.FailureCode != Status::Ok) return false;
			const double radius =
				std::max(1., std::ceil(maximum(synthetic ? "radius_map_range" : "radius", 2)));
			const double feather = mask ? std::ceil(maximum("mask_feather", 0)) : 0;
			// All modes are covered, including later Generalized/anisotropic rows. A cell updates eight
			// sectors.
			const auto *types = source2d::GeneratorOriginal(c, "types");
			const bool wide = types && KuwaharaWideMode(*types);
			const long double side =
				2.L * (wide ? radius : std::min(radius, double(KUWAHARA_NATIVE_RAD))) + 1;
			const long double fixed = types && KuwaharaWideMode(*types, true) ? 32768.L : 8192.L;
			const long double work =
				(fixed + 1024.L * side * side) * pixels * rows + (64.L + 128.L * feather) * mask * rows;
			if (!std::isfinite(radius) || !std::isfinite(feather) || work > KUWAHARA_WORK_LIMIT)
				return c.Fail(
					Status::LimitExceeded, "Kuwahara whole-array work exceeds native CPU limit", "radius"
				);
			constexpr uint64_t metadata = sizeof(std::pair<std::string, Image>) +
										  sizeof(std::pair<std::string, ImageArray>) +
										  sizeof(ImageArrayItem) + sizeof(ElementValue) + 256;
			// Worst-case output depth is RGBA32 for every row; tensor stages are RGBA8.
			const uint64_t bytes = rows * (pixels * 16 + metadata) + pixels * 64 + mask * 32 +
								   uint64_t(std::max(1., feather)) * sizeof(double);
			return bool(c.ReserveWorkspace(bytes, "surface_in"));
		}
		bool KuwaharaFinite(NodeContext &c, double n, std::string_view port) {
			return std::isfinite(n) || c.Fail(
										   Status::UnsupportedExecution,
										   "Kuwahara source arithmetic is not finite in the native profile",
										   port
									   );
		}
		bool KuwaharaSettingsRead(NodeContext &c, KuwaharaSettings &s) {
			s.Sampling = ReadSampler(c);
			s.Uv = ReadUvMap(c);
			s.Type = c.Integer("types", 0);
			if (s.Type < 0 || s.Type > 2)
				return c.Fail(Status::InvalidValue, "Kuwahara Types is unknown", "types");
			if (SourceRangeMapped(c, "radius")) {
				if (!ReadSourceMappedRange(c, "radius", s.Radius)) return false;
				s.RadiusMap = c.Input("radius_map");
			} else {
				const double radius = c.Scalar("radius", 2);
				s.Radius = {radius, radius};
			}
			const auto domain = c.InputDomain("radius");
			if (!domain || domain->Kind != SourceSocketKind::Surface) {
				// __NodeValue_Int validates each numeric endpoint before half-even rounding.
				s.Radius.X = source2d::GeneratorRoundHalfEven(std::max(1., s.Radius.X));
				s.Radius.Y = source2d::GeneratorRoundHalfEven(std::max(1., s.Radius.Y));
			}
			if (s.Type != 0) {
				s.Crossing = c.Scalar("zero_crossing", .58);
				s.Hardness = c.Scalar("hardness", 8);
				s.Sharpness = c.Scalar("sharpness", 8);
				if (!KuwaharaFinite(c, s.Crossing, "zero_crossing") ||
					!KuwaharaFinite(c, s.Hardness, "hardness") ||
					!KuwaharaFinite(c, s.Sharpness, "sharpness"))
					return false;
			}
			if (s.Type == 1) {
				s.Alpha = c.Scalar("alpha", 1);
			}
			return c.FailureCode == Status::Ok && KuwaharaFinite(c, s.Alpha, "alpha") &&
				   KuwaharaFinite(c, s.Radius.X, "radius") && KuwaharaFinite(c, s.Radius.Y, "radius") &&
				   KuwaharaFinite(c, s.Uv.Mix, "uv_mix");
		}
		Rgba KuwaharaSample(
			NodeContext &c, const Image &image, double u, double v, double amount, const KuwaharaSettings &s
		) {
			if (s.Uv.Map) {
				if (!std::isfinite(amount)) {
					c.Fail(
						Status::UnsupportedExecution,
						"Kuwahara source UV blend divides by a zero ellipse extent",
						"radius"
					);
					return {};
				}
				// shader_set_surface binds the UV sampler unfiltered, independently of the base texture.
				const auto map = Texture(*s.Uv.Map, u, v, false);
				const double mix = amount * s.Uv.Mix;
				u += (map[0] - u) * mix;
				v += ((1. - map[1]) - v) * mix;
			}
			if (!KuwaharaFinite(c, u, "uv_mix") || !KuwaharaFinite(c, v, "uv_mix")) return {};
			return SampleTextureSimple(image, u, v, s.Sampling.Oversample, Filtered(s.Sampling));
		}
		double KuwaharaRadius(const KuwaharaSettings &s, double u, double v) {
			if (!s.RadiusMap) return s.Radius.X;
			const auto p = Texture(*s.RadiusMap, u, v, false);
			return s.Radius.X + (s.Radius.Y - s.Radius.X) * ((p[0] + p[1] + p[2]) / 3.);
		}
		bool
		KuwaharaTensor(NodeContext &c, const Image &source, Image &out, int pass, const KuwaharaSettings &s) {
			for (uint32_t y = 0; y < source.Height; ++y)
				for (uint32_t x = 0; x < source.Width; ++x) {
					const double u = (x + .5) / source.Width, v = (y + .5) / source.Height;
					Rgba result{};
					if (pass == 0) {
						const auto tap = [&](int dx, int dy, double amount) {
							return KuwaharaSample(
								c,
								source,
								u + double(dx) / source.Width,
								v + double(dy) / source.Height,
								amount,
								s
							);
						};
						const auto a = tap(-1, -1, .25), b = tap(-1, 0, .5), d = tap(-1, 1, .25),
								   e = tap(1, -1, .25), f = tap(1, 0, .5), g = tap(1, 1, .25),
								   h = tap(0, -1, .5), i = tap(0, 1, .5);
						std::array<double, 3> sx{}, sy{};
						for (size_t k = 0; k < 3; ++k) {
							sx[k] = (a[k] + 2. * b[k] + d[k] + -1. * e[k] + -2. * f[k] + -1. * g[k]) / 4.;
							sy[k] = (a[k] + 2. * h[k] + e[k] + -1. * d[k] + -2. * i[k] + -1. * g[k]) / 4.;
							result[0] += sx[k] * sx[k];
							result[1] += sy[k] * sy[k];
							result[2] += sx[k] * sy[k];
						}
						result[3] = 1;
					} else {
						double total = 0;
						for (int offset = -5; offset <= 5; ++offset) {
							const auto p = KuwaharaSample(
								c,
								source,
								u + (pass == 1 ? double(offset) / source.Width : 0),
								v + (pass == 2 ? double(offset) / source.Height : 0),
								double(offset) / 5.,
								s
							);
							const double weight = (1. / std::sqrt(2. * std::numbers::pi * 2. * 2.)) *
												  std::exp(-(double(offset) * offset) / (2. * 2. * 2.));
							for (size_t k = 0; k < 4; ++k)
								result[k] += p[k] * weight;
							total += weight;
						}
						for (double &n : result)
							n /= total;
						if (pass == 2) {
							const double gx = result[0], gy = result[1], gz = result[2];
							const double delta = gy * gy - 2. * gx * gy + gx * gx + 4. * gz * gz;
							if (delta < 0)
								return c.Fail(
									Status::UnsupportedExecution,
									"Kuwahara tensor eigenvalue square root is undefined",
									"types"
								);
							const double root = std::sqrt(delta), l1 = .5 * (gy + gx + root),
										 l2 = .5 * (gy + gx - root);
							const double vx = l1 - gx, vy = -gz, length = std::sqrt(vx * vx + vy * vy);
							const double tx = length > 0 ? vx / length : 0, ty = length > 0 ? vy / length : 1;
							result = {tx, ty, -std::atan2(ty, tx), l1 + l2 > 0 ? (l1 - l2) / (l1 + l2) : 0};
						}
					}
					if (c.FailureCode != Status::Ok) return false;
					if (!WritePixel(out, x, y, result))
						return c.Fail(
							Status::UnsupportedExecution,
							"Kuwahara tensor stage exceeds native finite surface range",
							"types"
						);
				}
			return true;
		}
		struct KuwaharaSectors {
			std::array<Rgba, 8> Mean{};
			std::array<std::array<double, 3>, 8> Square{};
		};
		bool KuwaharaCell(
			NodeContext &c,
			KuwaharaSectors &accum,
			Rgba colour,
			double vx,
			double vy,
			double zeta,
			double eta,
			bool basic
		) {
			std::array<double, 8> w{};
			double sum = 0;
			const auto four = [&](double x, double y, int parity) {
				const double xx = zeta - eta * x * x, yy = zeta - eta * y * y;
				const std::array<double, 4> z{
					std::max(0., y + xx), std::max(0., -x + yy), std::max(0., -y + xx), std::max(0., x + yy)
				};
				for (size_t j = 0; j < 4; ++j) {
					w[j * 2 + parity] = z[j] * z[j];
					sum += w[j * 2 + parity];
				}
			};
			four(vx, vy, 0);
			const double scale = std::sqrt(2.) / 2., rx = scale * (vx - vy), ry = scale * (vx + vy);
			four(rx, ry, 1);
			const double gaussian = std::exp(-3.125 * (rx * rx + ry * ry)) / (sum + (basic ? .0001 : 0));
			if (!KuwaharaFinite(c, gaussian, "zero_crossing")) return false;
			for (size_t k = 0; k < 8; ++k) {
				const double weight = w[k] * gaussian;
				for (size_t j = 0; j < 3; ++j) {
					const double n = std::clamp(colour[j], 0., 1.);
					accum.Mean[k][j] += n * weight;
					accum.Square[k][j] += n * n * weight;
				}
				accum.Mean[k][3] += weight;
			}
			return true;
		}
		bool KuwaharaFilter(
			NodeContext &c, const Image &source, Image &out, const Image *tensor, const KuwaharaSettings &s
		) {
			for (uint32_t y = 0; y < source.Height; ++y)
				for (uint32_t x = 0; x < source.Width; ++x) {
					const double u = (x + .5) / source.Width, v = (y + .5) / source.Height;
					const double rad = KuwaharaRadius(s, u, v);
					if (!(rad > 0) || !KuwaharaFinite(c, rad, "radius"))
						return c.Fail(
							Status::UnsupportedExecution,
							"Kuwahara source radius must be positive for normalization",
							"radius"
						);
					double zeta = 2. / rad, eta = 0, cosPhi = 1, sinPhi = 0, a = 1, b = 1;
					double extentX = std::min(double(KUWAHARA_NATIVE_RAD), std::trunc(rad)),
						   extentY = extentX;
					double startX = -extentX, startY = -extentY;
					if (s.Type != 0) {
						const double kernel = rad / 2.;
						zeta = 2. / kernel;
						const double sine = std::sin(s.Crossing);
						const double denominator = s.Type == 1 ? sine * sine : sine * s.Crossing;
						if (denominator == 0)
							return c.Fail(
								Status::UnsupportedExecution,
								"Kuwahara source zero-crossing denominator is zero",
								"zero_crossing"
							);
						eta = (zeta + std::cos(s.Crossing)) / denominator;
						if (!KuwaharaFinite(c, eta, "zero_crossing")) return false;
						if (s.Type == 1) {
							if (s.Alpha == 0)
								return c.Fail(
									Status::UnsupportedExecution,
									"Kuwahara anisotropic source divides by zero Alpha",
									"alpha"
								);
							const auto t = Texture(*tensor, u, v, false);
							if (s.Alpha + t[3] == 0)
								return c.Fail(
									Status::UnsupportedExecution,
									"Kuwahara source Alpha plus anisotropy is zero",
									"alpha"
								);
							a = kernel * std::clamp((s.Alpha + t[3]) / s.Alpha, .1, 2.);
							b = kernel * std::clamp(s.Alpha / (s.Alpha + t[3]), .1, 2.);
							cosPhi = std::cos(t[2]);
							sinPhi = std::sin(t[2]);
							extentX =
								std::trunc(std::sqrt(a * a * cosPhi * cosPhi + b * b * sinPhi * sinPhi));
							extentY =
								std::trunc(std::sqrt(a * a * sinPhi * sinPhi + b * b * cosPhi * cosPhi));
							startX = -extentX;
							startY = -extentY;
						} else {
							// Preserve GLSL floating loop origins, not an integer neighbourhood centred at
							// zero.
							extentX = std::max(s.Radius.X, s.Radius.Y);
							extentY = extentX;
							startX = -extentX;
							startY = -extentY;
						}
					}
					KuwaharaSectors accum;
					for (double dy = startY; dy <= extentY; dy += 1.) {
						if (s.Type == 2 && dy < -rad) continue;
						if (s.Type == 2 && dy > rad) break;
						for (double dx = startX; dx <= extentX; dx += 1.) {
							if (s.Type == 2 && dx < -rad) continue;
							if (s.Type == 2 && dx > rad) break;
							double vx = dx / rad, vy = dy / rad;
							double amount = std::sqrt(vx * vx + vy * vy);
							if (s.Type == 2) {
								vx = dx / (rad / 2.);
								vy = dy / (rad / 2.);
								amount = std::sqrt(vx * vx + vy * vy);
							}
							if (s.Type == 1) {
								vx = .5 / a * (cosPhi * dx + sinPhi * dy);
								vy = .5 / b * (-sinPhi * dx + cosPhi * dy);
								if (vx * vx + vy * vy > .25) continue;
								const double ax = dx / extentX, ay = dy / extentY;
								amount = std::sqrt(ax * ax + ay * ay);
							}
							const auto colour = KuwaharaSample(
								c, source, u + dx / source.Width, v + dy / source.Height, amount, s
							);
							if (c.FailureCode != Status::Ok ||
								!KuwaharaCell(c, accum, colour, vx, vy, zeta, eta, s.Type == 0))
								return false;
						}
					}
					Rgba result{};
					for (size_t k = 0; k < 8; ++k) {
						auto mean = accum.Mean[k];
						if (!(mean[3] > 0))
							return c.Fail(
								Status::UnsupportedExecution,
								"Kuwahara source sector has zero accumulated weight",
								"radius"
							);
						double variance = 0;
						for (size_t j = 0; j < 3; ++j) {
							mean[j] /= mean[3];
							variance += std::abs(accum.Square[k][j] / mean[3] - mean[j] * mean[j]);
						}
						const double base = (s.Type == 0 ? 1 : s.Hardness) * 1000. * variance,
									 exponent = .5 * (s.Type == 0 ? 18 : s.Sharpness);
						if (base < 0 || (base == 0 && exponent <= 0))
							return c.Fail(
								Status::UnsupportedExecution,
								"Kuwahara source variance power is undefined",
								base < 0 ? "hardness" : "sharpness"
							);
						const double weight = 1. / (1. + std::pow(base, exponent));
						for (size_t j = 0; j < 3; ++j)
							result[j] += mean[j] * weight;
						result[3] += weight;
					}
					if (!(result[3] > 0))
						return c.Fail(
							Status::UnsupportedExecution,
							"Kuwahara source final sector denominator is zero",
							"hardness"
						);
					for (double &n : result)
						n = std::clamp(n / result[3], 0., 1.);
					if (!WritePixel(out, x, y, result))
						return c.Fail(
							Status::UnsupportedExecution,
							"Kuwahara final arithmetic exceeds native finite surface range",
							"radius"
						);
				}
			return true;
		}
	}
	bool SourceKuwahara(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.kuwahara");
		if (!KuwaharaAdmission(c)) return false;
		bool failed = false;
		if (CopyWhenInactive(c, failed)) return !failed;
		const auto *source = c.Input("surface_in");
		if (!source) return c.Fail(Status::InvalidValue, "Kuwahara needs Surface In", "surface_in");
		KuwaharaSettings settings;
		if (!KuwaharaSettingsRead(c, settings)) return false;
		const auto format = ResolveProcessorSurfaceFormat(c, source);
		if (!format) return false;
		auto output = MakeSurfaceScratch(c, source->Width, source->Height, *format, "surface_out");
		if (!output) return false;
		const bool red = source->Format == SurfaceFormat::R8Unorm ||
						 source->Format == SurfaceFormat::R16Float ||
						 source->Format == SurfaceFormat::R32Float;
		std::array<std::optional<SurfaceScratch>, 3> stages;
		if (settings.Type == 1) {
			for (size_t i = 0; i < 3; ++i) {
				stages[i] = MakeSurfaceScratch(
					c, source->Width, source->Height, SurfaceFormat::RGBA8Unorm, "surface_out"
				);
				if (!stages[i]) return false;
				const auto &input = i ? stages[i - 1]->Data : *source;
				if (i == 0 && red) {
					for (uint32_t y = 0; y < source->Height; ++y)
						for (uint32_t x = 0; x < source->Width; ++x)
							if (!WritePixel(stages[i]->Data, x, y, SourceSafeDrawPixel(*source, x, y)))
								return false;
				} else if (!KuwaharaTensor(c, input, stages[i]->Data, int(i), settings))
					return false;
			}
		}
		if (red) {
			for (uint32_t y = 0; y < source->Height; ++y)
				for (uint32_t x = 0; x < source->Width; ++x)
					if (!WritePixel(output->Data, x, y, SourceSafeDrawPixel(*source, x, y))) return false;
		} else if (!KuwaharaFilter(
					   c, *source, output->Data, stages[2] ? &stages[2]->Data : nullptr, settings
				   ))
			return false;
		FinishProcessor(c, *source, output->Data);
		if (c.FailureCode != Status::Ok) return false;
		auto *published = c.NewImage("surface_out", source->Width, source->Height, *format);
		if (!published) return false;
		published->Pixels.swap(output->Data.Pixels);
		return true;
	}
}
