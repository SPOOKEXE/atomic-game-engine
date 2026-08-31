#include <engine/datastore/Http.hpp>
#include <engine/world/SharedStoreFile.hpp>

#include <array>
#include <utility>

namespace engine::datastore {
	namespace {
		std::string EncodedName(const std::string_view name) {
			constexpr std::array<char, 16> HEX{
				'0',
				'1',
				'2',
				'3',
				'4',
				'5',
				'6',
				'7',
				'8',
				'9',
				'a',
				'b',
				'c',
				'd',
				'e',
				'f',
			};
			std::string encoded(name.size() * 2, '0');
			for (size_t index = 0; index < name.size(); ++index) {
				const auto byte = static_cast<unsigned char>(name[index]);
				encoded[index * 2] = HEX[byte >> 4u];
				encoded[index * 2 + 1] = HEX[byte & 0x0fu];
			}
			return encoded;
		}

		bool Valid(const HttpDataStoreSettings &settings) {
			const auto headerValue = [](const std::string_view value) {
				return value.find_first_of("\r\n") == std::string_view::npos;
			};
			const auto target = [](const std::string_view value) {
				return value.find_first_of("\r\n ?#") == std::string_view::npos;
			};
			return settings.Server.IsValid() && !settings.Host.empty() &&
				   settings.TargetPrefix.starts_with('/') && settings.TargetPrefix.ends_with('/') &&
				   target(settings.TargetPrefix) && headerValue(settings.Host) &&
				   headerValue(settings.Authorization) && settings.MaximumPumpCalls > 0;
		}

		world::DataStoreStatus FromImageStatus(const world::SharedStoreFileStatus status) {
			switch (status) {
			case world::SharedStoreFileStatus::Ok:
				return world::DataStoreStatus::Ok;
			case world::SharedStoreFileStatus::NotFound:
				return world::DataStoreStatus::NotFound;
			case world::SharedStoreFileStatus::IoError:
				return world::DataStoreStatus::IoError;
			case world::SharedStoreFileStatus::Malformed:
			case world::SharedStoreFileStatus::WrongStore:
				return world::DataStoreStatus::Malformed;
			}
			return world::DataStoreStatus::Malformed;
		}

		class HttpDataStoreAdapter final : public world::DataStoreAdapter {
		  public:
			HttpDataStoreAdapter(HttpDataStoreSettings settings, std::unique_ptr<net::http::Client> client)
				: Settings(std::move(settings)), Transport(std::move(client)) {}

			world::DataStoreStatus Load(
				const core::Name store, std::vector<world::SharedStoreEntry> &entries, std::string &error
			) override {
				net::http::Request request;
				request.Verb = net::http::Method::Get;
				if (!Prepare(store, request, error)) {
					return world::DataStoreStatus::Refused;
				}
				std::optional<net::http::Response> response = Exchange(request, error);
				if (!response) {
					return world::DataStoreStatus::IoError;
				}
				if (response->Code == net::http::Status::NotFound) {
					return world::DataStoreStatus::NotFound;
				}
				if (response->Code != net::http::Status::Ok) {
					error = "datastore provider answered " +
							std::to_string(static_cast<uint16_t>(response->Code));
					return world::DataStoreStatus::IoError;
				}
				return FromImageStatus(
					world::DecodeSharedStoreImage(response->Body, world::BusKind::DataStore, entries, error)
				);
			}

			world::DataStoreStatus Save(
				const core::Name store,
				const std::span<const world::SharedStoreEntry> entries,
				std::string &error
			) override {
				net::http::Request request;
				request.Verb = net::http::Method::Put;
				if (!Prepare(store, request, error)) {
					return world::DataStoreStatus::Refused;
				}
				const world::SharedStoreFileStatus encoded =
					world::EncodeSharedStoreImage(world::BusKind::DataStore, entries, request.Body, error);
				if (encoded != world::SharedStoreFileStatus::Ok) {
					return FromImageStatus(encoded);
				}
				std::optional<net::http::Response> response = Exchange(request, error);
				if (!response) {
					return world::DataStoreStatus::IoError;
				}
				if (response->Code != net::http::Status::Ok) {
					error = "datastore provider answered " +
							std::to_string(static_cast<uint16_t>(response->Code));
					return world::DataStoreStatus::IoError;
				}
				return world::DataStoreStatus::Ok;
			}

		  private:
			bool Prepare(const core::Name store, net::http::Request &request, std::string &error) const {
				error.clear();
				if (!Valid(Settings) || Transport == nullptr || !store.IsValid() || store.Text().empty() ||
					store.Text().size() > world::MAXIMUM_DATASTORE_NAME_BYTES) {
					error = "invalid HTTP datastore configuration or name";
					return false;
				}
				request.Target = Settings.TargetPrefix + EncodedName(store.Text());
				request.Headers.push_back({"accept", "application/octet-stream"});
				if (!Settings.Authorization.empty()) {
					request.Headers.push_back({"authorization", Settings.Authorization});
				}
				return true;
			}

			std::optional<net::http::Response>
			Exchange(const net::http::Request &request, std::string &error) {
				const net::http::FetchId fetch = Transport->Submit(Settings.Server, request, Settings.Host);
				if (!fetch.IsValid()) {
					error = "HTTP datastore request was refused by the transport";
					return std::nullopt;
				}
				for (uint32_t pump = 0; pump < Settings.MaximumPumpCalls; ++pump) {
					const net::http::FetchState state = Transport->StateOf(fetch);
					if (state == net::http::FetchState::Ready) {
						return Transport->Take(fetch);
					}
					if (state == net::http::FetchState::Failed || state == net::http::FetchState::Unknown ||
						state == net::http::FetchState::Cancelled) {
						error = "HTTP datastore request failed";
						return std::nullopt;
					}
					Transport->Pump();
				}
				Transport->Cancel(fetch);
				error = "HTTP datastore request exceeded its pump budget";
				return std::nullopt;
			}

			HttpDataStoreSettings Settings;
			std::unique_ptr<net::http::Client> Transport;
		};
	}

	std::unique_ptr<world::DataStoreAdapter> MakeHttpDataStoreAdapter(HttpDataStoreSettings settings) {
		net::http::ClientSettings clientSettings;
		clientSettings.MaximumOutstanding = 1;
		clientSettings.IdlePolls = settings.MaximumPumpCalls;
		clientSettings.Limits.BodyBytes = world::MAXIMUM_SHARED_STORE_IMAGE_BYTES;
		return MakeHttpDataStoreAdapter(std::move(settings), net::http::MakeClient(clientSettings));
	}

	std::unique_ptr<world::DataStoreAdapter>
	MakeHttpDataStoreAdapter(HttpDataStoreSettings settings, std::unique_ptr<net::http::Client> client) {
		return std::make_unique<HttpDataStoreAdapter>(std::move(settings), std::move(client));
	}
}
