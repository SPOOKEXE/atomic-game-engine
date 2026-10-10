#include "EditablePackingProperties.hpp"

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace engine::scene {

	namespace {
		bool EditableImageMutationAllowed(const ecs::Store &store, ecs::Entity instance) {
			return !store.AdoptOnly() ||
				   (ecs::Store::IsPredicted(instance) && ecs::IsClientLocalInstance(store, instance));
		}
		bool PixelByteCount(const EditableImage &image, size_t &bytes) {
			if (image.Width == 0 || image.Height == 0) {
				return false;
			}
			const uint64_t pixels = static_cast<uint64_t>(image.Width) * image.Height;
			if (pixels > MAXIMUM_EDITABLE_IMAGE_PIXELS) {
				return false;
			}
			bytes = static_cast<size_t>(pixels) * 4;
			return image.Pixels.size() == bytes;
		}

		// Writes one pixel with the ordinary Porter-Duff "over" operator,
		// straight (not premultiplied) alpha in and out. Out-of-range
		// coordinates are silently skipped - every caller has already
		// clipped its own loop bounds to the image, so this is the second
		// line of defence rather than the first.
		//
		// **Coverage accumulates rather than decaying towards the new
		// draw's alpha.** A half-transparent black rectangle drawn over an
		// opaque white one darkens it and leaves it opaque - two draws that
		// each leave *something* showing must not compose into a pixel more
		// transparent than either, which is what a plain per-channel lerp
		// against `under` would do the moment `under`'s own alpha was not
		// 255.
		void Blend(EditableImage &image, int32_t x, int32_t y, const core::Color3 &colour, float alpha) {
			if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= image.Width ||
				static_cast<uint32_t>(y) >= image.Height) {
				return;
			}
			const size_t offset = (static_cast<size_t>(y) * image.Width + static_cast<size_t>(x)) * 4;

			const float srcAlpha = std::clamp(alpha, 0.0f, 1.0f);
			const float dstAlpha = static_cast<float>(image.Pixels[offset + 3]) / 255.0f;
			const float outAlpha = srcAlpha + dstAlpha * (1.0f - srcAlpha);

			image.Pixels[offset + 3] = static_cast<uint8_t>(std::clamp(outAlpha * 255.0f, 0.0f, 255.0f));
			if (outAlpha <= 0.0f) {
				// Fully transparent either way - the colour channels of a
				// pixel nothing has ever covered are meaningless, so this
				// leaves them rather than dividing by zero below.
				return;
			}

			const auto mix = [&](uint8_t under, float over) {
				const float dstColour = image.Space == EditableImageSpace::SRGB
											? core::Color3::FromRGB(under, 0, 0).R
											: static_cast<float>(under) / 255.0f;
				const float outColour =
					(over * srcAlpha + dstColour * dstAlpha * (1.0f - srcAlpha)) / outAlpha;
				if (image.Space == EditableImageSpace::SRGB) {
					const float linear = std::clamp(outColour, 0.0f, 1.0f);
					const float encoded = linear <= .0031308f
											  ? linear * 12.92f
											  : 1.055f * std::pow(linear, 1.0f / 2.4f) - .055f;
					return static_cast<uint8_t>(std::lround(encoded * 255.0f));
				}
				return static_cast<uint8_t>(std::clamp(outColour * 255.0f, 0.0f, 255.0f));
			};
			image.Pixels[offset + 0] = mix(image.Pixels[offset + 0], colour.R);
			image.Pixels[offset + 1] = mix(image.Pixels[offset + 1], colour.G);
			image.Pixels[offset + 2] = mix(image.Pixels[offset + 2], colour.B);
		}

		ecs::PropertyDescriptor SizeProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Size");
			property.Type = ecs::PropertyType::Vector2;
			property.Size = sizeof(core::Vector2);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<EditableImage>()});
			property.Writable = false;
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const EditableImage *held = store.Get<EditableImage>(instance);
				if (held == nullptr) {
					return false;
				}
				*static_cast<core::Vector2 *>(out) =
					core::Vector2{static_cast<float>(held->Width), static_cast<float>(held->Height)};
				return true;
			};
			return property;
		}

		ecs::PropertyDescriptor ContentIdProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("ContentId");
			property.Type = ecs::PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<EditableImage>()});
			property.Writable = false;
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				if (store.Get<EditableImage>(instance) == nullptr) {
					return false;
				}
				*static_cast<core::Name *>(out) = EditableImageContentName(store, instance);
				return true;
			};
			return property;
		}

		ecs::PropertyDescriptor ColorSpaceProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("ColorSpace");
			property.Type = ecs::PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Reads = property.Writes =
				&ecs::ComponentSet::Intern({ecs::Components::Of<EditableImage>()});
			property.Get = [](const ecs::Store &store, ecs::Entity entity, void *out) {
				const auto *image = store.Get<EditableImage>(entity);
				if (image == nullptr ||
					(image->Space != EditableImageSpace::Linear && image->Space != EditableImageSpace::SRGB))
					return false;
				*static_cast<core::Name *>(out) =
					core::Name(image->Space == EditableImageSpace::SRGB ? "srgb" : "linear");
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity entity, const void *value) {
				const auto name = static_cast<const core::Name *>(value)->Text();
				if (name != "linear" && name != "srgb") return false;
				if (!SetEditableImageSpace(
						store, entity, name == "srgb" ? EditableImageSpace::SRGB : EditableImageSpace::Linear
					))
					return false;
				(void)store.GetMutable<EditableImage>(entity);
				return true;
			};
			return property;
		}

		ecs::ClassId RegisterEditableImageClass() {
			EnsureClassTree();
			const ecs::ClassId instance = ecs::Classes::Find(core::Name("Instance"));

			// An `Instance` and not a `PVInstance`, `EditableMesh`'s reason:
			// this has no place of its own in the world.
			const std::array components{ecs::Components::Of<EditableImage>()};
			const ecs::ClassId editableImage = ecs::Classes::Register("EditableImage", instance, components);

			ecs::Classes::Computed(editableImage, SizeProperty());
			ecs::Classes::Computed(editableImage, ContentIdProperty());
			ecs::Classes::Computed(editableImage, ColorSpaceProperty());
			for (auto &property : detail::PackingProperties<EditableImage, SetEditableImagePacking>())
				ecs::Classes::Computed(editableImage, std::move(property));
			for (const ecs::PropertyDescriptor &property : ecs::Classes::Describe(editableImage).Properties)
				(void)ecs::Classes::SetPropertiesTag(editableImage, property.Spelling, core::Name("Image"));
			(void)ecs::Classes::SetPropertiesTag(editableImage, "Size", core::Name("Geometry"));
			(void)ecs::Classes::SetPropertiesTag(editableImage, "ContentId", core::Name("Identity"));
			(void)ecs::Classes::SetPropertiesTag(editableImage, "ColorSpace", core::Name("Image"));
			return editableImage;
		}
	}

	core::Name EditableImageContentName(const ecs::Store &store, ecs::Entity instance) {
		if (store.Get<EditableImage>(instance) == nullptr) {
			return {};
		}
		return core::Name("editable-image://" + std::to_string(instance.Id));
	}

	bool ResizeEditableImage(ecs::Store &store, ecs::Entity instance, uint32_t width, uint32_t height) {
		if (!EditableImageMutationAllowed(store, instance)) return false;
		EditableImage *image = store.GetMutable<EditableImage>(instance);
		if (image == nullptr) {
			return false;
		}

		width = std::max(width, 1u);
		height = std::max(height, 1u);
		if (static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > MAXIMUM_EDITABLE_IMAGE_PIXELS) {
			return false;
		}

		image->Width = width;
		image->Height = height;
		image->Space = EditableImageSpace::Linear;
		image->Pixels.assign(static_cast<size_t>(width) * height * 4, 0);
		image->Revision++;
		return true;
	}

	std::vector<std::byte> EditableImageToBuffer(const ecs::Store &store, ecs::Entity instance) {
		const EditableImage *image = store.Get<EditableImage>(instance);
		size_t bytes = 0;
		if (image == nullptr || !PixelByteCount(*image, bytes)) {
			return {};
		}

		std::vector<std::byte> copy(bytes);
		std::transform(image->Pixels.begin(), image->Pixels.end(), copy.begin(), [](uint8_t byte) {
			return static_cast<std::byte>(byte);
		});
		return copy;
	}

	bool EditableImageFromBuffer(ecs::Store &store, ecs::Entity instance, std::span<const std::byte> pixels) {
		if (!EditableImageMutationAllowed(store, instance)) return false;
		EditableImage *image = store.GetMutable<EditableImage>(instance);
		size_t expected = 0;
		if (image == nullptr || !PixelByteCount(*image, expected) || pixels.size() != expected) {
			return false;
		}

		const bool changed = !std::equal(
			pixels.begin(), pixels.end(), image->Pixels.begin(), [](std::byte source, uint8_t stored) {
				return source == static_cast<std::byte>(stored);
			}
		);
		if (!changed) {
			return true;
		}

		std::transform(pixels.begin(), pixels.end(), image->Pixels.begin(), [](std::byte byte) {
			return std::to_integer<uint8_t>(byte);
		});
		image->Revision++;
		return true;
	}

	bool SetEditableImagePixels(
		ecs::Store &store,
		ecs::Entity instance,
		uint32_t width,
		uint32_t height,
		std::span<const std::byte> pixels,
		EditableImageSpace space
	) {
		if (!EditableImageMutationAllowed(store, instance) || width == 0 || height == 0 ||
			width > MAXIMUM_EDITABLE_IMAGE_IMPORT_WIDTH || height > MAXIMUM_EDITABLE_IMAGE_IMPORT_HEIGHT ||
			(space != EditableImageSpace::Linear && space != EditableImageSpace::SRGB) ||
			pixels.size() != static_cast<size_t>(width) * height * 4 ||
			pixels.size() > MAXIMUM_EDITABLE_IMAGE_IMPORT_BYTES)
			return false;
		const auto *held = store.Get<EditableImage>(instance);
		if (held == nullptr) return false;
		if (held->Width == width && held->Height == height && held->Space == space &&
			std::equal(
				pixels.begin(),
				pixels.end(),
				held->Pixels.begin(),
				held->Pixels.end(),
				[](std::byte source, uint8_t stored) { return std::to_integer<uint8_t>(source) == stored; }
			))
			return true;
		std::vector<uint8_t> replacement(pixels.size());
		std::transform(pixels.begin(), pixels.end(), replacement.begin(), [](std::byte byte) {
			return std::to_integer<uint8_t>(byte);
		});
		auto *image = store.GetMutable<EditableImage>(instance);
		image->Width = width;
		image->Height = height;
		image->Space = space;
		image->Pixels = std::move(replacement);
		++image->Revision;
		return true;
	}

	bool SetEditableImageSpace(ecs::Store &store, ecs::Entity instance, EditableImageSpace space) {
		if (!EditableImageMutationAllowed(store, instance) ||
			(space != EditableImageSpace::Linear && space != EditableImageSpace::SRGB))
			return false;
		const auto *held = store.Get<EditableImage>(instance);
		if (held == nullptr) return false;
		if (held->Space == space) return true;
		auto *image = store.GetMutable<EditableImage>(instance);
		image->Space = space;
		++image->Revision;
		return true;
	}

	bool SetEditableImagePacking(ecs::Store &store, ecs::Entity instance, const EditablePacking &packing) {
		if (!EditableImageMutationAllowed(store, instance)) return false;
		EditableImage *image = store.GetMutable<EditableImage>(instance);
		constexpr uint8_t attributes = static_cast<uint8_t>(EditablePackingAttribute::Colour) |
									   static_cast<uint8_t>(EditablePackingAttribute::Alpha);
		if (image == nullptr ||
			static_cast<uint8_t>(packing.Format) > static_cast<uint8_t>(EditablePackingFormat::Boolean) ||
			(packing.Attributes & ~attributes) != 0 || !std::isfinite(packing.Minimum) ||
			!std::isfinite(packing.Maximum) || packing.Maximum < packing.Minimum)
			return false;
		if (image->Packing.Attributes == packing.Attributes && image->Packing.Format == packing.Format &&
			image->Packing.Minimum == packing.Minimum && image->Packing.Maximum == packing.Maximum)
			return true;
		image->Packing.Attributes = packing.Attributes;
		image->Packing.Format = packing.Format;
		image->Packing.Minimum = packing.Minimum;
		image->Packing.Maximum = packing.Maximum;
		image->Packing.Revision++;
		image->Revision++;
		return true;
	}

	bool DrawRectangle(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector2 &position,
		const core::Vector2 &size,
		const core::Color3 &colour,
		float transparency
	) {
		if (!EditableImageMutationAllowed(store, instance)) return false;
		EditableImage *image = store.GetMutable<EditableImage>(instance);
		if (image == nullptr) {
			return false;
		}

		const float alpha = 1.0f - std::clamp(transparency, 0.0f, 1.0f);
		const int32_t left = static_cast<int32_t>(std::floor(position.X));
		const int32_t top = static_cast<int32_t>(std::floor(position.Y));
		const int32_t right = static_cast<int32_t>(std::ceil(position.X + size.X));
		const int32_t bottom = static_cast<int32_t>(std::ceil(position.Y + size.Y));

		for (int32_t y = std::max(0, top); y < std::min(bottom, static_cast<int32_t>(image->Height)); y++) {
			for (int32_t x = std::max(0, left); x < std::min(right, static_cast<int32_t>(image->Width));
				 x++) {
				Blend(*image, x, y, colour, alpha);
			}
		}

		image->Revision++;
		return true;
	}

	bool DrawLine(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector2 &from,
		const core::Vector2 &to,
		const core::Color3 &colour,
		float transparency
	) {
		if (!EditableImageMutationAllowed(store, instance)) return false;
		EditableImage *image = store.GetMutable<EditableImage>(instance);
		if (image == nullptr) {
			return false;
		}

		const float alpha = 1.0f - std::clamp(transparency, 0.0f, 1.0f);

		// Bresenham's integer line, the same shape every textbook gives -
		// no floating accumulation, so a very long line loses no precision
		// pixel to pixel.
		int32_t x0 = static_cast<int32_t>(std::lround(from.X));
		int32_t y0 = static_cast<int32_t>(std::lround(from.Y));
		const int32_t x1 = static_cast<int32_t>(std::lround(to.X));
		const int32_t y1 = static_cast<int32_t>(std::lround(to.Y));

		const int32_t dx = std::abs(x1 - x0);
		const int32_t sx = x0 < x1 ? 1 : -1;
		const int32_t dy = -std::abs(y1 - y0);
		const int32_t sy = y0 < y1 ? 1 : -1;
		int32_t error = dx + dy;

		while (true) {
			Blend(*image, x0, y0, colour, alpha);
			if (x0 == x1 && y0 == y1) {
				break;
			}
			const int32_t doubled = 2 * error;
			if (doubled >= dy) {
				error += dy;
				x0 += sx;
			}
			if (doubled <= dx) {
				error += dx;
				y0 += sy;
			}
		}

		image->Revision++;
		return true;
	}

	bool DrawCircle(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector2 &centre,
		float radius,
		const core::Color3 &colour,
		float transparency
	) {
		if (!EditableImageMutationAllowed(store, instance)) return false;
		EditableImage *image = store.GetMutable<EditableImage>(instance);
		if (image == nullptr) {
			return false;
		}

		const float alpha = 1.0f - std::clamp(transparency, 0.0f, 1.0f);
		const int32_t r = static_cast<int32_t>(std::lround(std::max(radius, 0.0f)));
		const int32_t cx = static_cast<int32_t>(std::lround(centre.X));
		const int32_t cy = static_cast<int32_t>(std::lround(centre.Y));

		// **Filled, by a bounding-box scan against the squared radius**,
		// rather than an outline algorithm. A script drawing a dot or a
		// disc is the ordinary case for a paint tool, and a filled circle
		// is what `DrawCircle`'s own name promises without a second method
		// for the ring.
		const int64_t squared = static_cast<int64_t>(r) * r;
		for (int32_t y = std::max(0, cy - r); y <= std::min(cy + r, static_cast<int32_t>(image->Height) - 1);
			 y++) {
			for (int32_t x = std::max(0, cx - r);
				 x <= std::min(cx + r, static_cast<int32_t>(image->Width) - 1);
				 x++) {
				const int64_t dx = x - cx;
				const int64_t dy = y - cy;
				if (dx * dx + dy * dy <= squared) {
					Blend(*image, x, y, colour, alpha);
				}
			}
		}

		image->Revision++;
		return true;
	}

	ecs::ClassId EditableImageClass() {
		static const ecs::ClassId editableImage = RegisterEditableImageClass();
		return editableImage;
	}
}
