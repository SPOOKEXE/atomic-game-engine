#include <engine/render/PortalExchange.hpp>
#include <engine/testing/Bench.hpp>

#include <stdexcept>

TEST_SUITE_ID("engine.render.bench.portal-exchange")

namespace {
	using namespace engine::render;
	constexpr size_t BATCHES = 32;
	enum class Delivery { Copied, Resident, Renewal };

	struct Messages {
		PortalImageReply Image;
		PortalResidentReceipt Receipt;
		Messages() {
			Image.Key = {1, "Workspace/Portal", 2, 3};
			Image.Status = PortalImageStatus::Ok;
			Image.CaptureTick = 4;
			Image.ContentRevision = 5;
			Image.LightingRevision = 6;
			Image.Width = Image.Height = 512;
			Image.RowStride = Image.Width * 8;
			Image.Pixels.resize(size_t(Image.RowStride) * Image.Height);
			Image.PixelHash = engine::assets::Hasher::Of(Image.Pixels);
			Receipt = {
				Image.Key,
				Image.Scope,
				Image.CaptureTick,
				Image.ContentRevision,
				Image.LightingRevision,
				Image.Width,
				Image.Height
			};
		}
	};

	void RoundTrips(size_t views, Delivery delivery) {
		static const Messages messages;
		for (size_t batch = 0; batch < BATCHES; ++batch) {
			for (size_t view = 0; view < views; ++view) {
				std::vector<std::byte> wire;
				std::string error;
				if (delivery == Delivery::Copied) {
					PortalImageReply decoded;
					if (!EncodePortalImageReply(messages.Image, wire, error) ||
						!DecodePortalImageReply(wire, decoded, error) ||
						decoded.PixelHash != messages.Image.PixelHash ||
						decoded.Pixels.size() != messages.Image.Pixels.size()) {
						throw std::runtime_error("portal copied benchmark failed: " + error);
					}
					engine::testing::Consume(decoded.PixelHash);
				} else {
					PortalResidentReceipt decoded;
					const bool renewal = delivery == Delivery::Renewal;
					const bool encoded = renewal ? EncodePortalImageRenewal(messages.Receipt, wire, error)
												 : EncodePortalResidentReceipt(messages.Receipt, wire, error);
					const bool accepted =
						encoded && (renewal ? DecodePortalImageRenewal(wire, decoded, error)
											: DecodePortalResidentReceipt(wire, decoded, error));
					if (!accepted || decoded != messages.Receipt) {
						throw std::runtime_error("portal receipt benchmark failed: " + error);
					}
					engine::testing::Consume(decoded.ContentRevision);
				}
				engine::testing::Consume(wire.size());
			}
		}
	}
}

BENCH("Portal codec | 512x512 HDR | copied | 1 view batch", BATCHES) {
	RoundTrips(1, Delivery::Copied);
}
BENCH("Portal codec | 512x512 HDR | copied | 2 view batch", BATCHES) {
	RoundTrips(2, Delivery::Copied);
}
BENCH("Portal codec | 512x512 HDR | copied | 8 view batch", BATCHES) {
	RoundTrips(8, Delivery::Copied);
}
BENCH("Portal codec | 512x512 HDR | resident receipt | 1 view batch", BATCHES) {
	RoundTrips(1, Delivery::Resident);
}
BENCH("Portal codec | 512x512 HDR | resident receipt | 2 view batch", BATCHES) {
	RoundTrips(2, Delivery::Resident);
}
BENCH("Portal codec | 512x512 HDR | resident receipt | 8 view batch", BATCHES) {
	RoundTrips(8, Delivery::Resident);
}
BENCH("Portal codec | 512x512 HDR | renewal | 1 view batch", BATCHES) {
	RoundTrips(1, Delivery::Renewal);
}
BENCH("Portal codec | 512x512 HDR | renewal | 2 view batch", BATCHES) {
	RoundTrips(2, Delivery::Renewal);
}
BENCH("Portal codec | 512x512 HDR | renewal | 8 view batch", BATCHES) {
	RoundTrips(8, Delivery::Renewal);
}
