#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/HashTree.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/GroupCodec.hpp>
#include <engine/delivery/Relay.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Client.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

// The fetch path, and the order of its checks.
//
// **Nothing that arrives is believed before it is verified**, and the order the
// checks run in is the design rather than an implementation detail:
//
// 1. The manifest's signature, against the publisher key in the settings.
//    Nothing else happens until this passes, because everything below is
//    checked *against* the manifest and an unverified manifest checks nothing.
// 2. A group's decompressed length, against what the signed manifest says that
//    bundle weighs. Sized from the manifest and never from the frame, or a few
//    kilobytes on the wire become a multi-gigabyte allocation.
// 3. Every asset cut out of that group, against its root.
//
// A source that fails any of them is passed over and the next one is tried.
// That is what makes a compromised or stale origin able to withhold content and
// unable to substitute it.

namespace engine::delivery {
	namespace {
		// How many bundle fetches may be in flight.
		//
		// Groups stream concurrently so a slow one does not
		// hold up the others. Bounded rather than unlimited because each is a
		// socket and a decompression buffer.
		constexpr size_t MAXIMUM_BUNDLES_IN_FLIGHT = 4;

		// How many bundles a local store may be read out of in one pump.
		//
		// **One, because a store read is not a fetch and the two cost the caller
		// entirely different things.** A wire fetch is a socket: four of them in
		// flight cost this thread almost nothing, which is what
		// `MAXIMUM_BUNDLES_IN_FLIGHT` is sized for. A `dir:` source has no wire,
		// so `Start` reads the file, verifies every member against the signed
		// manifest and writes them all to the cache before it returns - measured
		// at about 10 ms a bundle in `release` on this repository's own store.
		// Four of those is a 40 ms frame, which is the hitch a game shows while
		// it loads, and it is the frame the editor and every dev build take.
		//
		// The cost of one is that a local load takes more pumps. That is the
		// trade the whole intake path already makes: `IntakeBudget` spends
		// latency to keep a frame's work bounded, and this is the same currency
		// one layer down.
		constexpr size_t MAXIMUM_STORE_BUNDLES_PER_PUMP = 1;

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

		// One source, resolved as far as it can be before anything is fetched.
		struct Resolved {
			Source Descriptor;
			// For a Directory source: the store, opened once.
			std::optional<assets::ChunkStore> Store;
			// For an Http source: where to reach it.
			net::Endpoint Address;
			std::string Host;

			// Who fetches for this source, and the reason it is per source
			// rather than one client for the whole list: an `Http` source is
			// fetched over a socket this module owns and a `Relay` source over
			// a link somebody else owns. Both wear `net::http::Client`, so the
			// manifest walk, the dictionary, the bundle jobs and the three
			// verification checks stay one path rather than two that agree
			// until they do not.
			//
			// Null for a `Directory`, which has no transport at all.
			net::http::Client *Fetcher = nullptr;
		};

		class Client final : public AssetClient {
		  public:
			Client(DeliverySettings settings, RelayChannel *relay) : Config(std::move(settings)) {
				if (!Config.CachePath.empty()) {
					Cached = ContentCache::Open(Config.CachePath, Config.CacheCapacityBytes);
				}

				net::http::ClientSettings transfer;
				transfer.MaximumOutstanding = MAXIMUM_BUNDLES_IN_FLIGHT + 2;
				transfer.IdlePolls = Config.IdlePolls;
				Transfer = net::http::MakeClient(transfer);
				if (relay != nullptr) {
					Relayed = MakeRelayClient(*relay, Config.IdlePolls);
				}

				for (const Source &source : Config.Usable()) {
					Resolved resolved;
					resolved.Descriptor = source;
					if (source.Kind == SourceKind::Directory) {
						resolved.Store = assets::ChunkStore::Open(source.Location, false);
						if (!resolved.Store) {
							// Reported once, at start-up, rather than as a
							// stream of individually plausible missing assets
							// at request rate - `ContentRoot::Mount`'s rule.
							ENGINE_WARN(
								"delivery: source '{}' is not a content store at {}",
								source.Name,
								source.Location
							);
							continue;
						}
					} else if (source.Kind == SourceKind::Relay) {
						if (!Relayed) {
							// One line rather than a refusal: the rest of the list is
							// still perfectly good, and a caller that configured a
							// relay while owning no link has a configuration problem
							// rather than a broken client.
							ENGINE_WARN(
								"delivery: source '{}' is a relay and this client carries no link",
								source.Name
							);
							continue;
						}
						resolved.Fetcher = Relayed.get();
					} else {
						const std::optional<net::Endpoint> address = net::Endpoint::Parse(source.Location);
						if (!address) {
							// `Endpoint::Parse` refuses a host *name* on
							// purpose: resolving one blocks on a network
							// service, and nothing on this path may block. A
							// name has to be resolved by whoever wrote the
							// configuration.
							ENGINE_WARN(
								"delivery: source '{}' is not an address - {} (a host name needs resolving "
								"before it gets here)",
								source.Name,
								source.Location
							);
							continue;
						}
						resolved.Address = *address;
						resolved.Host = source.Location;
						resolved.Fetcher = Transfer.get();
					}
					Sources.push_back(std::move(resolved));
				}
			}

			void UseGrant(std::span<const std::byte> token) override {
				Grant.assign(token.begin(), token.end());
			}

			bool Ready() const override {
				// Both halves. A manifest without the dictionary its groups
				// were compressed against is a catalogue this client can read
				// and nothing it can expand, and reporting that as ready would
				// make the first fetch fail for a reason nobody could see.
				return Known.has_value() && CodebookSettled;
			}

			const assets::Manifest *Catalogue() const override {
				return Known ? &*Known : nullptr;
			}

			RequestId Request(std::string_view name) override {
				Pending pending;
				pending.Name = std::string(name);
				pending.ByName = true;
				return Accept(std::move(pending));
			}

			RequestId RequestRoot(const assets::ContentHash &root) override {
				Pending pending;
				pending.Root = root;
				pending.ByName = false;
				return Accept(std::move(pending));
			}

			std::vector<RequestId> RequestKind(assets::AssetKind kind) override {
				std::vector<RequestId> issued;
				if (!Known) {
					// Empty means "not yet" rather than "none". A caller that
					// wants every texture asks again once Ready - which is the
					// honest answer, since the kinds live in the manifest.
					return issued;
				}
				for (const assets::AssetEntry *asset : Known->OfKind(kind)) {
					issued.push_back(Request(asset->Name));
				}
				return issued;
			}

			size_t Pump() override {
				ENGINE_PROFILE_CAT("delivery::AssetClient::Pump", core::ProfileCategory::Assets);

				size_t finished = 0;
				if (!Known) {
					AcquireCatalogue();
				}
				if (Known && !CodebookSettled) {
					AcquireDictionary();
				}
				if (Known && CodebookSettled) {
					finished += Resolve();
					DriveBundles();
					finished += Deliver();
				} else if (CatalogueExhausted) {
					// Every source was tried and none produced a manifest that
					// verified. Every outstanding request fails rather than
					// waiting for ever on something that is not coming.
					for (auto &entry : Requests) {
						if (entry.second.State == RequestState::Pending) {
							entry.second.State = RequestState::Failed;
							++finished;
						}
					}
				}
				return finished;
			}

			RequestState StateOf(RequestId id) const override {
				for (const auto &entry : Requests) {
					if (entry.first == id.Value) {
						return entry.second.State;
					}
				}
				return RequestState::Unknown;
			}

			std::optional<Asset> Take(RequestId id) override {
				for (auto entry = Requests.begin(); entry != Requests.end(); ++entry) {
					if (entry->first != id.Value) {
						continue;
					}
					if (entry->second.State != RequestState::Ready) {
						return std::nullopt;
					}
					Asset asset = std::move(entry->second.Result);
					Requests.erase(entry);
					return asset;
				}
				return std::nullopt;
			}

			std::string_view NameOf(RequestId id) const override {
				for (const auto &entry : Requests) {
					if (entry.first == id.Value) {
						// Empty for a request by content address, which is what
						// `ByName` distinguishes: `Name` is not filled for one,
						// and answering the empty string is the honest form of
						// "it was not asked for by name".
						return entry.second.Name;
					}
				}
				return {};
			}

			bool Cancel(RequestId id) override {
				for (auto entry = Requests.begin(); entry != Requests.end(); ++entry) {
					if (entry->first != id.Value) {
						continue;
					}
					const bool live = entry->second.State == RequestState::Pending;
					Requests.erase(entry);
					return live;
				}
				return false;
			}

			size_t Outstanding() const override {
				return Requests.size();
			}

			const DeliveryCounters &Counters() const override {
				return Tally;
			}

			const DeliverySettings &Settings() const override {
				return Config;
			}

		  private:
			struct Pending {
				std::string Name;
				assets::ContentHash Root;
				bool ByName = true;

				// Filled once the manifest has resolved what was asked for.
				bool Located = false;

				// The bundle carrying it, from the same `Resolve` that located
				// it.
				//
				// **A memo of one lookup, not a second copy of the manifest.**
				// `Resolve` has to ask `BundleFor` anyway to queue the job, and
				// the two loops that later ask "is anyone still waiting on this
				// bundle?" asked it again per pending request per pump - which
				// is a walk of the bundle list inside a walk of the requests,
				// inside `Pump`. The catalogue is adopted once and never
				// replaced, so what was resolved against it cannot go stale.
				//
				// Set only where `BundleFor` answered, so it means something
				// only while the request is still `Pending` and `Located` -
				// which is the only state either loop that reads it looks at.
				assets::ContentHash Carrier;

				RequestState State = RequestState::Pending;
				Asset Result;
			};

			// One bundle being fetched, shared by every request waiting on it.
			struct BundleJob {
				assets::ContentHash Bundle;

				// Which source is being tried. An index into `Sources`, so a
				// failure walks to the next one.
				size_t SourceIndex = 0;

				// `FetchId::NONE` until one is in flight, like every other
				// member here - `BundleJob{.Bundle = ...}` is how one is made,
				// so a member with no default is a member left uninitialised.
				net::http::FetchId Fetch{};
				bool Active = false;
			};

			RequestId Accept(Pending pending) {
				const RequestId id{.Value = NextRequest++};
				Requests.emplace_back(id.Value, std::move(pending));
				return id;
			}

			// --- the manifest ---------------------------------------------

			void AcquireCatalogue() {
				if (CatalogueExhausted) {
					return;
				}

				// A Directory source answers immediately; an Http one needs a
				// fetch to be in flight. Both walk the same cursor, so the
				// priority order is one list rather than two policies.
				while (CatalogueCursor < Sources.size() && !CatalogueFetch.IsValid()) {
					Resolved &source = Sources[CatalogueCursor];
					if (source.Store) {
						assets::SignatureBytes signature;
						std::optional<assets::Manifest> manifest = source.Store->ReadManifest(signature);
						if (manifest && Accepts(*manifest, signature)) {
							Adopt(std::move(*manifest), CatalogueCursor);
							return;
						}
						Passed(source, manifest.has_value());
						++CatalogueCursor;
						continue;
					}

					net::http::Request request;
					request.Verb = net::http::Method::Get;
					request.Target = "/manifest";
					CatalogueFetch = source.Fetcher->Submit(source.Address, request, source.Host);
					if (!CatalogueFetch.IsValid()) {
						Passed(source, false);
						++CatalogueCursor;
						continue;
					}
					break;
				}

				if (CatalogueCursor >= Sources.size() && !CatalogueFetch.IsValid()) {
					CatalogueExhausted = true;
					ENGINE_ERROR("delivery: no source produced a manifest that verified");
					return;
				}
				if (!CatalogueFetch.IsValid()) {
					return;
				}

				Resolved &source = Sources[CatalogueCursor];
				source.Fetcher->Pump();
				const net::http::FetchState state = source.Fetcher->StateOf(CatalogueFetch);
				if (state == net::http::FetchState::Pending) {
					return;
				}

				if (state == net::http::FetchState::Ready) {
					const std::optional<net::http::Response> answer = source.Fetcher->Take(CatalogueFetch);
					CatalogueFetch = {};
					if (answer && answer->Code == net::http::Status::Ok) {
						assets::SignatureBytes signature;
						std::optional<assets::Manifest> manifest = ParseSigned(answer->Body, signature);
						if (manifest && Accepts(*manifest, signature)) {
							Tally.TransferredBytes += answer->Body.size();
							Adopt(std::move(*manifest), CatalogueCursor);
							return;
						}
						Passed(source, manifest.has_value());
					} else {
						Passed(source, false);
					}
				} else {
					source.Fetcher->Take(CatalogueFetch);
					CatalogueFetch = {};
					Passed(source, false);
				}
				++CatalogueCursor;
			}

			// The dictionary the catalogue's own source compresses against.
			//
			// **From the same source the manifest came from**, not from the
			// first source in the list. A dictionary is half of a prepared
			// group's cache key - `cdn::PreparedCache` - so one origin's
			// dictionary against another's groups produces bytes that will not
			// decode, and the failure would look like content corruption.
			//
			// An origin with no dictionary is ordinary: its groups are then
			// compressed without one, and `Decompress` is called the matching
			// way. So "no dictionary" settles the question rather than failing
			// it.
			void AcquireDictionary() {
				Resolved &source = Sources[CatalogueSource];

				if (source.Store) {
					if (std::optional<std::vector<std::byte>> bytes = source.Store->ReadDictionary()) {
						Codebook = Dictionary::Load(*bytes);
					}
					CodebookSettled = true;
					return;
				}

				if (!CodebookFetch.IsValid()) {
					net::http::Request request;
					request.Verb = net::http::Method::Get;
					request.Target = "/dictionary";
					CodebookFetch = source.Fetcher->Submit(source.Address, request, source.Host);
					if (!CodebookFetch.IsValid()) {
						return;
					}
				}

				source.Fetcher->Pump();
				const net::http::FetchState state = source.Fetcher->StateOf(CodebookFetch);
				if (state == net::http::FetchState::Pending) {
					return;
				}

				const std::optional<net::http::Response> answer = source.Fetcher->Take(CodebookFetch);
				CodebookFetch = {};
				if (answer && answer->Code == net::http::Status::Ok && !answer->Body.empty()) {
					Tally.TransferredBytes += answer->Body.size();
					Codebook = Dictionary::Load(answer->Body);
					if (!Codebook) {
						// `Dictionary::Load` refuses anything without a trained
						// dictionary's magic. Serving something else where one
						// was expected is a misconfigured origin, and carrying
						// on without it would cost ratio silently - so it is
						// said once rather than absorbed.
						ENGINE_WARN(
							"delivery: '{}' served bytes that are not a trained dictionary",
							source.Descriptor.Name
						);
					}
				}
				CodebookSettled = true;
			}

			// Splits `signature || manifest` and parses the manifest half.
			std::optional<assets::Manifest>
			ParseSigned(const std::vector<std::byte> &body, assets::SignatureBytes &signature) {
				if (body.size() <= assets::SignatureBytes::BYTES) {
					return std::nullopt;
				}
				std::memcpy(signature.Value.data(), body.data(), assets::SignatureBytes::BYTES);
				core::ByteReader reader(
					std::span<const std::byte>(
						body.data() + assets::SignatureBytes::BYTES,
						body.size() - assets::SignatureBytes::BYTES
					)
				);
				return assets::Manifest::Read(reader);
			}

			bool Accepts(const assets::Manifest &manifest, const assets::SignatureBytes &signature) const {
				// The root covers the descriptor table as well as the bundles,
				// so this one check binds the content, its names and its kinds
				// together - see `Manifest::Root`.
				return assets::VerifyManifestRoot(manifest.Root(), signature, Config.Publisher);
			}

			void Passed(const Resolved &source, bool parsed) {
				++Tally.SourceFailures;
				if (parsed) {
					// Parsed and did not verify. Counted apart from a source
					// that is simply down: one is an operational event and the
					// other is corruption or an attack.
					++Tally.VerificationFailures;
					ENGINE_WARN(
						"delivery: source '{}' served a manifest that did not verify", source.Descriptor.Name
					);
				} else {
					ENGINE_INFO("delivery: source '{}' had no manifest", source.Descriptor.Name);
				}
			}

			void Adopt(assets::Manifest manifest, size_t index) {
				Known = std::move(manifest);
				CatalogueSource = index;
				ENGINE_INFO(
					"delivery: catalogue from '{}' - {} assets, {} bundles",
					Sources[index].Descriptor.Name,
					Known->Assets().size(),
					Known->Bundles().size()
				);
			}

			// --- requests --------------------------------------------------

			// Turns names into roots, answers from the cache, and queues the
			// bundles that are still needed.
			size_t Resolve() {
				size_t finished = 0;
				for (auto &entry : Requests) {
					Pending &pending = entry.second;
					if (pending.State != RequestState::Pending || pending.Located) {
						continue;
					}

					const assets::AssetEntry *asset =
						pending.ByName ? Known->Find(pending.Name) : Known->FindByRoot(pending.Root);
					if (asset == nullptr) {
						// Failed here rather than refused at `Request`, so a
						// request made before the manifest arrived behaves the
						// same as one made after.
						pending.State = RequestState::Failed;
						++finished;
						continue;
					}

					pending.Located = true;
					pending.Root = asset->Root;
					pending.Name = asset->Name;
					pending.Result.Name = asset->Name;
					pending.Result.Kind = asset->Kind;
					pending.Result.Root = asset->Root;

					if (Cached) {
						if (std::optional<std::vector<std::byte>> bytes = Cached->Find(*asset)) {
							pending.Result.Bytes = std::move(*bytes);
							pending.State = RequestState::Ready;
							++Tally.CacheHits;
							++finished;
							continue;
						}
					}
					++Tally.CacheMisses;

					const assets::BundleEntry *const bundle = Known->BundleFor(asset->Root);
					if (bundle == nullptr) {
						// An asset in no bundle is unfetchable. A publisher's
						// mistake, and one worth surfacing rather than retrying
						// against every source.
						ENGINE_WARN("delivery: '{}' is in no bundle and cannot be fetched", asset->Name);
						pending.State = RequestState::Failed;
						++finished;
						continue;
					}
					pending.Carrier = bundle->Root;
					Queue(bundle->Root);
				}
				return finished;
			}

			void Queue(const assets::ContentHash &bundle) {
				const bool known = std::any_of(Jobs.begin(), Jobs.end(), [&bundle](const BundleJob &job) {
					return job.Bundle == bundle;
				});
				if (!known) {
					Jobs.push_back(BundleJob{.Bundle = bundle});
				}
			}

			void DriveBundles() {
				// **What the cap counts is bundles admitted this pump, not
				// sockets open.** A local store has no wire: `Start` reads it,
				// verifies every member and writes them to the cache before it
				// returns, and it used to report that as "nothing in flight" - so
				// `active` never moved and the cap never bit. Every queued bundle
				// was therefore read, hashed and cached in one pump, which on a
				// `dir:` source is the whole working set in the frame that
				// noticed it. That is the freeze a game shows while its meshes
				// load, and it is the reason this counts work rather than
				// sockets.
				size_t admitted = 0;
				for (BundleJob &job : Jobs) {
					if (job.Active) {
						++admitted;
					}
				}

				size_t read = 0;
				for (BundleJob &job : Jobs) {
					if (job.Active || admitted >= MAXIMUM_BUNDLES_IN_FLIGHT) {
						continue;
					}

					// **The store allowance is passed in rather than checked
					// here**, so a spent one stops store reads without also
					// stopping a wire fetch that costs this thread nothing. Which
					// kind a job turns out to be is not knowable until `Start`
					// has walked its sources.
					switch (Start(job, read < MAXIMUM_STORE_BUNDLES_PER_PUMP)) {
					case StartOutcome::Fetching:
						++admitted;
						break;
					case StartOutcome::Completed:
						++admitted;
						++read;
						break;
					case StartOutcome::Idle:
						break;
					}
				}
				Transfer->Pump();
				if (Relayed) {
					Relayed->Pump();
				}

				for (auto job = Jobs.begin(); job != Jobs.end();) {
					if (Advance(*job)) {
						job = Jobs.erase(job);
					} else {
						++job;
					}
				}
			}

			// What one attempt to start a bundle job actually did.
			//
			// `Fetching` and `Completed` both count against
			// `MAXIMUM_BUNDLES_IN_FLIGHT`, because both spent this pump's
			// allowance - one on a socket and one on a synchronous read of the
			// whole bundle. Only `Idle` is free.
			enum class StartOutcome {
				// A wire fetch is in flight and will be picked up by `Advance`.
				Fetching,

				// A local store answered inside this call: the bundle is already
				// read, verified, cached and split.
				Completed,

				// Nothing happened - the transfer client is full, or every source
				// has been walked and the job abandoned.
				Idle,
			};

			// @param mayReadStore Whether this pump still has room for a
			//                     synchronous store read. A store source reached
			//                     with this false is left for the next pump
			//                     rather than skipped, because skipping it would
			//                     burn a perfectly good origin over a budget.
			// @return What the attempt did. See `StartOutcome`.
			StartOutcome Start(BundleJob &job, bool mayReadStore) {
				while (job.SourceIndex < Sources.size()) {
					Resolved &source = Sources[job.SourceIndex];

					if (source.Store) {
						if (!mayReadStore) {
							return StartOutcome::Idle;
						}

						// A local store has no wire, so there is nothing to
						// compress and no group to expand - the chunks are read
						// and the assets fall out of them. Same manifest, same
						// verification, no transport.
						const assets::BundleEntry *const bundle = Known->FindBundle(job.Bundle);
						if (bundle != nullptr) {
							if (std::optional<std::vector<std::byte>> payload =
									source.Store->ReadBundle(*Known, *bundle)) {
								Tally.TransferredBytes += payload->size();
								Tally.ExpandedBytes += payload->size();
								++Tally.Bundles;
								Split(*bundle, *payload, true);
								return StartOutcome::Completed;
							}
						}
						++Tally.SourceFailures;
						++job.SourceIndex;
						continue;
					}

					net::http::Request request;
					request.Verb = net::http::Method::Get;
					request.Target = "/bundle/" + job.Bundle.ToHex();
					if (!Grant.empty()) {
						request.Headers.push_back(
							net::http::Header{
								.Name = "x-atomic-grant",
								.Value = ToHex(Grant),
							}
						);
					}

					job.Fetch = source.Fetcher->Submit(source.Address, request, source.Host);
					if (job.Fetch.IsValid()) {
						job.Active = true;
						return StartOutcome::Fetching;
					}
					// The transfer client is full. Left for the next pump
					// rather than skipping the source, which would burn a
					// perfectly good origin because this end was busy.
					return StartOutcome::Idle;
				}
				Abandon(job.Bundle);
				return StartOutcome::Idle;
			}

			// @return Whether the job is finished and should be dropped.
			bool Advance(BundleJob &job) {
				if (!job.Active) {
					// Either it completed synchronously from a store, or every
					// source has been walked.
					return job.SourceIndex >= Sources.size() || !AnyWaiting(job.Bundle);
				}

				Resolved &source = Sources[job.SourceIndex];
				const net::http::FetchState state = source.Fetcher->StateOf(job.Fetch);
				if (state == net::http::FetchState::Pending) {
					return false;
				}

				const std::optional<net::http::Response> answer = source.Fetcher->Take(job.Fetch);
				job.Active = false;
				job.Fetch = {};

				if (!answer || answer->Code != net::http::Status::Ok) {
					++Tally.SourceFailures;
					++job.SourceIndex;
					return false;
				}

				const assets::BundleEntry *const bundle = Known->FindBundle(job.Bundle);
				if (bundle == nullptr) {
					Abandon(job.Bundle);
					return true;
				}

				Tally.TransferredBytes += answer->Body.size();

				// **Sized from the manifest, never from the frame.** A frame
				// header can claim any decompressed size, so believing it is a
				// decompression bomb: kilobytes on the wire, gigabytes in the
				// allocator. The manifest records what this bundle weighs and
				// the manifest is signed.
				std::optional<std::vector<std::byte>> payload =
					Codebook ? GroupCodec::Decompress(answer->Body, *Codebook, bundle->TotalBytes)
							 : GroupCodec::Decompress(answer->Body, bundle->TotalBytes);

				if (!payload) {
					++Tally.SourceFailures;
					++Tally.VerificationFailures;
					++job.SourceIndex;
					return false;
				}

				Tally.ExpandedBytes += payload->size();
				++Tally.Bundles;
				Split(*bundle, *payload, false);
				return true;
			}

			bool AnyWaiting(const assets::ContentHash &bundle) const {
				for (const auto &entry : Requests) {
					const Pending &pending = entry.second;
					if (pending.State != RequestState::Pending || !pending.Located) {
						continue;
					}
					if (pending.Carrier == bundle) {
						return true;
					}
				}
				return false;
			}

			// Cuts a group up and verifies every asset in it.
			//
			// The other members land in the cache as a consequence of one being
			// asked for, which is the whole of "the game progressively builds"
			// seen from this end.
			//
			// @param verified Whether the payload has already been checked
			//                 against the signed manifest member by member.
			//                 `ChunkStore::ReadAsset` does exactly that on the
			//                 way out of a local store, over the same slices in
			//                 the same order, so hashing them a second time here
			//                 is the same answer for the same bytes - about a
			//                 fifth of a local bundle's cost, measured. It is
			//                 false for anything off a wire, where this is the
			//                 only check there is.
			void
			Split(const assets::BundleEntry &bundle, const std::vector<std::byte> &payload, bool verified) {
				ENGINE_PROFILE_CAT("delivery.split", core::ProfileCategory::Assets);

				for (const assets::ContentHash &member : bundle.Assets) {
					const assets::AssetEntry *const asset = Known->FindByRoot(member);
					if (asset == nullptr) {
						continue;
					}
					const std::optional<assets::BundleSlice> slice = Known->SliceOf(bundle, member);
					if (!slice || slice->Offset + slice->Bytes > payload.size()) {
						continue;
					}

					const std::span<const std::byte> bytes(
						payload.data() + slice->Offset, static_cast<size_t>(slice->Bytes)
					);
					if (!verified) {
						ENGINE_PROFILE_CAT("delivery.verify", core::ProfileCategory::Assets);
						if (!assets::VerifyAsset(*asset, bytes)) {
							++Tally.VerificationFailures;
							ENGINE_WARN(
								"delivery: '{}' did not verify against the signed manifest", asset->Name
							);
							continue;
						}
					}

					Delivered.emplace_back(member, std::vector<std::byte>(bytes.begin(), bytes.end()));
					if (Cached) {
						ENGINE_PROFILE_CAT("delivery.cache", core::ProfileCategory::Assets);
						Cached->Store(*asset, bytes);
					}
				}
			}

			void Abandon(const assets::ContentHash &bundle) {
				for (auto &entry : Requests) {
					Pending &pending = entry.second;
					if (pending.State != RequestState::Pending || !pending.Located) {
						continue;
					}
					if (pending.Carrier == bundle) {
						pending.State = RequestState::Failed;
					}
				}
			}

			// Hands verified bytes to the requests waiting for them.
			//
			// **This is the only place a request becomes `Ready`**, and it runs
			// inside `Pump`. That is the whole contract: a completion becomes
			// visible when the caller pumps and at no other moment, so a world
			// applying results at the barrier cannot see one mid-tick.
			size_t Deliver() {
				if (Delivered.empty()) {
					return 0;
				}
				size_t finished = 0;
				for (auto &entry : Requests) {
					Pending &pending = entry.second;
					if (pending.State != RequestState::Pending || !pending.Located) {
						continue;
					}
					for (const auto &[root, bytes] : Delivered) {
						if (root != pending.Root) {
							continue;
						}
						pending.Result.Bytes = bytes;
						pending.State = RequestState::Ready;
						++finished;
						break;
					}
				}
				Delivered.clear();
				return finished;
			}

			DeliverySettings Config;
			std::vector<Resolved> Sources;
			std::optional<ContentCache> Cached;
			std::unique_ptr<net::http::Client> Transfer;

			// The fetcher for `SourceKind::Relay` sources, when this caller owns a
			// link to relay over. Null otherwise, which is what makes a relay source
			// skipped rather than broken.
			std::unique_ptr<net::http::Client> Relayed;
			std::vector<std::byte> Grant;

			std::optional<assets::Manifest> Known;
			std::optional<Dictionary> Codebook;
			size_t CatalogueCursor = 0;
			size_t CatalogueSource = 0;
			net::http::FetchId CatalogueFetch;
			bool CatalogueExhausted = false;

			net::http::FetchId CodebookFetch;

			// Whether the dictionary question has been answered, which is not
			// the same as there being one.
			bool CodebookSettled = false;

			std::vector<std::pair<uint64_t, Pending>> Requests;
			std::vector<BundleJob> Jobs;

			// Verified bytes waiting to be handed out, cleared every pump.
			std::vector<std::pair<assets::ContentHash, std::vector<std::byte>>> Delivered;

			uint64_t NextRequest = 1;
			DeliveryCounters Tally;
		};
	}

	const char *Describe(RequestState state) {
		switch (state) {
		case RequestState::Unknown:
			return "unknown";
		case RequestState::Pending:
			return "pending";
		case RequestState::Ready:
			return "ready";
		case RequestState::Failed:
			return "failed";
		}
		return "unknown";
	}

	std::unique_ptr<AssetClient> MakeAssetClient(const DeliverySettings &settings, RelayChannel *relay) {
		if (!settings.IsValid()) {
			// Refused rather than half-applied. A client with no publisher key
			// verifies nothing and one with no source fetches nothing, and
			// neither should look like a working client.
			ENGINE_ERROR("delivery: settings name no usable source, or no publisher key to trust");
			return nullptr;
		}
		return std::make_unique<Client>(settings, relay);
	}
}
