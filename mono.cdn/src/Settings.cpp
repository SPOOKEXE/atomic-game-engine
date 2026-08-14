#include <engine/core/Flags.hpp>
#include <engine/delivery/Source.hpp>

#include <cdn/Settings.hpp>
#include <string>

namespace cdn {
	namespace {
		// **Built from default-constructed settings**, so the numbers here are
		// the member initialisers in `Origin.hpp` and `Service.hpp` and not a
		// second copy of them.
		const engine::core::FlagTableBuilder &Table() {
			static const engine::core::FlagTableBuilder table = [] {
				const CDNSettings origin;
				const ServiceSettings service;
				engine::core::FlagTableBuilder built;

				built.Integer(
					"cdn.port",
					engine::delivery::DEFAULT_ORIGIN_PORT,
					"Port to listen on. Zero binds an ephemeral one"
				);
				built.Text(
					"cdn.store", "", "The content store to serve. Empty reads the directory beside the binary"
				);
				built.Text(
					"cdn.grant-key",
					"",
					"64 hex characters — the secret shared with the server that issues grants"
				);

				built.Boolean(
					"cdn.local-first",
					origin.LocalFirst,
					"Look in the local publication before asking any upstream. Off is a pure proxy"
				);
				built.Boolean(
					"cdn.allow-upstream", origin.AllowUpstream, "Whether a miss may be forwarded at all"
				);
				built.Boolean(
					"cdn.cache-upstream",
					origin.CacheUpstream,
					"Keep what an upstream returned, so the second request is local"
				);
				built.List(
					"cdn.upstreams",
					"An origin to forward a miss to, as NAME=HOST:PORT. Repeat the key for more, in the "
					"order they should be tried"
				);
				built.Integer(
					"cdn.upstream-attempts",
					origin.UpstreamAttempts,
					"How many upstreams to try before refusing"
				);
				built.Boolean(
					"cdn.verify-upstream",
					origin.VerifyUpstream,
					"Whether what an upstream returned must match the local manifest. Leave this on"
				);
				built.Integer(
					"cdn.prepare-per-pump",
					static_cast<int64_t>(origin.PreparePerPump),
					"The most groups one pump will prepare"
				);
				built.Integer(
					"cdn.compression-level", origin.CompressionLevel, "Zstd level groups are prepared at"
				);
				built.Integer(
					"cdn.cache-bytes",
					static_cast<int64_t>(origin.CacheCapacityBytes),
					"What the prepared-group cache may hold"
				);

				built.Text(
					"cdn.ingest-key",
					service.Ingest.Key,
					"Accept uploads at /ingest from whoever sends this as x-atomic-ingest"
				);
				built.Integer(
					"cdn.ingest-maximum-file-bytes",
					static_cast<int64_t>(service.Ingest.MaximumFileBytes),
					"The largest single upload an inbox will take"
				);
				built.Boolean(
					"cdn.list-contents",
					service.Catalogue.Enabled,
					"Answer GET /catalogue for whoever holds the ingest key. Off unless asked for"
				);
				built.Integer(
					"cdn.catalogue-page-entries",
					service.Catalogue.PageEntries,
					"How many names one page of GET /catalogue carries"
				);

				return built;
			}();
			return table;
		}
	}

	bool DeclareFlags() {
		return engine::core::Flags::Declare(Table().Rows());
	}

	CDNSettings OriginFromFlags() {
		CDNSettings settings;
		if (!engine::core::Flags::Has("cdn.local-first")) {
			return settings;
		}

		using engine::core::Flag;

		settings.LocalFirst = Flag("cdn.local-first").Boolean();
		settings.AllowUpstream = Flag("cdn.allow-upstream").Boolean();
		settings.CacheUpstream = Flag("cdn.cache-upstream").Boolean();
		settings.UpstreamAttempts = static_cast<uint32_t>(Flag("cdn.upstream-attempts").Integer());
		settings.VerifyUpstream = Flag("cdn.verify-upstream").Boolean();
		settings.PreparePerPump = static_cast<size_t>(Flag("cdn.prepare-per-pump").Integer());
		settings.CompressionLevel = static_cast<int>(Flag("cdn.compression-level").Integer());
		settings.CacheCapacityBytes = static_cast<uint64_t>(Flag("cdn.cache-bytes").Integer());

		// **`NAME=HOST:PORT`, one per entry.** A row with no `=` is dropped
		// rather than half-read: an upstream named after its whole address would
		// be one nothing can refer to, which is worse than one that is missing.
		for (const std::string &row : Flag("cdn.upstreams").Items()) {
			const size_t equals = row.find('=');
			if (equals == std::string::npos) {
				continue;
			}
			settings.Upstreams.push_back(
				UpstreamOrigin{
					.Name = row.substr(0, equals),
					.Endpoint = row.substr(equals + 1),
				}
			);
		}

		return settings;
	}

	ServiceSettings ServiceFromFlags() {
		ServiceSettings settings;
		if (!engine::core::Flags::Has("cdn.port")) {
			settings.Port = engine::delivery::DEFAULT_ORIGIN_PORT;
			return settings;
		}

		using engine::core::Flag;

		settings.Port = static_cast<uint16_t>(Flag("cdn.port").Integer());
		settings.Ingest.Key = std::string(Flag("cdn.ingest-key").Text());
		settings.Ingest.MaximumFileBytes =
			static_cast<uint64_t>(Flag("cdn.ingest-maximum-file-bytes").Integer());
		settings.Catalogue.Enabled = Flag("cdn.list-contents").Boolean();
		settings.Catalogue.PageEntries = static_cast<uint32_t>(Flag("cdn.catalogue-page-entries").Integer());
		return settings;
	}
}
