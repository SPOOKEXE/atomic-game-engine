#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cdn/Service.hpp>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The routing table, and what each refusal means.
//
// **Every request is answered and none is dropped.** A client that asked for a
// route this origin does not have gets `404`; one whose grant does not admit
// the content gets `403`; one that asked for something malformed gets `400`.
// Dropping a connection instead would make every one of those look like a
// network failure, which is the hardest kind of misconfiguration to diagnose
// because it points at the wrong subsystem.

namespace cdn {
	namespace {
		namespace http = engine::net::http;
		using engine::assets::ChunkStore;
		using engine::assets::ContentHash;

		std::vector<std::byte> AsBytes(std::string_view text) {
			return {
				reinterpret_cast<const std::byte *>(text.data()),
				reinterpret_cast<const std::byte *>(text.data()) + text.size()
			};
		}

		// Hex, refusing anything else.
		//
		// A grant arrives as text in a header because a header is text; every
		// byte of it is hostile, and a decoder that skipped what it did not
		// understand would hand `Grant::Open` a token that is not the one the
		// client sent.
		std::optional<std::vector<std::byte>> FromHex(std::string_view text) {
			if (text.empty() || text.size() % 2 != 0) {
				return std::nullopt;
			}
			const auto digit = [](char value) -> int {
				if (value >= '0' && value <= '9') {
					return value - '0';
				}
				if (value >= 'a' && value <= 'f') {
					return value - 'a' + 10;
				}
				// Uppercase is refused rather than accepted: one spelling, for
				// `ContentHash::FromHex`'s reason.
				return -1;
			};

			std::vector<std::byte> bytes;
			bytes.reserve(text.size() / 2);
			for (size_t index = 0; index < text.size(); index += 2) {
				const int high = digit(text[index]);
				const int low = digit(text[index + 1]);
				if (high < 0 || low < 0) {
					return std::nullopt;
				}
				bytes.push_back(static_cast<std::byte>((high << 4) | low));
			}
			return bytes;
		}

		std::optional<std::vector<std::byte>> ReadWholeFile(const std::filesystem::path &path) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				return std::nullopt;
			}
			const std::streamoff size = file.tellg();
			if (size < 0) {
				return std::nullopt;
			}
			file.seekg(0);

			std::vector<std::byte> bytes(static_cast<size_t>(size));
			if (size > 0) {
				file.read(reinterpret_cast<char *>(bytes.data()), size);
				if (!file) {
					return std::nullopt;
				}
			}
			return bytes;
		}

		class HttpService final : public Service {
		  public:
			HttpService(
				Origin &origin,
				ChunkStore store,
				std::unique_ptr<http::Server> listener,
				const ServiceSettings &settings
			)
				: Serving(origin), Store(std::move(store)), Listener(std::move(listener)),
				  Writes(settings.Ingest), Listing(settings.Catalogue), Enumerates(settings.Lists()) {
				// Read once at start-up rather than per request. The published
				// file is the bytes that were signed, so serving it verbatim is
				// what lets a client verify what the publisher actually signed
				// rather than a re-serialisation that is *supposed* to be
				// identical.
				Published = ReadWholeFile(Store.Directory() / ChunkStore::MANIFEST_FILE);
				Codebook = Store.ReadDictionary();
				if (!Published) {
					ENGINE_WARN(
						"cdn: no manifest at {} — clients will get 503 until one is published",
						Store.Directory().string()
					);
				}
			}

			size_t Pump(uint64_t nowSeconds) override {
				ENGINE_PROFILE_CAT("cdn::Service::Pump", engine::core::ProfileCategory::Assets);

				Now = nowSeconds;
				const http::ServeReport served =
					Listener->Pump([this](const http::Request &request) { return Answer(request); });
				Tally.Rejected += served.Rejected;
				Tally.SentBytes += served.SentBytes;
				Tally.ReceivedBytes += served.ReceivedBytes;
				return served.Served;
			}

			engine::net::Endpoint Local() const override {
				return Listener->Local();
			}

			const ServiceCounters &Counters() const override {
				return Tally;
			}

			bool Open() const override {
				return Listener->Open();
			}

			void Close() override {
				Listener->Close();
			}

		  private:
			http::Response Answer(const http::Request &request) {
				if (request.Target == "/health") {
					return Health();
				}
				if (request.Target == "/manifest") {
					return ManifestOf(request);
				}
				if (request.Target == "/dictionary") {
					return DictionaryOf(request);
				}
				if (request.Target.starts_with("/bundle/")) {
					return BundleOf(request);
				}
				// Matched exactly or with a cursor, rather than by prefix, so
				// `/cataloguery` is the unknown route it is rather than a
				// malformed listing — a `400` there would tell an
				// unauthenticated caller that something answers near that word.
				if (request.Target == "/catalogue" || request.Target.starts_with("/catalogue/")) {
					return CatalogueOf(request);
				}
				if (request.Target.starts_with("/ingest/")) {
					return IngestOf(request);
				}

				++Tally.Rejected;
				return Refuse(http::Status::NotFound);
			}

			static http::Response Refuse(http::Status code) {
				http::Response response;
				response.Code = code;
				// **No body on a refusal.** A diagnostic in one is a reason
				// returned to a client, which is an oracle — Gate's rule.
				return response;
			}

			http::Response Health() {
				++Tally.Health;
				http::Response response;
				response.Code = http::Status::Ok;
				response.Set("content-type", "text/plain");

				const std::shared_ptr<const Publication> current = Serving.Current();
				const std::string body =
					current ? "ok " + std::to_string(current->Contents().Assets().size()) + " assets " +
								  std::to_string(current->Contents().Bundles().size()) + " bundles"
							: "ok nothing-published";
				response.Body = AsBytes(body);
				return response;
			}

			http::Response ManifestOf(const http::Request &request) {
				if (!Published) {
					// Nothing published is `503` rather than `404`: the route
					// exists and this origin is simply not ready, and a client
					// deciding whether to retry needs those to be different.
					return Refuse(http::Status::ServiceUnavailable);
				}
				++Tally.Manifests;
				return Body(*Published, request);
			}

			http::Response DictionaryOf(const http::Request &request) {
				if (!Codebook || Codebook->empty()) {
					// An origin with no dictionary is ordinary — its groups are
					// compressed without one — so this is a plain `404` and the
					// client carries on.
					return Refuse(http::Status::NotFound);
				}
				++Tally.Dictionaries;
				return Body(*Codebook, request);
			}

			// `GET` and `HEAD` at `/catalogue`, and at `/catalogue/<cursor>`.
			//
			// **The one route that answers a question the caller could not
			// already ask.** Every other route is looked up by a value the
			// caller arrived holding — a bundle root, a content hash, a manifest
			// a publisher signed — and this one hands over the names, which is
			// why it is off unless configured and admitted by the key that
			// already exists. `CatalogueSettings` carries the whole argument.
			//
			// **The page is text, one line an asset, and the name is last.** A
			// name is the one field with no bound on what is inside it —
			// AGENTS.md rule 4 — so it ends its line, where no separator has to
			// survive it. The two characters that could still break the format
			// are the two an entry is dropped for.
			http::Response CatalogueOf(const http::Request &request) {
				if (!Enumerates) {
					// **`404`, as though the route were not there** —
					// `IngestOf`'s argument on the read side. A `403` would
					// tell an unauthenticated caller that this origin can
					// enumerate and only the key is missing, which is exactly
					// the sentence somebody probing wants to hear.
					++Tally.CatalogueRefused;
					return Refuse(http::Status::NotFound);
				}

				const std::string_view offered = request.Find("x-atomic-ingest").value_or(std::string_view{});
				if (!SameSecret(offered, Writes.Key)) {
					++Tally.CatalogueRefused;
					return Refuse(http::Status::Forbidden);
				}

				if (request.Verb != http::Method::Get && request.Verb != http::Method::Head) {
					++Tally.Rejected;
					return Refuse(http::Status::NotFound);
				}

				const std::optional<uint64_t> cursor = CatalogueCursor(request.Target);
				if (!cursor) {
					++Tally.Rejected;
					return Refuse(http::Status::BadRequest);
				}

				const std::shared_ptr<const Publication> current = Serving.Current();
				if (!current) {
					// Nothing published is `503` rather than an empty page, for
					// `ManifestOf`'s reason: an origin that is not ready and an
					// origin holding nothing are different facts, and a caller
					// deciding whether to ask again needs them apart.
					return Refuse(http::Status::ServiceUnavailable);
				}

				const std::vector<engine::assets::AssetEntry> &assets = current->Contents().Assets();
				// At least one, because a page size of zero would hand back an
				// empty page whose cursor never moves — a caller following
				// `next` would walk it forever.
				const size_t page = std::max<size_t>(Listing.PageEntries, 1);
				const size_t first = std::min<size_t>(*cursor, assets.size());
				const size_t last = std::min<size_t>(first + page, assets.size());

				std::string body;
				body += "catalogue 1\n";
				// **The publication's root, on every page.** A publish may swap
				// underneath somebody walking the cursor, and an offset means
				// something different afterwards; a reader that keeps this can
				// tell that happened instead of silently merging two lists.
				body += "root " + current->Contents().Root().ToHex() + "\n";
				body += "total " + std::to_string(assets.size()) + "\n";
				if (last < assets.size()) {
					body += "next " + std::to_string(last) + "\n";
				}

				for (size_t index = first; index < last; index++) {
					const engine::assets::AssetEntry &asset = assets[index];
					if (asset.Name.find_first_of("\r\n") != std::string::npos) {
						// A name that would end its own line. Dropped rather
						// than escaped: an escape is a second spelling of a
						// name, and rule 4 wants exactly one.
						continue;
					}
					body += "asset ";
					body += asset.Root.ToHex();
					body += ' ';
					body += engine::assets::Describe(asset.Kind);
					body += ' ';
					body += std::to_string(asset.TotalBytes);
					body += ' ';
					body += asset.Name;
					body += '\n';
				}

				++Tally.Catalogues;

				http::Response response;
				response.Code = http::Status::Ok;
				response.Set("content-type", "text/plain");

				const std::vector<std::byte> encoded = AsBytes(body);
				return Body(encoded, request, std::move(response));
			}

			// The offset a listing page starts at.
			//
			// Decimal digits or nothing at all: a cursor is this origin's own
			// number handed back to it, so anything else is a caller that made
			// one up rather than a caller that has to be understood.
			//
			// @return The offset, or nothing when the target is not one.
			static std::optional<uint64_t> CatalogueCursor(std::string_view target) {
				const std::string_view rest = target.substr(std::string_view("/catalogue").size());
				if (rest.empty()) {
					return uint64_t{0};
				}

				// The routing check guarantees the separator, so what is left
				// is what follows it.
				const std::string_view digits = rest.substr(1);
				if (digits.empty() || digits.size() > 18) {
					return std::nullopt;
				}

				uint64_t cursor = 0;
				for (const char value : digits) {
					if (value < '0' || value > '9') {
						return std::nullopt;
					}
					cursor = cursor * 10 + static_cast<uint64_t>(value - '0');
				}
				return cursor;
			}

			// `PUT` and `HEAD` at `/ingest/<hash>`.
			//
			// **The target is a content address and the body is checked against
			// it**, which is what makes the shared secret only an admission
			// check rather than a trust decision: whoever holds the key can
			// spend this origin's disk, and cannot store anything under a name
			// that is not its own true hash. `IngestSettings` carries the rest
			// of that argument.
			http::Response IngestOf(const http::Request &request) {
				if (!Writes.Accepts()) {
					// **`404`, as though the route were not there**, because on
					// a read origin it is not: answering `403` would tell an
					// unauthenticated caller that this build has an ingest path
					// and only the key is missing.
					++Tally.IngestRefused;
					return Refuse(http::Status::NotFound);
				}

				const std::string_view target(request.Target);
				const std::string_view hex = target.substr(std::string_view("/ingest/").size());

				const std::optional<ContentHash> named = ContentHash::FromHex(hex);
				if (!named) {
					++Tally.Rejected;
					return Refuse(http::Status::BadRequest);
				}

				// Constant time, because a byte-at-a-time compare on a secret
				// leaks it one byte per round trip to anybody who can time a
				// response — `assets::Grant`'s rule, and this is the one place
				// in the service holding a secret to compare.
				const std::string_view offered = request.Find("x-atomic-ingest").value_or(std::string_view{});
				if (!SameSecret(offered, Writes.Key)) {
					++Tally.IngestRefused;
					return Refuse(http::Status::Forbidden);
				}

				const std::optional<std::string> suffix = IngestSuffix(request);
				if (!suffix) {
					// A suffix outside the closed set below. Refused rather
					// than dropped, because the filename is the one part of
					// this route built from something a client said.
					++Tally.IngestRefused;
					return Refuse(http::Status::BadRequest);
				}

				std::error_code failure;
				std::filesystem::create_directories(Writes.Inbox, failure);
				if (failure) {
					ENGINE_ERROR(
						"cdn: ingest inbox {} could not be created: {}",
						Writes.Inbox.string(),
						failure.message()
					);
					return Refuse(http::Status::InternalError);
				}

				// **The path is built from the parsed hash**, never from the
				// target text — CDN.md §8. `ToHex` re-renders it, so even a
				// target that parsed is not the thing that names the file.
				const std::filesystem::path stored = Writes.Inbox / (named->ToHex() + *suffix);
				const bool present = std::filesystem::exists(stored, failure);

				if (request.Verb == http::Method::Head) {
					// The probe that makes re-uploading a whole tree cheap: an
					// uploader asks before it spends the bandwidth, which is
					// what `IngestDuplicates` staying low is evidence of.
					http::Response response;
					response.Code = present ? http::Status::Ok : http::Status::NotFound;
					return response;
				}

				if (request.Verb != http::Method::Put) {
					++Tally.Rejected;
					return Refuse(http::Status::NotFound);
				}

				if (request.Body.empty()) {
					// An empty file has no chunks and `Publish` skips it, so
					// accepting one would put a row in the inbox that can never
					// become an asset.
					++Tally.IngestRefused;
					return Refuse(http::Status::BadRequest);
				}

				const ContentHash actual = engine::assets::Hasher::Of(request.Body);
				if (!(actual == *named)) {
					// **The check that lets the key be a shared secret.** Bytes
					// that do not hash to the name they were sent under are not
					// stored under any name at all.
					++Tally.IngestRefused;
					ENGINE_WARN("cdn: ingest refused — body did not hash to {}", named->ToHex());
					return Refuse(http::Status::BadRequest);
				}

				if (present) {
					// **Already here is a success.** The bytes are the identity
					// — `LocalStore::ImportFile`'s rule — so a retry after a
					// dropped socket costs a hash and nothing else.
					++Tally.IngestDuplicates;
					http::Response response;
					response.Code = http::Status::Ok;
					return response;
				}

				if (!WriteWholeFile(stored, request.Body)) {
					ENGINE_ERROR("cdn: could not write {}", stored.string());
					return Refuse(http::Status::InternalError);
				}

				++Tally.Ingested;
				Tally.IngestedBytes += request.Body.size();

				http::Response response;
				response.Code = http::Status::Ok;
				return response;
			}

			// The extension an upload asked to be stored under.
			//
			// **The extension is load-bearing rather than cosmetic**, which is
			// why it travels at all: `Publish` derives an asset's kind from its
			// name through `KindOfName`, so a file that lands without its
			// suffix publishes as `Unknown` and no subsystem claims it.
			//
			// **A closed set, and that is the whole of the check.** This is the
			// only part of the ingest path built from text a client chose, so
			// it is validated as a shape rather than scanned for bad ones: a
			// dot and up to fifteen lowercase alphanumerics. Nothing matching
			// that can be `..`, a separator, or a second extension.
			//
			// @return The suffix, or nothing when it is outside that set. An
			//         absent header is an empty suffix and not a refusal.
			static std::optional<std::string> IngestSuffix(const http::Request &request) {
				const std::optional<std::string_view> given = request.Find("x-atomic-suffix");
				if (!given || given->empty()) {
					return std::string{};
				}
				if (given->size() > 16 || given->front() != '.') {
					return std::nullopt;
				}
				for (const char value : given->substr(1)) {
					const bool allowed = (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
					if (!allowed) {
						return std::nullopt;
					}
				}
				return std::string(*given);
			}

			// Compares a secret without leaking where it stopped matching.
			static bool SameSecret(std::string_view offered, std::string_view expected) {
				// The lengths are compared openly because a length is not the
				// secret, and folding it into the loop would either read past
				// the end or exit early.
				if (offered.size() != expected.size() || expected.empty()) {
					return false;
				}
				unsigned char difference = 0;
				for (size_t index = 0; index < expected.size(); index++) {
					difference |= static_cast<unsigned char>(offered[index] ^ expected[index]);
				}
				return difference == 0;
			}

			static bool WriteWholeFile(const std::filesystem::path &path, std::span<const std::byte> bytes) {
				std::ofstream file(path, std::ios::binary | std::ios::trunc);
				if (!file) {
					return false;
				}
				file.write(
					reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
				);
				return file.good();
			}

			http::Response BundleOf(const http::Request &request) {
				const std::string_view target(request.Target);
				const std::string_view hex = target.substr(std::string_view("/bundle/").size());

				const std::optional<ContentHash> bundle = ContentHash::FromHex(hex);
				if (!bundle) {
					// A hash or nothing. There is no route here that takes a
					// name, so there is nothing to walk out of a directory.
					++Tally.Rejected;
					return Refuse(http::Status::BadRequest);
				}

				std::vector<std::byte> token;
				if (const std::optional<std::string_view> header = request.Find("x-atomic-grant")) {
					if (std::optional<std::vector<std::byte>> decoded = FromHex(*header)) {
						token = std::move(*decoded);
					}
				}

				const RequestId id = Serving.Submit(token, *bundle, Now);
				if (Serving.StateOf(id) == RequestState::Refused) {
					++Tally.Refused;
					return Refuse(http::Status::Forbidden);
				}

				// Prepared inline. CPU work with a known end, which is what
				// `Origin::Pump` is for — and the payload is resolved before
				// its fan-out, so no worker is occupied waiting on a disk.
				Serving.Pump([this](const ContentHash &root) { return Payload(root); });

				if (Serving.StateOf(id) != RequestState::Ready) {
					Serving.Cancel(id);
					++Tally.Missing;
					return Refuse(http::Status::NotFound);
				}

				const PreparedFrame frame = Serving.Take(id);
				if (frame == nullptr) {
					++Tally.Missing;
					return Refuse(http::Status::NotFound);
				}

				++Tally.Bundles;
				Tally.ServedBytes += frame->size();

				http::Response response;
				response.Code = http::Status::Ok;
				response.Set("content-type", "application/octet-stream");
				return Body(*frame, request, std::move(response));
			}

			// Resolves a bundle's uncompressed bytes out of the chunk store.
			//
			// This is what `PayloadSource` was a seam for, now that the on-disk
			// layout is decided — `assets::ChunkStore`. The bundle's payload is
			// its members concatenated in member order, which is the one
			// definition of a group's bytes and is also what the client's
			// `SliceOf` cuts back up.
			std::optional<std::vector<std::byte>> Payload(const ContentHash &root) {
				const std::shared_ptr<const Publication> current = Serving.Current();
				if (!current) {
					return std::nullopt;
				}
				for (const engine::assets::BundleEntry &bundle : current->Contents().Bundles()) {
					if (bundle.Root == root) {
						return Store.ReadBundle(current->Contents(), bundle);
					}
				}
				return std::nullopt;
			}

			// Applies a range if one was asked for.
			//
			// What makes a multi-megabyte group resumable rather than
			// restartable: a client holding six megabytes of a sixteen-megabyte
			// bundle asks for the rest instead of paying for the first six
			// twice.
			static http::Response Body(
				const std::vector<std::byte> &content,
				const http::Request &request,
				http::Response response = {}
			) {
				if (response.Code == http::Status::Unknown) {
					response.Code = http::Status::Ok;
				}
				response.Set("accept-ranges", "bytes");

				if (!request.Range) {
					response.Body = content;
					return response;
				}

				const std::optional<http::ByteRange> resolved =
					request.Range->Resolve(static_cast<uint64_t>(content.size()));
				if (!resolved) {
					// A range that does not overlap the entity at all. `416`
					// rather than an empty `200`, which a client would take as
					// a zero-length asset.
					http::Response refusal;
					refusal.Code = http::Status::RangeNotSatisfiable;
					refusal.Set("content-range", "bytes */" + std::to_string(content.size()));
					return refusal;
				}

				const size_t first = static_cast<size_t>(resolved->First);
				const size_t last = static_cast<size_t>(resolved->Last);
				response.Code = http::Status::PartialContent;
				response.Set(
					"content-range",
					"bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
						std::to_string(content.size())
				);
				response.Body.assign(
					content.begin() + static_cast<ptrdiff_t>(first),
					content.begin() + static_cast<ptrdiff_t>(last) + 1
				);
				return response;
			}

			Origin &Serving;
			ChunkStore Store;
			std::unique_ptr<http::Server> Listener;
			IngestSettings Writes;
			CatalogueSettings Listing;

			// Decided once at start-up rather than per request, because it is a
			// property of the configuration and re-deriving it on every request
			// is a second place for the two halves of "on, and keyed" to
			// disagree.
			bool Enumerates = false;

			std::optional<std::vector<std::byte>> Published;
			std::optional<std::vector<std::byte>> Codebook;

			ServiceCounters Tally;
			uint64_t Now = 0;
		};
	}

	bool IngestSettings::Accepts() const {
		// **Both, or neither.** A path with no key is an open dropbox and a key
		// with no path has nowhere to put anything; either alone is a
		// half-configured origin, and the safe reading of half-configured is
		// off.
		return !Inbox.empty() && !Key.empty();
	}

	bool ServiceSettings::Lists() const {
		// **The key is the ingest key and there is no second one.** An operator
		// who wants an origin that enumerates and takes no uploads sets the key
		// and leaves `Inbox` empty — `IngestSettings::Accepts` needs both, so
		// that origin still refuses every write.
		return Catalogue.Enabled && !Ingest.Key.empty();
	}

	std::unique_ptr<Service> Serve(Origin &origin, ChunkStore store, const ServiceSettings &settings) {
		http::ServerSettings socket = settings.Server;

		if (settings.Ingest.Accepts()) {
			// **A request is parsed whole, so an upload has to fit in the
			// connection's buffer before it can be answered at all.** Leaving
			// the read-only default in place would make every upload past 64 KB
			// die as a dropped socket — the worst diagnostic there is, because
			// it points at the network rather than at a limit.
			//
			// Raising `Limits.BodyBytes` to the same ceiling is what turns an
			// over-large upload into a `413`: the length is checked while the
			// *headers* parse, so the refusal is answered before a byte of body
			// is read. Order matters here — the buffer has to hold a file the
			// limit will admit, or the limit would let through what the buffer
			// then drops.
			const uint64_t ceiling = settings.Ingest.MaximumFileBytes;
			socket.Limits.BodyBytes = ceiling;

			const uint64_t room = ceiling + socket.Limits.RequestLineBytes + socket.Limits.HeaderBytes;
			socket.ConnectionBufferBytes =
				std::max<size_t>(socket.ConnectionBufferBytes, static_cast<size_t>(room));
		}

		std::unique_ptr<http::Server> listener = http::Listen(settings.Port, socket);
		if (!listener) {
			ENGINE_ERROR("cdn: could not bind port {}", settings.Port);
			return nullptr;
		}

		if (settings.Ingest.Accepts()) {
			ENGINE_INFO(
				"cdn: accepting uploads at /ingest into {} (up to {} bytes a file)",
				settings.Ingest.Inbox.string(),
				settings.Ingest.MaximumFileBytes
			);
		}

		if (settings.Lists()) {
			// Said out loud at start-up, because it is the one setting whose
			// effect an operator cannot see from outside without the key: an
			// origin that enumerates looks exactly like one that does not until
			// somebody asks with the right header.
			ENGINE_INFO(
				"cdn: enumerating contents at /catalogue, {} a page, against the ingest key",
				settings.Catalogue.PageEntries
			);
		} else if (settings.Catalogue.Enabled) {
			ENGINE_WARN("cdn: /catalogue is switched on and has no key to admit it — it stays closed");
		}

		ENGINE_INFO("cdn: serving on {}", listener->Local().Text());
		return std::make_unique<HttpService>(origin, std::move(store), std::move(listener), settings);
	}
}
