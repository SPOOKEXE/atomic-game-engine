#include <engine/core/Bytes.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/Paths.hpp>
#include <engine/game/PortalSession.hpp>
#include <engine/net/Transport.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/render/PortalExchange.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/scene/CameraPortalTopology.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/PresentationStream.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <fstream>
#include <thread>

TEST_SUITE_ID("client.portalsession")
TEST_DEPENDS("engine.game.portalsession")

static void RunPortalSuccessor(int outcome) {
	using namespace engine;
	const bool lostCancellationAck = outcome == 6;
	const bool readinessExpiry = outcome == 7;
	const bool readinessDisconnect = outcome == 8;
	const bool withheldSnapshot = readinessExpiry || readinessDisconnect;
	const bool retryRefusal = outcome == 1 || lostCancellationAck || withheldSnapshot;
	CAPTURE(outcome);
	scene::RegisterSceneClasses();
	ecs::Store source("source"), destination("destination");
	scene::InstallServices(source);
	scene::InstallServices(destination);
	const auto original = scene::AddPlayer(source, "traveller", false, 71);
	REQUIRE(scene::LoadCharacter(source, original) != ecs::NULL_ENTITY);
	if (outcome == 0) {
		scene::PartDesc part;
		part.Frame = core::CFrame({0, 3, -5});
		part.Size = {40, 40, .4f};
		const auto pane = scene::MakePart(source, part);
		part.Frame = core::CFrame({0, 3, -30});
		const auto exit = scene::MakePart(source, part);
		REQUIRE(source.SetParent(pane, scene::WorkspaceOf(source)));
		REQUIRE(source.SetParent(exit, scene::WorkspaceOf(source)));
		const auto portal = source.CreateInstance(ecs::Classes::Find(core::Name("Portal")), "Window");
		REQUIRE(source.SetParent(portal, pane));
		source.Set(portal, scene::Portal{exit, core::Name("other-side")});
	}
	const auto root = source.Get<scene::Character>(scene::CharacterOf(source, original))->Root;
	const auto startingRoot = source.Get<scene::Transform>(root)->Frame;
	ecs::Entity arrived;
	auto sourceSocket = net::MakeUdpTransport(0);
	auto destinationSocket = net::MakeUdpTransport(0);
	REQUIRE(sourceSocket);
	REQUIRE(destinationSocket);
	std::array<std::byte, assets::SigningKey::SEED_BYTES> seed;
	seed.fill(std::byte{37});
	const auto identity = assets::SigningKey::FromSeed(seed);
	REQUIRE(identity);
	replication::ListenerSettings settings;
	settings.Wire = net::WireMode::Datagram;
	replication::Listener from(*sourceSocket, settings);
	auto to = std::make_unique<replication::Listener>(*destinationSocket, settings);
	from.SetIdentity(&*identity);
	to->SetIdentity(&*identity);
	const auto configure = [&](replication::Listener *listener) {
		for (const auto &component : replication::DefaultReplicatedComponents()) {
			listener->Authority().Replicate(core::Name(component.Name), component.Detection);
			if (!component.Suppressor.empty())
				listener->Authority().SuppressWhenTagged(
					core::Name(component.Name), core::Name(component.Suppressor)
				);
		}
	};
	configure(&from);
	configure(to.get());
	game::PortalSessionMessage offer;
	offer.Kind = game::PortalSessionKind::Transfer;
	offer.Attempt = 42;
	offer.Claim.Transfer = {"source", 101, 1};
	offer.Claim.Destination = outcome == 0 ? "other-side" : "destination";
	offer.Claim.DestinationIncarnation = 202;
	offer.Claim.SourceSession = 123;
	offer.Claim.Capability.fill(std::byte{42});
	offer.Identity = identity->Public();
	if (retryRefusal && !withheldSnapshot) {
		seed.fill(std::byte{38});
		offer.Identity = assets::SigningKey::FromSeed(seed)->Public();
	}
	offer.Port = destinationSocket->Local().Port;
	offer.Through.Frame = core::CFrame({20, 0, 0}) * core::CFrame::Angles(0, .5f, 0);
	if (outcome == 0) {
		std::vector<scene::PortalSeam> seams;
		REQUIRE(scene::GatherPortalSeams(source, seams) == 1);
		offer.Through = scene::SeamMapping(seams.front());
	}
	std::optional<replication::ClientId> sourcePeer;
	std::vector<std::pair<replication::ClientId, game::PortalSessionMessage>> sourceReplies,
		destinationReplies;
	bool proceeded = false, resumed = false, committed = false, destinationInput = false;
	bool refusalSeen = false;
	size_t cancellationRequests = 0;
	std::optional<double> offerSentAt;
	bool destinationRefusalSeen = false;
	bool transportLost = false, restartDestination = false;
	size_t sourceFresh = 0, destinationFresh = 0, sourceInputsAfterAdoption = 0,
		   sourceInputsDuringContinuation = 0;
	std::optional<double> destinationInputAt;
	world::PresentationStream sourcePresentation;
	auto destinationPresentation = std::make_unique<world::PresentationStream>();
	std::optional<replication::ClientId> destinationPeer;
	bool destinationRoutesPublished = false;
	const world::PresentationDirectory producerRoutes{
		900,
		1,
		{{"source", "portal-image-requests", 900, 1},
		 {"source", "portal-topology-requests", 900, 2},
		 {offer.Claim.Destination, "portal-image-requests", 900, 3},
		 {offer.Claim.Destination, "portal-topology-requests", 900, 4}}
	};
	world::PresentationDirectory sourceConsumers, destinationConsumers;
	bool requestedImage = false, requestedEye = false, publishedRoutes = false;
	uint64_t topologySequence = 0;
	float walked = 0;
	const auto receivePresentation = [&](world::PresentationStream &stream,
										 world::PresentationDirectory &consumers,
										 std::span<const std::byte> bytes) {
		if (!world::PresentationStream::Recognizes(bytes)) return false;
		REQUIRE(stream.Receive(bytes) == world::PresentationStreamReceive::Accepted);
		for (auto &frame : stream.Take()) {
			if (frame.Kind == world::PresentationStreamKind::Message) {
				if (frame.Message.To.Channel == "portal-topology-requests") {
					world::PresentationStreamFrame reply;
					reply.Message.From = frame.Message.To;
					reply.Message.To = frame.Message.From;
					reply.Message.Sequence = ++topologySequence;
					reply.Message.Correlation = frame.Message.Correlation;
					std::string failure;
					REQUIRE(
						scene::EncodeCameraPortalTopology(
							{frame.Message.To.World, 1, {}}, reply.Message.Payload, failure
						)
					);
					REQUIRE(stream.Queue(reply) == world::PresentationStatus::Ok);
					continue;
				}
				REQUIRE(
					std::find(
						producerRoutes.Endpoints.begin(), producerRoutes.Endpoints.end(), frame.Message.To
					) != producerRoutes.Endpoints.end()
				);
				CHECK(
					std::find(consumers.Endpoints.begin(), consumers.Endpoints.end(), frame.Message.From) !=
					consumers.Endpoints.end()
				);
				render::PortalImageRequest request;
				std::string failure;
				REQUIRE(render::DecodePortalImageRequest(frame.Message.Payload, request, failure));
				if (request.Projection == render::PortalImageProjection::Eye) {
					if (!requestedEye && outcome == 0) {
						CHECK_FALSE(proceeded);
						CHECK(source.Alive(original));
					}
					requestedEye = true;
				} else {
					requestedImage = true;
				}
				render::PortalImageReply image;
				image.Key = request.Key;
				image.Scope = request.Scope;
				image.Status = render::PortalImageStatus::Ok;
				image.ContentRevision = image.LightingRevision = 1;
				image.Width = request.Width;
				image.Height = request.Height;
				image.RowStride = image.Width * 8;
				// Uniform HDR replies exercise drawability without asserting scene-image fidelity.
				core::ByteWriter pixels;
				for (size_t pixel = 0; pixel < size_t(image.Width) * image.Height; ++pixel) {
					pixels.WriteUInt16(0x3400);
					pixels.WriteUInt16(0);
					pixels.WriteUInt16(0);
					pixels.WriteUInt16(0x3c00);
				}
				image.Pixels.assign(pixels.Bytes().begin(), pixels.Bytes().end());
				image.PixelHash = assets::Hasher::Of(image.Pixels);
				world::PresentationStreamFrame reply;
				reply.Message.From = frame.Message.To;
				reply.Message.To = frame.Message.From;
				reply.Message.Sequence = ++topologySequence;
				reply.Message.Correlation = frame.Message.Correlation;
				REQUIRE(render::EncodePortalImageReply(image, reply.Message.Payload, failure));
				REQUIRE(stream.Queue(reply) == world::PresentationStatus::Ok);
				continue;
			}
			REQUIRE(frame.Kind == world::PresentationStreamKind::Directory);
			REQUIRE(frame.Directory.Session != 0);
			REQUIRE(frame.Directory.Revision > consumers.Revision);
			for (const auto &endpoint : frame.Directory.Endpoints) {
				CHECK(endpoint.Session == frame.Directory.Session);
				CHECK(endpoint.Generation != 0);
				CHECK(
					(endpoint.Channel == "portal-image-replies" ||
					 endpoint.Channel == "portal-topology-replies" ||
					 endpoint.Channel.starts_with("portal-image-replies/"))
				);
			}
			consumers = std::move(frame.Directory);
		}
		return true;
	};
	double now = core::Clock::Seconds();
	from.OnUserMessage([&](replication::ClientId peer, std::span<const std::byte> bytes) {
		if (receivePresentation(sourcePresentation, sourceConsumers, bytes)) return;
		game::PortalSessionMessage request;
		if (!game::DecodePortalSession(bytes, request)) return;
		if (request.Kind == game::PortalSessionKind::Fresh) {
			sourceFresh++;
			sourcePeer = peer;
			game::PortalSessionMessage ready;
			ready.Kind = game::PortalSessionKind::Ready;
			ready.Attempt = request.Attempt;
			ready.Player = original;
			ready.World = "source";
			sourceReplies.emplace_back(peer, ready);
		} else if (request.Kind == game::PortalSessionKind::Proceed) {
			CHECK(request.Attempt == offer.Attempt);
			CHECK(request.Claim == offer.Claim);
			proceeded = true;
			scene::PortalBodyCopy body;
			scene::PortalBodyArrival arrival;
			std::string failure;
			REQUIRE(scene::CapturePortalBody(source, original, body, failure));
			REQUIRE(scene::MapPortalBody(body, offer.Through, failure));
			REQUIRE(scene::AdmitPortalBody(destination, body, arrival, failure));
			arrived = arrival.Player;
			scene::RemoveCharacter(source, original);
			source.DestroyInstance(original);
			game::PortalSessionMessage crossed;
			crossed.Kind = game::PortalSessionKind::Crossed;
			crossed.Attempt = offer.Attempt;
			crossed.Claim = offer.Claim;
			sourceReplies.emplace_back(peer, crossed);
		} else if (request.Kind == game::PortalSessionKind::Refused) {
			REQUIRE(retryRefusal);
			CHECK(request.Attempt == offer.Attempt);
			++cancellationRequests;
			// The host completed cancellation, but its application acknowledgement
			// was lost. A repeated request must recover the same attempt.
			if (lostCancellationAck && cancellationRequests == 1) {
				CHECK_FALSE(proceeded);
				CHECK(source.Alive(original));
				return;
			}
			REQUIRE_FALSE(refusalSeen);
			CHECK_FALSE(proceeded);
			CHECK(source.Alive(original));
			if (readinessExpiry) {
				REQUIRE(offerSentAt.has_value());
				CHECK(now - *offerSentAt >= 15);
				CHECK(request.Diagnostic == "destination connection or snapshot did not become ready");
				REQUIRE(destinationPeer.has_value());
				CHECK(to->IdentityOf(*destinationPeer) == from.IdentityOf(*sourcePeer));
			}
			if (readinessDisconnect) {
				CHECK(transportLost);
				CHECK(request.Diagnostic == "destination authenticated connection ended");
			}
			refusalSeen = true;
			sourceReplies.emplace_back(peer, request);
			offer.Attempt++;
			offer.Claim.Transfer.Sequence++;
			offer.Identity = identity->Public();
			sourceReplies.emplace_back(peer, offer);
		}
	});
	const auto destinationMessages = [&](replication::ClientId peer, std::span<const std::byte> bytes) {
		if (world::PresentationStream::Recognizes(bytes) && destinationPeer && *destinationPeer != peer) {
			// Each authenticated connection starts its own framing sequence, even
			// when a cancelled attempt reconnects to the same listening host.
			destinationPresentation = std::make_unique<world::PresentationStream>();
			destinationConsumers = {};
			destinationRoutesPublished = false;
		}
		if (receivePresentation(*destinationPresentation, destinationConsumers, bytes)) {
			destinationPeer = peer;
			CHECK(to->IdentityOf(peer) == from.IdentityOf(*sourcePeer));
			if (readinessDisconnect && !transportLost) {
				transportLost = true;
				restartDestination = true;
			}
			if (!committed) CHECK(destinationConsumers.Endpoints.empty());
			return;
		}
		game::PortalSessionMessage request;
		if (!game::DecodePortalSession(bytes, request)) return;
		if (request.Kind == game::PortalSessionKind::Fresh) {
			destinationFresh++;
			return;
		}
		if (!transportLost && ((outcome == 4 && request.Kind == game::PortalSessionKind::Resume) ||
							   (outcome == 5 && request.Kind == game::PortalSessionKind::Commit))) {
			transportLost = true;
			restartDestination = true;
			return;
		}
		CHECK(request.Claim == offer.Claim);
		CHECK(request.Attempt == offer.Attempt);
		CHECK(to->IdentityOf(peer) == from.IdentityOf(*sourcePeer));
		game::PortalSessionMessage reply;
		reply.Attempt = request.Attempt;
		reply.Player = arrived;
		reply.World = offer.Claim.Destination;
		if (!destinationRefusalSeen && ((outcome == 2 && request.Kind == game::PortalSessionKind::Resume) ||
										(outcome == 3 && request.Kind == game::PortalSessionKind::Commit))) {
			destinationRefusalSeen = true;
			reply.Kind = game::PortalSessionKind::Refused;
			reply.Diagnostic = "destination lease temporarily unavailable";
			destinationReplies.emplace_back(peer, reply);
			return;
		}
		if (request.Kind == game::PortalSessionKind::Resume) {
			CHECK(proceeded);
			resumed = true;
			reply.Kind = game::PortalSessionKind::Ready;
		} else if (request.Kind == game::PortalSessionKind::Commit) {
			CHECK(resumed);
			committed = true;
			reply.Kind = game::PortalSessionKind::Committed;
		} else
			return;
		destinationReplies.emplace_back(peer, reply);
	};
	to->OnUserMessage(destinationMessages);
	const auto config = core::Paths::Base() / "portal-successor.ini";
	std::ofstream(config).close();
	parallel::Process child;
	REQUIRE(child.Start(
		core::Paths::Base().parent_path() / "client" / core::Paths::Program("client"),
		{"--headless",
		 "--frames",
		 "100000",
		 "--profile-seconds",
		 readinessExpiry ? "25"
		 : outcome >= 4	 ? "15"
		 : outcome == 0	 ? "8"
						 : "5",
		 "--config",
		 config.string(),
		 "--connect",
		 net::Endpoint::LoopbackIPv4(sourceSocket->Local().Port).Text(),
		 "--server-key",
		 identity->Public().ToHex(),
		 "--mcp-port",
		 "-1"}
	));
	bool offered = false;
	uint64_t tick = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(readinessExpiry ? 30 : 20);
	while (child.Poll().Alive() && std::chrono::steady_clock::now() < deadline) {
		now = core::Clock::Seconds();
		from.Poll(now);
		to->Poll(now);
		if (restartDestination) {
			to = std::make_unique<replication::Listener>(*destinationSocket, settings);
			to->SetIdentity(&*identity);
			configure(to.get());
			to->OnUserMessage(destinationMessages);
			destinationReplies.clear();
			destinationPresentation = std::make_unique<world::PresentationStream>();
			destinationConsumers = {};
			destinationPeer.reset();
			destinationRoutesPublished = false;
			restartDestination = false;
		}
		from.Advance(now);
		to->Advance(now);
		if (proceeded && !destinationInputAt) sourceInputsDuringContinuation += from.Inputs().size();
		if (committed) {
			destinationInput |= !to->Inputs().empty();
			if (destinationInput && !destinationInputAt) destinationInputAt = now;
			// Allow already-sent source packets to drain across the two independent connections.
			if (destinationInputAt && now > *destinationInputAt + .2)
				sourceInputsAfterAdoption += from.Inputs().size();
		}
		from.ClearInputs();
		to->ClearInputs();
		std::erase_if(sourceReplies, [&](const auto &reply) {
			return from.SendTo(reply.first, game::EncodePortalSession(reply.second), now);
		});
		std::erase_if(destinationReplies, [&](const auto &reply) {
			return to->SendTo(reply.first, game::EncodePortalSession(reply.second), now);
		});
		if (sourcePeer && !publishedRoutes && sourceConsumers.Revision != 0) {
			world::PresentationStreamFrame routes;
			routes.Kind = world::PresentationStreamKind::Routes;
			routes.Directory = producerRoutes;
			REQUIRE(sourcePresentation.Queue(routes) == world::PresentationStatus::Ok);
			publishedRoutes = true;
		}
		if (destinationPeer && !destinationRoutesPublished && destinationConsumers.Revision != 0) {
			world::PresentationStreamFrame routes;
			routes.Kind = world::PresentationStreamKind::Routes;
			routes.Directory = producerRoutes;
			REQUIRE(destinationPresentation->Queue(routes) == world::PresentationStatus::Ok);
			destinationRoutesPublished = true;
		}
		if (sourcePeer)
			sourcePresentation.Flush([&](auto bytes) { return from.SendTo(*sourcePeer, bytes, now); });
		if (destinationPeer)
			destinationPresentation->Flush([&](auto bytes) {
				return to->SendTo(*destinationPeer, bytes, now);
			});
		if (sourcePeer && !offered && tick > 40 && sourceReplies.empty() &&
			(outcome != 0 || requestedEye || tick > 1000))
			offered = from.SendTo(*sourcePeer, game::EncodePortalSession(offer), now);
		if (offered && !offerSentAt) offerSentAt = now;
		if (outcome == 0 && requestedImage && !requestedEye && !proceeded) {
			walked = std::min(walked + .1f, 40.f);
			auto frame = startingRoot;
			frame.Position.Z -= walked;
			source.Set(root, scene::Transform{frame});
		}
		from.Publish(source, ++tick, now);
		// Keep authentication and presentation discovery live while withholding
		// the first successor snapshot. The replacement attempt may then join.
		if (!withheldSnapshot || refusalSeen) to->Publish(destination, tick, now);
		from.Flush(now);
		to->Flush(now);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	const auto ended = child.Poll();
	REQUIRE_FALSE(ended.Alive());
	CHECK(ended.Reason == parallel::ExitReason::Exited);
	CHECK(ended.Code == 0);
	CHECK(offered);
	CHECK(proceeded);
	CHECK(resumed);
	CHECK(committed);
	CHECK(destinationInput);
	REQUIRE(destinationInputAt.has_value());
	CHECK(now > *destinationInputAt + 1);
	CHECK(sourceInputsAfterAdoption == 0);
	if (outcome >= 2) CHECK(sourceInputsDuringContinuation > 0);
	CHECK(sourceFresh == 1);
	CHECK(destinationFresh == 0);
	CHECK_FALSE(sourceConsumers.Endpoints.empty());
	CHECK(destinationConsumers.Revision != 0);
	CHECK(destinationConsumers.Endpoints.empty() == (outcome != 0));
	CHECK(destinationConsumers.Session == sourceConsumers.Session);
	CHECK(requestedImage == (outcome == 0));
	CHECK(requestedEye == (outcome == 0));
	CHECK(refusalSeen == retryRefusal);
	CHECK(destinationRefusalSeen == (outcome == 2 || outcome == 3));
	CHECK(transportLost == (outcome == 4 || outcome == 5 || readinessDisconnect));
	CHECK(cancellationRequests == (lostCancellationAck ? 2 : retryRefusal ? 1 : 0));
}

TEST_CASE(
	"client product adopts a portal successor without a fresh avatar", "[client][portal-successor][gpu][.]"
) {
	RunPortalSuccessor(GENERATE(0, 1, 2, 3, 4, 5));
}

TEST_CASE(
	"client retries a lost portal cancellation acknowledgement",
	"[client][portal-successor][portal-cancel-ack][gpu][.]"
) {
	RunPortalSuccessor(6);
}

TEST_CASE(
	"client cancels an expired live successor and accepts a later transfer",
	"[client][portal-successor][portal-readiness-expiry][gpu][.]"
) {
	RunPortalSuccessor(7);
}

TEST_CASE(
	"client cancels a disconnected unready successor and accepts a later transfer",
	"[client][portal-successor][portal-readiness-disconnect][gpu][.]"
) {
	RunPortalSuccessor(8);
}
