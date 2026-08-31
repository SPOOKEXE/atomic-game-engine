#include <engine/datastore/Http.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/SharedStoreFile.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>

TEST_SUITE_ID("engine.datastore.http")

namespace {
	using engine::net::http::FetchId;
	using engine::net::http::FetchState;
	using engine::net::http::Request;
	using engine::net::http::Response;

	std::vector<std::byte> Bytes(const std::string_view text) {
		const auto *first = reinterpret_cast<const std::byte *>(text.data());
		return {first, first + text.size()};
	}

	struct ClientState {
		Request Submitted;
		std::string Host;
		Response Answer;
		FetchState State = FetchState::Pending;
		uint32_t Pumps = 0;
		bool CompleteOnPump = true;
		bool Cancelled = false;
	};

	class Client final : public engine::net::http::Client {
	  public:
		explicit Client(std::shared_ptr<ClientState> state) : Shared(std::move(state)) {}

		FetchId
		Submit(const engine::net::Endpoint &, const Request &request, const std::string_view host) override {
			Shared->Submitted = request;
			Shared->Host = host;
			return {1};
		}

		FetchState StateOf(const FetchId id) const override {
			return id.Value == 1 ? Shared->State : FetchState::Unknown;
		}

		size_t Pump() override {
			++Shared->Pumps;
			if (Shared->CompleteOnPump) {
				Shared->State = FetchState::Ready;
				return 1;
			}
			return 0;
		}

		std::optional<Response> Take(const FetchId id) override {
			if (id.Value != 1 || Shared->State != FetchState::Ready) {
				return std::nullopt;
			}
			Shared->State = FetchState::Unknown;
			return Shared->Answer;
		}

		bool Cancel(const FetchId id) override {
			Shared->Cancelled = id.Value == 1;
			return Shared->Cancelled;
		}

		size_t Outstanding() const override {
			return Shared->State == FetchState::Unknown ? 0 : 1;
		}

		uint64_t ReceivedBytes() const override {
			return Shared->Answer.Body.size();
		}

	  private:
		std::shared_ptr<ClientState> Shared;
	};

	engine::datastore::HttpDataStoreSettings Settings() {
		return {
			.Server = *engine::net::Endpoint::Parse("127.0.0.1:8080"),
			.Host = "store.internal",
			.TargetPrefix = "/atomic/",
			.Authorization = "Bearer secret",
			.MaximumPumpCalls = 3,
		};
	}
}

TEST_CASE("the HTTP adapter writes a portable named datastore image", "[datastore][http]") {
	auto state = std::make_shared<ClientState>();
	state->Answer.Code = engine::net::http::Status::Ok;
	auto adapter = engine::datastore::MakeHttpDataStoreAdapter(Settings(), std::make_unique<Client>(state));
	const std::vector<engine::world::SharedStoreEntry> entries{
		{engine::world::BusKind::DataStore, engine::core::Name("player"), Bytes("value"), 4},
	};
	std::string error;
	REQUIRE(adapter->Save(engine::core::Name("main"), entries, error) == engine::world::DataStoreStatus::Ok);
	CHECK(state->Submitted.Verb == engine::net::http::Method::Put);
	CHECK(state->Submitted.Target == "/atomic/6d61696e");
	CHECK(state->Submitted.Find("authorization") == "Bearer secret");
	CHECK(state->Host == "store.internal");

	std::vector<engine::world::SharedStoreEntry> decoded;
	REQUIRE(
		engine::world::DecodeSharedStoreImage(
			state->Submitted.Body, engine::world::BusKind::DataStore, decoded, error
		) == engine::world::SharedStoreFileStatus::Ok
	);
	REQUIRE(decoded.size() == 1);
	CHECK(decoded[0] == entries[0]);
}

TEST_CASE("the HTTP adapter loads atomically and reports missing objects", "[datastore][http]") {
	auto state = std::make_shared<ClientState>();
	state->Answer.Code = engine::net::http::Status::Ok;
	const std::vector<engine::world::SharedStoreEntry> expected{
		{engine::world::BusKind::DataStore, engine::core::Name("player"), Bytes("new"), 5},
	};
	std::string error;
	REQUIRE(
		engine::world::EncodeSharedStoreImage(
			engine::world::BusKind::DataStore, expected, state->Answer.Body, error
		) == engine::world::SharedStoreFileStatus::Ok
	);
	auto adapter = engine::datastore::MakeHttpDataStoreAdapter(Settings(), std::make_unique<Client>(state));
	std::vector<engine::world::SharedStoreEntry> loaded;
	REQUIRE(adapter->Load(engine::core::Name("main"), loaded, error) == engine::world::DataStoreStatus::Ok);
	CHECK(loaded == expected);

	state->State = FetchState::Pending;
	state->Answer = {};
	state->Answer.Code = engine::net::http::Status::NotFound;
	CHECK(
		adapter->Load(engine::core::Name("missing"), loaded, error) ==
		engine::world::DataStoreStatus::NotFound
	);
	CHECK(loaded == expected);
}

TEST_CASE("the HTTP adapter cancels an operation past its pump budget", "[datastore][http]") {
	auto state = std::make_shared<ClientState>();
	state->CompleteOnPump = false;
	auto adapter = engine::datastore::MakeHttpDataStoreAdapter(Settings(), std::make_unique<Client>(state));
	std::vector<engine::world::SharedStoreEntry> loaded;
	std::string error;
	CHECK(
		adapter->Load(engine::core::Name("main"), loaded, error) == engine::world::DataStoreStatus::IoError
	);
	CHECK(state->Pumps == 3);
	CHECK(state->Cancelled);
	CHECK(error == "HTTP datastore request exceeded its pump budget");
}

TEST_CASE("the HTTP adapter refuses request and header injection", "[datastore][http]") {
	auto state = std::make_shared<ClientState>();
	engine::datastore::HttpDataStoreSettings settings = Settings();
	settings.Authorization = "Bearer token\r\nx-extra: injected";
	auto adapter =
		engine::datastore::MakeHttpDataStoreAdapter(std::move(settings), std::make_unique<Client>(state));
	std::vector<engine::world::SharedStoreEntry> loaded;
	std::string error;
	CHECK(
		adapter->Load(engine::core::Name("main"), loaded, error) == engine::world::DataStoreStatus::Refused
	);
	CHECK(state->Host.empty());
}
