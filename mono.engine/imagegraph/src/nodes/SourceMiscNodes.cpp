#include "../ValuePayload.hpp"
#include "Families.hpp"

namespace engine::imagegraph::detail {
	namespace {
		const Image *PinSurface(const NodeContext &c, std::string_view port) {
			for (const auto &[id, image] : c.Images)
				if (id == port) return image;
			return nullptr;
		}
		bool PinImageTree(
			const std::vector<ImageArrayItem> &items,
			size_t images,
			size_t depth,
			uint64_t &count,
			uint64_t &bytes
		) {
			if (depth > Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements - count)
				return false;
			count += items.size();
			bytes += items.size() * sizeof(ImageArrayItem);
			for (const auto &item : items) {
				if (const auto *i = std::get_if<size_t>(&item.Data)) {
					if (*i >= images) return false;
				} else if (!PinImageTree(
							   std::get<std::vector<ImageArrayItem>>(item.Data),
							   images,
							   depth + 1,
							   count,
							   bytes
						   ))
					return false;
				if (bytes > Limits::MaximumArrayBytes) return false;
			}
			return true;
		}
		bool PinForward(NodeContext &c, std::string_view port) {
			if (const auto *image = PinSurface(c, port)) {
				auto *result = c.NewImage("out", image->Width, image->Height, image->Format);
				if (!result) return false;
				result->Pixels = image->Pixels;
				result->Hash = image->Hash;
			} else {
				for (const auto &[id, images] : c.ImageArrays)
					if (id == port && images) {
						if (images->Images.size() > Limits::MaximumArrayElements)
							return c.Fail(
								Status::LimitExceeded, "pin image array exceeds element limit", port
							);
						uint64_t count = 0,
								 bytes = sizeof(ImageArray) + images->Images.size() * sizeof(Image);
						if (!PinImageTree(images->Items, images->Images.size(), 1, count, bytes))
							return c.Fail(
								Status::LimitExceeded, "pin image array shape exceeds bounds", port
							);
						for (const auto &image : images->Images) {
							if (bytes > Limits::MaximumArrayBytes)
								return c.Fail(
									Status::LimitExceeded, "pin image array metadata exceeds bounds", port
								);
							if (!ValidSurfaceLayout(
									image, c.Request.MaximumImageDimension, Limits::MaximumArrayBytes
								) ||
								image.Pixels.size() > Limits::MaximumArrayBytes - bytes)
								return c.Fail(
									Status::LimitExceeded, "pin image array payload exceeds bounds", port
								);
							bytes += image.Pixels.size();
						}
						if (!c.ReserveOutput(bytes + std::string{}.capacity(), "out")) return false;
						c.OutputImageArrays.emplace_back("out", *images);
						return true;
					}
				const Value missing = double{0};
				const auto *value = c.Find(port);
				if (!value) value = &missing;
				const auto bytes = ValueClonePayloadBytes(*value);
				if (!bytes)
					return c.Fail(Status::LimitExceeded, "pin selected payload exceeds clone bounds", port);
				if (!c.ReserveOutput(*bytes, "out")) return false;
				c.SetValue("out", *value);
			}
			return c.FailureCode == Status::Ok;
		}
		bool Pin(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.pin");
			return PinForward(context, "in");
		}
	}
	std::span<const ExecutorEntry> SourceMiscExecutors() {
		static constexpr ExecutorEntry entries[] = {{"pc.pin", Pin, true}};
		return entries;
	}
}
