#include "../SourceMappedInputs.hpp"
#include "../SourceSafeDraw.hpp"
#include "Source2DGenerator.hpp"
#include "SourceRefractClean.hpp"

namespace engine::imagegraph::detail {
	namespace {
		// Admit original rows before inactive copies; CleanEdge includes a 21-read slice cascade.
		constexpr uint64_t REFRACT_WORK_LIMIT = 64000000, REFRACT_PIXEL_WORK = 4096;
		uint64_t RefractPixels(const NodeContext &c, std::string_view port) {
			const auto *image = c.Input(port);
			uint64_t largest = image ? uint64_t(image->Width) * image->Height : 0;
			for (const auto &[id, images] : c.ImageArrays)
				if (id == port && images)
					for (const auto &frame : images->Images)
						largest = std::max(largest, uint64_t(frame.Width) * frame.Height);
			return largest;
		}
		template <class V> double RefractPositive(const V &value) {
			if (const auto *n = std::get_if<double>(&value)) return std::max(0., *n);
			if (const auto *n = std::get_if<int64_t>(&value)) return std::max(0., double(*n));
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *a = std::get_if<ArrayValue>(&value)) {
					double highest = 0;
					for (const auto &item : a->Elements)
						highest = std::max(highest, RefractPositive(item));
					for (const auto &row : a->Nested)
						for (const auto &item : row)
							highest = std::max(highest, RefractPositive(item));
					const auto visit =
						[&](auto &&self, const std::vector<SourceArrayItem> &items, size_t depth) -> double {
						if (depth > Limits::MaximumArrayDepth) return std::numeric_limits<double>::infinity();
						double out = 0;
						for (const auto &item : items)
							if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
								out = std::max(out, RefractPositive(*leaf));
							else if (const auto *children =
										 std::get_if<std::vector<SourceArrayItem>>(&item.Data))
								out = std::max(out, self(self, *children, depth + 1));
						return out;
					};
					return std::max(highest, visit(visit, a->Items, 0));
				}
			}
			return 0;
		}
		bool RefractAdmission(NodeContext &c) {
			if (c.ProcessorRow) return true;
			const uint64_t rows = std::max<size_t>(1, c.ProcessorCount),
						   pixels = RefractPixels(c, "surface_in"), mask = RefractPixels(c, "mask");
			if (rows > REFRACT_WORK_LIMIT / REFRACT_PIXEL_WORK ||
				pixels > REFRACT_WORK_LIMIT / REFRACT_PIXEL_WORK / rows)
				return c.Fail(
					Status::LimitExceeded, "Refract whole-array work exceeds native CPU limit", "surface_in"
				);
			double feather = 0;
			if (mask)
				if (const auto *v = source2d::GeneratorOriginal(c, "mask_feather"))
					feather = RefractPositive(*v);
			const double radius = feather > 0 ? std::max(1., std::round(feather)) : 0;
			if (!std::isfinite(radius) || radius > double(REFRACT_WORK_LIMIT / 128))
				return c.Fail(
					Status::LimitExceeded,
					"Refract whole-array feather work exceeds native CPU limit",
					"mask_feather"
				);
			const uint64_t maskWork = uint64_t(radius) * 128 + 32,
						   remaining = REFRACT_WORK_LIMIT - pixels * rows * REFRACT_PIXEL_WORK;
			if (mask && (rows > remaining / maskWork || mask > remaining / maskWork / rows))
				return c.Fail(
					Status::LimitExceeded,
					"Refract whole-array feather work exceeds native CPU limit",
					"mask_feather"
				);
			constexpr uint64_t metadata = sizeof(std::pair<std::string, Image>) +
										  sizeof(std::pair<std::string, ImageArray>) +
										  sizeof(ImageArrayItem) + sizeof(ElementValue) + 256;
			const uint64_t bytes =
				rows * (pixels * 16 + metadata) + mask * 32 + uint64_t(radius) * sizeof(double);
			auto admitted = c.ReserveWorkspace(bytes, "surface_in");
			return bool(admitted);
		}
		bool RefractRange(NodeContext &c, std::string_view port, double fallback, Vector2 &range) {
			if (SourceRangeMapped(c, port)) return ReadSourceMappedRange(c, port, range);
			const double n = c.Scalar(port, fallback);
			range = {n, n};
			return c.FailureCode == Status::Ok;
		}
		bool RefractFinite(NodeContext &c, double n, std::string_view port) {
			return std::isfinite(n) ||
				   c.Fail(
					   Status::UnsupportedExecution, "Refract arithmetic exceeds native finite range", port
				   );
		}
		bool RefractUnit(NodeContext &c, Vector3 p, Vector3 &unit, std::string_view port) {
			const double length = std::sqrt(p.X * p.X + p.Y * p.Y + p.Z * p.Z);
			if (length == 0 || !std::isfinite(length))
				return c.Fail(
					Status::UnsupportedExecution, "Refract source normalization is undefined", port
				);
			unit = {p.X / length, p.Y / length, p.Z / length};
			return true;
		}
	}
	bool SourceRefract(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.refract");
		if (!RefractAdmission(c)) return false;
		bool inactiveFailed = false;
		if (CopyWhenInactive(c, inactiveFailed)) return !inactiveFailed;
		const Image *source = c.Input("surface_in");
		if (!source) return c.Fail(Status::InvalidValue, "Refract needs its input surface", "surface_in");
		const bool single = DescribeSurfaceFormat(source->Format)->Channels == 1;
		// draw_surface_safe installs the source red-channel shader instead of sh_refract.
		if (single)
			return RunPixelProcessor(c, [](const Image &surface, uint32_t x, uint32_t y, double, double) {
				return SourceSafeDrawPixel(surface, x, y);
			});
		const auto *normal = c.Input("normal_map"), *depth = c.Input("depth_map");
		if (!normal)
			return c.Fail(
				Status::UnsupportedExecution,
				"Refract missing normal map retains unobserved sampler binding",
				"normal_map"
			);
		if (!depth)
			return c.Fail(
				Status::UnsupportedExecution,
				"Refract missing depth map retains unobserved sampler binding",
				"depth_map"
			);
		const auto sampler = ReadSampler(c);

		Vector2 height, distance, ior;
		if (!RefractRange(c, "height", 4, height) || !RefractRange(c, "distance", 4, distance) ||
			!RefractRange(c, "ior", 1.3, ior))
			return false;
		const double perspective = c.Scalar("perspective", 0);
		const UvMap uv = ReadUvMap(c);
		if (c.FailureCode != Status::Ok) return false;
		const auto mapValue = [&](std::string_view port, Vector2 range, double u, double v) {
			if (SourceRangeMapped(c, port))
				if (const auto *map = c.Input(std::string(port) + "_map")) {
					const auto p = SampleNearest(*map, u, v);
					return range.X + (range.Y - range.X) * (p[0] + p[1] + p[2]) / 3;
				}
			return range.X;
		};
		const auto shade = [&](const Image &surface, double u, double v) {
			double dep = mapValue("height", height, u, v), ofs = distance.X, eta = mapValue("ior", ior, u, v);
			// The pinned distance-map branch writes dep, leaving ofs at the first distance endpoint.
			if (SourceRangeMapped(c, "distance") && c.Input("distance_map"))
				dep = mapValue("distance", distance, u, v);
			double mapU = u, mapV = v;
			UvRemap(uv, mapU, mapV, 1, false);
			if (!RefractFinite(c, mapU, "uv_mix") || !RefractFinite(c, mapV, "uv_mix")) return Rgba{};
			const auto n = SampleNearest(*normal, mapU, mapV), z = SampleNearest(*depth, mapU, mapV);
			const double dist = ofs + (z[0] * .2126 + z[1] * .7152 + z[2] * .0722) * z[3] * dep;
			Vector3 norm, incident;
			if (!RefractUnit(c, {n[0] * 2 - 1, n[1] * 2 - 1, n[2]}, norm, "normal_map") ||
				!RefractUnit(
					c, {(u - .5) * perspective, (v - .5) * perspective, -1}, incident, "perspective"
				))
				return Rgba{};
			const double dot = norm.X * incident.X + norm.Y * incident.Y + norm.Z * incident.Z;
			const double k = 1 - eta * eta * (1 - dot * dot);
			if (!RefractFinite(c, k, "ior") || !RefractFinite(c, dist, "distance")) return Rgba{};
			Vector2 shift{};
			if (k >= 0) {
				const double factor = eta * dot + std::sqrt(k);
				shift = {eta * incident.X - factor * norm.X, eta * incident.Y - factor * norm.Y};
			}
			double sampleU = u + shift.X * dist, sampleV = v + shift.Y * dist;
			// The two-argument source sampleTexture overload uses mapBlend zero.
			// UV changes normal/depth lookups, but does not remap this final sample.
			if (!RefractFinite(c, sampleU, "distance") || !RefractFinite(c, sampleV, "distance"))
				return Rgba{};
			return source_refract_clean::Sample(surface, sampleU, sampleV, sampler);
		};
		const auto format = ResolveProcessorSurfaceFormat(c, source);
		if (!format) return false;
		auto *output = c.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				const auto pixel = shade(*source, (x + .5) / output->Width, (y + .5) / output->Height);
				if (c.FailureCode != Status::Ok) return false;
				if (!WritePixel(*output, x, y, pixel))
					return c.Fail(
						Status::InvalidValue, "Refract sample exceeds numeric surface range", "surface_out"
					);
			}
		FinishProcessor(c, *source, *output);
		return c.FailureCode == Status::Ok;
	}
}
