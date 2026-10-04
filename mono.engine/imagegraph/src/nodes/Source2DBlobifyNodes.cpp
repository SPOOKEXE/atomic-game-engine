#include "../SourceMappedInputs.hpp"
#include "../SourceSafeDraw.hpp"
#include "../SurfaceScratch.hpp"
#include "Sampler.hpp"
#include "Source2DGenerator.hpp"
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t BLOB_WORK_LIMIT = 64000000;
		constexpr double BLOB_TAU = 6.28318530718;
		template <class V> double BlobMaximum(const V &v) {
			if (const auto *n = std::get_if<double>(&v)) return std::max(0., *n);
			if (const auto *n = std::get_if<int64_t>(&v)) return std::max(0., double(*n));
			if (const auto *n = std::get_if<Vector2>(&v)) return std::max({0., n->X, n->Y});
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *a = std::get_if<ArrayValue>(&v)) {
					double maximum = 0;
					for (const auto &n : a->Elements)
						maximum = std::max(maximum, BlobMaximum(n));
					for (const auto &row : a->Nested)
						for (const auto &n : row)
							maximum = std::max(maximum, BlobMaximum(n));
					source2d::GeneratorVisitArrayItems(a->Items, 0, [&](const ElementValue &n) {
						maximum = std::max(maximum, BlobMaximum(n));
					});
					return maximum;
				}
			}
			return 0;
		}
		uint64_t BlobPixels(const NodeContext &c, std::string_view port) {
			const auto *image = c.Input(port);
			uint64_t largest = image ? uint64_t(image->Width) * image->Height : 0;
			for (const auto &[id, frames] : c.ImageArrays)
				if (id == port && frames)
					for (const auto &frame : frames->Images)
						largest = std::max(largest, uint64_t(frame.Width) * frame.Height);
			return largest;
		}

		template <class V> bool BlobShape(const V &v, bool circle) {
			if (const auto *n = std::get_if<EnumValue>(&v)) return circle ? n->Value == 0 : n->Value != 0;
			if (const auto *n = std::get_if<int64_t>(&v)) return circle ? *n == 0 : *n != 0;
			if (const auto *n = std::get_if<double>(&v)) return circle ? *n == 0 : *n != 0;
			if constexpr (std::is_same_v<V, Value>)
				if (const auto *a = std::get_if<ArrayValue>(&v)) {
					for (const auto &n : a->Elements)
						if (BlobShape(n, circle)) return true;
					for (const auto &row : a->Nested)
						for (const auto &n : row)
							if (BlobShape(n, circle)) return true;
					bool found = false;
					source2d::GeneratorVisitArrayItems(a->Items, 0, [&](const ElementValue &n) {
						found = found || BlobShape(n, circle);
					});
					return found;
				}
			return false;
		}
		bool BlobAdmission(NodeContext &c) {
			if (c.ProcessorRow) return true;
			const auto maximum = [&](std::string_view port, double fallback) {
				const auto *v = source2d::GeneratorOriginal(c, port);
				return v ? BlobMaximum(*v) : fallback;
			};

			const bool mapped = SourceRangeMapped(c, "radius");
			const bool staticRange =
				mapped && !c.IsLinked("radius") && c.IsCatalogueDefault("radius").value_or(false);
			const double radius =
				std::max(0., std::ceil(maximum(staticRange ? "radius_map_range" : "radius", 3)));
			if (c.FailureCode != Status::Ok) return false;
			const uint64_t rows = std::max<size_t>(1, c.ProcessorCount), pixels = BlobPixels(c, "surface_in"),
						   mask = BlobPixels(c, "mask");
			const double feather = mask ? std::ceil(maximum("mask_feather", 0)) : 0;

			// Circle has up to65 angular visits per ring; polygon source loops use square candidate bounds.
			const auto *shapes = source2d::GeneratorOriginal(c, "shape");
			const bool circle = !shapes || BlobShape(*shapes, true),
					   polygon = shapes && BlobShape(*shapes, false);
			const long double samples = std::max(
				circle ? 65.L * std::ceil(radius) : 0.L,
				polygon ? (2.L * radius + 1) * (2.L * radius + 1) : 0.L
			);
			// Simple shader sampling: nearest reads1 texel, filtering reads4.128 includes texel64+cell64;
			// 384 includes four texels256, bilinear64 and cell64. No extended Lanczos branch is executed.
			// Interpolate and mapped toggles are synthetic attributes excluded by AddRows;
			// they cannot change between processor rows. Refuse malformed array readers before admission.
			const uint64_t cell = Filtered(ReadSampler(c)) ? 384 : 128;
			if (c.FailureCode != Status::Ok) return false;
			const long double work =
				(512.L + cell * samples) * pixels * rows + (64.L + 128.L * feather) * mask * rows;
			if (!std::isfinite(radius) || !std::isfinite(feather) || work > BLOB_WORK_LIMIT)
				return c.Fail(
					Status::LimitExceeded, "Blobify whole-array work exceeds native CPU limit", "radius"
				);
			constexpr uint64_t metadata = sizeof(std::pair<std::string, Image>) +
										  sizeof(std::pair<std::string, ImageArray>) +
										  sizeof(ImageArrayItem) + sizeof(ElementValue) + 256;
			return bool(c.ReserveWorkspace(
				rows * (pixels * 16 + metadata) + pixels * 16 + mask * 32 +
					uint64_t(std::max(1., feather)) * sizeof(double),
				"surface_in"
			));
		}
		bool BlobUndefined(NodeContext &c, std::string_view port) {
			return c.Fail(
				Status::UnsupportedExecution, "Blobify source arithmetic is undefined in native profile", port
			);
		}
		bool BlobFilter(NodeContext &c, const Image &source, Image &out) {
			Vector2 range{};
			if (SourceRangeMapped(c, "radius")) {
				if (!ReadSourceMappedRange(c, "radius", range)) return false;
			} else {
				const double n = c.Scalar("radius", 3);
				range = {n, n};
			}
			const auto origin = c.InputDomain("radius");
			if (!origin || origin->Kind != SourceSocketKind::Surface) {
				range.X = source2d::GeneratorRoundHalfEven(std::max(0., range.X));
				range.Y = source2d::GeneratorRoundHalfEven(std::max(0., range.Y));
			}
			const double maximum = std::max(range.X, range.Y);
			const int64_t shape = c.Integer("shape", 0);
			const bool fade = c.Boolean("distance"), keep = c.Boolean("keep_alpha"),
					   invert = c.Boolean("inverted");
			const double threshold = c.Scalar("threshold", .5), smooth = c.Scalar("smoothness", 0);
			const auto sampling = ReadSampler(c);
			const auto *map = SourceRangeMapped(c, "radius") ? c.Input("radius_map") : nullptr;
			if (c.FailureCode != Status::Ok) return false;
			if (shape < 0 || shape > 2)
				return c.Fail(Status::InvalidValue, "Blobify Shape is unknown", "shape");
			if (!std::isfinite(maximum)) return BlobUndefined(c, "radius");
			const bool red = source.Format == SurfaceFormat::R8Unorm ||
							 source.Format == SurfaceFormat::R16Float ||
							 source.Format == SurfaceFormat::R32Float;
			for (uint32_t y = 0; y < source.Height; ++y)
				for (uint32_t x = 0; x < source.Width; ++x) {
					if (red) {
						if (!WritePixel(out, x, y, SourceSafeDrawPixel(source, x, y))) return false;
						continue;
					}
					if (smooth < 0) return BlobUndefined(c, "smoothness");
					const double u = (x + .5) / source.Width, v = (y + .5) / source.Height;
					double radius = range.X;
					if (map) {
						const auto n = Texture(*map, u, v, false);
						radius = range.X + (range.Y - range.X) * ((n[0] + n[1] + n[2]) / 3.);
					}
					Rgba base = SampleTextureSimple(source, u, v, sampling.Oversample, Filtered(sampling));
					if (invert)
						for (double &n : base)
							n = 1. - n;
					Rgba total{};
					double weight = 0;
					const auto accumulate = [&](double dx, double dy, double amount) {
						auto colour = SampleTextureSimple(
							source,
							u + dx / source.Width,
							v + dy / source.Height,
							sampling.Oversample,
							Filtered(sampling)
						);
						if (invert)
							for (double &n : colour)
								n = 1. - n;
						for (size_t channel = 0; channel < 4; ++channel)
							total[channel] += colour[channel] * amount;
						weight += amount;
					};
					if (shape == 0) {
						for (double i = 0; i < maximum; i += 1.) {
							if (i > radius) break;
							for (double j = 0; j < BLOB_TAU; j += BLOB_TAU / 64.)
								accumulate(std::cos(j) * i, std::sin(j) * i, fade ? 1. - i / radius : 1.);
						}
					} else
						for (double i = -maximum; i <= maximum; i += 1.)
							for (double j = -maximum; j <= maximum; j += 1.) {
								if (shape == 1 && (i < -radius || j < -radius || i > radius || j > radius))
									continue;
								if (shape == 2 && std::abs(i) + std::abs(j) > radius) continue;
								const double distance = shape == 1 ? std::max(std::abs(i), std::abs(j))
																   : std::abs(i) + std::abs(j);
								accumulate(i, j, fade ? 1. - distance / radius : 1.);
							}
					if (weight == 0 || !std::isfinite(weight)) return BlobUndefined(c, "radius");
					for (double &n : total)
						n /= weight;
					const double bright = (total[0] + total[1] + total[2]) / 3. * total[3];
					double amount = bright < threshold ? 0. : 1.;
					if (smooth != 0) {
						const double lower = threshold - smooth / 2., upper = threshold + smooth / 2.;
						if (!(upper > lower)) return BlobUndefined(c, "smoothness");
						const double t = std::clamp((bright - lower) / (upper - lower), 0., 1.);
						amount = t * t * (3. - 2. * t);
					}
					Rgba result = base;
					for (double &n : result)
						n *= amount;
					if (keep) result[3] = base[3];
					if (invert)
						for (double &n : result)
							n = 1. - n;
					for (double n : result)
						if (!std::isfinite(n)) return BlobUndefined(c, "radius");
					if (!WritePixel(out, x, y, result)) return BlobUndefined(c, "radius");
				}
			return true;
		}
	}
	bool SourceBlobify(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.blobify");
		if (!BlobAdmission(c)) return false;
		bool failed = false;
		if (CopyWhenInactive(c, failed)) return !failed;
		const auto *source = c.Input("surface_in");
		if (!source) return c.Fail(Status::InvalidValue, "Blobify needs Surface In", "surface_in");
		const auto format = ResolveProcessorSurfaceFormat(c, source);
		if (!format) return false;
		auto scratch = MakeSurfaceScratch(c, source->Width, source->Height, *format, "surface_out");
		if (!scratch) return false;
		if (!BlobFilter(c, *source, scratch->Data)) return false;
		FinishProcessor(c, *source, scratch->Data);
		if (c.FailureCode != Status::Ok) return false;
		auto *output = c.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		output->Pixels.swap(scratch->Data.Pixels);
		return true;
	}
}
