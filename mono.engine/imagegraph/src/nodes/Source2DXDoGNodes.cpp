#include "../SourceMappedInputs.hpp"
#include "../SourceSafeDraw.hpp"
#include "Blur.hpp"
#include "Source2DReferenceUnits.hpp"

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t XDOG_WORK_LIMIT = 64000000;
		bool XDoGSurfaceRadius(const NodeContext &c) {
			const auto domain = c.InputDomain("radius");
			return c.Input("radius") || (domain && domain->Kind == SourceSocketKind::Surface);
		}
		uint64_t XDoGPixels(const NodeContext &c, std::string_view port) {
			const auto *image = c.Input(port);
			uint64_t result = image ? uint64_t(image->Width) * image->Height : 0;
			for (const auto &[id, images] : c.ImageArrays)
				if (id == port && images)
					for (const auto &frame : images->Images)
						result = std::max(result, uint64_t(frame.Width) * frame.Height);
			return result;
		}
		template <class V> double XDoGMagnitude(const V &value) {
			if (const auto *n = std::get_if<double>(&value)) return std::abs(*n);
			if (const auto *n = std::get_if<int64_t>(&value)) return std::abs(double(*n));
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *array = std::get_if<ArrayValue>(&value)) {
					double result = 0;
					for (const auto &v : array->Elements)
						result = std::max(result, XDoGMagnitude(v));
					for (const auto &row : array->Nested)
						for (const auto &v : row)
							result = std::max(result, XDoGMagnitude(v));
					source2d::GeneratorVisitArrayItems(array->Items, 0, [&](const ElementValue &v) {
						result = std::max(result, XDoGMagnitude(v));
					});
					return result;
				}
			}
			return 0;
		}
		Vector2 XDoGReference(const NodeContext &c, const Image &source) {
			for (const auto &[id, images] : c.ImageArrays)
				if (id == "surface_in" && images)
					if (const auto *first = source2d::FirstReferenceImage(*images))
						return {double(first->Width), double(first->Height)};
			return {double(source.Width), double(source.Height)};
		}
		bool XDoGAdmission(NodeContext &c) {
			if (c.ProcessorRow) return true;
			const auto *source = c.Input("surface_in");
			if (!source) return c.Fail(Status::InvalidValue, "XDoG needs Surface In", "surface_in");
			const auto maximum = [&](std::string_view port, double fallback) {
				const auto *v = source2d::GeneratorOriginal(c, port);
				return v ? XDoGMagnitude(*v) : fallback;
			};
			double radius = maximum("radius", .25);
			// Reference is resolved against the first prepared source, before processor row selection.
			if (!XDoGSurfaceRadius(c) && c.Integer("radius_unit", 1) == 1) {
				const auto reference = XDoGReference(c, *source);
				radius *= reference.X;
			}
			const double wide = std::max(radius, radius * maximum("k", 8));
			const uint64_t rows = std::max<size_t>(1, c.ProcessorCount), pixels = XDoGPixels(c, "surface_in");
			// Four Gaussian passes, each two filtered reads per tap, plus map and finishing arithmetic.
			const long double work = (2048.L + std::max(1., wide) * 512.L) * pixels * rows;
			const uint64_t mask = XDoGPixels(c, "mask");
			const double feather = mask ? maximum("mask_feather", 0) : 0;
			const long double maskWork = mask * rows * (64.L + std::ceil(feather) * 128.L);
			if (!std::isfinite(wide) || !std::isfinite(feather) || work + maskWork > XDOG_WORK_LIMIT)
				return c.Fail(
					Status::LimitExceeded, "XDoG whole-array work exceeds native CPU limit", "radius"
				);
			constexpr uint64_t metadata = sizeof(std::pair<std::string, Image>) + sizeof(ImageArrayItem) +
										  sizeof(ElementValue) + sizeof(std::pair<std::string, ImageArray>) +
										  256;
			const uint64_t bytes = rows * (pixels * 32 + 2 * metadata) + pixels * 64 + mask * 32 +
								   uint64_t(std::max(1., std::ceil(feather))) * sizeof(double) +
								   2048 * sizeof(double);
			return bool(c.ReserveWorkspace(bytes, "surface_in"));
		}
		bool XDoGRange(NodeContext &c, std::string_view port, double fallback, Vector2 &out) {
			if (SourceRangeMapped(c, port)) return ReadSourceMappedRange(c, port, out);
			const double n = c.Scalar(port, fallback);
			out = {n, n};
			return c.FailureCode == Status::Ok;
		}
		double XDoGMapped(const NodeContext &c, std::string_view port, Vector2 range, double u, double v) {
			const Image *map = SourceRangeMapped(c, port) ? c.Input(std::string(port) + "_map") : nullptr;
			if (!map) return range.X;
			const auto sample = SampleNearest(*map, u, v);
			return range.X + (range.Y - range.X) * (sample[0] + sample[1] + sample[2]) / 3.;
		}
		std::optional<SurfaceScratch> XDoGStage(NodeContext &c, const Image &source, double radius) {
			const bool red = source.Format == SurfaceFormat::R8Unorm ||
							 source.Format == SurfaceFormat::R16Float ||
							 source.Format == SurfaceFormat::R32Float;
			std::optional<SurfaceScratch> blurred;
			if (!red) {
				GaussianArgs args;
				args.Size = args.SizeHigh = radius;
				args.SampleMode = c.Integer("oversample", 0);
				blurred = GaussianBlur(c, source, args);
				if (!blurred) return std::nullopt;
			}
			// node_xdog_threshold's staging surface_verify omits format, so this copy is RGBA8.
			auto staging =
				MakeSurfaceScratch(c, source.Width, source.Height, SurfaceFormat::RGBA8Unorm, "do_g");
			if (!staging) return std::nullopt;
			const auto &input = red ? source : blurred->Data;
			for (uint32_t y = 0; y < source.Height; ++y)
				for (uint32_t x = 0; x < source.Width; ++x) {
					// This is plain draw_surface, not draw_surface_safe. Red stays red-only here.
					if (!WritePixel(staging->Data, x, y, ReadPixel(input, x, y))) {
						c.Fail(Status::InvalidValue, "XDoG staging exceeds numeric surface range", "do_g");
						return std::nullopt;
					}
				}
			return staging;
		}
	}
	bool SourceXDoG(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.xdog");
		if (!XDoGAdmission(c)) return false;
		bool failed = false;
		if (CopyWhenInactive(c, failed)) {
			if (failed) return false;
			return c.SetOutputDiagnostic(
				"do_g",
				Status::UnsupportedExecution,
				"Inactive source XDoG retains its previous DoG resource; no prior output observation is "
				"available"
			);
		}
		const Image *source = c.Input("surface_in");
		if (!source) return c.Fail(Status::InvalidValue, "XDoG needs Surface In", "surface_in");
		double radius = c.Scalar("radius", .25);
		if (!XDoGSurfaceRadius(c) && c.Integer("radius_unit", 1) == 1) {
			const auto ref = XDoGReference(c, *source);
			radius *= ref.X;
		}
		const double wide = radius * c.Scalar("k", 8);
		if (!std::isfinite(radius) || !std::isfinite(wide))
			return c.Fail(Status::UnsupportedExecution, "XDoG Gaussian size is nonfinite", "radius");
		Vector2 gamma, epsilon, smoothness;
		if (!XDoGRange(c, "gamma", 1, gamma) || !XDoGRange(c, "epsilon", .1, epsilon) ||
			!XDoGRange(c, "smoothness", .1, smoothness))
			return false;
		auto g1 = XDoGStage(c, *source, radius);
		if (!g1) return false;
		auto g2 = XDoGStage(c, *source, wide);
		if (!g2) return false;
		const auto format = ResolveProcessorSurfaceFormat(c, source);
		if (!format) return false;
		auto dogImage = MakeSurfaceScratch(c, source->Width, source->Height, *format, "do_g");
		auto thresholdImage = MakeSurfaceScratch(c, source->Width, source->Height, *format, "surface_out");
		if (!dogImage || !thresholdImage) return false;
		Image *difference = &dogImage->Data, *output = &thresholdImage->Data;
		bool thresholdDefined = true;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				const double u = (x + .5) / source->Width, v = (y + .5) / source->Height;
				const double gam = XDoGMapped(c, "gamma", gamma, u, v);
				const auto a = ReadPixel(g1->Data, x, y), b = ReadPixel(g2->Data, x, y);
				Rgba dog{a[0] - b[0] * gam, a[1] - b[1] * gam, a[2] - b[2] * gam, 1};
				if (c.Boolean("edge"))
					for (size_t k = 0; k < 3; ++k)
						dog[k] = std::abs(dog[k]);
				if (!WritePixel(*difference, x, y, dog))
					return c.Fail(
						Status::InvalidValue, "XDoG difference exceeds numeric surface range", "gamma"
					);
				const auto stored = ReadPixel(*difference, x, y);
				const double s = (stored[0] + stored[1] + stored[2]) / 3. * stored[3],
							 eps = XDoGMapped(c, "epsilon", epsilon, u, v),
							 smt = XDoGMapped(c, "smoothness", smoothness, u, v),
							 lower = std::max(0., eps - smt), upper = std::min(eps + smt, 1.);
				if (!(lower < upper)) {
					thresholdDefined = false;
					continue;
				}
				const double t = std::clamp((s - lower) / (upper - lower), 0., 1.),
							 colour = t * t * (3 - 2 * t);
				if (!WritePixel(*output, x, y, {colour, colour, colour, 1}))
					return c.Fail(
						Status::InvalidValue, "XDoG threshold exceeds numeric surface range", "surface_out"
					);
			}
		Image *publishedDog = c.NewImage("do_g", source->Width, source->Height, *format);
		if (!publishedDog) return false;
		publishedDog->Pixels.swap(difference->Pixels);
		if (!thresholdDefined)
			return c.SetOutputDiagnostic(
				"surface_out",
				Status::UnsupportedExecution,
				"XDoG source smoothstep edges are not strictly increasing"
			);
		FinishProcessor(c, *source, *output);
		if (c.FailureCode != Status::Ok) return false;
		Image *publishedThreshold = c.NewImage("surface_out", source->Width, source->Height, *format);
		if (!publishedThreshold) return false;
		publishedThreshold->Pixels.swap(output->Pixels);
		return true;
	}
}
