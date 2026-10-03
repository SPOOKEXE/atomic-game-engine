#include "SourceRetainedImageOutputs.hpp"
#include "SourceSafeDraw.hpp"
#include "TimelineDrivers.hpp"
#include "nodes/Processor.hpp"
#include "nodes/Sampler.hpp"

namespace engine::imagegraph::detail {
	bool SurfaceSize(NodeContext &, double, double, uint32_t &, uint32_t &);
	namespace {
		constexpr uint64_t CROP_WORK = 64000000;
		struct CropInput {
			const Image *Surface;
			Vector2 Position{};
		};
		struct CropBounds {
			uint32_t Left = 0, Top = 0, Right = 0, Bottom = 0;
		};
		struct CropSize {
			uint32_t Width = 0, Height = 0;
			SurfaceFormat Format{};
		};
		bool RedOnly(const Image &s) {
			return s.Format == SurfaceFormat::R8Unorm || s.Format == SurfaceFormat::R16Float ||
				   s.Format == SurfaceFormat::R32Float;
		}
		bool PublishValueCopy(NodeContext &c, std::string_view port, const Value &value) {
			const bool distance = port == "crop_distance";
			const auto validAtlas = [](const AtlasValue &atlas) {
				return atlas.Data && atlas.Data->Kind == AtlasKind::SurfaceAtlas &&
					   ValidSurfaceLayout(
						   atlas.Data->Surface.Data, Limits::MaximumDimension, Limits::MaximumArrayBytes
					   );
			};
			bool schema = distance ? std::holds_alternative<Vector4>(value)
								   : (std::holds_alternative<AtlasValue>(value) &&
									  validAtlas(std::get<AtlasValue>(value)));
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				schema = array->ElementType == (distance ? ValueType::Vector4 : ValueType::Atlas) &&
						 array->Items.empty() && array->Nested.empty();
				for (const auto &element : array->Elements)
					schema &= distance ? std::holds_alternative<Vector4>(element)
									   : (std::holds_alternative<AtlasValue>(element) &&
										  validAtlas(std::get<AtlasValue>(element)));
			}
			if (!schema)
				return c.Fail(
					Status::InvalidValue, "retained Crop metadata has the wrong source output shape", port
				);
			const auto bytes = ValueClonePayloadBytes(value);
			if (!bytes)
				return c.Fail(Status::LimitExceeded, "Crop retained value exceeds clone bounds", port);
			auto copyCharge = c.ReserveWorkspace(*bytes, port);
			if (!copyCharge) return false;
			Value copy = value;
			c.SetValue(port, std::move(copy));
			return c.FailureCode == Status::Ok;
		}
		bool CropMetadata(NodeContext &c) {
			std::array<std::pair<std::string_view, const Value *>, 2> fields{
				{{"crop_distance", nullptr}, {"atlas", nullptr}}
			};
			for (const auto &out : c.OutputValues)
				for (auto &[name, value] : fields)
					if (out.Port == name) value = &out.Data;
			return StoreSourceRetainedMetadata(c, fields);
		}
		bool Bounds(NodeContext &c, const Image &source, Rgba background, CropBounds &bounds) {
			// The source emptiness shortcut sums sixteen taps into RGBA8 after each draw.
			// This CPU profile uses point sampling and pixel-center quad coverage.
			if (std::max(source.Width, source.Height) > 64) {
				Image stage{
					source.Width,
					source.Height,
					std::vector<uint8_t>(size_t(source.Width) * source.Height * 4),
					0
				};
				for (uint32_t y = 0; y < source.Height; ++y)
					for (uint32_t x = 0; x < source.Width; ++x)
						if (!WritePixel(stage, x, y, SourceSafeDrawPixel(source, x, y)))
							return c.Fail(
								Status::InvalidValue, "Crop staging pixel is nonfinite", "surface_in"
							);
				while (std::max(stage.Width, stage.Height) > 64) {
					const uint32_t width = (stage.Width + 3) / 4, height = (stage.Height + 3) / 4;
					Image next{width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
					for (uint32_t y = 0; y < height; ++y)
						for (uint32_t x = 0; x < width; ++x) {
							if ((x + .5) * 4 >= stage.Width || (y + .5) * 4 >= stage.Height) continue;
							Rgba sum{};
							for (uint32_t dx = 0; dx < 4; ++dx)
								for (uint32_t dy = 0; dy < 4; ++dy) {
									const auto pixel = Texture(
										stage,
										(x + .5) * 4 / stage.Width + dx / (4. * width),
										(y + .5) * 4 / stage.Height + dy / (4. * height),
										false
									);
									for (size_t k = 0; k < 4; ++k)
										sum[k] += pixel[k];
								}
							if (!WritePixel(next, x, y, sum))
								return c.Fail(
									Status::InvalidValue, "Crop downsample pixel is nonfinite", "surface_in"
								);
						}
					stage = std::move(next);
				}
				bool empty = true;
				for (size_t i = 3; i < stage.Pixels.size(); i += 4)
					empty &= stage.Pixels[i] == 0;
				if (empty) {
					bounds.Right = bounds.Bottom = 1;
					return true;
				}
			}
			bool found = false;
			for (uint32_t y = 0; y < source.Height; ++y)
				for (uint32_t x = 0; x < source.Width; ++x) {
					const auto pixel = SourceSafeDrawPixel(source, x, y);
					if (!std::all_of(pixel.begin(), pixel.end(), [](double v) { return std::isfinite(v); }))
						return c.Fail(Status::InvalidValue, "Crop source pixel is nonfinite", "surface_in");
					// Replace-color compares original sampled RGBA before byte readback;
					// the red-channel safe-draw shader replaces that shader entirely.
					if ((!RedOnly(source) && pixel == background) || std::clamp(pixel[3], 0., 1.) * 255 < .5)
						continue;
					if (!found)
						bounds = {x, y, x + 1, y + 1};
					else {
						bounds.Left = std::min(bounds.Left, x);
						bounds.Top = std::min(bounds.Top, y);
						bounds.Right = std::max(bounds.Right, x + 1);
						bounds.Bottom = std::max(bounds.Bottom, y + 1);
					}
					found = true;
				}
			return true;
		}
	}
	bool SourceCropContent(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.crop_content");
		const ImageArray *images = nullptr;
		for (const auto &[port, array] : c.ImageArrays)
			if (port == "surface_in") images = array;
		const Value *raw = c.Find("surface_in");
		const auto *atlases = raw ? std::get_if<ArrayValue>(raw) : nullptr;
		if (atlases &&
			(atlases->ElementType != ValueType::Atlas || !atlases->Items.empty() || !atlases->Nested.empty()))
			return c.Fail(Status::UnsupportedExecution, "Crop requires a flat Atlas array", "surface_in");
		const bool arrayInput = images || atlases;
		const size_t count = images ? (images->Items.empty() ? images->Images.size() : images->Items.size())
							 : atlases ? atlases->Elements.size()
									   : 1;
		if (count > Limits::MaximumArrayElements)
			return c.Fail(Status::LimitExceeded, "Crop source array exceeds element bounds", "surface_in");
		auto controlCharge = c.ReserveWorkspace(
			count * (sizeof(CropInput) + sizeof(CropBounds) + sizeof(CropSize)), "surface_in"
		);
		if (!controlCharge) return false;
		std::vector<CropInput> inputs;
		inputs.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			CropInput input{};
			if (images) {
				size_t index = i;
				if (!images->Items.empty()) {
					const auto *leaf = std::get_if<size_t>(&images->Items[i].Data);
					if (!leaf || *leaf >= images->Images.size())
						return c.Fail(
							Status::UnsupportedExecution, "Crop requires a flat image array", "surface_in"
						);
					index = *leaf;
				}
				input.Surface = &images->Images[index];
			} else if (atlases) {
				const auto *atlas = std::get_if<AtlasValue>(&atlases->Elements[i]);
				if (!atlas || !atlas->Data)
					return c.Fail(Status::InvalidValue, "Crop Atlas surface is absent", "surface_in");
				input = {
					&atlas->Data->Surface.Data,
					atlas->Data->Kind == AtlasKind::SurfaceAtlas ? atlas->Data->Position : Vector2{}
				};
			} else {
				input.Surface = c.Input("surface_in");
				if (const auto *atlas = raw ? std::get_if<AtlasValue>(raw) : nullptr;
					atlas && atlas->Data && atlas->Data->Kind == AtlasKind::SurfaceAtlas)
					input.Position = atlas->Data->Position;
			}
			if (!input.Surface ||
				!ValidSurfaceLayout(*input.Surface, Limits::MaximumDimension, Limits::MaximumOutputBytes))
				return c.Fail(Status::InvalidValue, "Crop source surface layout is invalid", "surface_in");
			inputs.push_back(input);
		}
		const bool active = c.Boolean("active", true);
		Vector4 padding{};
		if (active) {
			const Value *value = c.Find("padding");
			if (const auto *v = value ? std::get_if<Vector4>(value) : nullptr)
				padding = *v;
			else if (const auto *v = value ? std::get_if<Vector3>(value) : nullptr)
				padding = {v->X, v->Y, v->Z, 0};
			else if (const auto *v = value ? std::get_if<Vector2>(value) : nullptr)
				padding = {v->X, v->Y, 0, 0};
			else if (const auto *a = value ? std::get_if<ArrayValue>(value) : nullptr) {
				if (!a->Items.empty() || !a->Nested.empty())
					return c.Fail(
						Status::UnsupportedExecution, "Crop padding requires one numeric tuple", "padding"
					);
				std::array<double *, 4> target{&padding.X, &padding.Y, &padding.Z, &padding.W};
				for (size_t i = 0; i < std::min(size_t{4}, a->Elements.size()); ++i) {
					bool valid = false;
					std::visit(
						[&](const auto &leaf) {
							using T = std::decay_t<decltype(leaf)>;
							if constexpr (std::is_arithmetic_v<T>) {
								*target[i] = double(leaf);
								valid = true;
							} else if constexpr (std::is_same_v<T, EnumValue>) {
								*target[i] = double(leaf.Value);
								valid = true;
							}
						},
						a->Elements[i]
					);
					if (!valid)
						return c.Fail(
							Status::TypeMismatch, "Crop padding tuple contains a nonnumeric value", "padding"
						);
				}
			} else if (value) {
				if (!std::holds_alternative<double>(*value) && !std::holds_alternative<int64_t>(*value) &&
					!std::holds_alternative<bool>(*value) && !std::holds_alternative<EnumValue>(*value))
					return c.Fail(Status::TypeMismatch, "Crop padding requires numeric values", "padding");
				const double v = c.Scalar("padding");
				padding = {v, v, v, v};
			}
			for (double *v : {&padding.X, &padding.Y, &padding.Z, &padding.W}) {
				if (!std::isfinite(*v) || std::abs(*v) > Limits::MaximumDimension * 4)
					return c.Fail(Status::LimitExceeded, "Crop padding exceeds bounded geometry", "padding");
				*v = DriverRoundHalfEven(*v);
			}
		}
		uint64_t work = 0, staging = 0;
		uint32_t unionWidth = 0, unionHeight = 0;
		for (const auto &input : inputs) {
			unionWidth = std::max(unionWidth, input.Surface->Width);
			unionHeight = std::max(unionHeight, input.Surface->Height);
		}
		const bool unionSizing = active && c.Integer("array_sizing", 1) == 0;
		for (const auto &input : inputs) {
			const auto &s = *input.Surface;
			uint64_t current = uint64_t(s.Width) * s.Height * (active ? 3 : 1);
			uint32_t width = s.Width, height = s.Height;
			while (active && std::max(width, height) > 64) {
				width = (width + 3) / 4;
				height = (height + 3) / 4;
				current += uint64_t(width) * height * 16;
			}
			staging = std::max(staging, uint64_t(s.Width) * s.Height * 8);
			const double outWidth =
							 std::max(1., (unionSizing ? unionWidth : s.Width) + padding.X + padding.Z),
						 outHeight =
							 std::max(1., (unionSizing ? unionHeight : s.Height) + padding.Y + padding.W);
			if (outWidth > CROP_WORK || outHeight > CROP_WORK || outWidth * outHeight > CROP_WORK ||
				current > CROP_WORK - work || outWidth * outHeight > CROP_WORK - work - current)
				return c.Fail(
					Status::LimitExceeded, "Crop complete source array exceeds work budget", "surface_in"
				);
			work += current + uint64_t(outWidth * outHeight);
		}
		auto stagingCharge = c.ReserveWorkspace(active ? staging : 0, "surface_in");
		if (!stagingCharge) return false;
		std::vector<CropBounds> bounds(count);
		if (active) {
			const auto backgroundColour = c.Get<Colour>("background", {0, 0, 0, 0});
			const Rgba background{
				backgroundColour.Red / 255.,
				backgroundColour.Green / 255.,
				backgroundColour.Blue / 255.,
				backgroundColour.Alpha / 255.
			};
			for (size_t i = 0; i < count; ++i)
				if (!Bounds(c, *inputs[i].Surface, background, bounds[i])) return false;
			if (c.Integer("array_sizing", 1) == 0 && count) {
				CropBounds united = bounds[0];
				for (const auto &b : bounds) {
					united.Left = std::min(united.Left, b.Left);
					united.Top = std::min(united.Top, b.Top);
					united.Right = std::max(united.Right, b.Right);
					united.Bottom = std::max(united.Bottom, b.Bottom);
				}
				std::fill(bounds.begin(), bounds.end(), united);
			}
		}
		std::vector<CropSize> sizes(count);
		uint64_t outputBytes = count * (sizeof(Image) + sizeof(ImageArrayItem));
		for (size_t i = 0; i < count; ++i) {
			const auto &s = *inputs[i].Surface;
			const auto &b = bounds[i];
			if (!SurfaceSize(
					c,
					active ? double(b.Right - b.Left) + padding.X + padding.Z : s.Width,
					active ? double(b.Bottom - b.Top) + padding.Y + padding.W : s.Height,
					sizes[i].Width,
					sizes[i].Height
				))
				return false;
			const auto format =
				active ? ResolveProcessorSurfaceFormat(c, &s) : std::optional{SurfaceFormat::RGBA8Unorm};
			if (!format) return false;
			sizes[i].Format = *format;
			const uint64_t bytes =
				uint64_t(sizes[i].Width) * sizes[i].Height * DescribeSurfaceFormat(*format)->BytesPerPixel;
			if (bytes > Limits::MaximumEvaluationBytes - outputBytes)
				return c.Fail(Status::LimitExceeded, "Crop image array exceeds output bytes", "surface_out");
			outputBytes += bytes;
		}
		if (!c.ReserveOutput(arrayInput ? outputBytes + std::string{}.capacity() : 0, "surface_out"))
			return false;
		ImageArray result;
		if (arrayInput) {
			result.Images.reserve(count);
			result.Items.reserve(count);
		}
		const uint64_t metadataBytes =
			active ? outputBytes + count * (sizeof(AtlasData) + sizeof(ElementValue) * 2) : 0;
		auto metadataCharge = c.ReserveWorkspace(metadataBytes, "atlas");
		if (!metadataCharge) return false;
		ArrayValue distances, atlasValues;
		distances.ElementType = ValueType::Vector4;
		atlasValues.ElementType = ValueType::Atlas;
		if (active && arrayInput) {
			distances.Elements.reserve(count);
			atlasValues.Elements.reserve(count);
		}
		ArrayValue initialAtlas;
		initialAtlas.ElementType = ValueType::Atlas;
		Value distance = Vector4{}, atlas = std::move(initialAtlas);
		for (size_t i = 0; i < count; ++i) {
			const auto &s = *inputs[i].Surface;
			const auto &b = bounds[i];
			const auto &size = sizes[i];
			Image local;
			Image *out = nullptr;
			if (arrayInput) {
				local = {
					size.Width,
					size.Height,
					std::vector<uint8_t>(
						size_t(size.Width) * size.Height * DescribeSurfaceFormat(size.Format)->BytesPerPixel
					),
					0,
					size.Format
				};
				out = &local;
			} else
				out = c.NewImage("surface_out", size.Width, size.Height, size.Format);
			if (!out) return false;
			const double offsetX = active ? -double(b.Left) + padding.Z : 0,
						 offsetY = active ? -double(b.Top) + padding.Y : 0;
			for (uint32_t y = 0; y < out->Height; ++y)
				for (uint32_t x = 0; x < out->Width; ++x) {
					const double sx = x - offsetX, sy = y - offsetY;
					if (sx < 0 || sy < 0 || sx >= s.Width || sy >= s.Height) continue;
					Rgba pixel = active ? SourceSafeDrawPixel(s, uint32_t(sx), uint32_t(sy))
										: ReadPixel(s, uint32_t(sx), uint32_t(sy));
					if (!WritePixel(*out, x, y, pixel))
						return c.Fail(Status::InvalidValue, "Crop output pixel is nonfinite", "surface_out");
				}
			if (active) {
				Vector4 d{
					double(s.Width) - b.Right, double(b.Top), double(b.Left), double(s.Height) - b.Bottom
				};
				AtlasValue a;
				auto &data = a.Data.emplace();
				data.Kind = AtlasKind::SurfaceAtlas;
				out->Hash = SurfaceHash(*out);
				data.Surface.Data = *out;
				data.Position = {b.Left + inputs[i].Position.X, b.Top + inputs[i].Position.Y};
				data.Dimension = {double(out->Width), double(out->Height)};
				data.OriginalDimension = data.Dimension;
				if (arrayInput) {
					distances.Elements.emplace_back(d);
					atlasValues.Elements.emplace_back(std::move(a));
				} else {
					distance = d;
					atlas = std::move(a);
				}
			}
			if (arrayInput) {
				result.Items.push_back({result.Images.size()});
				result.Images.push_back(std::move(local));
			}
		}
		if (arrayInput) c.OutputImageArrays.emplace_back("surface_out", std::move(result));
		if (active) {
			if (arrayInput) {
				distance = std::move(distances);
				atlas = std::move(atlasValues);
			}
			c.SetValue("crop_distance", std::move(distance));
			c.SetValue("atlas", std::move(atlas));
			metadataCharge->Reset();
			return c.FailureCode == Status::Ok && CropMetadata(c);
		}
		const Value *priorDistance = SourceRetainedField(c, "crop_distance");
		if (c.FailureCode != Status::Ok) return false;
		if (priorDistance && !PublishValueCopy(c, "crop_distance", *priorDistance)) return false;
		if (!priorDistance) c.SetValue("crop_distance", Vector4{});
		const Value *priorAtlas = SourceRetainedField(c, "atlas");
		if (c.FailureCode != Status::Ok) return false;
		if (priorAtlas) return PublishValueCopy(c, "atlas", *priorAtlas);
		ArrayValue empty;
		empty.ElementType = ValueType::Atlas;
		c.SetValue("atlas", std::move(empty));
		return c.FailureCode == Status::Ok;
	}
}
