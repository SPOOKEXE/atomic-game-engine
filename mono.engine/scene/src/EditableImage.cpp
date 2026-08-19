#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace engine::scene {

	namespace {
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
				const float dstColour = static_cast<float>(under) / 255.0f;
				const float outColour =
					(over * srcAlpha + dstColour * dstAlpha * (1.0f - srcAlpha)) / outAlpha;
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

		ecs::ClassId RegisterEditableImageClass() {
			EnsureClassTree();
			const ecs::ClassId instance = ecs::Classes::Find(core::Name("Instance"));

			// An `Instance` and not a `PVInstance`, `EditableMesh`'s reason:
			// this has no place of its own in the world.
			const std::array components{ecs::Components::Of<EditableImage>()};
			const ecs::ClassId editableImage = ecs::Classes::Register("EditableImage", instance, components);

			ecs::Classes::Computed(editableImage, SizeProperty());
			ecs::Classes::Computed(editableImage, ContentIdProperty());
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
		image->Pixels.assign(static_cast<size_t>(width) * height * 4, 0);
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
