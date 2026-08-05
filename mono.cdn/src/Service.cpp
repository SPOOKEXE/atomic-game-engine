#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cdn/Service.hpp>
#include <fstream>
#include <string>
#include <string_view>
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
			HttpService(Origin &origin, ChunkStore store, std::unique_ptr<http::Server> listener)
				: Serving(origin), Store(std::move(store)), Listener(std::move(listener)) {
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

			std::optional<std::vector<std::byte>> Published;
			std::optional<std::vector<std::byte>> Codebook;

			ServiceCounters Tally;
			uint64_t Now = 0;
		};
	}

	std::unique_ptr<Service> Serve(Origin &origin, ChunkStore store, const ServiceSettings &settings) {
		std::unique_ptr<http::Server> listener = http::Listen(settings.Port, settings.Server);
		if (!listener) {
			ENGINE_ERROR("cdn: could not bind port {}", settings.Port);
			return nullptr;
		}
		ENGINE_INFO("cdn: serving on {}", listener->Local().Text());
		return std::make_unique<HttpService>(origin, std::move(store), std::move(listener));
	}
}
