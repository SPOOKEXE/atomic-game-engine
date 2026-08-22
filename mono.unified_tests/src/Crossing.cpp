#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/delivery/Relay.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Handshake.hpp>
#include <engine/net/quic/Tls.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cdn/Publisher.hpp>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <server/Simulation.hpp>
#include <stdexcept>
#include <system_error>
#include <unified/Crossing.hpp>
#include <utility>

namespace unified {

	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Store;
	using engine::scene::Transform;

	namespace {
		namespace fs = std::filesystem;

		// Jobs is process-wide and this diagnostic owns it only for its last instance.
		int LiveCrossings = 0;

		// The two routes the content axis cycles through.
		//
		// **The origin's own routes and not a second protocol**, which is
		// `delivery/Relay.hpp`'s rule: a relayed request is the same string
		// `cdn::Service` answers. The bundle routes are left out because
		// naming one means reading the manifest, and this is a check on the
		// path rather than on the catalogue.
		constexpr std::string_view ROUTES[] = {
			engine::delivery::MANIFEST_ROUTE,
			engine::delivery::DICTIONARY_ROUTE,
		};

		// Datagram arrival numbers a lossy arrangement drops when the caller
		// named none.
		//
		// **A lossy link that loses nothing passes for the wrong reason**, and
		// `--arrangement lossy` with no `--drop` is the way this is usually
		// run. Early enough to land inside the join, so the reliable channel's
		// retransmission is exercised rather than merely available.
		constexpr uint64_t DEFAULT_DROPS[] = {30, 60, 90};

		// A directory nothing else is using, created rather than named.
		//
		// **Created in a loop rather than named from a clock or from entropy.**
		// `create_directory` answers `false` for a directory that was already
		// there, so the first call that answers `true` is the first name nobody
		// else holds - which is a claim a unique-looking name cannot make, and
		// this module's rule about never reading a clock leaves no other way to
		// make it.
		fs::path Fresh(std::string_view purpose) {
			static std::atomic<uint64_t> next{0};

			const fs::path base = fs::temp_directory_path();
			for (int attempt = 0; attempt < 4096; attempt++) {
				const uint64_t ordinal = next.fetch_add(1, std::memory_order_relaxed);
				fs::path candidate =
					base / ("atomic-unified-" + std::string(purpose) + "-" + std::to_string(ordinal));

				std::error_code failed;
				if (fs::create_directories(candidate, failed) && !failed) {
					return candidate;
				}
			}
			return {};
		}

		// The inner payload of a `User` message, or nothing when this is not one.
		//
		// **The seam `Connector` and `Listener` sit on**, reproduced here
		// because this program drives a bare `Session` rather than either of
		// them - the same reason `replication/tests/Wire.hpp` does. A content
		// request is a payload `replication` carries and does not read, so it
		// crosses wrapped and has to be unwrapped on arrival.
		std::optional<std::vector<std::byte>> UserPayload(const std::vector<std::byte> &message) {
			if (engine::replication::PeekMessageKind(message) != engine::replication::MessageKind::User) {
				return std::nullopt;
			}

			engine::core::ByteReader reader(message);
			engine::replication::Message read;
			if (!engine::replication::ReadMessage(reader, read)) {
				return std::nullopt;
			}
			return std::move(read.User.Bytes);
		}

		// Records that share a shape and differ in their numbers, so a
		// publication compresses for the reason real content does and a
		// dictionary has something to learn from.
		std::string Structured(size_t index, size_t records) {
			std::string text;
			text.reserve(records * 48);
			for (size_t record = 0; record < records; record++) {
				text += "{\"asset\":";
				text += std::to_string(index);
				text += ",\"record\":";
				text += std::to_string(record);
				text += ",\"lod\":";
				text += std::to_string(record % 4);
				text += ",\"flags\":";
				text += std::to_string((record * 2654435761u) % 65536);
				text += "}\n";
			}
			return text;
		}
	}

	Crossing::Crossing(const Settings &settings, const Arrangement &arrangement)
		: Options(settings), Wired(arrangement), Server("unified.server"), Client("unified.client") {
		ENGINE_HEAP_SCOPE("unified.build");

		// One worker keeps diagnostic runs deterministic.
		if (LiveCrossings == 0) {
			engine::parallel::Jobs::Start(Options.Workers);
		}
		LiveCrossings++;

		BuildWorlds();

		// **Order matters here and it is the order a program comes up in.** The
		// wire has to exist before the content link, because under a wire
		// arrangement a route request leaves through the client's session; and
		// discovery is last because it is beside everything else rather than
		// under it.
		if (OverAWire(Wired.Carrying)) {
			BuildWire();
		}
		if (Wired.Serving == Content::Relayed) {
			BuildContent();
		}
		if (Wired.Finding == Discovery::Advertised) {
			BuildDiscovery();
		}

		Tally.Ran = Wired;
	}

	Crossing::~Crossing() {
		// The link holds a callback that reaches the relay and the session, so
		// it goes first. Nothing below it can be torn down while it may still
		// be asked to send.
		Link_.reset();
		Relay_.reset();

		std::error_code failed;
		if (!ContentRoot.empty()) {
			fs::remove_all(ContentRoot, failed);
		}
		if (!StoreRoot.empty()) {
			fs::remove_all(StoreRoot, failed);
		}

		LiveCrossings--;
		if (LiveCrossings == 0) {
			engine::parallel::Jobs::Stop();
		}
	}

	void Crossing::BuildWorlds() {
		// Both sides register the same names independently.
		server::RegisterPlaceholderComponents();
		if (Options.ScenePath.empty()) {
			server::BuildPlaceholderWorld(Server, ServerSystems, Options.Entities);
		} else {
			// Load only the server half; the client must receive its state.
			std::string error;
			if (!engine::examples::LoadScene(Server, ServerSystems, Options.ScenePath, error)) {
				ENGINE_ERROR("--scene '{}' failed:\n{}", Options.ScenePath, error);
				throw std::runtime_error("the scene script failed");
			}
		}

		// Observe per-tick components; signed components use snapshot signatures.
		Server.Observe<Transform>();
		Server.Observe<engine::scene::Motion>();

		if (Options.Interpolation.TickRate <= 0.0) {
			Options.Interpolation.TickRate = Options.TickRate;
		}
		client::BuildReplicatedWorld(Client, ClientSystems, Options.Interpolation);

		for (const engine::replication::ReplicatedComponent &component :
			 engine::replication::DefaultReplicatedComponents()) {
			Authority_.Replicate(engine::core::Name(component.Name), component.Detection);
		}

		Handle = Authority_.Admit();

		// Scan for the lowest row instead of assuming entity allocation order.
		Server.Each<const Transform>([this](Entity entity, const Transform &) {
			if (Probe_ == engine::ecs::NULL_ENTITY || entity.Id < Probe_.Id) {
				Probe_ = entity;
			}
		});
	}

	void Crossing::BuildWire() {
		// **The same arrangement `replication/tests/Wire.hpp` builds**, and
		// deliberately so: that header is where the protocol is cornered over a
		// real link, and a second way of standing one up here would be a second
		// thing to keep in step. What this adds is the client's draw seam and
		// the server's real world on either end of it.
		std::vector<std::unique_ptr<engine::net::Transport>> ends = engine::net::MakeLoopbackTransport(2);
		if (ends.size() != 2) {
			throw std::runtime_error("the loopback did not give two ends");
		}

		engine::net::LossSettings toClient;
		if (Loses(Wired.Carrying)) {
			toClient.Drop = Options.Drop;
			if (toClient.Drop.empty()) {
				toClient.Drop.assign(std::begin(DEFAULT_DROPS), std::end(DEFAULT_DROPS));
			}
		}

		// Loss is applied where a datagram lands, so the wrapper on the
		// client's end is the one that loses what the server sent. The server's
		// end loses nothing: a run where both directions fail is one where the
		// acknowledgement that would have repaired the loss is itself lost, and
		// that is a case about congestion rather than about these seams.
		ServerEnd = std::make_unique<engine::net::LossyTransport>(std::move(ends[0]));
		ClientEnd = std::make_unique<engine::net::LossyTransport>(std::move(ends[1]), toClient);

		if (OverQuic(Wired.Carrying)) {
			BuildQuicWire();
			return;
		}
		BuildDatagramWire();
	}

	void Crossing::BuildDatagramWire() {
		const engine::replication::SessionSettings session;
		auto serving = std::make_unique<engine::replication::Session>(
			*ServerEnd, ClientEnd->Local(), engine::net::ConnectionId{1, 1}, Now, session
		);
		auto replying = std::make_unique<engine::replication::Session>(
			*ClientEnd, ServerEnd->Local(), engine::net::ConnectionId{2, 1}, Now, session
		);
		ServerDatagram = serving.get();
		ClientDatagram = replying.get();

		// Both ends live before anything is sent. A link still handshaking
		// refuses traffic, which is correct and is not what this is about.
		if (!ServerDatagram->Link().CompleteHandshake(Now) ||
			!ClientDatagram->Link().CompleteHandshake(Now)) {
			throw std::runtime_error("a link refused to complete its handshake");
		}

		// And both hold keys, because a session that does not carries nothing
		// at all. The same two calls `Listener` and `Connector` make, in order.
		std::optional<engine::net::Handshake> responder =
			engine::net::Handshake::Begin(engine::net::HandshakeRole::Responder);
		std::optional<engine::net::Handshake> initiator =
			engine::net::Handshake::Begin(engine::net::HandshakeRole::Initiator);
		if (!responder.has_value() || !initiator.has_value()) {
			throw std::runtime_error("the handshake would not begin");
		}
		if (!responder->Consume(initiator->Message()) || !initiator->Consume(responder->Message())) {
			throw std::runtime_error("the handshake would not complete");
		}

		std::optional<engine::net::Handshake::Session> serverKeys = responder->TakeKeys();
		std::optional<engine::net::Handshake::Session> clientKeys = initiator->TakeKeys();
		if (!serverKeys.has_value() || !clientKeys.has_value()) {
			throw std::runtime_error("the handshake produced no keys");
		}
		if (!ServerDatagram->AdoptKeys(std::move(*serverKeys)) ||
			!ClientDatagram->AdoptKeys(std::move(*clientKeys))) {
			throw std::runtime_error("a session refused its keys");
		}

		ServerSide = std::move(serving);
		ClientSide = std::move(replying);
	}

	void Crossing::BuildQuicWire() {
		// **A real TLS 1.3 handshake, driven to completion here rather than
		// faked.** There is no equivalent of `AdoptKeys` under QUIC - the keys
		// are the handshake's own - so the only way to a carrying session is to
		// run the flights, which is also the only way this arrangement proves
		// anything the datagram one does not.
		engine::replication::QuicSessionSettings serving;
		// A seed stated rather than drawn, so a failing run reproduces from the
		// arrangement name alone.
		for (size_t index = 0; index < serving.Connection.Tls.Seed.size(); index++) {
			serving.Connection.Tls.Seed[index] = static_cast<std::byte>(index * 7 + 3);
		}
		serving.Connection.Tls.HasSeed = true;

		engine::replication::QuicSessionSettings joining;
		joining.Connection.Tls.PinIdentity = true;
		joining.Connection.Tls.Expected = engine::net::quic::IdentityFor(serving.Connection.Tls.Seed);

		std::unique_ptr<engine::replication::QuicSession> client =
			engine::replication::QuicSession::Connect(*ClientEnd, ServerEnd->Local(), Now, joining);
		if (client == nullptr) {
			throw std::runtime_error("the QUIC client would not begin");
		}
		client->Flush(Now);

		std::vector<std::byte> first;
		if (ServerEnd->Receive(first).Status != engine::net::TransportStatus::Ok) {
			throw std::runtime_error("the QUIC client's first packet did not arrive");
		}

		std::unique_ptr<engine::replication::QuicSession> server =
			engine::replication::QuicSession::Accept(*ServerEnd, ClientEnd->Local(), first, Now, serving);
		if (server == nullptr) {
			throw std::runtime_error("the QUIC server would not answer the first packet");
		}

		// Bounded, and the bound is stated: a handshake that has not finished in
		// this many tick periods is a failure to report rather than a loop to
		// keep spinning. Under `quic-lossy` the drops are what the retransmission
		// timers are for, and they need the clock to move.
		constexpr int MAXIMUM_FLIGHTS = 600;
		for (int flight = 0; flight < MAXIMUM_FLIGHTS; flight++) {
			server->Flush(Now);
			client->Flush(Now);
			Drain(*ClientEnd, *client);
			Drain(*ServerEnd, *server);
			if (server->Carrying() && client->Carrying()) {
				break;
			}
			Now += 1.0 / static_cast<double>(std::max<uint32_t>(Options.TickRate, 1));
			server->Advance(Now);
			client->Advance(Now);
		}

		if (!server->Carrying() || !client->Carrying()) {
			throw std::runtime_error("the QUIC handshake did not complete");
		}

		ServerSide = std::move(server);
		ClientSide = std::move(client);
	}

	void Crossing::BuildContent() {
		// **A real publication, written by `cdn::Publish`.** A store assembled
		// here by hand would be this program's idea of what a publisher writes,
		// and the seam being checked is precisely whether the publisher and the
		// fetcher agree - so the one thing that must not be imitated is the
		// publisher.
		const fs::path content = Fresh("content");
		const fs::path store = Fresh("store");
		if (content.empty() || store.empty()) {
			throw std::runtime_error("no directory could be made for the publication");
		}
		ContentRoot = content.string();
		StoreRoot = store.string();

		for (uint32_t index = 0; index < std::max<uint32_t>(Options.ContentFiles, 1); index++) {
			std::ofstream file(content / ("records-" + std::to_string(index) + ".json"), std::ios::binary);
			const std::string text = Structured(index, 1200);
			file.write(text.data(), static_cast<std::streamsize>(text.size()));
		}

		// A fixed seed rather than a drawn key: the run is reproducible from
		// its settings, and a signature that differed between two runs would be
		// the one thing in the report that could not be compared.
		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			seed[index] = static_cast<std::byte>(0x40 + index);
		}
		const std::optional<engine::assets::SigningKey> key = engine::assets::SigningKey::FromSeed(seed);
		if (!key.has_value()) {
			throw std::runtime_error("the publisher key would not derive");
		}

		cdn::PublishSettings publishing;
		// **Sized against the content rather than left at the default.** Zstd
		// warns on stderr when the dictionary it is asked for is more than a
		// tenth of what it may learn from, and a diagnostic whose clean run
		// prints a warning is one people stop reading.
		publishing.DictionaryBytes = 16 * 1024;

		Tally.Published = cdn::Publish(content, store, *key, publishing);
		if (!Tally.Published.has_value()) {
			throw std::runtime_error("the publication failed");
		}

		engine::delivery::DeliverySettings sources;
		sources.Sources.push_back(
			engine::delivery::Source{
				.Name = "unified",
				.Kind = engine::delivery::SourceKind::Directory,
				.Location = StoreRoot,
				.Enabled = true,
			}
		);
		Relay_ = std::make_unique<server::ContentRelay>(engine::delivery::MakeRouteFetcher(sources));

		// The client's requests leave the way a real client's do: through
		// whatever is carrying, or straight into the relay when nothing is.
		Link_ = std::make_unique<client::ContentLink>([this](std::span<const std::byte> request) {
			if (ClientSide != nullptr) {
				return SendUser(*ClientSide, request);
			}
			Relay_->Receive(Handle, request, Now);
			return true;
		});
	}

	void Crossing::BuildDiscovery() {
		// **A broadcast subnet, because that is what a beacon announces on.**
		// `Endpoint::BroadcastIPv4` is where every announcement goes, and a
		// loopback opened without `Broadcast` routes none of it - which would
		// make this axis a directory that hears nothing and reports it as a
		// clean run.
		engine::net::TransportSettings broadcast;
		broadcast.Broadcast = true;
		Subnet = engine::net::MakeLoopbackTransport(2, broadcast);
		if (Subnet.size() != 2) {
			throw std::runtime_error("the discovery subnet did not give two ends");
		}

		// A fixed id rather than a drawn one, for the reason the signing seed
		// above is fixed: two runs of one command have to agree.
		network::Advert advert;
		for (size_t index = 0; index < network::SessionId::BYTES; index++) {
			advert.Session.Value[index] = static_cast<std::byte>(0x10 + index);
		}
		advert.Use = network::Purpose::Game;
		advert.Admits = network::Access::Public;
		advert.At = ServerEnd == nullptr ? engine::net::Endpoint::LoopbackIPv4(1) : ServerEnd->Local();
		advert.Name = "unified";
		advert.Detail = Wired.Name();
		advert.PeerLimit = 1;
		advert.Peers = 1;

		network::BeaconSettings schedule;
		schedule.AnnounceEverySeconds =
			static_cast<double>(std::max<uint32_t>(Options.AnnounceEveryTicks, 1)) / Options.TickRate;

		Beacon_ = std::make_unique<network::Beacon>(*Subnet[0], advert, std::nullopt, schedule);

		network::DirectorySettings collecting;
		// Long enough that a run never forgets what it just heard: an expiry
		// firing mid-run would make the announcement count and the listing
		// count disagree for a reason that is not a bug.
		collecting.ForgetAfterSeconds = 1.0e6;
		Directory_ = std::make_unique<network::Directory>(collecting);
	}

	float Crossing::PositionOf(Store &store, Entity entity) const {
		const Transform *transform = store.Get<Transform>(entity);
		return transform == nullptr ? 0.0f : transform->Frame.Position.X;
	}

	float Crossing::DrawnPositionOf(const client::DrawList &drawList, Entity entity) {
		size_t ordinal = 0;
		bool found = false;

		Client.Each<const Transform, const engine::scene::Bounds, const engine::scene::Visual>(
			[&](Entity candidate,
				const Transform &,
				const engine::scene::Bounds &,
				const engine::scene::Visual &visual) {
				if (found) {
					return;
				}

				// **The same skip `CollectReplicated` makes, and leaving it out
				// was silently wrong.** This walk exists to turn an entity into
				// its ordinal in the draw list, which means it has to match the
				// walk that *built* the list row for row. That one drops an
				// invisible instance; this one counted it, so one hidden part
				// anywhere ahead of the subject shifted every ordinal after it
				// and this reported a different entity's position - as a plain
				// number, with nothing to say it was the wrong entity's.
				if (!visual.Visible) {
					return;
				}

				if (candidate == entity) {
					found = true;
					return;
				}
				ordinal++;
			}
		);

		if (!found || ordinal >= drawList.Instances.size()) {
			return 0.0f;
		}
		return drawList.Instances[ordinal].Frame.Position.X;
	}

	bool Crossing::Join(int limit) {
		for (int attempt = 0; attempt < limit && !Replica_.Joined(); attempt++) {
			Step();
		}
		return Replica_.Joined();
	}

	void Crossing::Drain(engine::net::Transport &transport, engine::replication::SessionPort &into) {
		std::vector<std::byte> datagram;
		while (transport.Receive(datagram).Status == engine::net::TransportStatus::Ok) {
			into.Receive(datagram, Now);
		}
	}

	bool Crossing::SendUser(engine::replication::SessionPort &over, std::span<const std::byte> payload) {
		// The two calls `Connector::SendUser` makes, and the reason they are
		// here is that this program drives a `Session` rather than a
		// `Connector`: a raw payload handed to `Session::Send` is refused,
		// because it is not a message this module can name a channel for.
		engine::core::ByteWriter writer;
		engine::replication::User carried;
		carried.Bytes.assign(payload.begin(), payload.end());
		engine::replication::WriteMessage(writer, carried);
		return over.Send(writer.Bytes(), Now);
	}

	void Crossing::TakeAtClient(const std::vector<std::byte> &message) {
		if (ClientSide != nullptr) {
			// Over a wire the kind decides, exactly as `Connector` decides:
			// there is a `MessageKind` on the front of everything and guessing
			// by trying one reader and then another would be a router that
			// works until two payloads happen to start the same way.
			if (std::optional<std::vector<std::byte>> carried = UserPayload(message)) {
				if (Link_ != nullptr) {
					Link_->Receive(*carried);
				}
				return;
			}
			Replica_.Receive(Client, message);
			return;
		}

		if (Link_ != nullptr && Link_->Receive(message)) {
			return;
		}
		Replica_.Receive(Client, message);
	}

	void Crossing::TakeAtServer(const std::vector<std::byte> &message) {
		if (ServerSide != nullptr) {
			if (std::optional<std::vector<std::byte>> carried = UserPayload(message)) {
				if (Relay_ != nullptr) {
					Relay_->Receive(Handle, *carried, Now);
				}
				return;
			}
			Authority_.Receive(Handle, message);
			return;
		}

		if (Relay_ != nullptr && Relay_->Receive(Handle, message, Now)) {
			return;
		}
		Authority_.Receive(Handle, message);
	}

	void Crossing::CarryDirect(Report &report) {
		ENGINE_HEAP_SCOPE("unified.carry.direct");

		// Direct handoff: this arrangement excludes transport framing and
		// timing, which is the whole of what it is for.
		const std::span<const std::vector<std::byte>> messages = Authority_.Outgoing(Handle);
		report.Messages = messages.size();

		for (const std::vector<std::byte> &message : messages) {
			report.Bytes += message.size();
			report.LargestMessage = std::max(report.LargestMessage, message.size());

			const uint64_t ordinal = Handed_++;
			Tally.Produced++;
			if (std::find(Options.Drop.begin(), Options.Drop.end(), ordinal) != Options.Drop.end()) {
				// Deliberate loss is invisible to the authority.
				report.Dropped++;
				Tally.Lost++;
				continue;
			}

			Tally.Handed++;
			TakeAtClient(message);
		}

		Server.ClearChanges();

		const std::vector<std::byte> acknowledgement = Replica_.Acknowledge();
		if (!acknowledgement.empty()) {
			TakeAtServer(acknowledgement);
		}
	}

	void Crossing::CarryOverWire(Report &report) {
		ENGINE_HEAP_SCOPE("unified.carry.wire");

		const uint64_t droppedBefore = ClientEnd->Stats().Dropped;

		// **Refusals handed straight back, exactly as `Listener` does.** A
		// snapshot chunk the link refuses is a permanent hole unless the
		// authority is told, so a send loop that ignored the return value would
		// be one no program uses and would pass while the real one hung.
		const std::span<const std::vector<std::byte>> messages = Authority_.Outgoing(Handle);
		report.Messages = messages.size();

		for (size_t index = 0; index < messages.size(); index++) {
			report.Bytes += messages[index].size();
			report.LargestMessage = std::max(report.LargestMessage, messages[index].size());

			Handed_++;
			Tally.Produced++;
			if (ServerSide->Send(messages[index], Now)) {
				Tally.Handed++;
				continue;
			}

			Authority_.Unsent(Handle, index);
			Tally.Refused++;
			report.Dropped++;
		}

		Server.ClearChanges();

		ServerSide->Flush(Now);
		Drain(*ClientEnd, *ClientSide);

		for (const std::vector<std::byte> &message : ClientSide->Inbound()) {
			TakeAtClient(message);
		}
		ClientSide->ClearInbound();

		const std::vector<std::byte> acknowledgement = Replica_.Acknowledge();
		if (!acknowledgement.empty()) {
			ClientSide->Send(acknowledgement, Now);
		}
		ClientSide->Flush(Now);
		Drain(*ServerEnd, *ServerSide);

		for (const std::vector<std::byte> &message : ServerSide->Inbound()) {
			TakeAtServer(message);
		}
		ServerSide->ClearInbound();

		// Both sessions turn their tick over, so an idle timeout is a thing this
		// could hit rather than a thing it is exempt from. One call rather than
		// the `Advance`/`ResetBudget` pair a caller used to make - see
		// `SessionPort::Advance` for why they were never usefully separate.
		ServerSide->Advance(Now);
		ClientSide->Advance(Now);

		const uint64_t lost = ClientEnd->Stats().Dropped - droppedBefore;
		Tally.Lost += lost;
		report.Dropped += static_cast<size_t>(lost);
	}

	size_t Crossing::PumpContent() {
		ENGINE_HEAP_SCOPE("unified.content");

		// One route at a time, cycling, so the counters keep moving for as long
		// as a run lasts without the client ever having more outstanding than a
		// real one would.
		if (!Asking) {
			const std::string_view route = ROUTES[NextRoute % std::size(ROUTES)];
			if (Link_->Ask(NextRoute + 1, route)) {
				Asking = true;
			}
			NextRoute++;
		}

		Relay_->Pump([this](engine::replication::ClientId, std::span<const std::byte> payload) {
			if (ServerSide != nullptr) {
				return SendUser(*ServerSide, payload);
			}
			Link_->Receive(payload);
			return true;
		});

		std::vector<engine::delivery::RelayAnswer> finished;
		Link_->Collect(finished);
		if (!finished.empty()) {
			Asking = false;
		}
		return finished.size();
	}

	void Crossing::PumpDiscovery() {
		ENGINE_HEAP_SCOPE("unified.discovery");

		Beacon_->Pump(Now);
		Directory_->Observe(*Subnet[1], Now);
	}

	Report Crossing::Step() {
		ENGINE_HEAP_SCOPE("unified.step");

		Report report;

		{
			ENGINE_HEAP_SCOPE("unified.server.simulate");

			// Clear before the tick so the delta sees this tick's changes.
			Server.ClearChanges();
			Server.AdvanceTick(static_cast<float>(1.0 / Options.TickRate));
			ServerSystems.RunPhases(Server, Phase::PreSimulation, Phase::PostSimulation);
			Server.FlushSignals();
		}

		Tick_ = Server.Time().Tick;
		report.Tick = Tick_;
		Now += 1.0 / Options.TickRate;

		{
			ENGINE_HEAP_SCOPE("unified.server.publish");
			Authority_.Publish(Server, Tick_);
		}

		if (Wired.Carrying == Transport::Direct) {
			CarryDirect(report);
		} else {
			CarryOverWire(report);
		}

		// **After the world has been published**, which is `ContentRelay`'s own
		// rule: content never outranks the simulation, so it is offered what is
		// left of the tick rather than the whole of it.
		if (Relay_ != nullptr) {
			report.Routes = PumpContent();
		}
		if (Beacon_ != nullptr) {
			PumpDiscovery();
		}

		report.Applied = Replica_.Applied();

		{
			ENGINE_HEAP_SCOPE("unified.client.record");

			// Record on receipt, before presentation can miss a tick.
			client::RecordReplicatedTick(Client, report.Applied);
		}

		const auto *buffer = Client.Resource<engine::replication::SnapshotBuffer>();

		{
			ENGINE_HEAP_SCOPE("unified.client.draw");

			float previousDrawn = 0.0f;
			bool haveDrawn = false;
			for (int frame = 0; frame < Options.FramesPerTick; frame++) {
				Client.SetFrame(static_cast<float>(1.0 / (Options.TickRate * Options.FramesPerTick)), 0.0f);
				ClientSystems.RunPhases(Client, Phase::PreRender, Phase::PreRender);

				const auto *drawList = Client.Resource<client::DrawList>();
				report.Drawn = drawList == nullptr ? 0 : drawList->Instances.size();

				// DrawList has no entity id; resolve the probe by traversal ordinal.
				if (drawList != nullptr) {
					report.DrawnX = DrawnPositionOf(*drawList, Probe_);
				}

				if (haveDrawn && std::abs(report.DrawnX - previousDrawn) < 1e-5f) {
					report.FrozenFrames++;
				}
				previousDrawn = report.DrawnX;
				haveDrawn = true;
			}
		}

		report.ServerEntities = Server.CountMatching<Transform>();
		report.ClientEntities = Client.CountMatching<Transform>();
		report.ServerX = PositionOf(Server, Probe_);
		report.ClientX = PositionOf(Client, Probe_);
		report.Behind = buffer == nullptr ? 0.0 : buffer->Behind();

		Tally.Ticks++;
		Tally.LargestMessage = std::max(Tally.LargestMessage, report.LargestMessage);
		Tally.ServerEntities = report.ServerEntities;
		Tally.ClientEntities = report.ClientEntities;
		Tally.Drawn = report.Drawn;

		return report;
	}

	const Reports &Crossing::Gather() {
		ENGINE_HEAP_SCOPE("unified.gather");

		Tally.Authority = Authority_.Stats();
		Tally.Replica = Replica_.Stats();

		if (const auto *buffer = Client.Resource<engine::replication::SnapshotBuffer>()) {
			Tally.Presentation = buffer->Stats();
		} else {
			Tally.Presentation.reset();
		}

		// **Datagram only, because the numbers are the datagram stack's.** A
		// `net::Link`'s counters have no QUIC equivalent to compare with, and
		// `Reports` reads them against each other - so under QUIC they are
		// absent rather than filled in with something that means something else.
		if (ServerDatagram != nullptr && ClientDatagram != nullptr) {
			Tally.ServerSession = ServerDatagram->Stats();
			Tally.ClientSession = ClientDatagram->Stats();
			Tally.ServerLink = ServerDatagram->Link().Stats();
			Tally.ClientLink = ClientDatagram->Link().Stats();
		}
		if (Loses(Wired.Carrying) && ServerEnd != nullptr) {
			Tally.ToClient = ClientEnd->Stats();
			Tally.ToServer = ServerEnd->Stats();
		}

		Tally.Heap = engine::core::HeapProfile::Totals();

		if (Relay_ != nullptr) {
			Tally.Relay = Relay_->Stats();
		}
		if (Link_ != nullptr) {
			Tally.Link = Link_->Stats();
		}
		if (Beacon_ != nullptr && Directory_ != nullptr) {
			Tally.Beacon = Beacon_->Counters();
			Tally.Directory = Directory_->Counters();
		}

		return Tally;
	}
}
