#include <engine/parallel/Channel.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/PresentationRelay.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.world.presentationrelay")
TEST_DEPENDS("engine.world.presentationbus")
TEST_DEPENDS("engine.world.hostlink")

namespace {
	using namespace engine;
	using namespace engine::world;
	using core::Name;
	using Status = PresentationStatus;

	WorldSettings Named(const char *name) {
		WorldSettings settings;
		settings.Name = Name(name);
		return settings;
	}
	struct Fixture {
		Universe Parent, Producer;
		WorldId Near = Parent.Create(Named("near"));
		WorldId Far = Parent.Create(Named("far"));
		WorldId Replica = Producer.Create(Named("far"));
		std::pair<std::unique_ptr<parallel::Channel>, std::unique_ptr<parallel::Channel>> Pipe =
			parallel::MakeLocalChannel();
		HostLink Driver{std::move(Pipe.first)}, Child{std::move(Pipe.second)};
		PresentationRelay Relay{Parent, Far, 200, {"images", "topology"}};
		PresentationAddress NearReply, FarReply, Sessions, Image;
		Fixture() {
			REQUIRE(Parent.ConfigurePresentation(100));
			REQUIRE(Producer.ConfigurePresentation(200));
			NearReply = Parent.OpenPresentation(Near, Name("reply")).Address;
			FarReply = Parent.OpenPresentation(Far, Name("reply")).Address;
			Sessions = Parent.OpenPresentation(Far, Name("portal-sessions")).Address;
			Image = Producer.OpenPresentation(Replica, Name("images")).Address;
		}
		void Publish() {
			REQUIRE(Child.PublishPresentationDirectory(Producer.LocalPresentationDirectory()));
			REQUIRE(Relay.Pump(Driver));
		}
		void Receive() {
			std::vector<HostFrame> frames;
			Child.Receive(frames);
			for (const auto &frame : frames) {
				if (frame.Signal == HostSignal::PresentationRoutes) {
					for (const auto &address : frame.Directory.Endpoints) {
						if (!Producer.Find(Name(address.World)).IsValid()) {
							WorldSettings settings;
							settings.Name = Name(address.World);
							REQUIRE(Producer.CreateRemote(settings, Name("presentation-driver")).IsValid());
						}
					}
					const auto status = Producer.AcceptPresentationRoutesFromDriver(frame.Directory);
					REQUIRE((status == Status::Ok || status == Status::StaleEndpoint));
				} else {
					REQUIRE(frame.Signal == HostSignal::Presentation);
					REQUIRE(Producer.AcceptPresentationFromDriver(frame.Presentation) == Status::Ok);
				}
			}
		}
		void SendReplies() {
			for (const auto &outbound : Producer.TakePresentationOutbound())
				REQUIRE(Child.SendPresentation(outbound.Message));
		}
	};
}

TEST_CASE(
	"presentation relay defers directory changes and disconnects until the frame ends",
	"[world][presentation-relay]"
) {
	Fixture fixture;
	fixture.Publish();
	const auto mirror = fixture.Parent.LookupPresentation(fixture.Far, "images");
	REQUIRE(fixture.Producer.OpenPresentation(fixture.Replica, Name("topology")).Status == Status::Ok);
	REQUIRE(fixture.Child.PublishPresentationDirectory(fixture.Producer.LocalPresentationDirectory()));
	REQUIRE(fixture.Parent.BeginTickExchangeFrame(0) == 0);
	REQUIRE(fixture.Parent.TickExchangeFrameOpen());
	CHECK(fixture.Relay.Pump(fixture.Driver));
	CHECK(fixture.Relay.Refused() == 0);
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "images") == mirror);
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "topology").Session == 0);
	REQUIRE(fixture.Parent.EndTickExchangeFrame());
	REQUIRE(fixture.Relay.Pump(fixture.Driver));
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "images") == mirror);
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "topology").Session == 100);

	REQUIRE(fixture.Parent.BeginTickExchangeFrame(0) == 0);
	fixture.Child.Close();
	CHECK(fixture.Relay.Pump(fixture.Driver));
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "images") == mirror);
	REQUIRE(fixture.Parent.EndTickExchangeFrame());
	CHECK_FALSE(fixture.Relay.Pump(fixture.Driver));
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "images").Session == 0);
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "topology").Session == 0);
}

TEST_CASE(
	"presentation relay translates endpoint incarnations and same-world return routes",
	"[world][presentation-relay]"
) {
	Fixture fixture;
	fixture.Publish();
	const auto mirror = fixture.Parent.LookupPresentation(fixture.Far, "images");
	REQUIRE(mirror.Session == 100);
	CHECK(mirror.Generation != fixture.Image.Generation);
	for (const auto &source : {fixture.NearReply, fixture.FarReply}) {
		REQUIRE(
			fixture.Parent.SendPresentation(
				fixture.Parent.Find(Name(source.World)), source, mirror, 81, {}
			) == Status::Ok
		);
	}
	REQUIRE(fixture.Relay.Pump(fixture.Driver));
	fixture.Receive();
	const auto requests = fixture.Producer.TakePresentation(fixture.Image);
	REQUIRE(requests.size() == 2);
	CHECK(requests[0].From == fixture.NearReply);
	CHECK(requests[1].From.World != "far");
	CHECK(requests[1].From.Channel == fixture.FarReply.Channel);
	for (const auto &request : requests) {
		CHECK(request.To == fixture.Image);
		REQUIRE(
			fixture.Producer.SendPresentation(
				fixture.Replica,
				fixture.Image,
				request.From,
				request.Correlation,
				std::vector<std::byte>{std::byte{42}}
			) == Status::Ok
		);
	}
	const auto replies = fixture.Producer.TakePresentationOutbound();
	REQUIRE(replies.size() == 2);
	for (const auto &reply : replies)
		REQUIRE(fixture.Child.SendPresentation(reply.Message));
	REQUIRE(fixture.Relay.Pump(fixture.Driver));
	for (const auto &source : {fixture.NearReply, fixture.FarReply}) {
		const auto received = fixture.Parent.TakePresentation(source);
		REQUIRE(received.size() == 1);
		CHECK(received[0].From == mirror);
		CHECK(received[0].To == source);
		CHECK(received[0].Correlation == 81);
		CHECK(received[0].Payload == std::vector<std::byte>{std::byte{42}});
	}
	REQUIRE(fixture.Child.SendPresentation(replies.back().Message));
	REQUIRE(fixture.Relay.Pump(fixture.Driver));
	CHECK(fixture.Relay.Refused() == 1);
	CHECK(fixture.Parent.TakePresentation(fixture.FarReply).empty());
	REQUIRE(fixture.Producer.ClosePresentation(fixture.Image) == Status::Ok);
	fixture.Publish();
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "images").Session == 0);
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "portal-sessions") == fixture.Sessions);
	fixture.Image = fixture.Producer.OpenPresentation(fixture.Replica, Name("images")).Address;
	fixture.Publish();
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "images").Generation != mirror.Generation);
	fixture.Child.Close();
	CHECK_FALSE(fixture.Relay.Pump(fixture.Driver));
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "images").Session == 0);
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "portal-sessions") == fixture.Sessions);
}

TEST_CASE("presentation relay refuses ungranted child channels", "[world][presentation-relay]") {
	Fixture fixture;
	fixture.Publish();
	REQUIRE(fixture.Producer.OpenPresentation(fixture.Replica, Name("portal-sessions")).Status == Status::Ok);
	REQUIRE(fixture.Child.PublishPresentationDirectory(fixture.Producer.LocalPresentationDirectory()));
	CHECK_FALSE(fixture.Relay.Pump(fixture.Driver));
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "images").Session == 0);
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "portal-sessions") == fixture.Sessions);
	CHECK(fixture.Relay.Refused() == 1);
}

TEST_CASE(
	"presentation relay checks child identity world and local channel ownership",
	"[world][presentation-relay]"
) {
	Fixture fixture;
	auto directory = fixture.Producer.LocalPresentationDirectory();
	PresentationAddress existing;
	SECTION("another child session") {
		directory.Session++;
		directory.Endpoints[0].Session++;
	}
	SECTION("another world") {
		directory.Endpoints[0].World = "near";
	}
	SECTION("a channel already owned by the parent") {
		const auto opened = fixture.Parent.OpenPresentation(fixture.Far, Name("images"));
		REQUIRE(opened.Status == Status::Ok);
		existing = opened.Address;
	}
	REQUIRE(fixture.Child.PublishPresentationDirectory(directory));
	CHECK_FALSE(fixture.Relay.Pump(fixture.Driver));
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "images") == existing);
	CHECK(fixture.Parent.LookupPresentation(fixture.Far, "portal-sessions") == fixture.Sessions);
	CHECK(fixture.Relay.Refused() == 1);
}

TEST_CASE(
	"presentation relay returns replies through the authoritative host route", "[world][presentation-relay]"
) {
	Fixture fixture;
	const auto sourceHost = Name("source-host");
	REQUIRE(fixture.Parent.CreateRemote(Named("remote-source"), sourceHost).IsValid());
	const PresentationAddress source{"remote-source", "reply", 300, 7};
	REQUIRE(fixture.Parent.RegisterRemotePresentation(sourceHost, source) == Status::Ok);
	fixture.Publish();
	const auto mirror = fixture.Parent.LookupPresentation(fixture.Far, "images");
	REQUIRE(
		fixture.Parent.IngestPresentation(sourceHost, {source, mirror, 1, 90, {std::byte{9}}}) == Status::Ok
	);
	REQUIRE(fixture.Relay.Pump(fixture.Driver));
	fixture.Receive();
	const auto requests = fixture.Producer.TakePresentation(fixture.Image);
	REQUIRE(requests.size() == 1);
	CHECK(requests[0].From == source);
	REQUIRE(
		fixture.Producer.SendPresentation(
			fixture.Replica, fixture.Image, source, requests[0].Correlation, requests[0].Payload
		) == Status::Ok
	);
	fixture.SendReplies();
	REQUIRE(fixture.Relay.Pump(fixture.Driver));
	const auto replies = fixture.Parent.TakePresentationOutbound();
	REQUIRE(replies.size() == 1);
	CHECK(replies[0].Host == sourceHost);
	CHECK(replies[0].Message.From == mirror);
	CHECK(replies[0].Message.To == source);
	CHECK(replies[0].Message.Correlation == 90);
	CHECK(replies[0].Message.Payload == requests[0].Payload);
}

TEST_CASE(
	"presentation relay discards queued replies when their consumer incarnation expires",
	"[world][presentation-relay]"
) {
	Fixture fixture;
	fixture.Publish();
	fixture.Receive();
	for (uint64_t i = 0; i < 9; ++i) {
		REQUIRE(
			fixture.Producer.SendPresentation(
				fixture.Replica, fixture.Image, fixture.NearReply, i, std::vector<std::byte>{std::byte{42}}
			) == Status::Ok
		);
		fixture.SendReplies();
		REQUIRE(fixture.Relay.Pump(fixture.Driver));
	}
	REQUIRE(fixture.Relay.Queued().Messages == 1);
	REQUIRE(fixture.Parent.ClosePresentation(fixture.NearReply) == Status::Ok);
	const auto replacement = fixture.Parent.OpenPresentation(fixture.Near, Name("reply"));
	REQUIRE(replacement.Status == Status::Ok);
	REQUIRE(fixture.Relay.Pump(fixture.Driver));
	CHECK(fixture.Relay.Queued().Messages == 0);
	CHECK(fixture.Relay.Refused() == 1);
	CHECK(fixture.Parent.TakePresentation(replacement.Address).empty());
}

TEST_CASE("presentation relay retains bounded replies until consumer drains", "[world][presentation-relay]") {
	Fixture fixture;
	fixture.Publish();
	fixture.Receive();
	const auto mirror = fixture.Parent.LookupPresentation(fixture.Far, "images");
	for (uint64_t i = 0; i < 80; ++i) {
		REQUIRE(
			fixture.Producer.SendPresentation(
				fixture.Replica, fixture.Image, fixture.NearReply, i, std::vector<std::byte>{std::byte{42}}
			) == Status::Ok
		);
		fixture.SendReplies();
		REQUIRE(fixture.Relay.Pump(fixture.Driver));
		fixture.Receive();
	}
	CHECK(fixture.Relay.Queued().Messages == 64);
	CHECK(fixture.Relay.Queued().Bytes == 64);
	CHECK(fixture.Relay.Refused() == 8);
	for (uint64_t batch = 0; batch < 9; ++batch) {
		const auto received = fixture.Parent.TakePresentation(fixture.NearReply);
		REQUIRE(received.size() == 8);
		for (uint64_t i = 0; i < received.size(); ++i) {
			CHECK(received[i].From == mirror);
			CHECK(received[i].Correlation == batch * 8 + i);
		}
		REQUIRE(fixture.Relay.Pump(fixture.Driver));
	}
	CHECK(fixture.Relay.Queued().Messages == 0);
	CHECK(fixture.Relay.Queued().Bytes == 0);
}
