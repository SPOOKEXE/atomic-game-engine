#include "../PixelOps.hpp"
#include "../SourceGradient.hpp"
#include "Families.hpp"
namespace engine::imagegraph::detail {
	namespace {
		bool ReadNumber(const ElementValue &value, double &output) {
			if (const auto *number = std::get_if<double>(&value))
				output = *number;
			else if (const auto *number = std::get_if<int64_t>(&value))
				output = double(*number);
			else if (const auto *number = std::get_if<bool>(&value))
				output = *number ? 1 : 0;
			else if (const auto *number = std::get_if<EnumValue>(&value))
				output = double(number->Value);
			else
				return false;
			return std::isfinite(output);
		}
		bool ReadPoint(
			NodeContext &context, const ArrayValue *array, const Vector2 *single, size_t index, Vector2 &point
		) {
			if (single)
				point = *single;
			else if (!array->Items.empty()) {
				const auto &item = array->Items[index];
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					const auto *vector = std::get_if<Vector2>(leaf);
					if (!vector)
						return context.Fail(
							Status::TypeMismatch, "point row needs two coordinates", "points"
						);
					point = *vector;
				} else {
					const auto &row = std::get<std::vector<SourceArrayItem>>(item.Data);
					if (row.size() < 2)
						return context.Fail(Status::InvalidValue, "point row is short", "points");
					const auto *x = std::get_if<ElementValue>(&row[0].Data),
							   *y = std::get_if<ElementValue>(&row[1].Data);
					if (!x || !y || !ReadNumber(*x, point.X) || !ReadNumber(*y, point.Y))
						return context.Fail(
							Status::TypeMismatch, "point row needs numeric coordinates", "points"
						);
				}
			} else if (!array->Nested.empty()) {
				const auto &row = array->Nested[index];
				if (row.size() < 2) return context.Fail(Status::InvalidValue, "point row is short", "points");
				if (!ReadNumber(row[0], point.X) || !ReadNumber(row[1], point.Y))
					return context.Fail(
						Status::TypeMismatch, "point row needs numeric coordinates", "points"
					);
			} else {
				const auto *vector = std::get_if<Vector2>(&array->Elements[index]);
				if (!vector)
					return context.Fail(Status::TypeMismatch, "points require vector rows", "points");
				point = *vector;
			}
			return true;
		}
		bool PointsRemap(NodeContext &context) {
			const auto *image = context.Input("uv_map");
			if (!image || !image->Width || !image->Height)
				return context.Fail(Status::InvalidValue, "points remapping requires a UV surface", "uv_map");
			if (image->Format == SurfaceFormat::RGBA4Unorm || image->Format == SurfaceFormat::R8Unorm)
				return context.Fail(
					Status::UnsupportedExecution,
					"source UV sampler does not define this surface format",
					"uv_map"
				);
			const auto *input = context.Find("points");
			if (!input || !ValidRuntimeValue(*input))
				return context.Fail(Status::InvalidValue, "points payload is invalid", "points");
			const auto *array = std::get_if<ArrayValue>(input);
			const auto *single = std::get_if<Vector2>(input);
			if (!array && !single)
				return context.Fail(Status::TypeMismatch, "points require two-coordinate rows", "points");
			const size_t count = single					  ? 1
								 : !array->Items.empty()  ? array->Items.size()
								 : !array->Nested.empty() ? array->Nested.size()
														  : array->Elements.size();
			if (count > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "points exceed array bounds", "points");
			if (!context.ReserveOutput(count * sizeof(ElementValue) + std::string{}.capacity(), "points"))
				return false;
			ArrayValue output;
			output.ElementType = ValueType::Vector2;
			output.Elements.reserve(count);
			const double amount = context.Scalar("amount", 1);
			if (!std::isfinite(amount))
				return context.Fail(Status::InvalidValue, "points amount is nonfinite", "amount");
			for (size_t index = 0; index < count; ++index) {
				Vector2 point;
				if (!ReadPoint(context, array, single, index, point)) return false;
				const auto x = uint32_t(std::clamp(SourceRoundEven(point.X), 0.0, double(image->Width - 1)));
				const auto y = uint32_t(std::clamp(SourceRoundEven(point.Y), 0.0, double(image->Height - 1)));
				auto pixel = ReadPixel(*image, x, y);
				if (image->Format == SurfaceFormat::R16Float || image->Format == SurfaceFormat::R32Float) {
					if (!std::isfinite(pixel[0]))
						return context.Fail(Status::InvalidValue, "UV sample is nonfinite", "uv_map");
					double packed = std::fmod(std::trunc(pixel[0]), 4294967296.0);
					if (packed < 0) packed += 4294967296.0;
					const auto color = uint32_t(packed);
					pixel[0] = (color & 255u) / 255.0;
					pixel[1] = ((color >> 8) & 255u) / 255.0;
				}
				Vector2 result{
					point.X + (pixel[0] * image->Width - point.X) * amount,
					point.Y + (pixel[1] * image->Height - point.Y) * amount
				};
				if (!std::isfinite(result.X) || !std::isfinite(result.Y))
					return context.Fail(Status::InvalidValue, "remapped point is nonfinite", "points");
				output.Elements.emplace_back(result);
			}
			context.SetValue("points", std::move(output));
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourcePointsExecutors() {
		static constexpr std::array entries{ExecutorEntry{"pc.points_remap", PointsRemap, true}};
		return entries;
	}
}
