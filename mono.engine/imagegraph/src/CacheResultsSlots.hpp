#pragma once

#include <engine/imagegraph/CacheResultsReplay.hpp>

namespace engine::imagegraph::detail {
	// A flat source surface list may own pixels or a freed-slot marker at each position.
	// Mixed lists use the existing Any.Items carrier, not a permissive Any.Elements array.
	struct CacheResultsSlots {
		const ArrayValue *Array = nullptr;
		size_t Count() const {
			return !Array ? 0 : Array->Items.empty() ? Array->Elements.size() : Array->Items.size();
		}
		const ElementValue *Element(size_t index) const {
			if (!Array || index >= Count()) return nullptr;
			if (Array->Items.empty()) return &Array->Elements[index];
			return std::get_if<ElementValue>(&Array->Items[index].Data);
		}
		const Image *ImageAt(size_t index) const {
			if (!Array || index >= Count()) return nullptr;
			if (const auto *element = Element(index))
				if (const auto *surface = std::get_if<SurfaceValue>(element)) return &surface->Data;
			return Array->Items.empty() ? nullptr : std::get_if<Image>(&Array->Items[index].Data);
		}
		bool Freed(size_t index) const {
			const auto *element = Element(index);
			return element && IsFreedCacheResultsSlot(*element);
		}
		bool Valid(uint32_t maximumDimension = Limits::MaximumDimension) const {
			if (!Array || !Array->Nested.empty() || Count() > Limits::MaximumArrayElements) return false;
			if (!Array->Items.empty()) {
				if (Array->ElementType != ValueType::Any || !Array->Elements.empty()) return false;
			} else if (Array->ElementType != ValueType::Image && Array->ElementType != ValueType::Struct)
				return false;
			for (size_t index = 0; index < Count(); ++index) {
				if (Freed(index)) {
					if (Array->Items.empty() && Array->ElementType != ValueType::Struct) return false;
					continue;
				}
				const auto *image = ImageAt(index);
				if (!image || (Array->Items.empty() && Array->ElementType != ValueType::Image) ||
					image->Format != SurfaceFormat::RGBA8Unorm ||
					!ValidSurfaceLayout(*image, maximumDimension, Limits::MaximumArrayBytes) ||
					!FiniteSurfaceSamples(*image))
					return false;
			}
			return true;
		}
		ElementValue Copy(size_t index) const {
			if (const auto *element = Element(index)) return *element;
			return SurfaceValue{*ImageAt(index)};
		}
	};
}
