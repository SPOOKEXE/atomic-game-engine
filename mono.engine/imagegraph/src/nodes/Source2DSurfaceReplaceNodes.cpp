#include "../SourceSafeDraw.hpp"
#include "Processor.hpp"

namespace engine::imagegraph::detail {
	namespace {
		struct ReplaceImages {
			const Image *Single = nullptr;
			const ImageArray *Array = nullptr;
			size_t Count() const {
				return Array ? (Array->Items.empty() ? Array->Images.size() : Array->Items.size())
							 : size_t(Single != nullptr);
			}
			const Image &At(size_t i) const {
				return Array
						   ? Array->Images[Array->Items.empty() ? i : std::get<size_t>(Array->Items[i].Data)]
						   : *Single;
			}
		};
		ReplaceImages ReplaceList(const NodeContext &c, std::string_view port) {
			for (const auto &[id, images] : c.ImageArrays)
				if (id == port && images) return {nullptr, images};
			return {c.Input(port), nullptr};
		}
		bool ReplaceSingleRed(const Image &image) {
			return image.Format == SurfaceFormat::R8Unorm || image.Format == SurfaceFormat::R16Float ||
				   image.Format == SurfaceFormat::R32Float;
		}
		uint64_t ReplaceLargest(const ReplaceImages &list) {
			uint64_t n = 0;
			for (size_t i = 0; i < list.Count(); ++i)
				n = std::max(n, uint64_t(list.At(i).Width) * list.At(i).Height);
			return n;
		}
		bool ReplaceOriginalUniformArray(const NodeContext &c, std::string_view port) {
			const Value *value = c.Find(port);
			for (auto i = c.ProcessorOriginalValues.rbegin(); i != c.ProcessorOriginalValues.rend(); ++i)
				if (i->first == port) {
					value = i->second;
					break;
				}
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (!array) return false;
			if (!array->Nested.empty() || array->Elements.size() > 1 || array->Items.size() > 1) return true;
			return !array->Items.empty() && !std::holds_alternative<ElementValue>(array->Items[0].Data);
		}
		bool ReplaceMayUseFull(const NodeContext &c) {
			const Value *v = c.Find("fast_mode");
			for (const auto &[port, value] : c.ProcessorOriginalValues)
				if (port == "fast_mode") v = value;
			if (!v) return false;
			const auto leaf = [](const auto &value) {
				if (const auto *b = std::get_if<bool>(&value)) return !*b;
				if (const auto *n = std::get_if<double>(&value)) return *n == 0;
				if (const auto *n = std::get_if<int64_t>(&value)) return *n == 0;
				return true;
			};
			const auto *a = std::get_if<ArrayValue>(v);
			if (!a) return leaf(*v);
			for (const auto &item : a->Elements)
				if (leaf(item)) return true;
			for (const auto &row : a->Nested)
				for (const auto &item : row)
					if (leaf(item)) return true;
			const auto visit =
				[&](auto &&self, const std::vector<SourceArrayItem> &items, size_t depth) -> bool {
				if (depth > Limits::MaximumArrayDepth) return true;
				for (const auto &item : items) {
					if (const auto *value = std::get_if<ElementValue>(&item.Data)) {
						if (leaf(*value)) return true;
					} else if (const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
						if (self(self, *row, depth + 1)) return true;
					}
				}
				return false;
			};
			return visit(visit, a->Items, 0);
		}
		bool ReplaceValidList(NodeContext &c, const ReplaceImages &list, std::string_view port) {
			if (!list.Array) return true;
			for (const auto &item : list.Array->Items) {
				const auto *index = std::get_if<size_t>(&item.Data);
				if (!index || *index >= list.Array->Images.size())
					return c.Fail(
						Status::UnsupportedExecution,
						"Surface Replace requires a flat source surface list",
						port
					);
			}
			return true;
		}
		bool ReplaceAdmitBatch(NodeContext &c) {
			if (c.ProcessorRow) return true;
			const uint64_t pixels = ReplaceLargest(ReplaceList(c, "base_image"));
			const auto targets = ReplaceList(c, "target_image"),
					   replacements = ReplaceList(c, "replacement_image");
			const uint64_t targetsCount = targets.Count(), replacementsCount = replacements.Count();
			const uint64_t targetPixels = ReplaceLargest(targets),
						   replacementPixels = ReplaceLargest(replacements);
			// Both source find branches are bounded before any scratch or output is created.
			// Full matching searches target-sized origins, then target-sized template pixels at each origin.
			constexpr uint64_t limit = 64000000;
			uint64_t cost = 32;
			const auto add = [&](uint64_t count, uint64_t taps, uint64_t perTap) {
				if (count > limit / perTap || taps > limit / (std::max(uint64_t{1}, count) * perTap))
					return false;
				const uint64_t value = count * taps * perTap;
				if (value > limit - cost) return false;
				cost += value;
				return true;
			};
			if (targetPixels > limit || !add(targetsCount, targetPixels, 12) ||
				(ReplaceMayUseFull(c) && targetPixels &&
				 !add(targetsCount, targetPixels * targetPixels, 12)) ||
				!add(std::max(targetsCount, replacementsCount), replacementPixels, 16))
				return c.Fail(
					Status::LimitExceeded,
					"Surface Replace complete processor batch exceeds work budget",
					"target_image"
				);
			const uint64_t rows = std::max(uint64_t{1}, uint64_t(c.ProcessorCount));
			if (rows > limit / cost || pixels > limit / (rows * cost))
				return c.Fail(
					Status::LimitExceeded,
					"Surface Replace complete processor batch exceeds work budget",
					"base_image"
				);
			// Reserve a conservative whole-output shape plus the largest simultaneous three surfaces.
			// This lease is released before the real per-row reservations so payloads are charged once.
			auto admittedBytes = c.ReserveWorkspace(pixels * 4 * (rows + 3), "base_image");
			return admittedBytes.has_value();
		}
		double ReplaceDistance(const Rgba &a, const Rgba &b) {
			double d = 0;
			for (size_t k = 0; k < 4; ++k)
				d += (a[k] - b[k]) * (a[k] - b[k]);
			return std::sqrt(d);
		}
		Rgba ReplaceTexturePixel(const Image &image, double x, double y) {
			return SampleNearest(image, x / image.Width, y / image.Height);
		}
		double ReplaceIndex(double index, bool random, double seed, double u, double v, size_t count) {
			if (!random) return index;
			const double shift = (seed - std::floor(seed / 100000.) * 100000.) / 10.;
			const double raw = std::sin((u + shift) * 12.9898 + (v + shift) * 78.233) * 43758.5453123;
			const double value = (raw - std::floor(raw)) * double(count - 1),
						 fraction = value - std::floor(value);
			return (fraction > .5 ? std::ceil(value) : std::floor(value)) / count;
		}
		Rgba ReplaceNormal(const Rgba &src, const Rgba &dst) {
			Rgba out{};
			for (size_t k = 0; k < 4; ++k)
				out[k] = src[k] * src[3] + dst[k] * (1 - src[3]);
			return out;
		}
		Rgba ReplaceQuantized(Rgba value) {
			for (auto &channel : value)
				channel = Quantize(channel) / 255.;
			return value;
		}
		Rgba ReplaceSourceAlpha(const Rgba &src, const Rgba &dst) {
			Rgba out{};
			for (size_t k = 0; k < 3; ++k)
				out[k] = src[k] + dst[k] * (1 - src[3]);
			out[3] = src[3] + dst[3];
			return out;
		}
	}
	bool SourceSurfaceReplace(NodeContext &c) {
		for (const auto port : {"base_image", "target_image", "replacement_image"})
			if (!ReplaceValidList(c, ReplaceList(c, port), port)) return false;
		if (!ReplaceAdmitBatch(c)) return false;
		const Image *base = c.Input("base_image");
		if (!base) return c.Fail(Status::InvalidValue, "Surface Replace requires Base Image", "base_image");
		const auto targets = ReplaceList(c, "target_image"),
				   replacements = ReplaceList(c, "replacement_image");
		if (!replacements.Count())
			return c.Fail(
				Status::UnsupportedExecution,
				"Surface Replace empty replacement array has undefined modulo",
				"replacement_image"
			);
		if (ReplaceSingleRed(*base))
			return c.Fail(
				Status::UnsupportedExecution,
				"Surface Replace safe single-channel draw leaves the second MRT output unobserved",
				"base_image"
			);
		if (ReplaceOriginalUniformArray(c, "array_mode"))
			return c.Fail(
				Status::UnsupportedExecution,
				"Surface Replace original mode array uploads an unobserved scalar uniform",
				"array_mode"
			);
		const int64_t mode = c.Integer("array_mode");
		if (mode < 0 || mode > 1)
			return c.Fail(Status::InvalidValue, "Surface Replace array mode is invalid", "array_mode");
		if (mode == 1 && ReplaceOriginalUniformArray(c, "seed"))
			return c.Fail(
				Status::UnsupportedExecution,
				"Surface Replace original seed array uploads an unobserved scalar uniform",
				"seed"
			);
		if (mode == 1 && !c.Find("seed"))
			return c.Fail(
				Status::InvalidValue, "Surface Replace randomized mode requires resolved seed", "seed"
			);
		const bool fast = c.Boolean("fast_mode", true);
		const double threshold = c.Scalar("color_threshold", .1),
					 pixelThreshold = c.Scalar("pixel_threshold", .1),
					 seed = mode == 1 ? c.Scalar("seed") : 0;
		const uint64_t bytes = uint64_t(base->Width) * base->Height * 4;
		auto storage = c.ReserveWorkspace(bytes * 3, "target_image");
		if (!storage) return false;
		std::array<Image, 3> scratch;
		uint64_t actual = 0;
		for (auto &image : scratch) {
			image.Width = base->Width;
			image.Height = base->Height;
			image.Pixels.resize(size_t(bytes));
			actual += image.Pixels.capacity();
		}
		auto extra = c.ReserveWorkspace(actual - bytes * 3, "target_image");
		if (!extra) return false;
		for (size_t t = 0; t < targets.Count(); ++t) {
			const auto &target = targets.At(t);
			for (uint32_t y = 0; y < base->Height; ++y)
				for (uint32_t x = 0; x < base->Width; ++x) {
					const double px = x + .5, py = y + .5;
					Rgba found{};
					if (fast) {
						if (ReadPixel(*base, x, y)[3] != 0) {
							double matches = 0;
							bool done = false;
							for (uint32_t i = 0; i < target.Width && !done; ++i)
								for (uint32_t j = 0; j < target.Height; ++j) {
									const auto targ = ReadPixel(target, i, j);
									if (targ[3] == 0) continue;
									if (ReplaceDistance(ReplaceTexturePixel(*base, px + i, py + j), targ) <=
										2 * threshold) {
										++matches;
										if (matches >=
											double(target.Width) * target.Height * (1 - pixelThreshold)) {
											found = {
												1,
												ReplaceIndex(
													double(t) / targets.Count(),
													mode == 1,
													seed,
													px / base->Width,
													py / base->Height,
													replacements.Count()
												),
												0,
												1
											};
											done = true;
											break;
										}
									}
								}
						}
					} else {
						double best = 0, bu = 0, bv = 0, ou = 0, ov = 0;
						for (uint32_t i = 0; i < target.Width; ++i)
							for (uint32_t j = 0; j < target.Height; ++j) {
								const double ax = px - i, ay = py - j;
								if (ax < 0 || ay < 0 || ax - .5 + target.Width > base->Width ||
									ay - .5 + target.Height > base->Height)
									continue;
								double matches = 0;
								for (uint32_t a = 0; a < target.Width; ++a)
									for (uint32_t b = 0; b < target.Height; ++b)
										if (ReplaceDistance(
												ReplaceTexturePixel(*base, ax + a, ay + b),
												ReadPixel(target, a, b)
											) <= 2 * threshold)
											++matches;
								const double ratio = matches / (double(target.Width) * target.Height);
								if (ratio > best) {
									best = ratio;
									bu = (i + .5) / target.Width;
									bv = (j + .5) / target.Height;
									ou = ax / base->Width;
									ov = ay / base->Height;
								}
							}
						if (best >= 1 - pixelThreshold)
							found = {
								bu,
								bv,
								ReplaceIndex(
									double(t) / targets.Count(), mode == 1, seed, ou, ov, replacements.Count()
								),
								1
							};
					}
					if (!std::all_of(found.begin(), found.end(), [](double value) {
							return std::isfinite(value);
						}))
						return c.Fail(
							Status::UnsupportedExecution,
							"Surface Replace randomized shader index is nonfinite",
							"seed"
						);
					auto dest = ReadPixel(scratch[0], x, y);
					for (size_t k = 0; k < 4; ++k)
						dest[k] += found[k] * found[3];
					if (!WritePixel(scratch[0], x, y, dest))
						return c.Fail(
							Status::InvalidValue,
							"Surface Replace find output exceeds numeric range",
							"surface_out"
						);
				}
		}
		const size_t passes = std::max(targets.Count(), replacements.Count());
		for (size_t pass = 0; pass < passes; ++pass) {
			const size_t ri = pass % replacements.Count();
			const auto &rep = replacements.At(ri);
			const double index = double(ri) / passes;
			for (uint32_t y = 0; y < base->Height; ++y)
				for (uint32_t x = 0; x < base->Width; ++x) {
					Rgba fragment{}, erase{};
					bool written = false;
					if (fast) {
						const double px = x + .5 - (rep.Width - 1.), py = y + .5 - (rep.Height - 1.);
						Rgba combined{};
						for (uint32_t i = 0; i < rep.Width; ++i)
							for (uint32_t j = 0; j < rep.Height; ++j) {
								const double ax = px + i, ay = py + j;
								if (ax < 0 || ay < 0) continue;
								const auto weight = ReplaceTexturePixel(scratch[0], ax, ay);
								if (weight[0] != 1 || std::abs(weight[1] - index) >= .01) continue;
								const auto colour = ReadPixel(rep, rep.Width - i - 1, rep.Height - j - 1);
								if (colour[3] <= 0) continue;
								const double alpha = colour[3] + combined[3] * (1 - colour[3]);
								for (size_t k = 0; k < 3; ++k)
									combined[k] = (combined[k] * combined[3] * (1 - colour[3]) +
												   colour[k] * colour[3]) /
												  alpha;
								combined[3] = alpha;
								fragment = combined;
								erase = {1, 1, 1, combined[3]};
								written = true;
							}
					} else {
						const auto result = ReadPixel(scratch[0], x, y);
						if (result[3] == 1 && std::abs(result[2] - index) < .01) {
							fragment = SampleNearest(rep, result[0], result[1]);
							if (fragment[3] > 0) {
								erase = {1, 1, 1, 1};
								written = true;
							}
						}
					}
					if (!written)
						return c.Fail(
							Status::UnsupportedExecution,
							"Surface Replace fragment leaves source MRT outputs unwritten",
							"replacement_image"
						);
					if (!WritePixel(scratch[1], x, y, ReplaceNormal(fragment, ReadPixel(scratch[1], x, y))) ||
						!WritePixel(scratch[2], x, y, ReplaceNormal(erase, ReadPixel(scratch[2], x, y))))
						return c.Fail(
							Status::InvalidValue,
							"Surface Replace MRT output exceeds numeric range",
							"surface_out"
						);
				}
		}
		auto *out = c.NewImage("surface_out", base->Width, base->Height);
		if (!out) return false;
		const bool draw = c.Boolean("draw_base_image", true), empty = c.Boolean("replace_empty");
		for (uint32_t y = 0; y < base->Height; ++y)
			for (uint32_t x = 0; x < base->Width; ++x) {
				Rgba result = draw ? ReplaceQuantized(SourceSafeDrawPixel(*base, x, y)) : Rgba{};
				if (empty) {
					const auto erase = ReadPixel(scratch[2], x, y);
					for (size_t k = 0; k < 4; ++k)
						result[k] *= 1 - erase[k];
					result = ReplaceQuantized(result);
				}
				result = ReplaceSourceAlpha(ReadPixel(scratch[1], x, y), result);
				if (!WritePixel(*out, x, y, result))
					return c.Fail(
						Status::InvalidValue,
						"Surface Replace final output exceeds numeric range",
						"surface_out"
					);
			}
		return c.FailureCode == Status::Ok;
	}
}
