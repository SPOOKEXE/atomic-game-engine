#include "../PixelBoxMath.hpp"
#include "../SourceSafeDraw.hpp"
#include "PixelBuilderEffects.hpp"
#include "PixelBuilderPrimitives.hpp"
#include "Processor.hpp"

namespace engine::imagegraph::detail {
	namespace {
		Vector2 BuilderDimension(NodeContext &context) {
			if (context.InlineOwnerType == "pc.pixel_builder") {
				for (const auto &[port, value] : context.InlineOwnerValues)
					if (port == "dimension" && value) {
						if (const auto *dimension = std::get_if<Vector2>(value)) return *dimension;
					}
				context.Fail(Status::InvalidValue, "Pixel Builder dimension is unresolved", "dimension");
			}
			return {double(context.Project.SurfaceWidth), double(context.Project.SurfaceHeight)};
		}
		PixelBoxData ReadBox(NodeContext &context, std::string_view port, bool resetUnlinked = false) {
			PixelBoxData box;
			if (const Value *value = context.Find(port)) {
				if (const auto *payload = std::get_if<PixelBoxValue>(value)) {
					if (payload->Data) box = *payload->Data;
				} else
					context.Fail(Status::InvalidValue, "PBbox requires a typed Pixel Builder box", port);
			}
			if (resetUnlinked && !context.IsLinked(port)) {
				const Vector2 dimension = BuilderDimension(context);
				box.BaseBounds = {0, 0, dimension.X, dimension.Y};
			}
			return box;
		}
		bool PublishBox(NodeContext &context, std::string_view port, const PixelBoxData &data) {
			if (context.FailureCode != Status::Ok) return false;
			if (!context.ReserveOutput(sizeof(PixelBoxData), port)) return false;
			PixelBoxValue value;
			value.Data.emplace() = data;
			if (!ValidPayload(value, true))
				return context.Fail(Status::InvalidValue, "PBbox anchors are nonfinite", port);
			context.SetValue(port, std::move(value));
			return context.FailureCode == Status::Ok;
		}
	}
	bool PixelBuilderOutput(NodeContext &context) {
		const double layer = context.Scalar("layer", 1);
		if (!std::isfinite(layer))
			return context.Fail(Status::InvalidValue, "PB output layer must be finite", "layer");
		context.PixelBuilderUpdate = PixelBuilderLayer{
			context.Authored.Id,
			context.Input("surface"),
			layer,
			context.Integer("blend_mode"),
			context.Boolean("active", true)
		};
		return context.FailureCode == Status::Ok;
	}
	bool PixelBuilderDimension(NodeContext &context) {
		const Vector2 dimension = BuilderDimension(context);
		context.SetValue("dimension", dimension);
		context.SetValue("width", dimension.X);
		context.SetValue("height", dimension.Y);
		return context.FailureCode == Status::Ok;
	}
	bool PixelBoxCrop(NodeContext &context) {
		const Image *surface = context.Input("surface");
		if (!surface) return context.Fail(Status::InvalidValue, "Crop requires a surface", "surface");
		const auto bounds = PixelBoxBounds(ReadBox(context, "pbbox"));
		const double width = bounds[2] - bounds[0], height = bounds[3] - bounds[1];
		if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 ||
			width > Limits::MaximumDimension || height > Limits::MaximumDimension)
			return context.Fail(
				Status::InvalidValue, "PBbox crop dimensions must be positive and bounded", "pbbox"
			);
		for (double coordinate : bounds)
			if (!std::isfinite(coordinate) || coordinate != std::trunc(coordinate))
				return context.Fail(
					Status::UnsupportedExecution,
					"Fractional fixed PBbox raster coordinates are unresolved",
					"pbbox"
				);
		Image *output =
			context.NewImage("surface", uint32_t(width), uint32_t(height), SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				const double sourceX = x + bounds[0], sourceY = y + bounds[1];
				if (sourceX < 0 || sourceY < 0 || sourceX >= surface->Width || sourceY >= surface->Height)
					continue;
				if (!WritePixel(
						*output, x, y, SourceSafeDrawPixel(*surface, uint32_t(sourceX), uint32_t(sourceY))
					))
					return context.Fail(Status::InvalidValue, "PBbox crop sample is nonfinite", "surface");
			}
		return context.FailureCode == Status::Ok;
	}
	bool PixelBoxSurfaceMirror(NodeContext &context) {
		const Image *surface = context.Input("surface");
		if (!surface) return context.Fail(Status::InvalidValue, "Mirror requires a surface", "surface");
		const auto bounds = PixelBoxBounds(ReadBox(context, "pbbox", true));
		const double twiceX = bounds[0] + bounds[2], twiceY = bounds[1] + bounds[3];
		if (!std::isfinite(twiceX) || !std::isfinite(twiceY) || twiceX != std::trunc(twiceX) ||
			twiceY != std::trunc(twiceY))
			return context.Fail(
				Status::UnsupportedExecution,
				"Fractional fixed PBbox raster coordinates are unresolved",
				"pbbox"
			);
		Image *output =
			context.NewImage("surface_out", surface->Width, surface->Height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		const int64_t axes = context.Integer("axis");
		for (int layer = 0; layer < 4; ++layer) {
			if (layer != 0 && ((axes & layer) != layer)) continue;
			for (uint32_t y = 0; y < output->Height; ++y)
				for (uint32_t x = 0; x < output->Width; ++x) {
					const double sourceX = layer & 1 ? twiceX - 1 - x : x;
					const double sourceY = layer & 2 ? twiceY - 1 - y : y;
					if (sourceX < 0 || sourceY < 0 || sourceX >= surface->Width || sourceY >= surface->Height)
						continue;
					const Rgba source = SourceSafeDrawPixel(*surface, uint32_t(sourceX), uint32_t(sourceY));
					Rgba result = ReadPixel(*output, x, y);
					for (size_t channel = 0; channel < 3; ++channel)
						result[channel] = source[channel] + result[channel] * (1 - source[3]);
					result[3] += source[3];
					if (!WritePixel(*output, x, y, result))
						return context.Fail(
							Status::InvalidValue, "PB mirror sample is nonfinite", "surface_out"
						);
				}
		}
		return context.FailureCode == Status::Ok;
	}
	bool PixelBoxPolar(NodeContext &context) {
		const Image *surface = context.Input("surface");
		if (!surface) return context.Fail(Status::InvalidValue, "PB Polar requires a surface", "surface");
		const auto bounds = PixelBoxBounds(ReadBox(context, "pbbox", true));
		const Vector2 center{(bounds[0] + bounds[2]) / 2, (bounds[1] + bounds[3]) / 2};
		const int64_t copies = context.Integer("copies", 4);
		const uint64_t amount = uint64_t(std::max(int64_t{0}, copies));
		if (amount > 64000000 ||
			uint64_t(surface->Width) * surface->Height > 64000000 / std::max(uint64_t{1}, amount))
			return context.Fail(Status::LimitExceeded, "PB Polar copies exceed work budget", "copies");
		if (!std::isfinite(center.X) || !std::isfinite(center.Y))
			return context.Fail(Status::InvalidValue, "PB Polar center must be finite", "pbbox");
		Image *output =
			context.NewImage("surface_out", surface->Width, surface->Height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		for (uint64_t copy = 0; copy < amount; ++copy) {
			const double angle = copy * 2 * std::acos(-1.0) / amount;
			const double cosine = std::cos(angle), sine = std::sin(angle);
			for (uint32_t y = 0; y < output->Height; ++y)
				for (uint32_t x = 0; x < output->Width; ++x) {
					const double dx = x + .5 - center.X, dy = y + .5 - center.Y;
					const double sourceX = center.X + cosine * dx - sine * dy;
					const double sourceY = center.Y + sine * dx + cosine * dy;
					if (sourceX < 0 || sourceY < 0 || sourceX >= surface->Width || sourceY >= surface->Height)
						continue;
					const auto source = SourceSafeDrawPixel(*surface, uint32_t(sourceX), uint32_t(sourceY));
					auto result = ReadPixel(*output, x, y);
					for (size_t channel = 0; channel < 3; ++channel)
						result[channel] = source[channel] + result[channel] * (1 - source[3]);
					result[3] += source[3];
					if (!WritePixel(*output, x, y, result))
						return context.Fail(Status::InvalidValue, "PB Polar sample is nonfinite");
				}
		}
		return context.FailureCode == Status::Ok;
	}
	bool PixelDrawSurface(NodeContext &context) {
		const Image *surface = context.Input("surface");
		const DynamicSurfaceValue *dynamic = nullptr;
		if (!surface)
			if (const Value *value = context.Find("surface"))
				dynamic = std::get_if<DynamicSurfaceValue>(value);
		if (!surface && (!dynamic || !dynamic->Data))
			return context.Fail(Status::InvalidValue, "PB Draw Surface requires an owned surface", "surface");
		const Vector2 sourceDimension =
			surface ? Vector2{double(surface->Width), double(surface->Height)} : dynamic->Data->BaseDimension;
		auto box = ReadBox(context, "pbbox");
		const auto base = ReadBox(context, "base_pbbox", true);
		constexpr std::array<std::string_view, 4> overrides{
			"pbbox_left", "pbbox_top", "pbbox_right", "pbbox_bottom"
		};
		for (size_t index = 0; index < overrides.size(); ++index) {
			const auto port = overrides[index];
			const bool animated = std::find(
									  context.Authored.SourceAnimatedInputs.begin(),
									  context.Authored.SourceAnimatedInputs.end(),
									  port
								  ) != context.Authored.SourceAnimatedInputs.end();
			if (context.IsLinked(port) || animated) box.Anchors[index] = context.Scalar(port);
		}
		box.BaseBounds = PixelBoxBounds(base);
		box.Fractional[4] = false;
		box.Fractional[5] = false;
		box.Anchors[4] = sourceDimension.X;
		box.Anchors[5] = sourceDimension.Y;
		const auto bounds = PixelBoxBounds(box);
		for (double coordinate : bounds)
			if (!std::isfinite(coordinate) || coordinate != std::trunc(coordinate))
				return context.Fail(
					Status::UnsupportedExecution,
					"Fractional fixed PBbox raster coordinates are unresolved",
					"pbbox"
				);
		const Vector2 dimension = BuilderDimension(context);
		const auto sourceRound = [](double value) {
			const double low = std::floor(value), part = value - low;
			return low + (part > 0.5 || (part == 0.5 && std::fmod(low, 2) != 0));
		};
		const double width = std::max(1.0, sourceRound(dimension.X)),
					 height = std::max(1.0, sourceRound(dimension.Y));
		if (!std::isfinite(width) || !std::isfinite(height) || width > Limits::MaximumDimension ||
			height > Limits::MaximumDimension)
			return context.Fail(Status::LimitExceeded, "PB Draw Surface dimensions exceed bounded canvas");
		Image rendered;
		std::optional<AllocationReservation> renderingCharge;
		if (dynamic) {
			const uint64_t budget = context.AvailableBytes();
			renderingCharge = context.ReserveWorkspace(budget, "surface");
			if (!renderingCharge) return false;
			Diagnostic diagnostic;
			const Status status = RasterizePixelBuilder(
				*dynamic, sourceDimension, rendered, diagnostic, budget, context.Request.RigidProvider
			);
			if (status != Status::Ok) return context.Fail(status, diagnostic.Message, "surface");
			surface = &rendered;
			if (!renderingCharge->Resize(rendered.Pixels.capacity()))
				return context.Fail(
					Status::LimitExceeded, "Rendered PB surface exceeds its reserved bytes", "surface"
				);
		}
		Image *output =
			context.NewImage("surface", uint32_t(width), uint32_t(height), SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		const bool crop = context.Boolean("crop");
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				// The source scissor mistakenly passes absolute right/bottom as width/height.
				if (crop && (x < bounds[0] || y < bounds[1] || x >= bounds[0] + bounds[2] ||
							 y >= bounds[1] + bounds[3]))
					continue;
				const double sourceX = x - bounds[0], sourceY = y - bounds[1];
				if (sourceX < 0 || sourceY < 0 || sourceX >= surface->Width || sourceY >= surface->Height)
					continue;
				if (!WritePixel(
						*output, x, y, SourceSafeDrawPixel(*surface, uint32_t(sourceX), uint32_t(sourceY))
					))
					return context.Fail(Status::InvalidValue, "PB surface sample is nonfinite", "surface");
			}
		return context.FailureCode == Status::Ok;
	}
	bool PixelBox(NodeContext &context) {
		const auto base = ReadBox(context, "base_pbbox", true);
		auto box = ReadBox(context, "pbbox");
		constexpr std::array<std::string_view, 6> overrides{
			"pbbox_left", "pbbox_top", "pbbox_right", "pbbox_bottom", "pbbox_width", "pbbox_height"
		};
		for (size_t index = 0; index < overrides.size(); ++index) {
			const auto port = overrides[index];
			const bool animated = std::find(
									  context.Authored.SourceAnimatedInputs.begin(),
									  context.Authored.SourceAnimatedInputs.end(),
									  port
								  ) != context.Authored.SourceAnimatedInputs.end();
			if (context.IsLinked(port) || animated) box.Anchors[index] = context.Scalar(port);
		}
		box.BaseBounds = PixelBoxBounds(base);
		return PublishBox(context, "pbbox", box);
	}
	bool PixelDrawShape(NodeContext &context) {
		const Vector2 dimension = BuilderDimension(context);
		const auto sourceRound = [](double value) {
			const double low = std::floor(value), part = value - low;
			return low + (part > .5 || (part == .5 && std::fmod(low, 2) != 0));
		};
		const double width = std::max(1.0, sourceRound(dimension.X)),
					 height = std::max(1.0, sourceRound(dimension.Y));
		if (!std::isfinite(width) || !std::isfinite(height) || width > Limits::MaximumDimension ||
			height > Limits::MaximumDimension)
			return context.Fail(Status::LimitExceeded, "PB draw dimensions exceed bounded canvas");
		if (!PixelBox(context)) return false;
		const auto *published = std::get_if<PixelBoxValue>(&context.OutputValues.back().Data);
		if (!published || !published->Data)
			return context.Fail(Status::InvalidValue, "PB draw box is unresolved");
		const auto bounds = PixelBoxBounds(*published->Data);
		for (double coordinate : bounds)
			if (!std::isfinite(coordinate))
				return context.Fail(Status::InvalidValue, "PB draw bounds must be finite", "pbbox");
		auto effectCharge = context.ReserveWorkspace(64 * (sizeof(PixelBuilderEffect) + 64 * sizeof(double)));
		if (!effectCharge) return false;
		std::vector<PixelBuilderEffect> effects;
		if (!ReadPixelBuilderEffects(context, dimension, effects)) return false;
		auto shapeCharge = context.ReserveWorkspace(uint64_t(width) * uint64_t(height) * 4);
		if (!shapeCharge) return false;
		Image shape{
			uint32_t(width), uint32_t(height), std::vector<uint8_t>(size_t(width) * size_t(height) * 4), 0
		};
		if (context.Authored.Type != "pc.pb_draw_rectangle" &&
			context.Authored.Type != "pc.pb_draw_diamond") {
			if (!RasterPixelBuilderPrimitive(context, bounds, shape)) return false;
		} else {
			const bool diamond = context.Authored.Type == "pc.pb_draw_diamond";
			const int64_t corner = diamond ? context.Integer("corner") : 0;
			const double boxWidth = bounds[2] - bounds[0], boxHeight = bounds[3] - bounds[1];
			const double halfWidth = std::ceil(boxWidth / 2) - 1, halfHeight = std::ceil(boxHeight / 2) - 1;
			for (uint32_t y = 0; y < shape.Height; ++y)
				for (uint32_t x = 0; x < shape.Width; ++x) {
					if (boxWidth == 0 || boxHeight == 0) continue;
					const double pxCenter = x + .5, pyCenter = y + .5;
					const double left =
						diamond ? std::min(bounds[0], bounds[2]) : std::min(bounds[0], bounds[2] - 1);
					const double right =
						diamond ? std::max(bounds[0], bounds[2]) : std::max(bounds[0], bounds[2] - 1) + 1;
					const double top =
						diamond ? std::min(bounds[1], bounds[3]) : std::min(bounds[1], bounds[3] - 1);
					const double bottom =
						diamond ? std::max(bounds[1], bounds[3]) : std::max(bounds[1], bounds[3] - 1) + 1;
					if (pxCenter < left || pyCenter < top || pxCenter >= right || pyCenter >= bottom)
						continue;
					if (diamond) {
						double px = std::floor(x + .5 - bounds[0]), py = std::floor(y + .5 - bounds[1]);
						if (px > halfWidth) px = boxWidth - px - 1;
						if (py > halfHeight) py = boxHeight - py - 1;
						const bool filled = corner == 0 ? px / halfWidth + py / halfHeight >= 1
														: px + py >= std::min(halfWidth, halfHeight);
						if (px > halfWidth || py > halfHeight || !filled) continue;
					}
					WritePixel(shape, x, y, {1, 1, 1, 1});
				}
		}
		Image *output = context.NewImage("surface_out", shape.Width, shape.Height, SurfaceFormat::RGBA8Unorm);
		return output && ApplyPixelBuilderEffects(context, shape, bounds, effects, *output, dimension);
	}
	bool PixelBoxConvert(NodeContext &context) {
		const auto bounds = PixelBoxBounds(ReadBox(context, "pbbox"));
		const double width = bounds[2] - bounds[0], height = bounds[3] - bounds[1];
		context.SetValue("bbox", Vector4{bounds[0], bounds[1], bounds[2], bounds[3]});
		ArrayValue area;
		area.ElementType = ValueType::Scalar;
		for (const double value :
			 {(bounds[0] + bounds[2]) / 2, (bounds[1] + bounds[3]) / 2, width / 2, height / 2, 0.0})
			area.Elements.emplace_back(value);
		context.SetValue("area", std::move(area));
		context.SetValue("width", width);
		context.SetValue("height", height);
		context.SetValue("dimension", Vector2{width, height});
		return context.FailureCode == Status::Ok;
	}
	bool PixelBoxPoint(NodeContext &context) {
		const auto bounds = PixelBoxBounds(ReadBox(context, "pbbox", true));
		const Vector2 anchor = context.Vec2("anchor", {0.5, 0.5});
		Vector2 position = context.Vec2("position");
		if (!context.IsLinked("position") && context.Integer("position_unit", 1) == 1) {
			position.X *= bounds[2] - bounds[0];
			position.Y *= bounds[3] - bounds[1];
		}
		context.SetValue(
			"point",
			Vector2{
				bounds[0] + (bounds[2] - bounds[0]) * anchor.X + position.X,
				bounds[1] + (bounds[3] - bounds[1]) * anchor.Y + position.Y
			}
		);
		return context.FailureCode == Status::Ok;
	}
	bool PixelBoxMirror(NodeContext &context) {
		const auto baseBounds = PixelBoxBounds(ReadBox(context, "mirror_pbbox", true));
		auto box = ReadBox(context, "pbbox");
		auto bounds = PixelBoxBounds(box);
		const int64_t axes = context.Integer("axis");
		for (size_t axis = 0; axis < 2; ++axis)
			if (axes & (1 << axis)) {
				const double twiceCenter = baseBounds[axis] + baseBounds[axis + 2];
				const double low = twiceCenter - bounds[axis + 2];
				bounds[axis + 2] = twiceCenter - bounds[axis];
				bounds[axis] = low;
			}
		SetPixelBoxBounds(box, bounds);
		return PublishBox(context, "pbbox", box);
	}
	bool PixelBoxSplit(NodeContext &context) {
		auto first = ReadBox(context, "pbbox", true), second = first;
		auto firstBounds = PixelBoxBounds(first), secondBounds = firstBounds;
		const size_t axis = context.Integer("axis") == 0 ? 0 : 1;
		const double extent = firstBounds[axis + 2] - firstBounds[axis];
		double split = context.Integer("unit") == 0 ? std::floor(extent * context.Scalar("ratio", 0.5))
													: double(context.Integer("size", 4));
		if (context.Integer("anchor") != 0) split = extent - split;
		firstBounds[axis + 2] = firstBounds[axis] + split;
		secondBounds[axis] = firstBounds[axis + 2];
		SetPixelBoxBounds(first, firstBounds);
		SetPixelBoxBounds(second, secondBounds);
		return PublishBox(context, "pbbox", first) && PublishBox(context, "pbbox_2", second);
	}
}
