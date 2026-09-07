#include "PortalContacts.hpp"

#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/CopiedContacts.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/world/TickExchange.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace engine::script {
	namespace {
		constexpr std::string_view CHANNEL = "portal.static-contacts";
		struct PendingContact {
			ecs::Entity Root;
			scene::SeamTransform Back;
		};
		struct ContactRequests {
			uint64_t Tick = 0;
			std::vector<PendingContact> Pending;
		};
		void WriteRequests(core::ByteWriter &writer, const void *, size_t count) {
			for (size_t index = 0; index < count; ++index)
				writer.WriteUInt8(0);
		}
		void ReadRequests(core::ByteReader &reader, void *destination, size_t count) {
			auto *requests = static_cast<ContactRequests *>(destination);
			for (size_t index = 0; index < count; ++index) {
				if (reader.ReadUInt8() != 0) reader.Fail();
				requests[index] = {};
			}
		}
		void Collect(ecs::Store &store, std::vector<world::TickExchangeRequest> &out) {
			ContactRequests pending;
			pending.Tick = store.Time().Tick;
			std::vector<scene::PortalSeam> seams;
			scene::GatherPortalSeams(store, seams);
			std::erase_if(seams, [](const auto &seam) { return !seam.Crosses; });
			if (seams.empty()) {
				store.SetResource(pending);
				return;
			}
			std::vector<std::pair<uint64_t, float>> speeds;
			store.Each<const scene::Humanoid>([&](ecs::Entity entity, const auto &humanoid) {
				const auto root = humanoid.RootPart == ecs::NULL_ENTITY ? entity : humanoid.RootPart;
				speeds.emplace_back(root.Id, humanoid.WalkSpeed);
			});
			std::sort(speeds.begin(), speeds.end());
			store.Each<const scene::Transform, const scene::Collider, const scene::Motion>(
				[&](ecs::Entity root, const auto &pose, const auto &collider, const auto &motion) {
					if (collider.Trigger || !store.Has<scene::Simulated>(root)) return;
					float speed = motion.Linear.Magnitude();
					const auto controlled = std::lower_bound(
						speeds.begin(), speeds.end(), root.Id, [](const auto &entry, uint64_t id) {
							return entry.first < id;
						}
					);
					if (controlled != speeds.end() && controlled->first == root.Id)
						speed = std::max(speed, controlled->second);
					const float reach = collider.Extent.Magnitude() + speed * store.Time().Delta + .01f;
					for (const auto &seam : seams) {
						const auto relative = pose.Frame.Position - seam.Centre;
						const float offset = relative.Dot(seam.Normal);
						if (std::abs(offset) > reach || (!seam.Bidirectional && offset < 0)) continue;
						const float first = seam.First.Magnitude(), second = seam.Second.Magnitude();
						if (!(first > 0 && second > 0) ||
							std::abs(relative.Dot(seam.First)) > first * (first + reach) ||
							std::abs(relative.Dot(seam.Second)) > second * (second + reach))
							continue;
						if (pending.Pending.size() >= world::MAXIMUM_TICK_EXCHANGE_MESSAGES)
							throw std::runtime_error("portal contact request capacity exceeded");
						const auto through = scene::SeamMapping(seam);
						physics::ContactWindow window;
						window.Centre = through.Point(seam.Centre);
						window.Normal = through.Rotate(seam.Normal * (offset < 0 ? -1.0f : 1.0f));
						window.First = through.Carry(seam.First);
						window.Second = through.Carry(seam.Second);
						window.Depth = reach * through.Scale;
						window.Layer = collider.Layer;
						window.Mask = collider.Mask;
						core::ByteWriter writer;
						if (!physics::WriteContactWindow(writer, window))
							throw std::runtime_error("invalid portal contact window");
						world::TickExchangeRequest request;
						request.Stamp.DestinationWorld = std::string(seam.DestinationWorld.Text());
						request.Payload.assign(writer.Bytes().begin(), writer.Bytes().end());
						out.push_back(std::move(request));
						pending.Pending.push_back(
							{root,
							 {through.Frame.Inverse(), through.Point(through.Origin), 1 / through.Scale}}
						);
					}
				}
			);
			store.SetResource(pending);
		}
		world::TickExchangeStatus
		Serve(ecs::Store &store, const world::TickExchangeRequest &request, std::vector<std::byte> &out) {
			core::ByteReader reader(request.Payload);
			physics::ContactWindow window;
			if (!physics::ReadContactWindow(reader, window) || reader.Remaining() != 0)
				return world::TickExchangeStatus::Refused;
			(void)scene::OpenPortals(store);
			physics::CopiedStaticContacts contacts;
			std::string failure;
			if (!physics::CollectStaticContacts(store, window, contacts, failure)) {
				return world::TickExchangeStatus::Overflow;
			}
			core::ByteWriter writer;
			if (!physics::WriteCopiedContacts(writer, contacts)) return world::TickExchangeStatus::Refused;
			out.assign(writer.Bytes().begin(), writer.Bytes().end());
			return world::TickExchangeStatus::Complete;
		}
		void Apply(ecs::Store &store, std::span<const world::TickExchangeReply> replies) {
			std::vector<physics::CopiedBodyContacts> bodies;
			const auto *pending = store.Resource<ContactRequests>();
			if (pending && pending->Tick == store.Time().Tick) {
				for (size_t index = 0; index < pending->Pending.size(); ++index) {
					const auto &request = pending->Pending[index];
					physics::CopiedBodyContacts body;
					body.Root = request.Root;
					for (const auto &reply : replies) {
						if (reply.Stamp.Sequence != index + 1 ||
							reply.Status != world::TickExchangeStatus::Complete)
							continue;
						core::ByteReader reader(reply.Payload);
						body.Complete =
							physics::ReadCopiedContacts(reader, body.Geometry) && reader.Remaining() == 0;
						if (body.Complete) {
							for (auto &shape : body.Geometry.Shapes) {
								shape.Frame = request.Back.Place(shape.Frame);
								shape.Extent = shape.Extent * request.Back.Scale;
								for (auto &point : shape.Points)
									point = point * request.Back.Scale;
							}
						}
						break;
					}
					bodies.push_back(std::move(body));
				}
			}
			physics::SetCopiedBodyContacts(store, std::move(bodies));
		}
	}
	bool RegisterPortalContacts() {
		ecs::Components::Register<ContactRequests>(
			"script.PortalContactRequests", WriteRequests, ReadRequests
		);
		physics::RegisterCopiedContactComponents();
		return world::RegisterTickExchangeChannel({std::string(CHANNEL), Collect, Serve, Apply});
	}
	bool ConfigurePortalContacts(ecs::Store &store, uint64_t incarnation) {
		if (!RegisterPortalContacts()) return false;
		return world::OpenTickExchange(store, CHANNEL, incarnation);
	}
}
