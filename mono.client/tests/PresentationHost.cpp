#include <engine/core/Clock.hpp>
#include <engine/core/Paths.hpp>
#include <engine/game/Game.hpp>
#include <engine/game/PortalSession.hpp>
#include <engine/net/Transport.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/render/PortalExchange.hpp>
#include <engine/render/PortalImageDemand.hpp>
#include <engine/render/PortalImageRuntime.hpp>
#include <engine/render/PortalTopologyHost.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/PresentationStream.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <fstream>
#include <thread>

TEST_SUITE_ID("client.presentationhost")
TEST_DEPENDS("engine.render.portalimagehost")

TEST_CASE(
	"listening Server launches a live image producer and relays its pixels",
	"[client][server-presentation-host][gpu][.]"
) {
	using namespace engine;
	using namespace engine::render;
	const bool disconnect = GENERATE(false, true);
	const bool playConnection = GENERATE(false, true);
	const uint32_t imageSize = playConnection ? 128 : 16;
	const auto projection = GENERATE(PortalImageProjection::Seam, PortalImageProjection::Eye);
	bool restartProducer = false;
#ifndef _WIN32
	restartProducer = playConnection && !disconnect && projection == PortalImageProjection::Seam;
#endif
	scene::RegisterSceneClasses();
	world::Universe authored;
	const auto authoredNear = authored.Create({.Name = core::Name("near")});
	authored.Enter(authoredNear, [](ecs::Store &store) { scene::InstallServices(store); });
	const auto authoredFar = authored.Create({.Name = core::Name("far")});
	authored.Enter(authoredFar, [](ecs::Store &store) {
		const auto workspace = scene::InstallServices(store);
		scene::PartDesc panel;
		panel.Frame = core::CFrame(core::Vector3{0, 0, -8});
		panel.Size = {40, 40, 1};
		REQUIRE(store.SetParent(scene::MakePart(store, panel), workspace));
	});
	const auto gamePath = core::Paths::Base() / "portal-server-producer.agame";
	const auto configPath = core::Paths::Base() / "portal-server-producer.ini";
	std::ofstream(configPath).close();
	std::string error;
	REQUIRE(game::SaveGame(authored, core::Name("portal server producer"), gamePath, error));
	world::Universe universe;
	const auto near = universe.Create({.Name = core::Name("near")});
	const auto far = universe.CreateRemote({.Name = core::Name("far")}, core::Name("server"));
	REQUIRE(universe.ConfigurePresentation(123));
	const auto replies = universe.OpenPresentation(near, core::Name(PORTAL_REPLY_CHANNEL)).Address;
	auto channel = parallel::MakeProcessChannel();
	REQUIRE(channel.Valid());
	parallel::Process server;
	const auto programs = core::Paths::Base().parent_path();
	auto producerProgram = programs / "client" / core::Paths::Program("client");
#ifndef _WIN32
	if (restartProducer) {
		const auto quote = [](const std::filesystem::path &path) {
			std::string quoted = "'";
			for (char character : path.string()) {
				if (character == '\'')
					quoted += "'\"'\"'";
				else
					quoted += character;
			}
			return quoted + "'";
		};
		const auto wrapper = core::Paths::Base() / "portal-producer-lifetime.sh";
		std::ofstream script(wrapper);
		script << "#!/bin/sh\nexec " << quote(producerProgram) << " \"$@\" --profile-seconds 2 --config "
			   << quote(configPath) << "\n";
		script.close();
		REQUIRE(script.good());
		std::filesystem::permissions(wrapper, std::filesystem::perms::owner_all);
		producerProgram = wrapper;
	}
#endif
	std::array<std::byte, assets::SigningKey::SEED_BYTES> seed{};
	seed.fill(std::byte{17});
	const auto allowed = assets::SigningKey::FromSeed(seed);
	REQUIRE(allowed);
	REQUIRE(server.Start(
		programs / "server" / core::Paths::Program("server"),
		{"--host",
		 "image-host",
		 "--world",
		 "far",
		 "--listen",
		 "0",
		 "--transport",
		 "datagram",
		 "--game",
		 gamePath.string(),
		 "--config",
		 configPath.string(),
		 "--mcp-port",
		 "-1",
		 "--identity-key",
		 std::string(64, '1'),
		 "--admit-key",
		 allowed->Public().ToHex(),
		 "--presentation-program",
		 producerProgram.string()},
		std::move(channel.Remote)
	));
	world::HostLink link(std::move(channel.Local));
	bool received = false, issued = false;
	bool missingEndpointIssued = false;
	world::PresentationAddress retiredProducer;
	size_t imageCount = 0;
	bool sawWithdrawal = false, staleRequestIssued = false;
	world::PresentationStream play;
	std::unique_ptr<net::Transport> socket;
	std::unique_ptr<replication::Connector> player;
	ecs::Store replica("play-wire replica");
	uint64_t publishedRevision = 0;
	PortalTopologyHost topology(universe);
	bool topologyReceived = !playConnection;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while ((!received || !topologyReceived || (restartProducer && imageCount < 2)) && server.Poll().Alive() &&
		   std::chrono::steady_clock::now() < deadline) {
		std::vector<world::HostFrame> frames;
		link.Receive(frames);
		for (const auto &frame : frames) {
			if (frame.Signal == world::HostSignal::Ready && playConnection) {
				socket = net::MakeUdpTransport(0);
				REQUIRE(socket);
				replication::ConnectorSettings settings;
				settings.Advertised = net::WireMode::Datagram;
				settings.ServerIdentity = allowed->Public();
				settings.ClientIdentity = &*allowed;
				player = std::make_unique<replication::Connector>(
					*socket, net::Endpoint::LoopbackIPv4(frame.Port), core::Clock::Seconds(), settings
				);
				player->OnUserMessage([&](auto bytes) {
					REQUIRE(play.Receive(bytes) == world::PresentationStreamReceive::Accepted);
				});
			} else if (frame.Signal == world::HostSignal::PresentationDirectory && !playConnection) {
				REQUIRE(
					universe.ApplyPresentationDirectory(core::Name("server"), frame.Directory) ==
					world::PresentationStatus::Ok
				);
			} else if (frame.Signal == world::HostSignal::Presentation && !playConnection) {
				REQUIRE(
					universe.IngestPresentation(core::Name("server"), frame.Presentation) ==
					world::PresentationStatus::Ok
				);
			}
		}
		if (player) {
			const auto now = core::Clock::Seconds();
			player->Poll(replica, now);
			player->Advance(now);
			for (const auto &frame : play.Take()) {
				if (frame.Kind == world::PresentationStreamKind::Routes) {
					REQUIRE(
						universe.AcceptPresentationRoutesFromDriver(frame.Directory) ==
						world::PresentationStatus::Ok
					);
				} else {
					REQUIRE(frame.Kind == world::PresentationStreamKind::Message);
					REQUIRE(
						universe.AcceptPresentationFromDriver(frame.Message) == world::PresentationStatus::Ok
					);
				}
			}
		}
		for (const auto &message : universe.TakePresentation(replies)) {
			PortalImageReply image;
			REQUIRE(DecodePortalImageReply(message.Payload, image, error));
			CHECK(image.Status == PortalImageStatus::Ok);
			CHECK(image.Width == imageSize);
			CHECK(image.Height == imageSize);
			CHECK_FALSE(image.Pixels.empty());
			received = true;
			++imageCount;
			if (restartProducer && imageCount == 1) {
				retiredProducer = message.From;
				issued = false;
			} else if (restartProducer) {
				CHECK(message.From != retiredProducer);
				CHECK(message.From.Generation > retiredProducer.Generation);
			}
		}
		const auto endpoint = universe.LookupPresentation(far, PORTAL_REQUEST_CHANNEL);
		if (retiredProducer.Session && !endpoint.Session) sawWithdrawal = true;
		if (!issued && endpoint.Session && endpoint != retiredProducer) {
			if (!playConnection)
				REQUIRE(universe.LookupPresentation(far, "portal-sessions").Session == endpoint.Session);
			else
				CHECK(universe.LookupPresentation(far, "portal-sessions").Session == 0);
			REQUIRE(universe.LookupPresentation(far, PORTAL_TOPOLOGY_REQUESTS).Session == endpoint.Session);
			if (!playConnection)
				REQUIRE(link.PublishPresentationRoutes(universe.PresentationRoutesFor(core::Name("server"))));
			else
				REQUIRE(topology.Request(near, far, std::chrono::steady_clock::now()));
			PortalImageRequest request;
			request.Key = {1, "Door", 1, 1};
			request.Width = request.Height = imageSize;
			request.PixelBudget = imageSize * imageSize;
			request.ClipPlane = {0, 0, -1, -1};
			request.Projection = projection;
			if (projection == PortalImageProjection::Eye) {
				PortalImageDemand demand;
				View eye;
				eye.CameraFrame = core::CFrame(core::Vector3{2, 3, 4});
				REQUIRE(
					BuildPortalEyeDemand(
						core::Name("player-eye"),
						eye,
						{.Width = imageSize,
						 .Height = imageSize,
						 .RecursionDepth = 0,
						 .PixelBudget = imageSize * imageSize},
						demand
					) == PortalDemandStatus::Ready
				);
				request = std::move(demand.Request);
				request.Key.RequestId = 1;
			}
			std::vector<std::byte> wire;
			REQUIRE(EncodePortalImageRequest(request, wire, error));
			REQUIRE(
				universe.SendPresentation(near, replies, endpoint, 1, wire) == world::PresentationStatus::Ok
			);
			if (!playConnection)
				for (const auto &outgoing : universe.TakePresentationOutbound())
					REQUIRE(link.SendPresentation(outgoing.Message));
			issued = true;
		}
		if (player && player->Admitted()) {
			const auto directory = universe.LocalPresentationDirectory();
			if (directory.Revision != publishedRevision) {
				world::PresentationStreamFrame frame;
				frame.Kind = world::PresentationStreamKind::Directory;
				frame.Directory = directory;
				REQUIRE(play.Queue(frame) == world::PresentationStatus::Ok);
				publishedRevision = directory.Revision;
			}
			for (auto &outbound : universe.TakePresentationOutbound()) {
				world::PresentationStreamFrame frame;
				frame.Message = std::move(outbound.Message);
				if (retiredProducer.Session && !staleRequestIssued &&
					frame.Message.To.Channel == PORTAL_REQUEST_CHANNEL) {
					auto stale = frame;
					stale.Message.To = retiredProducer;
					REQUIRE(play.Queue(stale) == world::PresentationStatus::Ok);
					staleRequestIssued = true;
				}
				if (!missingEndpointIssued) {
					auto missing = frame;
					missing.Message.To.World = "withdrawn-image-world";
					REQUIRE(play.Queue(missing) == world::PresentationStatus::Ok);
					missingEndpointIssued = true;
				}
				REQUIRE(play.Queue(frame) == world::PresentationStatus::Ok);
			}
			play.Flush([&](auto bytes) { return player->SendUser(bytes, core::Clock::Seconds()); });
			topology.Pump(std::chrono::steady_clock::now());
			topologyReceived = topology.Snapshot(far, std::chrono::steady_clock::now()) != nullptr;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(received);
	REQUIRE(topologyReceived);
	if (restartProducer) {
		CHECK(imageCount == 2);
		CHECK(sawWithdrawal);
		CHECK(staleRequestIssued);
	}
	world::HostFrame stop;
	stop.Signal = world::HostSignal::Stop;
	if (disconnect)
		link.Close();
	else
		REQUIRE(link.Send(stop));
	const auto stopped = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	auto status = server.Poll();
	while (status.Alive() && std::chrono::steady_clock::now() < stopped) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		status = server.Poll();
	}
	CHECK(status.Reason == parallel::ExitReason::Exited);
	CHECK(status.Code == 0);
}

TEST_CASE(
	"client product serves a saved or replicated world over its inherited driver link",
	"[client][presentation-host][gpu][.]"
) {
	using namespace engine;
	using namespace engine::render;
	const bool disconnect = GENERATE(false, true);
	const bool liveReplica = GENERATE(false, true);
	const auto projection = GENERATE(PortalImageProjection::Seam, PortalImageProjection::Eye);
	scene::RegisterSceneClasses();
	world::Universe authored;
	for (const auto *name : {"near", "far"}) {
		const auto world = authored.Create({.Name = core::Name(name)});
		authored.Enter(world, [](ecs::Store &store) {
			const auto workspace = scene::InstallServices(store);
			scene::PartDesc panel;
			panel.Frame = core::CFrame(core::Vector3{0, 0, -8});
			panel.Size = {40, 40, 1};
			const auto part = scene::MakePart(store, panel);
			REQUIRE(part != ecs::NULL_ENTITY);
			REQUIRE(store.SetParent(part, workspace));
		});
	}
	const auto gamePath = core::Paths::Base() / "portal-client-host.agame";
	const auto configPath = core::Paths::Base() / "portal-client-host.ini";
	std::ofstream(configPath).close();
	std::string error;
	REQUIRE(game::SaveGame(authored, core::Name("portal host"), gamePath, error));
	std::array<std::byte, assets::SigningKey::SEED_BYTES> seed{};
	seed.fill(std::byte{39});
	const auto identity = assets::SigningKey::FromSeed(seed);
	REQUIRE(identity);
	std::unique_ptr<net::Transport> socket;
	std::unique_ptr<replication::Listener> authority;
	size_t freshPlayers = 0, movement = 0;
	uint64_t tick = 0;
	if (liveReplica) {
		socket = net::MakeUdpTransport(0);
		REQUIRE(socket);
		replication::ListenerSettings settings;
		settings.Wire = net::WireMode::Datagram;
		authority = std::make_unique<replication::Listener>(*socket, settings);
		authority->SetIdentity(&*identity);
		for (const auto &component : replication::DefaultReplicatedComponents()) {
			authority->Authority().Replicate(core::Name(component.Name), component.Detection);
			if (!component.Suppressor.empty())
				authority->Authority().SuppressWhenTagged(
					core::Name(component.Name), core::Name(component.Suppressor)
				);
		}
		authority->OnUserMessage([&](replication::ClientId, std::span<const std::byte> bytes) {
			game::PortalSessionMessage message;
			if (game::DecodePortalSession(bytes, message) && message.Kind == game::PortalSessionKind::Fresh)
				freshPlayers++;
		});
	}
	world::Universe universe;
	const auto near = universe.Create({.Name = core::Name("near")});
	const auto far = universe.CreateRemote({.Name = core::Name("far")}, core::Name("child"));
	REQUIRE(universe.ConfigurePresentation(123));
	const auto replies = universe.OpenPresentation(near, core::Name(PORTAL_REPLY_CHANNEL)).Address;
	PortalTopologyHost topology(universe);
	auto pair = parallel::MakeProcessChannel();
	REQUIRE(pair.Valid());
	parallel::Process child;
	std::vector<std::string> arguments{
		"--headless",
		"--frames",
		"100000",
		"--config",
		configPath.string(),
		"--presentation-world",
		"far",
		"--presentation-session",
		"456",
		"--mcp-port",
		"-1"
	};
	if (liveReplica) {
		arguments.insert(
			arguments.end(),
			{"--connect",
			 net::Endpoint::LoopbackIPv4(socket->Local().Port).Text(),
			 "--server-key",
			 identity->Public().ToHex()}
		);
	} else
		arguments.insert(arguments.end(), {"--game", gamePath.string()});
	REQUIRE(child.Start(
		core::Paths::Base().parent_path() / "client" / core::Paths::Program("client"),
		arguments,
		std::move(pair.Remote)
	));
	world::HostLink link(std::move(pair.Local));
	bool ready = false, issued = false, received = false, topologyReceived = false;
	bool relit = !liveReplica;
	uint64_t requestId = 0;
	std::optional<PortalImageReply> initialImage;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while ((!received || !topologyReceived || !relit) && std::chrono::steady_clock::now() < deadline &&
		   child.Poll().Alive()) {
		if (authority) {
			const double now = core::Clock::Seconds();
			authority->Poll(now);
			authority->Advance(now);
			movement += authority->Inputs().size();
			authority->ClearInputs();
			authored.Enter(authored.Find(core::Name("far")), [&](ecs::Store &store) {
				authority->Publish(store, ++tick, now);
			});
			authority->Flush(now);
		}
		std::vector<world::HostFrame> frames;
		link.Receive(frames);
		for (const auto &frame : frames) {
			if (frame.Signal == world::HostSignal::Ready)
				ready = true;
			else if (frame.Signal == world::HostSignal::PresentationDirectory) {
				if (liveReplica && !ready && frame.Directory.Endpoints.empty()) continue;
				REQUIRE(frame.Directory.Endpoints.size() == 2);
				for (const auto &endpoint : frame.Directory.Endpoints)
					CHECK(endpoint.World == "far");
				REQUIRE(
					universe.ApplyPresentationDirectory(core::Name("child"), frame.Directory) ==
					world::PresentationStatus::Ok
				);
			} else {
				REQUIRE(frame.Signal == world::HostSignal::Presentation);
				if (frame.Presentation.To.Channel == PORTAL_TOPOLOGY_REPLIES) {
					REQUIRE(
						universe.IngestPresentation(core::Name("child"), frame.Presentation) ==
						world::PresentationStatus::Ok
					);
					continue;
				}
				PortalImageReply image;
				REQUIRE(DecodePortalImageReply(frame.Presentation.Payload, image, error));
				INFO(image.Diagnostic);
				REQUIRE(image.Status == PortalImageStatus::Ok);
				CHECK(image.Width == 16);
				CHECK(image.Height == 16);
				CHECK(image.Pixels.size() == 16 * 16 * 8);
				REQUIRE(
					universe.IngestPresentation(core::Name("child"), frame.Presentation) ==
					world::PresentationStatus::Ok
				);
				received = universe.TakePresentation(replies).size() == 1;
				if (liveReplica && !initialImage) {
					initialImage = image;
					authored.Enter(authored.Find(core::Name("far")), [](ecs::Store &store) {
						auto *lighting = store.GetMutable<scene::LightingServiceComponent>(
							store.FindFirstRoot("Lighting")
						);
						REQUIRE(lighting);
						lighting->ClockTime = 0;
						lighting->Ambient = {.8f, .05f, .05f};
						lighting->OutdoorAmbient = lighting->Ambient;
					});
					issued = false;
				} else if (liveReplica && image.LightingRevision != initialImage->LightingRevision) {
					CHECK(image.PixelHash != initialImage->PixelHash);
					relit = true;
				} else if (!relit)
					issued = false;
			}
		}
		const auto endpoint = universe.LookupPresentation(far, PORTAL_REQUEST_CHANNEL);
		if (!issued && ready && endpoint.Session != 0) {
			if (requestId == 0) REQUIRE(topology.Request(near, far, std::chrono::steady_clock::now()));
			REQUIRE(link.PublishPresentationRoutes(universe.PresentationRoutesFor(core::Name("child"))));
			PortalImageRequest request;
			request.Key = {++requestId, "Door", 1, 1};
			request.Width = request.Height = 16;
			request.PixelBudget = 256;
			request.ClipPlane = {0, 0, -1, -1};
			request.Projection = projection;
			if (projection == PortalImageProjection::Eye) {
				PortalImageDemand demand;
				View eye;
				eye.CameraFrame = core::CFrame(core::Vector3{2, 3, 4}) * core::CFrame::Angles(.2f, .3f, .1f);
				REQUIRE(
					BuildPortalEyeDemand(
						core::Name("player-eye"),
						eye,
						{.Width = 16, .Height = 16, .RecursionDepth = 0, .PixelBudget = 256},
						demand
					) == PortalDemandStatus::Ready
				);
				request = std::move(demand.Request);
				request.Key.RequestId = requestId;
			}
			std::vector<std::byte> wire;
			REQUIRE(EncodePortalImageRequest(request, wire, error));
			REQUIRE(
				universe.SendPresentation(near, replies, endpoint, requestId, wire) ==
				world::PresentationStatus::Ok
			);
			for (const auto &outgoing : universe.TakePresentationOutbound())
				REQUIRE(link.SendPresentation(outgoing.Message));
			issued = true;
		}
		const auto now = std::chrono::steady_clock::now();
		topology.Pump(now);
		if (const auto *snapshot = topology.Snapshot(far, now)) {
			CHECK(snapshot->Seams.empty());
			topologyReceived = true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(ready);
	REQUIRE(issued);
	REQUIRE(received);
	REQUIRE(topologyReceived);
	REQUIRE(relit);
	if (disconnect && liveReplica) {
		authority.reset();
		socket->Close();
	} else if (disconnect) {
		link.Close();
	} else {
		world::HostFrame stop;
		stop.Signal = world::HostSignal::Stop;
		REQUIRE(link.Send(stop));
	}
	const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	bool retired = false;
	auto ended = child.Poll();
	while (ended.Alive() && std::chrono::steady_clock::now() < stopDeadline) {
		if (authority) {
			const double now = core::Clock::Seconds();
			authority->Poll(now);
			authority->Advance(now);
			movement += authority->Inputs().size();
			authority->ClearInputs();
			authored.Enter(authored.Find(core::Name("far")), [&](ecs::Store &store) {
				authority->Publish(store, ++tick, now);
			});
			authority->Flush(now);
		}
		std::vector<world::HostFrame> frames;
		link.Receive(frames);
		for (const auto &frame : frames)
			if (frame.Signal == world::HostSignal::PresentationDirectory && frame.Directory.Endpoints.empty())
				retired = true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		ended = child.Poll();
	}
	REQUIRE_FALSE(ended.Alive());
	CHECK(ended.Reason == parallel::ExitReason::Exited);
	CHECK(ended.Code == 0);
	std::vector<world::HostFrame> finalFrames;
	link.Receive(finalFrames);
	for (const auto &frame : finalFrames)
		if (frame.Signal == world::HostSignal::PresentationDirectory && frame.Directory.Endpoints.empty())
			retired = true;
	if (!disconnect || liveReplica) CHECK(retired);
	CHECK(freshPlayers == 0);
	CHECK(movement == 0);
}
