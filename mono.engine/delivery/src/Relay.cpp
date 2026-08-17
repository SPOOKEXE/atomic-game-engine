#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/delivery/Relay.hpp>
#include <engine/net/Endpoint.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <utility>

// The two ends of a relayed route.
//
// **Neither end verifies on the other's behalf.** The asking half hands what
// arrives to the ordinary fetch path, which checks the manifest's signature,
// then a group's length against that manifest, then every asset against its
// root - so a relay that served the wrong bytes is refused exactly where a
// compromised origin would be. The answering half checks a manifest against a
// publisher key when it was given one, which is `cdn::Origin::Accepts`' defence
// in depth rather than a boundary: the boundary is still the client's.

namespace engine::delivery {
	namespace {
		std::string ToHex(std::span<const std::byte> bytes) {
			static constexpr char DIGITS[] = "0123456789abcdef";
			std::string hex;
			hex.reserve(bytes.size() * 2);
			for (const std::byte value : bytes) {
				hex.push_back(DIGITS[(static_cast<unsigned>(value) >> 4) & 0xF]);
				hex.push_back(DIGITS[static_cast<unsigned>(value) & 0xF]);
			}
			return hex;
		}

		std::optional<std::vector<std::byte>> ReadWholeFile(const std::filesystem::path &path) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				return std::nullopt;
			}
			const auto size = static_cast<size_t>(file.tellg());
			std::vector<std::byte> bytes(size);
			file.seekg(0);
			file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
			if (!file) {
				return std::nullopt;
			}
			return bytes;
		}

		// Splits `signature || manifest` and checks the signature over the root.
		//
		// The same split `delivery::Client` makes, and it has to be: what a
		// publisher signed is a file, so a relay checking a re-serialisation
		// would be checking something the publisher never wrote.
		bool ManifestVerifies(const std::vector<std::byte> &body, const assets::PublicKey &publisher) {
			if (body.size() <= assets::SignatureBytes::BYTES) {
				return false;
			}
			assets::SignatureBytes signature;
			std::memcpy(signature.Value.data(), body.data(), assets::SignatureBytes::BYTES);
			core::ByteReader reader(
				std::span<const std::byte>(
					body.data() + assets::SignatureBytes::BYTES, body.size() - assets::SignatureBytes::BYTES
				)
			);
			const std::optional<assets::Manifest> manifest = assets::Manifest::Read(reader);
			if (!manifest) {
				return false;
			}
			return assets::VerifyManifestRoot(manifest->Root(), signature, publisher);
		}

		// --- the asking half ------------------------------------------------

		class RelayFetch final : public net::http::Client {
		  public:
			RelayFetch(RelayChannel &channel, uint32_t idlePolls) : Carrier(&channel), Idle(idlePolls) {}

			net::http::FetchId
			Submit(const net::Endpoint &, const net::http::Request &request, std::string_view) override {
				// A relay carries the three routes an origin serves and nothing
				// else, so a target that is not one of them is refused here
				// rather than sent and refused at the far end - the far end is
				// somebody else's process and a round trip is what it costs.
				if (request.Verb != net::http::Method::Get || !RelayableRoute(request.Target)) {
					return {};
				}
				if (Live.size() >= MAXIMUM_OUTSTANDING) {
					return {};
				}

				Entry entry;
				entry.Id = NextFetch++;
				entry.Route = request.Target;
				entry.Asked = Carrier->Ask(entry.Id, entry.Route);
				Live.push_back(std::move(entry));
				return net::http::FetchId{.Value = Live.back().Id};
			}

			net::http::FetchState StateOf(net::http::FetchId id) const override {
				const Entry *const entry = Find(id.Value);
				return entry != nullptr ? entry->State : net::http::FetchState::Unknown;
			}

			size_t Pump() override {
				size_t finished = 0;

				std::vector<RelayAnswer> arrived;
				Carrier->Collect(arrived);
				for (const RelayAnswer &answer : arrived) {
					Entry *const entry = Find(answer.Ticket);
					if (entry == nullptr || entry->State != net::http::FetchState::Pending) {
						continue;
					}
					entry->State =
						answer.Served ? net::http::FetchState::Ready : net::http::FetchState::Failed;
					entry->Body = answer.Bytes;
					Received += answer.Bytes.size();
					++finished;
				}

				for (Entry &entry : Live) {
					if (entry.State != net::http::FetchState::Pending) {
						continue;
					}
					if (!entry.Asked) {
						// The link would not take it. Ordinary backpressure -
						// offered again rather than counted as a failure, which
						// is what `Link::Reserve` refusing already means
						// everywhere else in this engine.
						entry.Asked = Carrier->Ask(entry.Id, entry.Route);
					}
					if (Idle != 0 && ++entry.Waited >= Idle) {
						entry.State = net::http::FetchState::Failed;
						Carrier->Abandon(entry.Id);
						++finished;
					}
				}
				return finished;
			}

			std::optional<net::http::Response> Take(net::http::FetchId id) override {
				for (auto entry = Live.begin(); entry != Live.end(); ++entry) {
					if (entry->Id != id.Value) {
						continue;
					}
					if (entry->State != net::http::FetchState::Ready) {
						return std::nullopt;
					}
					net::http::Response answer;
					answer.Code = net::http::Status::Ok;
					answer.Body = std::move(entry->Body);
					Live.erase(entry);
					return answer;
				}
				return std::nullopt;
			}

			bool Cancel(net::http::FetchId id) override {
				for (auto entry = Live.begin(); entry != Live.end(); ++entry) {
					if (entry->Id != id.Value) {
						continue;
					}
					const bool live = entry->State == net::http::FetchState::Pending;
					Carrier->Abandon(entry->Id);
					Live.erase(entry);
					return live;
				}
				return false;
			}

			size_t Outstanding() const override {
				return Live.size();
			}

			uint64_t ReceivedBytes() const override {
				return Received;
			}

		  private:
			// How many routes may be asked for at once.
			//
			// A relay shares a link with a simulation, so this is smaller than a
			// socket client's: every outstanding route is bytes the far end is
			// taking out of a per-tick budget the game is also spending.
			static constexpr size_t MAXIMUM_OUTSTANDING = 4;

			struct Entry {
				uint64_t Id = 0;
				std::string Route;
				bool Asked = false;
				uint32_t Waited = 0;
				net::http::FetchState State = net::http::FetchState::Pending;
				std::vector<std::byte> Body;
			};

			Entry *Find(uint64_t ticket) {
				for (Entry &entry : Live) {
					if (entry.Id == ticket) {
						return &entry;
					}
				}
				return nullptr;
			}

			const Entry *Find(uint64_t ticket) const {
				for (const Entry &entry : Live) {
					if (entry.Id == ticket) {
						return &entry;
					}
				}
				return nullptr;
			}

			RelayChannel *Carrier;
			uint32_t Idle;
			std::vector<Entry> Live;
			uint64_t NextFetch = 1;
			uint64_t Received = 0;
		};

		// --- the answering half ----------------------------------------------

		// One source, opened as far as it can be before anything is asked for.
		struct Attached {
			Source Descriptor;

			// For a Directory source: the store, and what it holds.
			std::optional<assets::ChunkStore> Store;
			std::optional<assets::Manifest> Catalogue;
			std::optional<Dictionary> Codebook;
			bool Opened = false;

			// For an Http source: where to reach it.
			net::Endpoint Address;
			std::string Host;
		};

		class Fetcher final : public RouteFetcher {
		  public:
			Fetcher(DeliverySettings settings, const RouteFetcherSettings &limits)
				: Config(std::move(settings)), Limits(limits) {
				net::http::ClientSettings transfer;
				transfer.MaximumOutstanding = limits.MaximumOutstanding + 2;
				transfer.IdlePolls = limits.IdlePolls;
				Transfer = net::http::MakeClient(transfer);

				for (const Source &source : Config.Usable()) {
					Attached attached;
					attached.Descriptor = source;
					if (source.Kind == SourceKind::Directory) {
						attached.Store = assets::ChunkStore::Open(source.Location, false);
						if (!attached.Store) {
							// Said once at start-up rather than as a stream of
							// individually plausible refusals at request rate -
							// `ContentRoot::Mount`'s rule.
							ENGINE_WARN(
								"relay: source '{}' is not a content store at {}",
								source.Name,
								source.Location
							);
							continue;
						}
					} else if (source.Kind == SourceKind::Http) {
						const std::optional<net::Endpoint> address = net::Endpoint::Parse(source.Location);
						if (!address) {
							ENGINE_WARN(
								"relay: source '{}' is not an address - {} (a host name needs resolving "
								"before it gets here)",
								source.Name,
								source.Location
							);
							continue;
						}
						attached.Address = *address;
						attached.Host = source.Location;
					} else {
						// A relay of a relay is a hop nothing owns: this end has
						// no channel to ask through, and pretending otherwise
						// would refuse every route with no explanation.
						ENGINE_WARN("relay: source '{}' is itself a relay and cannot be one", source.Name);
						continue;
					}
					Sources.push_back(std::move(attached));
				}
			}

			void UseGrant(std::span<const std::byte> token) override {
				Grant.assign(token.begin(), token.end());
			}

			uint64_t Request(std::string_view route) override {
				if (!RelayableRoute(route) || Sources.empty()) {
					return 0;
				}
				if (Requests.size() >= Limits.MaximumOutstanding) {
					return 0;
				}
				Live request;
				request.Route = std::string(route);
				const uint64_t ticket = NextTicket++;
				Requests.emplace_back(ticket, std::move(request));
				return ticket;
			}

			size_t Pump() override {
				size_t finished = 0;
				for (auto &entry : Requests) {
					Live &request = entry.second;
					if (request.State != RouteState::Pending || request.Active) {
						continue;
					}
					finished += Start(request) ? 1 : 0;
				}

				Transfer->Pump();

				for (auto &entry : Requests) {
					Live &request = entry.second;
					if (request.State != RouteState::Pending || !request.Active) {
						continue;
					}
					finished += Advance(request) ? 1 : 0;
				}
				return finished;
			}

			RouteState StateOf(uint64_t ticket) const override {
				for (const auto &entry : Requests) {
					if (entry.first == ticket) {
						return entry.second.State;
					}
				}
				return RouteState::Unknown;
			}

			bool Take(uint64_t ticket, std::vector<std::byte> &bytes) override {
				for (auto entry = Requests.begin(); entry != Requests.end(); ++entry) {
					if (entry->first != ticket) {
						continue;
					}
					if (entry->second.State == RouteState::Pending) {
						return false;
					}
					if (entry->second.State == RouteState::Ready) {
						bytes = std::move(entry->second.Bytes);
					}
					Requests.erase(entry);
					return true;
				}
				return false;
			}

			bool Cancel(uint64_t ticket) override {
				for (auto entry = Requests.begin(); entry != Requests.end(); ++entry) {
					if (entry->first != ticket) {
						continue;
					}
					if (entry->second.Active) {
						Transfer->Cancel(entry->second.Fetch);
					}
					Requests.erase(entry);
					return true;
				}
				return false;
			}

			size_t Outstanding() const override {
				return Requests.size();
			}

			const RouteCounters &Counters() const override {
				return Tally;
			}

		  private:
			struct Live {
				std::string Route;
				size_t SourceIndex = 0;
				net::http::FetchId Fetch{};
				bool Active = false;
				RouteState State = RouteState::Pending;
				std::vector<std::byte> Bytes;
			};

			// Opens a directory source's manifest and dictionary, once.
			void Open(Attached &source) {
				if (source.Opened) {
					return;
				}
				source.Opened = true;
				assets::SignatureBytes signature;
				source.Catalogue = source.Store->ReadManifest(signature);
				if (std::optional<std::vector<std::byte>> bytes = source.Store->ReadDictionary()) {
					source.Codebook = Dictionary::Load(*bytes);
				}
			}

			// @return Whether the request finished on this call.
			bool Start(Live &request) {
				while (request.SourceIndex < Sources.size()) {
					Attached &source = Sources[request.SourceIndex];
					if (source.Store) {
						std::vector<std::byte> bytes;
						if (FromStore(source, request.Route, bytes)) {
							request.Bytes = std::move(bytes);
							request.State = RouteState::Ready;
							++Tally.Served;
							Tally.ServedBytes += request.Bytes.size();
							return true;
						}
						++Tally.SourceFailures;
						++request.SourceIndex;
						continue;
					}

					net::http::Request ask;
					ask.Verb = net::http::Method::Get;
					ask.Target = request.Route;
					if (!Grant.empty()) {
						ask.Headers.push_back(
							net::http::Header{.Name = "x-atomic-grant", .Value = ToHex(Grant)}
						);
					}
					request.Fetch = Transfer->Submit(source.Address, ask, source.Host);
					if (request.Fetch.IsValid()) {
						request.Active = true;
						return false;
					}
					// The transfer client is full. Offered again on the next
					// pump rather than burning a perfectly good source because
					// this end was busy.
					return false;
				}
				request.State = RouteState::Refused;
				++Tally.Refused;
				return true;
			}

			// @return Whether the request finished on this call.
			bool Advance(Live &request) {
				const net::http::FetchState state = Transfer->StateOf(request.Fetch);
				if (state == net::http::FetchState::Pending) {
					return false;
				}

				const std::optional<net::http::Response> answer = Transfer->Take(request.Fetch);
				request.Active = false;
				request.Fetch = {};

				if (answer && answer->Code == net::http::Status::Ok && Accepts(request.Route, answer->Body)) {
					request.Bytes = answer->Body;
					request.State = RouteState::Ready;
					++Tally.Served;
					Tally.ServedBytes += request.Bytes.size();
					return true;
				}

				++Tally.SourceFailures;
				++request.SourceIndex;
				if (request.SourceIndex >= Sources.size()) {
					request.State = RouteState::Refused;
					++Tally.Refused;
					return true;
				}
				return false;
			}

			// Whether a manifest a source served may be passed on.
			//
			// Only the manifest is checked, and only when a publisher key was
			// configured. Everything else a source serves is bound *by* that
			// manifest at the far end, so a second check here would be a second
			// implementation of one that already exists where it matters.
			bool Accepts(const std::string &route, const std::vector<std::byte> &body) {
				if (route != MANIFEST_ROUTE || Config.Publisher.IsZero()) {
					return true;
				}
				if (ManifestVerifies(body, Config.Publisher)) {
					return true;
				}
				++Tally.VerificationFailures;
				ENGINE_WARN("relay: a source served a manifest that did not verify - passing it over");
				return false;
			}

			// Answers one route out of a local store.
			bool FromStore(Attached &source, const std::string &route, std::vector<std::byte> &bytes) {
				Open(source);

				if (route == MANIFEST_ROUTE) {
					// **The published file, verbatim.** It is the bytes the
					// publisher signed, so serving a re-serialisation that is
					// *supposed* to be identical would make a client verify
					// something nobody signed - `cdn::Service` reads it the same
					// way for the same reason.
					std::optional<std::vector<std::byte>> file =
						ReadWholeFile(source.Store->Directory() / assets::ChunkStore::MANIFEST_FILE);
					if (!file || !Accepts(route, *file)) {
						return false;
					}
					bytes = std::move(*file);
					return true;
				}

				if (route == DICTIONARY_ROUTE) {
					std::optional<std::vector<std::byte>> codebook = source.Store->ReadDictionary();
					if (!codebook || codebook->empty()) {
						return false;
					}
					bytes = std::move(*codebook);
					return true;
				}

				const std::optional<assets::ContentHash> root = BundleOfRoute(route);
				if (!root || !source.Catalogue) {
					return false;
				}
				const assets::BundleEntry *found = nullptr;
				for (const assets::BundleEntry &bundle : source.Catalogue->Bundles()) {
					if (bundle.Root == *root) {
						found = &bundle;
						break;
					}
				}
				if (found == nullptr) {
					return false;
				}
				const std::optional<std::vector<std::byte>> payload =
					source.Store->ReadBundle(*source.Catalogue, *found);
				if (!payload) {
					return false;
				}

				// **A store holds chunks and a relay carries a frame**, so the
				// compression happens here - at the same level and against the
				// same dictionary `cdn::Origin` would have used, because a
				// relayed group and a served one have to be the same artefact.
				std::optional<std::vector<std::byte>> frame =
					source.Codebook
						? GroupCodec::Compress(*payload, *source.Codebook, Limits.CompressionLevel)
						: GroupCodec::Compress(*payload, Limits.CompressionLevel);
				if (!frame) {
					return false;
				}
				bytes = std::move(*frame);
				return true;
			}

			DeliverySettings Config;
			RouteFetcherSettings Limits;
			std::vector<Attached> Sources;
			std::unique_ptr<net::http::Client> Transfer;
			std::vector<std::byte> Grant;
			std::vector<std::pair<uint64_t, Live>> Requests;
			uint64_t NextTicket = 1;
			RouteCounters Tally;
		};
	}

	std::string BundleRoute(const assets::ContentHash &bundle) {
		return std::string(BUNDLE_ROUTE_PREFIX) + bundle.ToHex();
	}

	std::optional<assets::ContentHash> BundleOfRoute(std::string_view route) {
		if (!route.starts_with(BUNDLE_ROUTE_PREFIX)) {
			return std::nullopt;
		}
		return assets::ContentHash::FromHex(route.substr(BUNDLE_ROUTE_PREFIX.size()));
	}

	bool RelayableRoute(std::string_view route) {
		if (route == MANIFEST_ROUTE || route == DICTIONARY_ROUTE) {
			return true;
		}
		return BundleOfRoute(route).has_value();
	}

	const char *Describe(RouteState state) {
		switch (state) {
		case RouteState::Unknown:
			return "unknown";
		case RouteState::Pending:
			return "pending";
		case RouteState::Ready:
			return "ready";
		case RouteState::Refused:
			return "refused";
		}
		return "unknown";
	}

	std::unique_ptr<net::http::Client> MakeRelayClient(RelayChannel &channel, uint32_t idlePolls) {
		return std::make_unique<RelayFetch>(channel, idlePolls);
	}

	std::unique_ptr<RouteFetcher>
	MakeRouteFetcher(const DeliverySettings &settings, const RouteFetcherSettings &limits) {
		if (settings.Usable().empty()) {
			// Refused rather than half-applied, `MakeAssetClient`'s reason: a
			// relay with nowhere to fetch from would refuse every route and look
			// exactly like an origin that is down.
			ENGINE_ERROR("relay: settings name no usable content source");
			return nullptr;
		}
		return std::make_unique<Fetcher>(settings, limits);
	}
}
