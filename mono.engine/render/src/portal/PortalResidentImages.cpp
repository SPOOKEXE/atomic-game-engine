#include <engine/render/PortalResidentImages.hpp>
#include <engine/render/Renderer.hpp>

#include <algorithm>

namespace engine::render {
	namespace {
		constexpr auto TIMEOUT = std::chrono::seconds(1);
		bool Valid(const world::PresentationAddress &endpoint) {
			const auto text = [](std::string_view value) {
				return !value.empty() && value.size() <= world::MAX_PRESENTATION_NAME &&
					   value.find('\0') == std::string_view::npos;
			};
			return endpoint.Session != 0 && endpoint.Generation != 0 && text(endpoint.World) &&
				   text(endpoint.Channel);
		}
	}
	PortalResidentImages::PortalResidentImages(Renderer &renderer) : Render(renderer) {}
	PortalResidentImages::~PortalResidentImages() {
		Clear();
	}
	bool PortalResidentImages::Owns(const Renderer &renderer) const {
		return &Render == &renderer;
	}
	bool PortalResidentImages::Expire(Time now) {
		if ((LastTime && now < *LastTime) || now > Time::max() - TIMEOUT) {
			return false;
		}
		LastTime = now;
		std::erase_if(Entries, [&](Entry &entry) {
			if (now < entry.Deadline) {
				return false;
			}
			if (entry.Token != 0) {
				Render.CancelResourceImage(entry.Token);
			}
			return true;
		});
		return true;
	}
	bool PortalResidentImages::Reserve(
		const world::PresentationAddress &source,
		const world::PresentationAddress &producer,
		const PortalImageRequest &request,
		const PortalImageBinding &binding,
		Time now
	) {
		if (!Valid(source) || !Valid(producer) || binding.WorldName.Text() != source.World ||
			binding.Portal.Text() != request.Key.PortalKey || binding.Expected != request.Key ||
			binding.ExpectedScope != request.Scope || binding.Index < 0 ||
			size_t(binding.Index) >= scene::MAX_SURFACES || !Expire(now) ||
			Entries.size() >= MAX_IMPORTED_PORTAL_IMAGES) {
			return false;
		}
		if (std::any_of(Entries.begin(), Entries.end(), [&](const Entry &entry) {
				return entry.Source == source && entry.Request.Key.RequestId == request.Key.RequestId;
			})) {
			return false;
		}
		std::vector<std::byte> bytes;
		std::string error;
		if (!EncodePortalImageRequest(request, bytes, error)) {
			return false;
		}
		Entries.push_back({source, producer, request, binding, {}, 0, now + TIMEOUT});
		return true;
	}
	bool PortalResidentImages::Contains(
		const world::PresentationAddress &source,
		const world::PresentationAddress &producer,
		const PortalImageRequest &request,
		Time now
	) {
		return Expire(now) && std::any_of(Entries.begin(), Entries.end(), [&](const Entry &entry) {
				   return entry.Source == source && entry.Producer == producer && entry.Request == request &&
						  entry.Token == 0;
			   });
	}
	bool PortalResidentImages::Publish(
		const world::PresentationAddress &source,
		const world::PresentationAddress &producer,
		const PortalResidentReceipt &receipt,
		uint64_t token,
		Time now
	) {
		if (!Expire(now) || !Render.CanPublishResourceImage(token, receipt.Width, receipt.Height) ||
			std::any_of(Entries.begin(), Entries.end(), [token](const Entry &entry) {
				return entry.Token == token;
			})) {
			return false;
		}
		for (auto &entry : Entries) {
			if (entry.Source != source || entry.Producer != producer || entry.Request.Key != receipt.Key ||
				entry.Request.Scope != receipt.Scope || entry.Request.Width != receipt.Width ||
				entry.Request.Height != receipt.Height || entry.Token != 0) {
				continue;
			}
			entry.Receipt = receipt;
			entry.Token = token;
			return true;
		}
		return false;
	}
	uint64_t PortalResidentImages::Take(
		const world::PresentationAddress &source,
		const world::PresentationAddress &producer,
		const PortalResidentReceipt &receipt,
		Time now
	) {
		if (!Expire(now)) {
			return 0;
		}
		for (auto entry = Entries.begin(); entry != Entries.end(); ++entry) {
			if (entry->Source != source || entry->Producer != producer || !entry->Receipt ||
				*entry->Receipt != receipt) {
				continue;
			}
			const auto handle = Render.AdoptResourceImage(entry->Token, entry->Binding);
			if (handle != 0) {
				Entries.erase(entry);
			}
			return handle;
		}
		return 0;
	}
	void PortalResidentImages::Cancel(const world::PresentationAddress &source, uint64_t requestId) {
		std::erase_if(Entries, [&](Entry &entry) {
			if (entry.Source != source || entry.Request.Key.RequestId != requestId) {
				return false;
			}
			if (entry.Token != 0) {
				Render.CancelResourceImage(entry.Token);
			}
			return true;
		});
	}
	void PortalResidentImages::Invalidate(const world::PresentationAddress &endpoint) {
		std::erase_if(Entries, [&](Entry &entry) {
			if (entry.Source != endpoint && entry.Producer != endpoint) {
				return false;
			}
			if (entry.Token != 0) {
				Render.CancelResourceImage(entry.Token);
			}
			return true;
		});
	}
	void PortalResidentImages::Clear() {
		for (auto &entry : Entries) {
			if (entry.Token != 0) {
				Render.CancelResourceImage(entry.Token);
			}
		}
		Entries.clear();
	}
}
