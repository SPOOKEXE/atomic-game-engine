#include <engine/assets/Manifest.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/Paths.hpp>
#include <engine/delivery/GroupCodec.hpp>
#include <engine/game/Content.hpp>
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

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

TEST_SUITE_ID("client.portalsession")
TEST_DEPENDS("engine.game.portalsession")

static void RunPortalSuccessor(int outcome) {
	using namespace engine;
	const bool renderContent = outcome >= 10;
	const bool meshContent = outcome == 11 || outcome == 13;
	const bool refuseSourceContent = outcome == 12 || outcome == 13;
	const bool lateLocalDemand = outcome == 14;
	const bool retireSource = outcome == 15;
	bool sourceClosed = false;
	const bool observeContent = outcome == 9 || renderContent;
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
	struct ContentPublication {
		std::string Name;
		std::string BundleRoute;
		std::vector<std::byte> Manifest;
		std::vector<std::byte> Bundle;
		std::vector<std::pair<replication::ClientId, game::ContentRouteRequest>> Requests;
		size_t BundleRequests = 0;
		bool Offered = false;
		std::optional<double> FirstBundleReplyAt;
		std::optional<double> SecondBundleRequestedAt;
	};
	const auto publication = [&](std::string name, std::byte colour) {
		ContentPublication published;
		published.Name = std::move(name);
		if (!observeContent) return published;
		core::ByteWriter encoded;
		if (meshContent) {
			assets::MeshData mesh;
			for (const auto position : std::array{
					 core::Vector3{-.5f, -.5f, 0},
					 core::Vector3{.5f, -.5f, 0},
					 core::Vector3{.5f, .5f, 0},
					 core::Vector3{-.5f, .5f, 0}
				 }) {
				assets::MeshVertex vertex{};
				vertex.Position[0] = position.X;
				vertex.Position[1] = position.Y;
				vertex.Normal[2] = 1;
				mesh.Vertices.push_back(vertex);
			}
			mesh.Indices = {0, 1, 2, 2, 1, 0};
			if (colour == std::byte{142}) mesh.Indices.insert(mesh.Indices.end(), {0, 2, 3, 3, 2, 0});
			assets::Submesh surface;
			surface.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
			surface.BaseColour[0] = colour == std::byte{142} ? 0 : 1;
			surface.BaseColour[1] = 0;
			surface.BaseColour[2] = colour == std::byte{142} ? 1 : 0;
			mesh.Submeshes.push_back(surface);
			mesh.ComputeBounds();
			REQUIRE(assets::Mesh::Write(encoded, mesh));
		} else {
			assets::TextureData texture;
			texture.Width = texture.Height = 1;
			texture.Pixels = {colour, std::byte{0}, std::byte{0}, std::byte{255}};
			if (renderContent && colour == std::byte{142})
				texture.Pixels = {std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
			REQUIRE(assets::Texture::Write(encoded, texture));
		}
		assets::Manifest manifest;
		const auto root = manifest.AddAsset(
			published.Name,
			meshContent ? assets::AssetKind::Mesh : assets::AssetKind::Texture,
			{{assets::Hasher::Of(encoded.Bytes()), static_cast<uint32_t>(encoded.Bytes().size())}}
		);
		const auto bundle = manifest.AddBundle(std::span(&root, 1));
		REQUIRE(bundle);
		published.BundleRoute = "/bundle/" + bundle->ToHex();
		const auto compressed = delivery::GroupCodec::Compress(encoded.Bytes());
		REQUIRE(compressed);
		published.Bundle = *compressed;
		core::ByteWriter signedManifest;
		const auto signature = identity->SignManifestRoot(manifest.Root());
		signedManifest.WriteRaw(signature.Value.data(), signature.Value.size());
		manifest.Write(signedManifest);
		published.Manifest.assign(signedManifest.Bytes().begin(), signedManifest.Bytes().end());
		return published;
	};
	const auto sharedName = meshContent ? "portal-same-name.amesh" : "portal-same-name.atex";
	auto sourceContent = publication(renderContent ? sharedName : "portal-source-only.atex", std::byte{71});
	auto destinationContent =
		publication(renderContent ? sharedName : "portal-destination-only.atex", std::byte{142});
	const auto contentWall = [&](ecs::Store &store, const ContentPublication &published) {
		scene::PartDesc wall;
		wall.Frame.Position = {0, 3, -100};
		wall.Size = {1000, 1000, 1};
		wall.Simulated = false;
		const auto part = scene::MakePart(store, wall);
		REQUIRE(store.SetParent(part, scene::WorkspaceOf(store)));
		if (meshContent)
			store.GetMutable<scene::Visual>(part)->Mesh = core::Name(published.Name);
		else
			store.GetMutable<scene::SurfaceAppearance>(part)->ColourMap = core::Name(published.Name);
	};
	if (renderContent) contentWall(destination, destinationContent);
	bool destinationContentRequestedBeforeAdoption = false;
	const auto contentRequest =
		[&](ContentPublication &published, replication::ClientId peer, std::span<const std::byte> bytes) {
			game::ContentRouteRequest request;
			if (!game::DecodeContentRequest(bytes, request)) return false;
			REQUIRE(observeContent);
			if (request.Route.starts_with("/bundle/")) {
				CHECK(request.Route == published.BundleRoute);
				++published.BundleRequests;
				if (published.BundleRequests == 2) published.SecondBundleRequestedAt = core::Clock::Seconds();
				if (&published == &destinationContent && !destinationInputAt)
					destinationContentRequestedBeforeAdoption = true;
			}
			published.Requests.emplace_back(peer, std::move(request));
			return true;
		};
	bool sourceManifestWaited = false;
	const auto serveContent = [&](ContentPublication &published,
								  replication::Listener &listener,
								  std::optional<replication::ClientId> peer,
								  double now) {
		if (!observeContent || !peer) return;
		if (!published.Offered) {
			game::ContentDirectory directory;
			directory.PublisherKey = identity->Public().ToHex();
			published.Offered = listener.SendTo(*peer, game::EncodeContentDirectory(directory), now);
		}
		// The source manifest was requested before adoption and completes afterward.
		if (&published == &sourceContent && !destinationInputAt) {
			sourceManifestWaited |=
				std::any_of(published.Requests.begin(), published.Requests.end(), [](const auto &queued) {
					return queued.second.Route == "/manifest";
				});
			return;
		}
		std::erase_if(published.Requests, [&](const auto &queued) {
			const auto &[requestPeer, request] = queued;
			if (request.Route != "/manifest" && request.Route != published.BundleRoute)
				return listener.SendTo(requestPeer, game::EncodeContentRefusal({request.Ticket}), now);
			if (refuseSourceContent && &published == &sourceContent &&
				request.Route == published.BundleRoute) {
				const bool sent =
					listener.SendTo(requestPeer, game::EncodeContentRefusal({request.Ticket}), now);
				if (sent && !published.FirstBundleReplyAt) published.FirstBundleReplyAt = now;
				return sent;
			}
			const auto &payload = request.Route == "/manifest" ? published.Manifest : published.Bundle;
			game::ContentChunk chunk;
			chunk.Ticket = request.Ticket;
			chunk.TotalBytes = static_cast<uint32_t>(payload.size());
			chunk.Bytes = payload;
			const bool sent = listener.SendTo(requestPeer, game::EncodeContentChunk(chunk), now);
			if (sent && request.Route == published.BundleRoute && !published.FirstBundleReplyAt)
				published.FirstBundleReplyAt = now;
			return sent;
		});
	};
	bool lateContentAuthored = false;
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
		if (contentRequest(sourceContent, peer, bytes)) return;
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
		if (contentRequest(destinationContent, peer, bytes)) return;
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
	std::vector<std::string> arguments{
		"--headless",
		"--frames",
		"100000",
		"--profile-seconds",
		readinessExpiry ? "25"
		: outcome >= 4	? "15"
		: outcome == 0	? "8"
						: "5",
		"--config",
		config.string(),
		"--connect",
		net::Endpoint::LoopbackIPv4(sourceSocket->Local().Port).Text(),
		"--server-key",
		identity->Public().ToHex(),
		"--mcp-port",
		"-1"
	};
	if (observeContent) {
		const auto absentStore = config.string() + ".no-content";
		REQUIRE_FALSE(std::filesystem::exists(absentStore));
		arguments.insert(
			arguments.end(),
			{"--cdn",
			 "dir:" + absentStore,
			 "--publisher-key",
			 identity->Public().ToHex(),
			 "--content-cache",
			 ""}
		);
	}
	const auto captures =
		core::Paths::Base() / ("portal-published-owner-" + std::to_string(outcome) + "-frames");
	if (renderContent) {
		std::filesystem::remove_all(captures);
		const auto emptyScene = core::Paths::Base() / "portal-published-empty.luau";
		std::ofstream(emptyScene)
			<< (lateLocalDemand ? R"(
local elapsed = 0
local requested = false
game:GetService("RunService").Heartbeat:Connect(function(delta)
    elapsed += delta
    if requested or elapsed < 3 then return end
    requested = true
    local part = Instance.new("MeshPart")
    part.Anchored = true
    part.MeshId = "engine.Cube"
    part.TextureID = "portal-same-name.atex"
    part.Position = Vector3.new(5000, 0, 0)
    part.Parent = workspace
end)
)"
								: "return\n");
		arguments.insert(
			arguments.end(),
			{"--uncapped",
			 "--max-fps",
			 "60",
			 "--width",
			 "64",
			 "--height",
			 "64",
			 "--script",
			 emptyScene.string(),
			 "--capture-sequence",
			 captures.string()}
		);
	}
	REQUIRE(
		child.Start(core::Paths::Base().parent_path() / "client" / core::Paths::Program("client"), arguments)
	);

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
		if (observeContent && destinationInputAt && !lateContentAuthored) {
			if (renderContent) contentWall(source, sourceContent);
			for (auto pair :
				 {std::pair{&source, &sourceContent}, std::pair{&destination, &destinationContent}}) {
				if (renderContent) continue;
				const auto part = scene::MakePart(*pair.first, {});
				scene::SurfaceAppearance appearance;
				appearance.ColourMap = core::Name(pair.second->Name);
				pair.first->Set(part, appearance);
			}
			lateContentAuthored = true;
		}
		if (retireSource && !sourceClosed && sourceContent.FirstBundleReplyAt &&
			now > *sourceContent.FirstBundleReplyAt + 1) {
			sourceSocket->Close();
			sourceClosed = true;
		}
		serveContent(sourceContent, from, sourcePeer, now);
		serveContent(destinationContent, *to, destinationPeer, now);
		from.ClearInputs();
		to->ClearInputs();
		std::erase_if(sourceReplies, [&](const auto &reply) {
			return from.SendTo(reply.first, game::EncodePortalSession(reply.second), now);
		});
		std::erase_if(destinationReplies, [&](const auto &reply) {
			if (renderContent && reply.second.Kind == game::PortalSessionKind::Committed &&
				(!destinationContent.FirstBundleReplyAt || now < *destinationContent.FirstBundleReplyAt + .2))
				return false;
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
	if (observeContent) {
		CHECK(lateContentAuthored);
		CHECK(sourceManifestWaited);
		CHECK(sourceContent.BundleRequests == 1);
		if (renderContent) {
			CHECK(destinationContentRequestedBeforeAdoption);
			CHECK(destinationContent.BundleRequests == (lateLocalDemand ? 2 : 1));
			if (lateLocalDemand) {
				REQUIRE(destinationContent.SecondBundleRequestedAt);
				CHECK(*destinationContent.SecondBundleRequestedAt > *destinationInputAt + 1);
			}
			REQUIRE(sourceContent.FirstBundleReplyAt);
			REQUIRE(destinationContent.FirstBundleReplyAt);
			CHECK(*destinationContent.FirstBundleReplyAt < *destinationInputAt);
			CHECK(*sourceContent.FirstBundleReplyAt > *destinationInputAt);
			CHECK(now > *sourceContent.FirstBundleReplyAt + 1);
			std::filesystem::path latest;
			uint64_t lastFrame = 0;
			for (const auto &entry : std::filesystem::directory_iterator(captures)) {
				if (entry.path().extension() != ".bmp") continue;
				const auto frame = std::stoull(entry.path().stem().string());
				if (latest.empty() || frame > lastFrame) {
					lastFrame = frame;
					latest = entry.path();
				}
			}
			REQUIRE_FALSE(latest.empty());
			auto metadata = latest;
			metadata.replace_extension(".json");
			std::ifstream recorded(metadata);
			REQUIRE(recorded.good());
			const auto finalFrame = nlohmann::json::parse(recorded);
			CHECK(
				finalFrame.at(meshContent ? "delivered_meshes" : "delivered_textures") ==
				(refuseSourceContent ? 1
				 : lateLocalDemand	 ? 3
									 : 2)
			);
			CHECK(finalFrame.at("pending_content") == 0);
			if (retireSource) {
				CHECK(sourceClosed);
				CHECK_FALSE(finalFrame.contains("retained_world"));
				nlohmann::json retained;
				uint64_t retainedFrame = 0;
				for (const auto &entry : std::filesystem::directory_iterator(captures)) {
					if (entry.path().extension() != ".json") continue;
					std::ifstream input(entry.path());
					const auto sample = nlohmann::json::parse(input);
					if (!sample.contains("retained_world") || sample.at("delivered_textures") != 2) continue;
					const auto number = sample.at("frame").get<uint64_t>();
					if (retained.empty() || number > retainedFrame) {
						retained = sample;
						retainedFrame = number;
					}
				}
				REQUIRE_FALSE(retained.empty());
				CHECK(retained.at("pending_content") == 0);
				CHECK(retained.at("content_owner") == "client.portal.1");
				const auto &before = retained.at("gpu_memory");
				const auto &after = finalFrame.at("gpu_memory");
				CHECK(after.at("buffer_bytes") == before.at("buffer_bytes"));
				CHECK(finalFrame.at("seconds").get<double>() > retained.at("seconds").get<double>() + 1);
				CHECK(
					after.at("texture_bytes").get<uint64_t>() + 4 ==
					before.at("texture_bytes").get<uint64_t>()
				);
				CHECK(after.at("textures").get<uint64_t>() + 1 == before.at("textures").get<uint64_t>());
				CHECK(
					after.at("released_bytes").get<uint64_t>() >=
					before.at("released_bytes").get<uint64_t>() + 4
				);
			} else
				CHECK(finalFrame.at("retained_world") == "client.replica");
			CHECK(finalFrame.at("content_owner") == "client.portal.1");
			CHECK(finalFrame.at("view_world") == "client.portal.1");
			CHECK_FALSE(finalFrame.at("eye_image").get<bool>());
			std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
				SDL_LoadBMP(latest.string().c_str()), SDL_DestroySurface
			);
			REQUIRE(image);
			Uint8 red = 0, green = 0, blue = 0, alpha = 0;
			REQUIRE(
				SDL_ReadSurfacePixel(image.get(), image->w / 8, image->h / 8, &red, &green, &blue, &alpha)
			);
			CAPTURE(latest, red, green, blue, destinationContent.BundleRequests);
			CHECK(blue > 2 * red);
			CHECK(blue > 60);
		} else
			CHECK(destinationContent.BundleRequests == 1);
		CHECK(sourceContent.Requests.empty());
		CHECK(destinationContent.Requests.empty());
	}

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

TEST_CASE(
	"portal observation retains its admitted content route after player adoption",
	"[client][portal-observation-content][gpu][.]"
) {
	RunPortalSuccessor(9);
}

TEST_CASE(
	"late source delivery cannot replace successor same-name assets",
	"[client][portal-published-owner][gpu][.]"
) {
	RunPortalSuccessor(GENERATE(10, 11));
}

TEST_CASE(
	"refused source content leaves successor assets intact", "[client][portal-published-refusal][gpu][.]"
) {
	RunPortalSuccessor(GENERATE(12, 13));
}

TEST_CASE(
	"a newly served world can request an already delivered name later",
	"[client][portal-published-late-demand][gpu][.]"
) {
	RunPortalSuccessor(14);
}

TEST_CASE(
	"expired source observation retires its texture and preserves the successor",
	"[client][portal-published-retirement][gpu][.]"
) {
	RunPortalSuccessor(15);
}
