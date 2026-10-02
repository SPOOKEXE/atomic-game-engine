#pragma once

// Borrowed image inputs are measured before cloning pixels or nested array storage.
#include "EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>

#include <span>
#include <variant>

namespace engine::imagegraph::detail {
	using ImageArrayInput = std::variant<const Image *, const ImageArray *>;
	struct ImageArrayFootprint {
		size_t Images = 0, Roots = 0, Items = 0;
		uint64_t Pixels = 0;
		uint64_t Bytes() const {
			return Images * sizeof(Image) + Items * sizeof(ImageArrayItem) + Pixels;
		}
	};

	inline Status
	MeasureImageArray(std::span<const ImageArrayInput> inputs, bool spread, ImageArrayFootprint &output) {
		if (inputs.size() > Limits::MaximumDynamicInputsPerNode) return Status::LimitExceeded;
		ImageArrayFootprint measured;
		const auto image = [&](const Image &value) {
			if (measured.Images == Limits::MaximumArrayElements ||
				value.Pixels.size() > Limits::MaximumOutputBytes - measured.Pixels)
				return false;
			++measured.Images;
			measured.Pixels += value.Pixels.size();
			return true;
		};
		const auto item = [&](auto &&self, const ImageArrayItem &value, size_t depth) -> bool {
			if (measured.Items == Limits::MaximumArrayElements) return false;
			++measured.Items;
			if (const auto *children = std::get_if<std::vector<ImageArrayItem>>(&value.Data)) {
				if (depth >= 16 && !children->empty()) return false;
				for (const auto &child : *children)
					if (!self(self, child, depth + 1)) return false;
			}
			return true;
		};
		for (const auto &input : inputs) {
			if (const auto *single = std::get_if<const Image *>(&input)) {
				if (!*single) return Status::InvalidValue;
				if (!image(**single) || measured.Items == Limits::MaximumArrayElements)
					return Status::LimitExceeded;
				++measured.Roots;
				++measured.Items;
			} else {
				const auto *array = std::get<const ImageArray *>(input);
				if (!array) return Status::InvalidValue;
				for (const auto &value : array->Images)
					if (!image(value)) return Status::LimitExceeded;
				if (!spread) {
					if (measured.Items == Limits::MaximumArrayElements) return Status::LimitExceeded;
					++measured.Items;
					++measured.Roots;
				} else
					measured.Roots += array->Items.size();
				for (const auto &value : array->Items)
					if (!item(item, value, spread ? 1 : 2)) return Status::LimitExceeded;
			}
		}
		output = measured;
		return Status::Ok;
	}

	inline ImageArrayItem CopyRebasedImageItem(const ImageArrayItem &source, size_t offset) {
		if (const auto *leaf = std::get_if<size_t>(&source.Data)) return ImageArrayItem{*leaf + offset};
		const auto &sourceChildren = std::get<std::vector<ImageArrayItem>>(source.Data);
		std::vector<ImageArrayItem> children;
		children.reserve(sourceChildren.size());
		for (const auto &child : sourceChildren)
			children.push_back(CopyRebasedImageItem(child, offset));
		return ImageArrayItem{std::move(children)};
	}

	// Old output and its lease stay live through admission and construction of the complete replacement.
	inline Status CollectImageArray(
		std::span<const ImageArrayInput> inputs,
		bool spread,
		EvaluationBudget &budget,
		ImageArray &output,
		AllocationReservation &outputCharge
	) {
		ImageArrayFootprint footprint;
		const Status status = MeasureImageArray(inputs, spread, footprint);
		if (status != Status::Ok) return status;
		auto replacementCharge = budget.Reserve(footprint.Bytes());
		if (!replacementCharge) return Status::LimitExceeded;
		ImageArray candidate;
		candidate.Images.reserve(footprint.Images);
		candidate.Items.reserve(footprint.Roots);
		const auto image = [&](const Image &source) {
			Image clone;
			clone.Width = source.Width;
			clone.Height = source.Height;
			clone.Hash = source.Hash;
			clone.Format = source.Format;
			clone.Pixels.reserve(source.Pixels.size());
			clone.Pixels.assign(source.Pixels.begin(), source.Pixels.end());
			candidate.Images.push_back(std::move(clone));
		};
		for (const auto &input : inputs) {
			if (const auto *single = std::get_if<const Image *>(&input)) {
				candidate.Items.push_back(ImageArrayItem{candidate.Images.size()});
				image(**single);
			} else {
				const auto &array = *std::get<const ImageArray *>(input);
				const size_t offset = candidate.Images.size();
				for (const auto &source : array.Images)
					image(source);
				if (spread) {
					for (const auto &source : array.Items)
						candidate.Items.push_back(CopyRebasedImageItem(source, offset));
				} else {
					std::vector<ImageArrayItem> children;
					children.reserve(array.Items.size());
					for (const auto &source : array.Items)
						children.push_back(CopyRebasedImageItem(source, offset));
					candidate.Items.push_back(ImageArrayItem{std::move(children)});
				}
			}
		}
		output = std::move(candidate);
		outputCharge = std::move(*replacementCharge);
		return Status::Ok;
	}
}
