#include "ImageGraphPreviewResult.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <cmath>
#include <new>

namespace studio::detail {
	namespace {
		using namespace engine::imagegraph;
		bool Add(uint64_t &bytes, uint64_t value) {
			if (value > UINT64_MAX - bytes) return false;
			bytes += value;
			return true;
		}
		bool ArrayBytes(uint64_t &bytes, size_t count, size_t stride) {
			return !count || (stride <= UINT64_MAX / count && Add(bytes, count * stride));
		}
		bool
		RetainItems(const std::vector<ImageArrayItem> &items, size_t depth, size_t &count, uint64_t &bytes) {
			if (depth > Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements - count ||
				!ArrayBytes(bytes, items.capacity(), sizeof(ImageArrayItem)))
				return false;
			count += items.size();
			for (const auto &item : items)
				if (const auto *nested = std::get_if<std::vector<ImageArrayItem>>(&item.Data))
					if (!RetainItems(*nested, depth + 1, count, bytes)) return false;
			return true;
		}
		bool Surface(const Image &image) {
			const auto layout = CheckedSurfaceLayout(
				image.Width, image.Height, image.Format, IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES
			);
			return image.Width && image.Height && image.Width <= IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION &&
				   image.Height <= IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION && layout &&
				   layout->Bytes == image.Pixels.size() && FiniteSurfaceSamples(image);
		}
	}
	uint64_t ImageGraphSequenceBytes(const engine::imagegraph::ImageArray &sequence) noexcept {
		using namespace engine::imagegraph;
		uint64_t bytes = sizeof(ImageArray);
		size_t count = 0;
		if (sequence.Images.size() > Limits::MaximumArrayElements ||
			!ArrayBytes(bytes, sequence.Images.capacity(), sizeof(Image)) ||
			!RetainItems(sequence.Items, 0, count, bytes))
			return UINT64_MAX;
		for (const auto &image : sequence.Images)
			if (!Add(bytes, image.Pixels.capacity())) return UINT64_MAX;
		return bytes;
	}
	bool ValidateImageGraphSequence(
		const engine::imagegraph::ImageArray &sequence, engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		if (ImageGraphSequenceBytes(sequence) == UINT64_MAX) {
			diagnostic = {Status::LimitExceeded, {}, {}, "Preview sequence storage exceeds array bounds"};
			return false;
		}
		for (const auto &image : sequence.Images)
			if (!Surface(image)) {
				diagnostic = {
					Status::InvalidValue, {}, {}, "Preview sequence has invalid or oversized pixels"
				};
				return false;
			}
		const auto valid = [&](auto &&self, const auto &items) -> bool {
			for (const auto &item : items) {
				if (const auto *index = std::get_if<size_t>(&item.Data)) {
					if (*index >= sequence.Images.size()) return false;
				} else if (!self(self, std::get<std::vector<ImageArrayItem>>(item.Data)))
					return false;
			}
			return true;
		};
		if (!valid(valid, sequence.Items)) {
			diagnostic = {Status::InvalidOutput, {}, {}, "Preview sequence refers to a missing image"};
			return false;
		}
		diagnostic = {};
		return true;
	}
	uint64_t ImageGraphPreviewBytes(const ImageGraphPreviewValue &preview) noexcept {
		using namespace engine::imagegraph;
		uint64_t bytes = sizeof(preview);
		const auto nested = std::visit(
			[](const auto &value) -> uint64_t {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, Image>)
					return value.Pixels.capacity();
				else if constexpr (std::is_same_v<T, ImageArray>) {
					const auto retained = ImageGraphSequenceBytes(value);
					return retained == UINT64_MAX ? retained : retained - sizeof(value);
				} else {
					const auto retained = ValueClonePayloadBytes(value.Data);
					uint64_t total = value.Port.capacity();
					return retained && *retained >= sizeof(Value) && Add(total, *retained - sizeof(Value))
							   ? total
							   : UINT64_MAX;
				}
			},
			preview
		);
		return Add(bytes, nested) ? bytes : UINT64_MAX;
	}
	bool ValidateImageGraphPreview(
		const ImageGraphPreviewValue &preview, engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		if (const auto *sequence = std::get_if<ImageArray>(&preview))
			return ValidateImageGraphSequence(*sequence, diagnostic);
		if (const auto *image = std::get_if<Image>(&preview)) {
			if (!Surface(*image)) {
				diagnostic = {Status::InvalidValue, {}, {}, "Preview requires finite bounded surface pixels"};
				return false;
			}
		} else {
			const auto &value = std::get<EvaluatedValue>(preview);
			if (const auto *vector = std::get_if<Vector2>(&value.Data);
				vector && (!std::isfinite(vector->X) || !std::isfinite(vector->Y))) {
				diagnostic = {Status::InvalidValue, {}, {}, "Vector preview requires finite coordinates"};
				return false;
			}
			if (const auto *array = std::get_if<ArrayValue>(&value.Data);
				array &&
				(array->ElementType == ValueType::Scalar || array->ElementType == ValueType::Integer))
				return CheckImageGraphArrayPreview(*array, diagnostic);
		}
		diagnostic = {};
		return true;
	}
	engine::imagegraph::Status PrepareImageGraphPreviewResult(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Plan &plan,
		std::string_view outputId,
		engine::imagegraph::EvaluationRequest request,
		engine::imagegraph::CapturedFeedbackHost &feedback,
		uint64_t revision,
		uint64_t inputRevision,
		ImageGraphPreviewValue &preview,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		using namespace engine::imagegraph;
		ENGINE_PROFILE_CAT("image composer preview result", engine::core::ProfileCategory::Render);
		const auto refuse = [&] {
			diagnostic = {Status::LimitExceeded, {}, {}, "Preview replacement exceeds caller byte allowance"};
			return diagnostic.Code;
		};
		const uint64_t oldBytes = ImageGraphPreviewBytes(preview);
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || oldBytes >= maximumBytes)
			return refuse();
		if (!request.MaximumImageDimension || request.MaximumImageDimension > Limits::MaximumDimension) {
			diagnostic = {
				Status::InvalidValue, {}, {}, "Preview dimension allowance is outside supported bounds"
			};
			return diagnostic.Code;
		}
		request.MaximumImageDimension =
			std::min(request.MaximumImageDimension, IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION);
		if (!feedback.Prepare(
				document,
				plan,
				revision,
				inputRevision,
				request,
				diagnostic,
				maximumBytes - oldBytes,
				outputId
			))
			return diagnostic.Code;
		if (const auto *selected = feedback.Value(outputId)) {
			const uint64_t held = feedback.RetainedBytes();
			// Measure the source by borrowing its alternatives, before any clone.
			uint64_t clone = sizeof(ImageGraphPreviewValue);
			const bool admitted = std::visit(
				[&](const auto &value) {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, Image>) {
						if (!Surface(value)) {
							diagnostic = {
								Status::InvalidValue,
								{},
								{},
								"Feedback preview requires finite bounded pixels"
							};
							return false;
						}
						return Add(clone, value.Pixels.capacity());
					} else if constexpr (std::is_same_v<T, ImageArray>) {
						const auto bytes = ImageGraphSequenceBytes(value);
						return bytes != UINT64_MAX && ValidateImageGraphSequence(value, diagnostic) &&
							   Add(clone, bytes - sizeof(value));
					} else {
						const auto bytes = ValueClonePayloadBytes(value.Data);
						return bytes && *bytes >= sizeof(Value) && Add(clone, *bytes - sizeof(Value)) &&
							   Add(clone, value.Port.capacity());
					}
				},
				selected->Output
			);
			if (!admitted && diagnostic.Code != Status::Ok) return diagnostic.Code;
			if (!admitted || held > maximumBytes - oldBytes || clone > maximumBytes - oldBytes - held)
				return refuse();
			ImageGraphPreviewValue candidate = std::visit(
				[](const auto &value) -> ImageGraphPreviewValue { return value; }, selected->Output
			);
			if (ImageGraphPreviewBytes(candidate) > maximumBytes - oldBytes - held) return refuse();
			if (!ValidateImageGraphPreview(candidate, diagnostic)) return diagnostic.Code;
			preview = std::move(candidate);
			engine::core::Metrics::Count(
				"studio.composer.preview.retained_copy_bytes", ImageGraphPreviewBytes(preview)
			);
			engine::core::Metrics::Count("studio.composer.preview.copies", 1);
			return Status::Ok;
		}
		const uint64_t held = feedback.RetainedBytes();
		if (held >= maximumBytes - oldBytes) return refuse();
		return EvaluateImageGraphPreview(
			document, plan, outputId, request, preview, diagnostic, maximumBytes - held
		);
	} catch (const std::bad_alloc &) {
		diagnostic = {
			engine::imagegraph::Status::LimitExceeded, {}, {}, "Preview publication allocation refused"
		};
		return diagnostic.Code;
	}
}
