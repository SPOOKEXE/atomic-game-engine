#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <cdn/Origin.hpp>
#include <utility>

namespace cdn {

	namespace {
		using engine::assets::ContentHash;
	}

	const char *Describe(RequestState state) {
		switch (state) {
		case RequestState::Unknown:
			return "unknown";
		case RequestState::Pending:
			return "pending";
		case RequestState::Ready:
			return "ready";
		case RequestState::Cancelled:
			return "cancelled";
		case RequestState::Refused:
			return "refused";
		}
		// No default label, so adding a state is a compiler warning here rather
		// than a log line reading "?" that nobody traces back.
		return "?";
	}

	bool CDNSettings::IsValid() const {
		if (PreparePerPump == 0) {
			return false;
		}
		if (AllowUpstream && (Upstreams.empty() || UpstreamAttempts == 0)) {
			// Reads as "this will forward" and behaves as "this refuses every
			// miss". The gap between those two is a deployment that looks
			// healthy and serves nothing.
			return false;
		}
		return true;
	}

	Publication::Publication(
		ContentRoot root,
		engine::assets::Manifest manifest,
		std::optional<engine::delivery::Dictionary> dictionary
	)
		: Directory(std::move(root)), Described(std::move(manifest)), Codebook(std::move(dictionary)) {
		if (Codebook) {
			CodebookHash = Codebook->Hash();
		}
	}

	Origin::Origin(engine::assets::GrantKey key, const CDNSettings &settings)
		: Admission(std::move(key)), Configured(settings.IsValid() ? settings : CDNSettings{}),
		  Prepared(settings.IsValid() ? settings.CacheCapacityBytes : CDNSettings{}.CacheCapacityBytes) {}

	bool Origin::Accepts(const ContentHash &bundle, const Publication &against, size_t bytes) const {
		if (!Configured.VerifyUpstream) {
			return true;
		}

		// The manifest is signed, so what it records a bundle weighs is a fact
		// about content the publisher committed to — not a hint from whoever
		// just answered the fetch.
		for (const auto &entry : against.Contents().Bundles()) {
			if (entry.Root == bundle) {
				return entry.TotalBytes == bytes;
			}
		}

		// A bundle the local manifest does not describe. Refused rather than
		// trusted: this origin cannot say anything about content it was not
		// published, and caching it would let an upstream decide what this
		// origin serves.
		return false;
	}

	std::optional<std::vector<std::byte>> Origin::Resolve(
		const ContentHash &bundle,
		const Publication &against,
		const PayloadSource &source,
		const UpstreamFetch &upstream,
		bool *fromUpstream
	) {
		if (fromUpstream != nullptr) {
			*fromUpstream = false;
		}
		const auto askUpstream = [&]() -> std::optional<std::vector<std::byte>> {
			if (!Configured.AllowUpstream || !upstream) {
				return std::nullopt;
			}

			// Bounded rather than "all of them": a request that walks ten dead
			// upstreams has spent ten timeouts before it refuses, and the client
			// gave up long before.
			const size_t attempts =
				std::min<size_t>(Configured.UpstreamAttempts, Configured.Upstreams.size());

			for (size_t index = 0; index < attempts; ++index) {
				auto payload = upstream(Configured.Upstreams[index], bundle);
				if (!payload) {
					engine::core::Metrics::Count("cdn.upstream.failed", 1.0);
					continue;
				}

				if (!Accepts(bundle, against, payload->size())) {
					// A proxy that forwards bytes it cannot check is a proxy
					// that launders a compromised upstream. Counted apart from a
					// failure: this one is an upstream answering *wrongly*
					// rather than not answering.
					engine::core::Metrics::Count("cdn.upstream.rejected", 1.0);
					continue;
				}

				engine::core::Metrics::Count("cdn.upstream.served", 1.0);
				if (fromUpstream != nullptr) {
					*fromUpstream = true;
				}
				return payload;
			}
			return std::nullopt;
		};

		const auto askLocal = [&]() -> std::optional<std::vector<std::byte>> {
			if (!source) {
				return std::nullopt;
			}
			auto payload = source(bundle);
			if (payload) {
				engine::core::Metrics::Count("cdn.local.served", 1.0);
			}
			return payload;
		};

		// Local first is what makes this a cache server rather than a proxy: a
		// hit costs a lookup and no network at all. Off, it is an edge node
		// holding no content of its own, which is a real deployment and not the
		// common one.
		if (Configured.LocalFirst) {
			if (auto payload = askLocal()) {
				return payload;
			}
			return askUpstream();
		}

		if (auto payload = askUpstream()) {
			return payload;
		}
		return askLocal();
	}

	bool Origin::Publish(std::shared_ptr<const Publication> publication) {
		if (!publication) {
			// An origin with nothing to serve refuses requests rather than
			// serving nothing, and the two are only distinguishable if a null
			// publish is refused here.
			return false;
		}

		// The swap. Requests already accepted hold their own shared pointer to
		// the publication they were admitted against, so this changes what
		// *future* requests see and nothing about the ones in flight.
		Serving = std::move(publication);

		// The previous publication's groups were compressed against content and
		// a dictionary that are no longer current, so keeping them wastes the
		// capacity the new publication needs.
		Prepared.Clear();

		engine::core::Metrics::Count("cdn.origin.published", 1.0);
		return true;
	}

	std::shared_ptr<const Publication> Origin::Current() const {
		return Serving;
	}

	RequestId
	Origin::Submit(std::span<const std::byte> token, const ContentHash &bundleRoot, uint64_t nowSeconds) {
		ENGINE_PROFILE_CAT("Origin::Submit", engine::core::ProfileCategory::Assets);

		const RequestId id{NextRequest++};

		Pending pending;
		pending.Bundle = bundleRoot;
		pending.Against = Serving;

		// The gate first, and it learns nothing about who asked. A refused
		// request still gets a handle so a caller can ask why once, rather than
		// a bare failure carrying no state.
		if (!Serving || !Admission.Admits(token, bundleRoot, nowSeconds)) {
			pending.State = RequestState::Refused;
			engine::core::Metrics::Count("cdn.origin.refused", 1.0);
		}

		Requests.emplace_back(id.Value, std::move(pending));
		return id;
	}

	RequestState Origin::StateOf(RequestId id) const {
		const auto found = std::find_if(Requests.begin(), Requests.end(), [id](const auto &entry) {
			return entry.first == id.Value;
		});
		return found == Requests.end() ? RequestState::Unknown : found->second.State;
	}

	bool Origin::Cancel(RequestId id) {
		const auto found = std::find_if(Requests.begin(), Requests.end(), [id](const auto &entry) {
			return entry.first == id.Value;
		});
		if (found == Requests.end()) {
			return false;
		}
		if (found->second.State != RequestState::Pending) {
			// Already finished. Cancelling a ready request would discard bytes a
			// caller is entitled to, and cancelling a refused one changes
			// nothing but the reason it reports.
			return false;
		}

		found->second.State = RequestState::Cancelled;
		// The frame, if one was somehow attached, goes with it. A cancelled
		// request's result is discarded rather than delivered to nobody.
		found->second.Frame.reset();

		engine::core::Metrics::Count("cdn.origin.cancelled", 1.0);
		return true;
	}

	size_t Origin::Pump(const PayloadSource &source, const UpstreamFetch &upstream) {
		ENGINE_PROFILE_CAT("Origin::Pump", engine::core::ProfileCategory::Assets);

		// What to prepare this pump. Gathered first so that the parallel step
		// below has a fixed set — a fan-out over a container something else may
		// append to is a data race with a plausible-looking body.
		std::vector<size_t> work;
		for (size_t index = 0; index < Requests.size() && work.size() < Configured.PreparePerPump; ++index) {
			if (Requests[index].second.State == RequestState::Pending) {
				work.push_back(index);
			}
		}

		if (work.empty()) {
			return 0;
		}

		const engine::delivery::Dictionary *dictionary = nullptr;
		ContentHash dictionaryHash;
		if (Serving) {
			dictionary = Serving->CompressionDictionary();
			dictionaryHash = Serving->DictionaryHash();
		}

		// The cache lookups happen before the fan-out, on one thread. A hit
		// costs a lookup and no compression at all, and doing them here keeps
		// the parallel step to the work that is actually expensive.
		std::vector<size_t> toCompress;
		for (const size_t index : work) {
			Pending &pending = Requests[index].second;

			// A request is served against the publication it was *admitted*
			// against, not whatever is current now. That is what makes the
			// atomic swap meaningful rather than decorative.
			const Publication *against = pending.Against.get();
			if (against == nullptr) {
				pending.State = RequestState::Refused;
				continue;
			}

			const PreparedKey key{pending.Bundle, against->DictionaryHash()};
			if (PreparedFrame cached = Prepared.Find(key)) {
				pending.Frame = std::move(cached);
				pending.State = RequestState::Ready;
				continue;
			}

			toCompress.push_back(index);
		}

		// **The IO, and it is deliberately outside the fan-out below.**
		//
		// Resolving a payload reads a filesystem or talks to an upstream. A
		// construct that occupies a worker while it waits turns an IO-bound
		// origin into a thread-starved one — CDN.md §3 — so this stage is its
		// own, and only the compression that follows is fanned out.
		std::vector<std::optional<std::vector<std::byte>>> payloads(toCompress.size());
		std::vector<bool> forwarded(toCompress.size(), false);
		for (size_t slot = 0; slot < toCompress.size(); ++slot) {
			const Pending &pending = Requests[toCompress[slot]].second;
			bool fromUpstream = false;
			payloads[slot] = Resolve(pending.Bundle, *pending.Against, source, upstream, &fromUpstream);
			forwarded[slot] = fromUpstream;
		}

		// One entry per group being compressed, so the parallel step writes only
		// what its own index names — the property `Jobs::For` requires and the
		// reason inline and pooled execution are observationally identical.
		std::vector<std::optional<std::vector<std::byte>>> frames(toCompress.size());

		if (!toCompress.empty()) {
			// **The one place a fan-out job is right in this module.**
			// Compressing a known set of groups is CPU work with a known end,
			// which is exactly what Jobs::For is for. Nothing in this body waits
			// on anything: the bytes were resolved above.
			//
			// Grain of one: a group is tens of milliseconds of compression, so
			// the default grain of 4096 would put every group on one worker.
			engine::parallel::Jobs::For(toCompress.size(), 1, [&](size_t begin, size_t end) {
				for (size_t slot = begin; slot < end; ++slot) {
					if (!payloads[slot]) {
						continue;
					}

					frames[slot] = dictionary != nullptr
									   ? engine::delivery::GroupCodec::Compress(
											 *payloads[slot], *dictionary, Configured.CompressionLevel
										 )
									   : engine::delivery::GroupCodec::Compress(
											 *payloads[slot], Configured.CompressionLevel
										 );
				}
			});
		}

		// Back on one thread to publish the results, because the cache insert
		// and the request table are shared and this is the cheap half.
		size_t finished = 0;
		for (const size_t index : work) {
			Pending &pending = Requests[index].second;
			if (pending.State == RequestState::Ready) {
				++finished;
			}
		}

		for (size_t slot = 0; slot < toCompress.size(); ++slot) {
			Pending &pending = Requests[toCompress[slot]].second;

			// Cancelled while this pump was compressing. The result is discarded
			// rather than delivered to nobody — and deliberately not cached
			// either, because a group nobody asked for evicts one somebody did.
			if (pending.State != RequestState::Pending) {
				continue;
			}

			if (!frames[slot]) {
				pending.State = RequestState::Refused;
				engine::core::Metrics::Count("cdn.origin.refused", 1.0);
				++finished;
				continue;
			}

			// A forwarded group is kept only when this origin is configured as a
			// cache. Without that, it fetches the same bundle from the same
			// upstream for every client that asks — a proxy rather than a cache
			// server, which is a legitimate deployment and a different one.
			const bool keep = !forwarded[slot] || Configured.CacheUpstream;

			PreparedFrame stored;
			if (keep) {
				const PreparedKey key{pending.Bundle, dictionaryHash};
				stored = Prepared.Insert(key, std::move(*frames[slot]));
			}
			if (!stored) {
				// Not kept, or too large for the cache to hold. Still served —
				// the request asked for it and the bytes exist.
				stored = std::make_shared<const std::vector<std::byte>>(std::move(*frames[slot]));
			}

			pending.Frame = std::move(stored);
			pending.State = RequestState::Ready;
			engine::core::Metrics::Count("cdn.origin.prepared", 1.0);
			++finished;
		}

		return finished;
	}

	PreparedFrame Origin::Take(RequestId id) {
		const auto found = std::find_if(Requests.begin(), Requests.end(), [id](const auto &entry) {
			return entry.first == id.Value;
		});
		if (found == Requests.end() || found->second.State != RequestState::Ready) {
			return nullptr;
		}

		PreparedFrame frame = std::move(found->second.Frame);

		// The request is finished by this call. Leaving it in the table would
		// grow it for the life of the process, and a second Take answering the
		// same bytes would hide a caller taking one result twice.
		Requests.erase(found);

		engine::core::Metrics::Count("cdn.origin.served", 1.0);
		return frame;
	}

	size_t Origin::Outstanding() const {
		return Requests.size();
	}
}
