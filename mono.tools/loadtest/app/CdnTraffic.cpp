#include "CdnTraffic.hpp"

#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/Grant.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/delivery/GroupCodec.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Client.hpp>
#include <engine/net/http/Message.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace loadtest {

	namespace {
		using engine::assets::ChunkStore;
		using engine::assets::Grant;
		using engine::assets::GrantKey;
		using engine::assets::GrantScope;
		using engine::assets::Manifest;
		using engine::assets::PublicKey;
		using engine::assets::SignatureBytes;
		using engine::delivery::Dictionary;
		using engine::net::Endpoint;
		using engine::net::http::Client;
		using engine::net::http::FetchId;
		using engine::net::http::FetchState;
		using engine::net::http::Method;
		using engine::net::http::Request;
		using engine::net::http::Response;
		using engine::net::http::Status;

		struct FetchWork {
			FetchId Id;
			size_t PlanIndex = 0;
			std::chrono::steady_clock::time_point Started;
		};

		struct PlannedFetch {
			size_t BundleIndex = 0;
			bool Hot = false;
		};

		struct FetchTotals {
			std::vector<double> HotMilliseconds;
			std::vector<double> ColdMilliseconds;
			uint64_t WireBytes = 0;
			uint64_t VerifiedBytes = 0;
			size_t QueueHighWater = 0;
		};

		int HexDigit(char value) {
			if (value >= '0' && value <= '9') return value - '0';
			if (value >= 'a' && value <= 'f') return value - 'a' + 10;
			return -1;
		}

		std::optional<GrantKey> ReadGrantKey(std::string_view text) {
			if (text.size() != GrantKey::BYTES * 2) return std::nullopt;
			std::array<std::byte, GrantKey::BYTES> bytes{};
			for (size_t index = 0; index < bytes.size(); index++) {
				const int high = HexDigit(text[index * 2]);
				const int low = HexDigit(text[index * 2 + 1]);
				if (high < 0 || low < 0) return std::nullopt;
				bytes[index] = static_cast<std::byte>((high << 4) | low);
			}
			return GrantKey::FromSecret(bytes);
		}

		std::string ToHex(std::span<const std::byte> bytes) {
			static constexpr char DIGITS[] = "0123456789abcdef";
			std::string result;
			result.reserve(bytes.size() * 2);
			for (const std::byte value : bytes) {
				const unsigned byte = static_cast<unsigned>(value);
				result.push_back(DIGITS[(byte >> 4u) & 0x0Fu]);
				result.push_back(DIGITS[byte & 0x0Fu]);
			}
			return result;
		}

		Request Get(std::string target) {
			Request request;
			request.Verb = Method::Get;
			request.Target = std::move(target);
			return request;
		}

		std::optional<Response> FetchOne(
			Client &client,
			const Endpoint &endpoint,
			const Request &request,
			std::string_view host,
			std::string &error
		) {
			const FetchId id = client.Submit(endpoint, request, host);
			if (!id.IsValid()) {
				error = "HTTP client refused a request before it was queued";
				return std::nullopt;
			}

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			for (;;) {
				client.Pump();
				const FetchState state = client.StateOf(id);
				if (state == FetchState::Ready) return client.Take(id);
				if (state == FetchState::Failed) {
					error = "HTTP request failed before a response was ready";
					return std::nullopt;
				}
				if (std::chrono::steady_clock::now() >= deadline) {
					client.Cancel(id);
					error = "HTTP request exceeded its 30 second deadline";
					return std::nullopt;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		double Percentile(const std::vector<double> &readings, double fraction) {
			if (readings.empty()) return 0.0;
			std::vector<double> sorted = readings;
			std::sort(sorted.begin(), sorted.end());
			const size_t rank = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1) + 0.5);
			return sorted[std::min(rank, sorted.size() - 1)];
		}

		bool VerifyBundleResponse(
			const Response &response,
			const engine::assets::BundleEntry &bundle,
			std::span<const std::byte> expected,
			const Dictionary *dictionary,
			std::string &error
		) {
			if (response.Code != Status::Ok) {
				error =
					"bundle request returned HTTP " + std::to_string(static_cast<unsigned>(response.Code));
				return false;
			}
			const std::optional<std::vector<std::byte>> decoded =
				dictionary == nullptr
					? engine::delivery::GroupCodec::Decompress(response.Body, bundle.TotalBytes)
					: engine::delivery::GroupCodec::Decompress(response.Body, *dictionary, bundle.TotalBytes);
			if (!decoded) {
				error = "bundle response did not decompress to the signed manifest size";
				return false;
			}
			if (decoded->size() != expected.size() ||
				!std::equal(decoded->begin(), decoded->end(), expected.begin())) {
				error = "decompressed bundle differs from the hash-verified local store";
				return false;
			}
			return true;
		}

		bool RunBundleCohort(
			Client &client,
			const Endpoint &endpoint,
			std::string_view host,
			std::span<const std::byte> grant,
			const Manifest &manifest,
			const std::vector<std::vector<std::byte>> &expected,
			const Dictionary *dictionary,
			uint32_t requestCount,
			uint32_t concurrency,
			FetchTotals &totals,
			std::string &error
		) {
			constexpr uint32_t HOT_PERCENT = 80;
			const uint32_t hotCount = requestCount * HOT_PERCENT / 100;
			const uint32_t coldCount = requestCount - hotCount;
			if (hotCount == 0 || coldCount == 0) {
				error = "CDN request count must leave both hot and cold requests";
				return false;
			}
			if (manifest.Bundles().size() < static_cast<size_t>(coldCount) + 1) {
				error = "CDN store needs one hot bundle and one unique bundle per cold request";
				return false;
			}

			std::vector<PlannedFetch> plan;
			plan.reserve(requestCount);
			for (uint32_t index = 0; index < hotCount; index++)
				plan.push_back({.BundleIndex = 0, .Hot = true});
			for (uint32_t index = 0; index < coldCount; index++)
				plan.push_back({.BundleIndex = static_cast<size_t>(index) + 1, .Hot = false});

			const std::string token = ToHex(grant);
			std::vector<FetchWork> active;
			active.reserve(concurrency);
			size_t next = 0;
			size_t complete = 0;
			while (complete < plan.size()) {
				while (next < plan.size() && active.size() < concurrency) {
					const PlannedFetch &item = plan[next];
					Request request = Get("/bundle/" + manifest.Bundles()[item.BundleIndex].Root.ToHex());
					request.Headers.push_back({.Name = "x-atomic-grant", .Value = token});
					const FetchId id = client.Submit(endpoint, request, host);
					if (!id.IsValid()) {
						error = "HTTP client refused a bundle request before it was queued";
						return false;
					}
					active.push_back({id, next, std::chrono::steady_clock::now()});
					++next;
					totals.QueueHighWater = std::max(totals.QueueHighWater, active.size());
				}

				client.Pump();
				for (size_t index = 0; index < active.size();) {
					const FetchState state = client.StateOf(active[index].Id);
					if (state == FetchState::Pending) {
						++index;
						continue;
					}
					if (state != FetchState::Ready) {
						error = "HTTP bundle request failed before a response was ready";
						return false;
					}
					const auto finished = std::chrono::steady_clock::now();
					std::optional<Response> response = client.Take(active[index].Id);
					if (!response) {
						error = "HTTP client lost a ready bundle response";
						return false;
					}
					const PlannedFetch &item = plan[active[index].PlanIndex];
					const auto &bundle = manifest.Bundles()[item.BundleIndex];
					if (!VerifyBundleResponse(
							*response, bundle, expected[item.BundleIndex], dictionary, error
						)) {
						return false;
					}
					const double milliseconds =
						std::chrono::duration<double, std::milli>(finished - active[index].Started).count();
					(item.Hot ? totals.HotMilliseconds : totals.ColdMilliseconds).push_back(milliseconds);
					totals.WireBytes += response->Body.size();
					totals.VerifiedBytes += bundle.TotalBytes;
					active[index] = active.back();
					active.pop_back();
					++complete;
				}
				if (complete < plan.size()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}
	}

	bool RunCdnTraffic(
		const std::filesystem::path &storePath,
		std::string_view publisherKeyText,
		std::string_view grantKeyText,
		std::string_view address,
		uint16_t port,
		uint32_t requests,
		uint32_t concurrency,
		std::string &error
	) {
		if (requests < 5 || requests > 1000 || concurrency == 0 || concurrency > 16 || port == 0) {
			error = "CDN traffic needs 5 to 1000 requests, concurrency from 1 to 16, and a valid port";
			return false;
		}
		const std::optional<PublicKey> publisherKey = PublicKey::FromHex(publisherKeyText);
		if (!publisherKey) {
			error = "CDN publisher key must be 64 lowercase hex characters";
			return false;
		}
		std::optional<GrantKey> grantKey = ReadGrantKey(grantKeyText);
		if (!grantKey) {
			error = "CDN grant key must be 64 lowercase hex characters";
			return false;
		}
		const std::optional<Endpoint> endpoint =
			Endpoint::Parse(std::string(address) + ":" + std::to_string(port));
		if (!endpoint) {
			error = "CDN address and port do not form a valid endpoint";
			return false;
		}

		std::optional<ChunkStore> store = ChunkStore::Open(storePath);
		if (!store) {
			error = "CDN local store could not be opened";
			return false;
		}
		SignatureBytes localSignature;
		std::optional<Manifest> localManifest = store->ReadManifest(localSignature);
		if (!localManifest ||
			!engine::assets::VerifyManifestRoot(localManifest->Root(), localSignature, *publisherKey)) {
			error = "CDN local manifest did not verify against the publisher key";
			return false;
		}

		engine::net::http::ClientSettings httpSettings;
		httpSettings.MaximumOutstanding = concurrency;
		httpSettings.IdlePolls = 30'000;
		httpSettings.Limits.BodyBytes = 64ull * 1024 * 1024;
		std::unique_ptr<Client> client = engine::net::http::MakeClient(httpSettings);
		const std::string host(address);

		Request manifestRequest = Get("/manifest");
		std::optional<Response> manifestResponse = FetchOne(*client, *endpoint, manifestRequest, host, error);
		if (!manifestResponse || manifestResponse->Code != Status::Ok ||
			manifestResponse->Body.size() <= SignatureBytes::BYTES) {
			if (error.empty()) error = "CDN manifest request did not return a signed manifest";
			return false;
		}
		SignatureBytes remoteSignature;
		std::memcpy(remoteSignature.Value.data(), manifestResponse->Body.data(), SignatureBytes::BYTES);
		engine::core::ByteReader reader(
			std::span<const std::byte>(
				manifestResponse->Body.data() + SignatureBytes::BYTES,
				manifestResponse->Body.size() - SignatureBytes::BYTES
			)
		);
		std::optional<Manifest> remoteManifest = Manifest::Read(reader);
		if (!remoteManifest || !reader.AtEnd() || remoteManifest->Root() != localManifest->Root() ||
			remoteSignature != localSignature ||
			!engine::assets::VerifyManifestRoot(remoteManifest->Root(), remoteSignature, *publisherKey)) {
			error = "CDN response manifest or signature differs from the trusted local publication";
			return false;
		}

		std::vector<std::vector<std::byte>> expected;
		expected.reserve(localManifest->Bundles().size());
		for (const auto &bundle : localManifest->Bundles()) {
			std::optional<std::vector<std::byte>> payload = store->ReadBundle(*localManifest, bundle);
			if (!payload || payload->size() != bundle.TotalBytes) {
				error = "CDN local store could not reassemble a manifest bundle";
				return false;
			}
			expected.push_back(std::move(*payload));
		}

		std::optional<Dictionary> dictionary;
		if (const std::optional<std::vector<std::byte>> localDictionary = store->ReadDictionary()) {
			Request dictionaryRequest = Get("/dictionary");
			std::optional<Response> response = FetchOne(*client, *endpoint, dictionaryRequest, host, error);
			if (!response || response->Code != Status::Ok || response->Body != *localDictionary) {
				if (error.empty())
					error = "CDN dictionary response differs from the trusted local dictionary";
				return false;
			}
			dictionary = Dictionary::Load(response->Body);
			if (!dictionary) {
				error = "CDN dictionary bytes are not a valid delivery dictionary";
				return false;
			}
		}

		const uint32_t hotCount = requests * 80u / 100u;
		const uint32_t coldCount = requests - hotCount;
		if (localManifest->Bundles().size() < static_cast<size_t>(coldCount) + 1) {
			error = "CDN store needs one hot bundle and one unique bundle per cold request";
			return false;
		}
		GrantScope scope;
		scope.Session = 1;
		scope.ExpiresAtSeconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
														   std::chrono::system_clock::now().time_since_epoch()
								 )
														   .count()) +
								 300;
		scope.ByteBudget = 1;
		for (const auto &bundle : remoteManifest->Bundles()) {
			scope.Bundles.push_back(bundle.Root);
		}
		// The signed budget covers the priming request plus every measured fetch.
		for (uint32_t index = 0; index <= hotCount; index++) {
			const uint64_t bytes = remoteManifest->Bundles().front().TotalBytes;
			if (bytes > std::numeric_limits<uint64_t>::max() - scope.ByteBudget) {
				error = "CDN grant byte budget overflowed";
				return false;
			}
			scope.ByteBudget += bytes;
		}
		for (uint32_t index = 0; index < coldCount; index++) {
			const uint64_t bytes = remoteManifest->Bundles()[static_cast<size_t>(index) + 1].TotalBytes;
			if (bytes > std::numeric_limits<uint64_t>::max() - scope.ByteBudget) {
				error = "CDN grant byte budget overflowed";
				return false;
			}
			scope.ByteBudget += bytes;
		}
		const std::optional<Grant> grant = Grant::Issue(std::move(scope), *grantKey);
		if (!grant) {
			error = "CDN grant could not be issued for this request cohort";
			return false;
		}
		const std::vector<std::byte> token = grant->Encode();

		Request warmupRequest = Get("/bundle/" + remoteManifest->Bundles().front().Root.ToHex());
		warmupRequest.Headers.push_back({.Name = "x-atomic-grant", .Value = ToHex(token)});
		std::optional<Response> warmup = FetchOne(*client, *endpoint, warmupRequest, host, error);
		if (!warmup || !VerifyBundleResponse(
						   *warmup,
						   remoteManifest->Bundles().front(),
						   expected.front(),
						   dictionary ? &*dictionary : nullptr,
						   error
					   )) {
			return false;
		}

		FetchTotals totals;
		if (!RunBundleCohort(
				*client,
				*endpoint,
				host,
				token,
				*remoteManifest,
				expected,
				dictionary ? &*dictionary : nullptr,
				requests,
				concurrency,
				totals,
				error
			)) {
			return false;
		}

		std::printf(
			"cdn fetch cohort\n  manifest signature: verified\n  dictionary: %s\n  bundle parity: %u cohort "
			"responses plus 1 warmup\n"
			"  requests: %u (80%% hot, 20%% cold)\n  warmup: 1 bundle\n  queue high-water: %zu\n"
			"  compressed bytes received: %llu\n  uncompressed bytes verified: %llu\n",
			dictionary ? "parity verified" : "not present",
			requests,
			requests,
			totals.QueueHighWater,
			static_cast<unsigned long long>(totals.WireBytes),
			static_cast<unsigned long long>(totals.VerifiedBytes)
		);
		std::printf(
			"  hot latency ms p50/p95/p99: %.3f / %.3f / %.3f\n",
			Percentile(totals.HotMilliseconds, 0.50),
			Percentile(totals.HotMilliseconds, 0.95),
			Percentile(totals.HotMilliseconds, 0.99)
		);
		std::printf(
			"  cold latency ms p50/p95/p99: %.3f / %.3f / %.3f\n",
			Percentile(totals.ColdMilliseconds, 0.50),
			Percentile(totals.ColdMilliseconds, 0.95),
			Percentile(totals.ColdMilliseconds, 0.99)
		);
		return true;
	}

}
