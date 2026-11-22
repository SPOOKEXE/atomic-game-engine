#include "PortalContacts.hpp"

#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/CopiedContacts.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/PortalCrossing.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/world/TickExchange.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace engine::script {
	namespace {
		// Bodies per worker for the seam search: a lookup, a component read and
		// a walk over every crossing seam per row.
		constexpr size_t CONTACT_SEARCH_GRAIN = 512;
		constexpr std::string_view PORTAL_CONTACTS_CHANNEL = "portal.static-contacts";
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
			// Search, then apply. Each body's nearest seam is a read of its own
			// rows and the gathered seams, so it runs on workers into the row's
			// slot; requests are then built serially in row order.
			struct Nearest {
				int32_t Seam = -1;
				float Reach = 0.0f;
			};
			static thread_local std::vector<Nearest> ownerNearest;
			std::vector<Nearest> &nearest = ownerNearest;
			nearest.resize(
				store.Query<const scene::Transform, const scene::Collider, const scene::Motion>().Count()
			);
			const ecs::Store &reader = store;
			const float delta = store.Time().Delta;
			store.Query<const scene::Transform, const scene::Collider, const scene::Motion>()
				.EachBatchEntitiesParallel(
					[&](size_t first,
						size_t rows,
						const ecs::Entity *roots,
						const scene::Transform *poses,
						const scene::Collider *colliders,
						const scene::Motion *motions) {
						for (size_t row = 0; row < rows; row++) {
							Nearest &found = nearest[first + row];
							found = {};
							const ecs::Entity root = roots[row];
							const scene::Transform &pose = poses[row];
							const scene::Collider &collider = colliders[row];
							if (collider.Trigger || !reader.Has<scene::Simulated>(root)) continue;
							float speed = motions[row].Linear.Magnitude();
							const auto controlled = std::lower_bound(
								speeds.begin(), speeds.end(), root.Id, [](const auto &entry, uint64_t id) {
									return entry.first < id;
								}
							);
							if (controlled != speeds.end() && controlled->first == root.Id)
								speed = std::max(speed, controlled->second);
							const float reach = collider.Extent.Magnitude() + speed * delta + .01f;
							const auto *crossing = reader.Get<scene::PortalCrossingState>(root);
							const scene::PortalSeam *selected = nullptr;
							float selectedDistance = std::numeric_limits<float>::infinity();
							for (const auto &seam : seams) {
								if (crossing && crossing->Phase != scene::PortalCrossingPhase::Idle &&
									(seam.Pane != crossing->Pane || seam.Far != crossing->Far))
									continue;
								const auto relative = pose.Frame.Position - seam.Centre;
								const float offset = relative.Dot(seam.Normal);
								if (std::abs(offset) > reach || (!seam.Bidirectional && offset < 0)) continue;
								const float firstAxis = seam.First.Magnitude(),
											secondAxis = seam.Second.Magnitude();
								if (!(firstAxis > 0 && secondAxis > 0) ||
									std::abs(relative.Dot(seam.First)) > firstAxis * (firstAxis + reach) ||
									std::abs(relative.Dot(seam.Second)) > secondAxis * (secondAxis + reach))
									continue;
								const float distance = std::abs(offset);
								if (selected &&
									(distance > selectedDistance ||
									 (distance == selectedDistance && seam.PanePath >= selected->PanePath)))
									continue;
								selected = &seam;
								selectedDistance = distance;
							}
							if (selected) found = {static_cast<int32_t>(selected - seams.data()), reach};
						}
					},
					CONTACT_SEARCH_GRAIN
				);
			size_t row = 0;
			store.Each<const scene::Transform, const scene::Collider, const scene::Motion>(
				[&](ecs::Entity root, const auto &pose, const auto &collider, const auto &) {
					const Nearest found = nearest[row++];
					const scene::PortalSeam *selected = found.Seam < 0 ? nullptr : &seams[found.Seam];
					const float reach = found.Reach;
					if (selected) {
						const auto &seam = *selected;
						const float offset = (pose.Frame.Position - seam.Centre).Dot(seam.Normal);
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
			physics::CopiedDynamicContacts dynamic;
			if (!physics::CollectDynamicContacts(store, window, dynamic, failure)) {
				return world::TickExchangeStatus::Overflow;
			}
			core::ByteWriter writer;
			if (!physics::WriteCopiedContacts(writer, contacts) ||
				!physics::WriteCopiedDynamicContacts(writer, dynamic))
				return world::TickExchangeStatus::Refused;
			out.assign(writer.Bytes().begin(), writer.Bytes().end());
			return world::TickExchangeStatus::Complete;
		}
		void Apply(ecs::Store &store, std::span<const world::TickExchangeReply> replies) {
			std::vector<physics::CopiedBodyContacts> bodies;
			std::vector<physics::CopiedDynamicBodyContacts> dynamicBodies;
			const auto *pending = store.Resource<ContactRequests>();
			if (pending && pending->Tick == store.Time().Tick) {
				for (size_t index = 0; index < pending->Pending.size(); ++index) {
					const auto &request = pending->Pending[index];
					physics::CopiedBodyContacts body;
					physics::CopiedDynamicBodyContacts dynamic;
					body.Root = request.Root;
					dynamic.Root = request.Root;
					for (const auto &reply : replies) {
						if (reply.Stamp.Sequence != index + 1 ||
							reply.Status != world::TickExchangeStatus::Complete)
							continue;
						core::ByteReader reader(reply.Payload);
						body.Complete = physics::ReadCopiedContacts(reader, body.Geometry);
						dynamic.Complete = body.Complete &&
										   physics::ReadCopiedDynamicContacts(reader, dynamic.Contacts) &&
										   reader.Remaining() == 0;
						body.Complete = dynamic.Complete;
						if (body.Complete) {
							for (auto &shape : body.Geometry.Shapes) {
								shape.Frame = request.Back.Place(shape.Frame);
								shape.Extent = shape.Extent * request.Back.Scale;
								shape.Linear = request.Back.Carry(shape.Linear);
								shape.Angular = request.Back.Rotate(shape.Angular);
								for (auto &point : shape.Points)
									point = request.Back.Point(point);
							}
							for (auto &far : dynamic.Contacts.Bodies) {
								far.Frame = request.Back.Place(far.Frame);
								far.Extent = far.Extent * request.Back.Scale;
								far.Motion.Linear = request.Back.Carry(far.Motion.Linear);
								far.Motion.Angular = request.Back.Rotate(far.Motion.Angular);
								far.Window.Centre = request.Back.Point(far.Window.Centre);
								far.Window.Normal = request.Back.Rotate(far.Window.Normal);
								far.Window.First = request.Back.Carry(far.Window.First);
								far.Window.Second = request.Back.Carry(far.Window.Second);
								far.Window.Depth *= request.Back.Scale;
							}
						}
						break;
					}
					bodies.push_back(std::move(body));
					dynamicBodies.push_back(std::move(dynamic));
				}
			}
			physics::SetCopiedBodyContacts(store, std::move(bodies));
			physics::SetCopiedDynamicBodyContacts(store, std::move(dynamicBodies));
		}
	}
	bool RegisterPortalContacts() {
		ecs::Components::Register<ContactRequests>(
			"script.PortalContactRequests", WriteRequests, ReadRequests
		);
		physics::RegisterCopiedContactComponents();
		return world::RegisterTickExchangeChannel(
			{std::string(PORTAL_CONTACTS_CHANNEL), Collect, Serve, Apply}
		);
	}
	bool ConfigurePortalContacts(ecs::Store &store, uint64_t incarnation) {
		if (!RegisterPortalContacts()) return false;
		return world::OpenTickExchange(store, PORTAL_CONTACTS_CHANNEL, incarnation);
	}
}
