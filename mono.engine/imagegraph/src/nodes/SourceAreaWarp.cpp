#include "../AtlasPayload.hpp"
#include "Families.hpp"
#include "SourceRefractClean.hpp"

namespace engine::imagegraph::detail {
	namespace {
		template <class V> uint64_t AreaWarpPixels(const V &value) {
			if (const auto *surface = std::get_if<SurfaceValue>(&value))
				return uint64_t(surface->Data.Width) * surface->Data.Height;
			if (const auto *atlas = std::get_if<AtlasValue>(&value); atlas && atlas->Data)
				return uint64_t(atlas->Data->Surface.Data.Width) * atlas->Data->Surface.Data.Height;
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *array = std::get_if<ArrayValue>(&value)) {
					uint64_t largest = 0;
					for (const auto &item : array->Elements)
						largest = std::max(largest, AreaWarpPixels(item));
					for (const auto &row : array->Nested)
						for (const auto &item : row)
							largest = std::max(largest, AreaWarpPixels(item));
					const auto visit =
						[&](auto &&self, const std::vector<SourceArrayItem> &items, size_t depth) -> void {
						if (depth > Limits::MaximumArrayDepth) {
							largest = UINT64_MAX;
							return;
						}
						for (const auto &item : items) {
							if (const auto *image = std::get_if<Image>(&item.Data))
								largest = std::max(largest, uint64_t(image->Width) * image->Height);
							else if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
								largest = std::max(largest, AreaWarpPixels(*leaf));
							else
								self(self, std::get<std::vector<SourceArrayItem>>(item.Data), depth + 1);
						}
					};
					visit(visit, array->Items, 0);
					return largest;
				}
			}
			return 0;
		}
		template <class V> uint64_t AreaWarpSampleWork(const V &value, int64_t inherited) {
			double mode = 6;
			if (const auto *v = std::get_if<EnumValue>(&value)) mode = double(v->Value);
			if (const auto *v = std::get_if<int64_t>(&value)) mode = double(*v);
			if (const auto *v = std::get_if<double>(&value)) mode = *v;
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *array = std::get_if<ArrayValue>(&value)) {
					uint64_t work = 64;
					for (const auto &item : array->Elements)
						work = std::max(work, AreaWarpSampleWork(item, inherited));
					for (const auto &row : array->Nested)
						for (const auto &item : row)
							work = std::max(work, AreaWarpSampleWork(item, inherited));
					if (!array->Items.empty()) return 4096;
					return work;
				}
			}
			if (mode == 0) mode = double(inherited);
			return mode == 6 || !std::isfinite(mode) ? 4096 : mode == 4 ? 256 : 64;
		}

	}
	bool SourceAreaWarp(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.source.area_warp");
		const auto *input = context.Find("surface_in");
		const auto *atlas = input ? std::get_if<AtlasValue>(input) : nullptr;
		if (atlas && (!atlas->Data || !ValidAtlasPayload(*atlas)))
			return context.Fail(Status::InvalidValue, "Area Warp Atlas payload is invalid", "surface_in");
		if (atlas && atlas->Data->Kind != AtlasKind::SurfaceAtlas)
			return context.Fail(
				Status::UnsupportedExecution,
				"Area Warp requires SurfaceAtlas rather than base Atlas",
				"surface_in"
			);
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Area Warp requires Surface In", "surface_in");
		// grug admit every original row before copying or drawing; CleanEdge has 21 reads and three
		// slices.
		if (context.ProcessorRow == 0) {
			uint64_t pixels = uint64_t(source->Width) * source->Height;
			for (const auto &[port, array] : context.ImageArrays)
				if (port == "surface_in" && array)
					for (const auto &image : array->Images)
						pixels = std::max(pixels, uint64_t(image.Width) * image.Height);
			for (const auto &[port, value] : context.ProcessorOriginalValues)
				if (port == "surface_in" && value) pixels = std::max(pixels, AreaWarpPixels(*value));
			uint64_t work = ReadSampler(context).Interpolation == 6	  ? 4096
							: ReadSampler(context).Interpolation == 4 ? 256
																	  : 64;
			for (const auto &[port, value] : context.ProcessorOriginalValues)
				if (port == "interpolate" && value)
					work = std::max(work, AreaWarpSampleWork(*value, context.InheritedInterpolation));
			const uint64_t rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
			if (rows > 64000000 / work || pixels > 64000000 / work / rows)
				return context.Fail(
					Status::LimitExceeded, "Area Warp complete batch exceeds work limit", "surface_in"
				);
		}
		// grug inactive surface_clone unwraps SurfaceAtlas before plain drawing.
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const auto *areaValue = context.Find("area");
		const auto *typedArea = areaValue ? std::get_if<Area>(areaValue) : nullptr;
		if (!typedArea)
			return context.Fail(
				Status::UnsupportedExecution, "Area Warp requires a typed Area getter", "area"
			);
		Area area = *typedArea;
		const int64_t unit = context.Integer("area_unit", 1);
		if (unit < 0 || unit > 1 || area.Mode > 2)
			return context.Fail(Status::InvalidValue, "Area Warp mode or unit is invalid", "area");
		const auto *origin = context.InputProducer("area");
		if (context.IsLinked("area") && area.Mode && origin && origin->FromPort.ends_with(".bypass"))
			return context.Fail(
				Status::UnsupportedExecution, "Area bypass requires originating surface getter", "area"
			);
		if (!context.IsLinked("area")) {
			if (area.Mode == 1) {
				const double w = unit ? 1 : source->Width, h = unit ? 1 : source->Height;
				area = {
					(w - area.CenterX + area.HalfWidth) / 2,
					(area.CenterY + h - area.HalfHeight) / 2,
					std::abs(w - area.CenterX - area.HalfWidth) / 2,
					std::abs(area.CenterY - h + area.HalfHeight) / 2,
					area.Shape,
					area.Mode
				};
			} else if (area.Mode == 2) {
				area = {
					(area.CenterX + area.HalfWidth) / 2,
					(area.CenterY + area.HalfHeight) / 2,
					std::abs(area.CenterX - area.HalfWidth) / 2,
					std::abs(area.CenterY - area.HalfHeight) / 2,
					area.Shape,
					area.Mode
				};
			}
		}
		if (unit) {
			area.CenterX *= source->Width;
			area.HalfWidth *= source->Width;
			area.CenterY *= source->Height;
			area.HalfHeight *= source->Height;
		}
		double left = area.CenterX - area.HalfWidth, top = area.CenterY - area.HalfHeight,
			   width = area.HalfWidth * 2, height = area.HalfHeight * 2;
		double cosine = 1, sine = 0;
		if (atlas) {
			// grug source SurfaceAtlas.draw takes only the Area offset, then uses its own transform.
			const auto &draw = *atlas->Data;
			left += draw.Position.X;
			top += draw.Position.Y;
			width = source->Width * draw.Scale.X;
			height = source->Height * draw.Scale.Y;
			const double angle = draw.RotationDegrees * std::numbers::pi / 180;
			cosine = std::cos(angle);
			sine = std::sin(angle);
		}
		if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(width) || !std::isfinite(height) ||
			!std::isfinite(left + width) || !std::isfinite(top + height))
			return context.Fail(Status::InvalidValue, "Area Warp rectangle is nonfinite", "area");
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		const auto sampler = ReadSampler(context);
		const bool red = source->Format == SurfaceFormat::R8Unorm ||
						 source->Format == SurfaceFormat::R16Float ||
						 source->Format == SurfaceFormat::R32Float;
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				if (!width || !height) continue;
				const double dx = double(x) + .5 - left, dy = double(y) + .5 - top;
				const double localX = cosine * dx - sine * dy, localY = sine * dx + cosine * dy;
				if (localX < std::min(0., width) || localX >= std::max(0., width) ||
					localY < std::min(0., height) || localY >= std::max(0., height))
					continue;
				const double u = localX / width, v = localY / height;
				if (!std::isfinite(u) || !std::isfinite(v))
					return context.Fail(
						Status::InvalidValue, "Area Warp sampling coordinate is undefined", "area"
					);
				auto colour = red ? Texture(*source, u, v, Filtered(sampler))
								  : source_refract_clean::Sample(*source, u, v, sampler);
				if (red) colour = {colour[0], colour[0], colour[0], 1};
				if (atlas) {
					colour[0] *= atlas->Data->Blend.Red / 255.;
					colour[1] *= atlas->Data->Blend.Green / 255.;
					colour[2] *= atlas->Data->Blend.Blue / 255.;
					colour[3] *= atlas->Data->Alpha;
				}
				if (!WritePixel(*output, x, y, colour))
					return context.Fail(
						Status::InvalidValue, "Area Warp sample exceeds surface range", "surface_out"
					);
			}
		return true;
	}
	std::span<const ExecutorEntry> SourceAreaWarpExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.wrap_area", SourceAreaWarp, true}};
		return entries;
	}
}
