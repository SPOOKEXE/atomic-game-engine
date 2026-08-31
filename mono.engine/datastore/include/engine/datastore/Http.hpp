#pragma once

// A bounded HTTP object adapter for complete DataStore snapshots.
//
// @tier L12 · shared

#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Client.hpp>
#include <engine/world/DataStore.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace engine::datastore {
	// Conventional router name for the built-in HTTP adapter.
	inline constexpr std::string_view HTTP_DATASTORE_ADAPTER = "http";

	// Connection and request bounds for the plain HTTP provider.
	struct HttpDataStoreSettings {
		// Numeric address and port of the plain HTTP provider.
		net::Endpoint Server;
		// Value sent in the HTTP Host header.
		std::string Host;
		// Target prefix. The hex-encoded datastore name is appended to it.
		std::string TargetPrefix = "/datastores/";
		// Optional Authorization header value, including its scheme.
		std::string Authorization;
		// Maximum calls into the nonblocking transport for one operation.
		uint32_t MaximumPumpCalls = 100'000;
	};

	// Builds a remote adapter using the engine HTTP client.
	std::unique_ptr<world::DataStoreAdapter> MakeHttpDataStoreAdapter(HttpDataStoreSettings settings);

	// Builds an adapter around an injected client. Used by hosts with another
	// HTTP transport and by tests that must not touch the network.
	std::unique_ptr<world::DataStoreAdapter>
	MakeHttpDataStoreAdapter(HttpDataStoreSettings settings, std::unique_ptr<net::http::Client> client);
}
