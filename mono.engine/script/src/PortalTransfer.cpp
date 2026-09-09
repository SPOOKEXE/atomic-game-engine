#include "PortalContacts.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Continuous.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/world/Postbox.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace engine::script {
	namespace {
		using core::ByteReader;
		using core::ByteWriter;
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		constexpr std::string_view CHANNEL = "engine.portal.transfer";
		constexpr uint32_t MAGIC = 0x35545050;
		constexpr size_t MAXIMUM_ACTIVE = 16;
		constexpr size_t MAXIMUM_NATIVE_INPUTS = 64;
		struct ScheduledMove {
			uint64_t InputTick = 0;
			uint64_t PhysicsTick = 0;
			core::Vector3 Direction;
			bool Jump = false;
		};
		struct PlayerInputClock {
			Entity Root;
			Entity Humanoid;
			uint64_t AnchorWorldTick = 0;
			uint64_t AnchorInputTick = 0;
			double InputStep = 0;
			double WorldStep = 0;
			uint64_t AppliedInputTick = 0;
			uint64_t LastQueuedInputTick = 0;
			bool Native = false;
			size_t Begin = 0;
			size_t Count = 0;
			std::array<ScheduledMove, MAXIMUM_NATIVE_INPUTS> Pending{};
		};
		constexpr size_t MAXIMUM_RECORDS = 64;
		constexpr size_t MAXIMUM_BYTES = 4 * 1024 * 1024;
		constexpr uint64_t RETRY_TICKS = 15;
		enum class MessageKind : uint8_t {
			Offer,
			Ready,
			Commit,
			Done,
			Refuse,
			Cancel,
			Cancelled,
			Move,
			MoveAck
		};
		struct ForwardMove {
			uint64_t Sequence = 0;
			uint64_t JumpSequence = 0;
			uint64_t InputTick = 0;
			double StepSeconds = 0;
			core::Vector3 Direction;
		};
		struct Message {
			PortalTransferId Id;
			uint64_t DestinationIncarnation = 0;
			MessageKind Kind = MessageKind::Offer;
			scene::SeamTransform Through;
			std::vector<std::byte> Body;
			std::string Diagnostic;
		};
		struct AnimationHold {
			Entity Owner;
			std::optional<scene::Animator> Animator;
			std::optional<scene::AnimationTrack> Track;
		};
		struct Outgoing {
			PortalTransferReceipt Receipt;
			std::vector<AnimationHold> Animation;
			Entity Subject;
			Entity Root;
			Entity Humanoid;
			scene::Motion OriginalMotion;
			scene::Humanoid OriginalHumanoid;
			bool WasSimulated = false;
			bool HadOwner = false;
			scene::NetworkOwner OriginalOwner;
			uint64_t DestinationIncarnation = 0;
			uint64_t RetryAt = 0;
			uint64_t AdmittedIncarnation = 0;
			world::Ticket Ticket;
			ForwardMove Move{};
			uint64_t AcknowledgedMove = 0;
			bool InputClosed = false;
			std::vector<std::byte> Body;
		};
		struct Incoming {
			scene::PortalBodyKind Kind = scene::PortalBodyKind::Player;
			PortalTransferId Id;
			std::vector<std::byte> Body;
			scene::SeamTransform Through;
			Entity Subject;
			bool Committed = false;
			bool Cancelled = false;
			ForwardMove Move{};
			uint64_t AppliedMove = 0;
			uint64_t AppliedJump = 0;
			uint64_t AppliedInputTick = 0;
			std::optional<PortalTransferMotion> Motion;
			bool MotionReplyPending = false;
			bool InputClosed = false;
		};
		struct Peer {
			std::string World;
			uint64_t Incarnation = 0;
			uint64_t RetiredThrough = 0;
			std::vector<uint64_t> RetiredIncarnations;
		};
		struct TransferState {
			uint64_t Incarnation = 0;
			uint64_t NextSequence = 1;
			world::Ticket Opening;
			bool Open = false;
			bool RequirePlayerAdmission = false;
			std::vector<Outgoing> Out;
			std::vector<Incoming> In;
			std::vector<Peer> Peers;
		};

		bool ValidText(std::string_view text, bool empty = false) {
			return (empty || !text.empty()) && text.size() <= 256 &&
				   text.find('\0') == std::string_view::npos;
		}
		std::string Text(ByteReader &reader, bool empty = false) {
			const uint32_t count = reader.ReadUInt32();
			if (count > 256 || count > reader.Remaining()) {
				reader.Fail();
				return {};
			}
			std::string value(count, '\0');
			reader.ReadRaw(value.data(), count);
			if (!ValidText(value, empty)) reader.Fail();
			return value;
		}
		void WriteId(ByteWriter &writer, const PortalTransferId &id) {
			writer.WriteString(id.SourceWorld);
			writer.WriteUInt64(id.SourceIncarnation);
			writer.WriteUInt64(id.Sequence);
		}
		PortalTransferId ReadId(ByteReader &reader) {
			PortalTransferId id;
			id.SourceWorld = Text(reader);
			id.SourceIncarnation = reader.ReadUInt64();
			id.Sequence = reader.ReadUInt64();
			if (id.SourceIncarnation == 0 || id.Sequence == 0) reader.Fail();
			return id;
		}
		void WriteBytes(ByteWriter &writer, std::span<const std::byte> bytes) {
			writer.WriteUInt32(static_cast<uint32_t>(bytes.size()));
			writer.WriteRaw(bytes.data(), bytes.size());
		}
		std::vector<std::byte> ReadBytes(ByteReader &reader, size_t budget = MAXIMUM_BYTES) {
			const uint32_t size = reader.ReadUInt32();
			if (size > scene::MAXIMUM_PORTAL_BODY_BYTES + 4 || size > reader.Remaining() || size > budget) {
				reader.Fail();
				return {};
			}
			std::vector<std::byte> bytes(size);
			reader.ReadRaw(bytes.data(), size);
			return bytes;
		}
		bool Similarity(const scene::SeamTransform &through) {
			const auto finite = [](const core::Vector3 &v) {
				return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
			};
			const auto q = through.Frame.Rotation();
			const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
			return finite(through.Frame.Position) && finite(through.Origin) && std::isfinite(norm) &&
				   std::abs(norm - 1) < .001f && std::isfinite(through.Scale) && through.Scale > 0;
		}
		void WriteThrough(ByteWriter &writer, const scene::SeamTransform &through) {
			writer.WriteRaw(&through.Frame, sizeof(through.Frame));
			writer.WriteRaw(&through.Origin, sizeof(through.Origin));
			writer.WriteFloat(through.Scale);
		}
		scene::SeamTransform ReadThrough(ByteReader &reader) {
			scene::SeamTransform through;
			reader.ReadRaw(&through.Frame, sizeof(through.Frame));
			reader.ReadRaw(&through.Origin, sizeof(through.Origin));
			through.Scale = reader.ReadFloat();
			if (!Similarity(through)) reader.Fail();
			return through;
		}
		bool ValidDirection(const core::Vector3 &direction) {
			return std::isfinite(direction.X) && std::isfinite(direction.Y) && std::isfinite(direction.Z) &&
				   direction.Magnitude() <= 1.001f;
		}
		void WriteMove(ByteWriter &writer, const ForwardMove &move) {
			writer.WriteUInt64(move.Sequence);
			writer.WriteUInt64(move.JumpSequence);
			writer.WriteUInt64(move.InputTick);
			writer.WriteDouble(move.StepSeconds);
			writer.WriteFloat(move.Direction.X);
			writer.WriteFloat(move.Direction.Y);
			writer.WriteFloat(move.Direction.Z);
		}
		ForwardMove ReadMove(ByteReader &reader) {
			ForwardMove move;
			move.Sequence = reader.ReadUInt64();
			move.JumpSequence = reader.ReadUInt64();
			move.InputTick = reader.ReadUInt64();
			move.StepSeconds = reader.ReadDouble();
			move.Direction.X = reader.ReadFloat();
			move.Direction.Y = reader.ReadFloat();
			move.Direction.Z = reader.ReadFloat();
			if (move.JumpSequence > move.Sequence || !ValidDirection(move.Direction) ||
				!std::isfinite(move.StepSeconds) || move.StepSeconds < 0)
				reader.Fail();
			return move;
		}
		std::vector<std::byte> Encode(const Message &message) {
			ByteWriter writer;
			writer.WriteUInt32(MAGIC);
			writer.WriteUInt8(static_cast<uint8_t>(message.Kind));
			WriteId(writer, message.Id);
			writer.WriteUInt64(message.DestinationIncarnation);
			WriteThrough(writer, message.Through);
			writer.WriteString(message.Diagnostic);
			WriteBytes(writer, message.Body);
			return {writer.Bytes().begin(), writer.Bytes().end()};
		}
		bool Decode(std::span<const std::byte> bytes, Message &out) {
			if (bytes.size() > scene::MAXIMUM_PORTAL_BODY_BYTES + 1024) return false;
			ByteReader reader(bytes);
			if (reader.ReadUInt32() != MAGIC) return false;
			const auto kind = reader.ReadUInt8();
			if (kind > static_cast<uint8_t>(MessageKind::MoveAck)) return false;
			Message message;
			message.Kind = static_cast<MessageKind>(kind);
			message.Id = ReadId(reader);
			message.DestinationIncarnation = reader.ReadUInt64();
			message.Through = ReadThrough(reader);
			message.Diagnostic = Text(reader, true);
			message.Body = ReadBytes(reader);
			if (reader.Failed() || !reader.AtEnd() ||
				(message.Kind == MessageKind::Offer || message.Kind == MessageKind::Move ||
				 message.Kind == MessageKind::MoveAck) != !message.Body.empty())
				return false;
			out = std::move(message);
			return true;
		}
		TransferState *State(ecs::Store &store) {
			return ecs::Components::Assigned<TransferState>().IsValid()
					   ? store.ResourceMutable<TransferState>()
					   : nullptr;
		}
		const TransferState *State(const ecs::Store &store) {
			return ecs::Components::Assigned<TransferState>().IsValid() ? store.Resource<TransferState>()
																		: nullptr;
		}
		size_t HeldBytes(const TransferState &state) {
			size_t bytes = 0;
			for (const auto &record : state.In)
				bytes += record.Body.size();
			for (const auto &record : state.Out)
				bytes += record.Body.size();
			return bytes;
		}
		bool Active(const Outgoing &record) {
			return record.Receipt.Stage == PortalTransferStage::Preparing ||
				   record.Receipt.Stage == PortalTransferStage::Committing ||
				   record.Receipt.Stage == PortalTransferStage::Cancelling;
		}

		bool Retired(const Outgoing &record) {
			return !Active(record) &&
				   (record.Receipt.Kind != scene::PortalBodyKind::Player || record.InputClosed ||
					record.Receipt.Stage == PortalTransferStage::Refused);
		}

		world::Ticket Send(ecs::Store &store, std::string_view destination, const Message &message) {
			const auto encoded = Encode(message);
			const auto ticket = world::Postbox(store).SendTo(destination, CHANNEL, encoded);
			if (ticket.Expected()) {
				core::Metrics::Count("world.portal.sent.messages", 1);
				core::Metrics::Count("world.portal.sent.bytes", encoded.size());
			}
			return ticket;
		}
		std::vector<AnimationHold>
		HoldAnimation(const ecs::Store &store, Entity subject, scene::PortalBodyKind kind) {
			std::vector<AnimationHold> holds;
			const auto capture = [&](Entity entity) {
				const auto *animator = store.Get<scene::Animator>(entity);
				const auto *track = store.Get<scene::AnimationTrack>(entity);
				if (!animator && !track) return;
				AnimationHold held;
				held.Owner = entity;
				if (animator) held.Animator = *animator;
				if (track) held.Track = *track;
				holds.push_back(held);
			};
			capture(subject);
			store.EachDescendant(subject, capture);
			if (kind == scene::PortalBodyKind::Player) {
				const Entity character = scene::CharacterOf(store, subject);
				capture(character);
				store.EachDescendant(character, capture);
			}
			return holds;
		}
		template <class T> void WriteAnimationRow(ByteWriter &writer, const T &value) {
			ByteWriter encoded;
			ecs::Components::Describe(ecs::Components::Of<T>()).Write(encoded, &value, 1);
			WriteBytes(writer, encoded.Bytes());
		}
		template <class T> T ReadAnimationRow(ByteReader &reader) {
			T value;
			const auto bytes = ReadBytes(reader, 256);
			const auto &descriptor = ecs::Components::Describe(ecs::Components::Of<T>());
			if (descriptor.RawSerialisation) {
				const auto boolean = [&](size_t offset) {
					return offset < bytes.size() && std::to_integer<unsigned>(bytes[offset]) <= 1;
				};
				bool valid = bytes.size() == sizeof(T);
				if constexpr (std::is_same_v<T, scene::Animator>)
					valid &= boolean(offsetof(T, RootMotion)) && boolean(offsetof(T, EvaluationThrottled));
				else
					valid &= boolean(offsetof(T, Looped)) && boolean(offsetof(T, Playing));
				if (!valid) {
					reader.Fail();
					return value;
				}
			}
			ByteReader input(bytes);
			descriptor.Read(input, &value, 1);
			ByteWriter canonical;
			descriptor.Write(canonical, &value, 1);
			if (input.Failed() || !input.AtEnd() || !std::ranges::equal(bytes, canonical.Bytes()))
				reader.Fail();
			if constexpr (std::is_same_v<T, scene::Animator>) {
				if (!std::isfinite(value.RootMotionWeight) || value.RootMotionWeight < 0 ||
					value.RootMotionWeight > 1)
					reader.Fail();
			} else {
				if (!std::isfinite(value.TimePosition) || !std::isfinite(value.Speed) ||
					!std::isfinite(value.Weight) || !std::isfinite(value.WeightTarget) ||
					!std::isfinite(value.FadeTime) || value.Weight < 0 || value.Weight > 1 ||
					value.WeightTarget < 0 || value.WeightTarget > 1 || value.FadeTime < 0 ||
					value.Priority > scene::AnimationPriority::Override)
					reader.Fail();
			}
			return value;
		}
		void WriteAnimationHolds(ByteWriter &writer, const std::vector<AnimationHold> &holds) {
			writer.WriteUInt32(static_cast<uint32_t>(holds.size()));
			for (const auto &held : holds) {
				writer.WriteUInt64(held.Owner.Id);
				writer.WriteBool(held.Animator.has_value());
				if (held.Animator) WriteAnimationRow(writer, *held.Animator);
				writer.WriteBool(held.Track.has_value());
				if (held.Track) WriteAnimationRow(writer, *held.Track);
			}
		}
		bool ValidMotion(const PortalTransferMotion &motion) {
			const auto finite = [](const core::Vector3 &v) {
				return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
			};
			return motion.DestinationIncarnation != 0 && motion.DestinationTick != 0 &&
				   Similarity({motion.Frame, {}, 1}) && finite(motion.Linear) && finite(motion.Angular) &&
				   std::isfinite(motion.WalkSpeed) && motion.WalkSpeed >= 0 &&
				   std::isfinite(motion.JumpSpeed) && motion.JumpSpeed >= 0 &&
				   std::isfinite(motion.SimulationSeconds) && motion.SimulationSeconds >= 0;
		}
		void WriteMotion(ByteWriter &writer, const std::optional<PortalTransferMotion> &motion) {
			writer.WriteBool(motion.has_value());
			if (!motion) return;
			writer.WriteUInt64(motion->DestinationIncarnation);
			writer.WriteUInt64(motion->DestinationTick);
			writer.WriteUInt64(motion->InputTick);
			const auto vector = [&](const core::Vector3 &v) {
				writer.WriteFloat(v.X);
				writer.WriteFloat(v.Y);
				writer.WriteFloat(v.Z);
			};
			vector(motion->Frame.Position);
			const auto q = motion->Frame.Rotation();
			writer.WriteFloat(q.x);
			writer.WriteFloat(q.y);
			writer.WriteFloat(q.z);
			writer.WriteFloat(q.w);
			vector(motion->Linear);
			vector(motion->Angular);
			writer.WriteFloat(motion->WalkSpeed);
			writer.WriteFloat(motion->JumpSpeed);
			writer.WriteBool(motion->Grounded);
			writer.WriteDouble(motion->SimulationSeconds);
		}
		std::optional<PortalTransferMotion> ReadMotion(ByteReader &reader) {
			const auto present = reader.ReadUInt8();
			if (present > 1) reader.Fail();
			if (present != 1) return std::nullopt;
			PortalTransferMotion motion;
			motion.DestinationIncarnation = reader.ReadUInt64();
			motion.DestinationTick = reader.ReadUInt64();
			motion.InputTick = reader.ReadUInt64();
			const auto vector = [&] {
				const float x = reader.ReadFloat(), y = reader.ReadFloat(), z = reader.ReadFloat();
				return core::Vector3{x, y, z};
			};
			const auto position = vector();
			const float x = reader.ReadFloat(), y = reader.ReadFloat(), z = reader.ReadFloat(),
						w = reader.ReadFloat();
			motion.Frame = core::CFrame(position, glm::quat(w, x, y, z));
			motion.Linear = vector();
			motion.Angular = vector();
			motion.WalkSpeed = reader.ReadFloat();
			motion.JumpSpeed = reader.ReadFloat();
			const auto grounded = reader.ReadUInt8();
			motion.Grounded = grounded != 0;
			motion.SimulationSeconds = reader.ReadDouble();
			if (grounded > 1 || !ValidMotion(motion)) reader.Fail();
			return motion;
		}
		bool SameMotion(const PortalTransferMotion &a, const PortalTransferMotion &b) {
			return a.DestinationIncarnation == b.DestinationIncarnation && a.InputTick == b.InputTick &&
				   a.Frame.Position == b.Frame.Position && a.Frame.Rotation() == b.Frame.Rotation() &&
				   a.Linear == b.Linear && a.Angular == b.Angular && a.WalkSpeed == b.WalkSpeed &&
				   a.JumpSpeed == b.JumpSpeed && a.Grounded == b.Grounded;
		}
		bool StrictBool(ByteReader &reader) {
			const uint8_t value = reader.ReadUInt8();
			if (value > 1) reader.Fail();
			return value == 1;
		}
		std::vector<AnimationHold> ReadAnimationHolds(ByteReader &reader) {
			const uint32_t count = reader.ReadUInt32();
			if (count > scene::MAXIMUM_PORTAL_NODES) {
				reader.Fail();
				return {};
			}
			std::vector<AnimationHold> holds;
			for (uint32_t index = 0; index < count; ++index) {
				AnimationHold held;
				held.Owner = Entity{reader.ReadUInt64()};
				if (StrictBool(reader)) held.Animator = ReadAnimationRow<scene::Animator>(reader);
				if (StrictBool(reader)) held.Track = ReadAnimationRow<scene::AnimationTrack>(reader);
				if ((!held.Animator && !held.Track) || held.Owner == NULL_ENTITY ||
					std::any_of(holds.begin(), holds.end(), [&](const auto &previous) {
						return previous.Owner == held.Owner;
					}))
					reader.Fail();
				if (reader.Failed()) return {};
				holds.push_back(held);
			}
			return holds;
		}

		void Restore(ecs::Store &store, Outgoing &record, std::string diagnostic) {
			// Once commit is queued, no timeout or refusal can recreate source authority.
			if (record.Receipt.Stage != PortalTransferStage::Preparing &&
				record.Receipt.Stage != PortalTransferStage::Cancelling)
				return;
			if (store.Alive(record.Root)) {
				store.Set(record.Root, record.OriginalMotion);
				if (record.WasSimulated) store.Set(record.Root, scene::Simulated{});
				if (record.HadOwner) store.Set(record.Root, record.OriginalOwner);
			}
			if (store.Alive(record.Humanoid)) {
				auto humanoid = record.OriginalHumanoid;
				if (record.Move.Sequence != 0) {
					humanoid.MoveDirection = record.Move.Direction;
					humanoid.JumpRequested |= record.Move.JumpSequence != 0;
				}
				store.Set(record.Humanoid, humanoid);
			}
			for (const auto &held : record.Animation) {
				if (!store.Alive(held.Owner)) continue;
				if (held.Animator) store.Set(held.Owner, *held.Animator);
				if (held.Track) store.Set(held.Owner, *held.Track);
			}
			record.Animation.clear();
			record.Receipt.Stage = PortalTransferStage::Refused;
			record.Receipt.Diagnostic = std::move(diagnostic);
			record.Body.clear();
		}
		Message Response(
			const TransferState &state, const Message &request, MessageKind kind, std::string diagnostic = {}
		) {
			Message response;
			response.Id = request.Id;
			response.DestinationIncarnation = state.Incarnation;
			response.Kind = kind;
			response.Through = request.Through;
			response.Diagnostic = std::move(diagnostic);
			return response;
		}

		bool ResolveSuffix(ecs::Store &store, scene::PortalBodyCopy &body, std::string &failure) {
			if (!body.Sweep) return true;
			scene::Collider collider;
			scene::Transform root;
			bool hasCollider = false, hasRoot = false;
			for (const auto &node : body.Nodes) {
				if (node.Key != body.Root) continue;
				for (const auto &component : node.Components) {
					ByteReader reader(component.Bytes);
					if (component.Type == "scene.Collider") {
						ecs::Components::Describe(ecs::Components::Of<scene::Collider>())
							.Read(reader, &collider, 1);
						hasCollider = !reader.Failed();
					}
					if (component.Type == "scene.Transform") {
						ecs::Components::Describe(ecs::Components::Of<scene::Transform>())
							.Read(reader, &root, 1);
						hasRoot = !reader.Failed();
					}
				}
			}
			const auto &sweep = *body.Sweep;
			if (!hasCollider || !hasRoot ||
				(sweep.From.Position + sweep.Displacement - root.Frame.Position).Magnitude() > .001f) {
				failure = "portal suffix does not end at the copied root";
				return false;
			}
			physics::SyncBroadphase(store);
			const auto hit = physics::SweepPlacement(
				store, collider, sweep.From, sweep.Displacement, sweep.AngularDisplacement
			);
			if (!hit.Complete) {
				failure = "destination suffix collision query is unavailable or exceeds its candidate bound";
				return false;
			}
			if (!hit.Hit) return true;
			const float reach = sweep.Displacement.Magnitude() +
								sweep.AngularDisplacement.Magnitude() * collider.Extent.Magnitude();
			const float fraction =
				std::min(1.0f, hit.Fraction + physics::CONTINUOUS_BITE / std::max(reach, .001f));
			const auto clamped =
				physics::Advanced(sweep.From, sweep.Displacement, sweep.AngularDisplacement, fraction);
			const auto adjustment = clamped * root.Frame.Inverse();
			for (auto &node : body.Nodes)
				for (auto &component : node.Components) {
					if (component.Type != "scene.Transform" && component.Type != "scene.PreviousTransform")
						continue;
					const auto adjust = [&]<class T>() {
						const auto &descriptor = ecs::Components::Describe(ecs::Components::Of<T>());
						T pose;
						ByteReader reader(component.Bytes);
						descriptor.Read(reader, &pose, 1);
						pose.Frame = adjustment * pose.Frame;
						ByteWriter writer;
						descriptor.Write(writer, &pose, 1);
						component.Bytes.assign(writer.Bytes().begin(), writer.Bytes().end());
					};
					if (component.Type == "scene.Transform")
						adjust.template operator()<scene::Transform>();
					else
						adjust.template operator()<scene::PreviousTransform>();
				}
			body.Sweep.reset();
			return true;
		}

		bool RoomForIncoming(const ecs::Store &store, TransferState &state) {
			if (state.In.size() < MAXIMUM_RECORDS) return true;
			auto retired = std::find_if(state.In.begin(), state.In.end(), [&](const auto &r) {
				return r.Cancelled || (r.Committed && (r.Kind != scene::PortalBodyKind::Player ||
													   r.InputClosed || !store.Alive(r.Subject)));
			});
			if (retired == state.In.end()) return false;
			auto origin = std::find_if(state.Peers.begin(), state.Peers.end(), [&](const auto &p) {
				return p.World == retired->Id.SourceWorld && p.Incarnation == retired->Id.SourceIncarnation;
			});
			if (origin != state.Peers.end())
				origin->RetiredThrough = std::max(origin->RetiredThrough, retired->Id.Sequence);
			state.In.erase(retired);
			return true;
		}

		void ApplyForwardMove(ecs::Store &store, Incoming &record) {
			if (!record.Committed || record.InputClosed) return;
			if (!store.Alive(record.Subject)) {
				record.InputClosed = true;
				return;
			}
			if (record.Move.Sequence <= record.AppliedMove) return;
			const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, record.Subject));
			if (!rig) return;
			auto *humanoid = store.GetMutable<scene::Humanoid>(rig->Humanoid);
			if (!humanoid) return;
			humanoid->MoveDirection = record.Through.Rotate(record.Move.Direction);
			humanoid->JumpRequested |= record.Move.JumpSequence > record.AppliedJump;
			record.AppliedMove = record.Move.Sequence;
			record.AppliedJump = record.Move.JumpSequence;
			record.AppliedInputTick = record.Move.InputTick;
			if (record.Move.InputTick != 0 && record.Move.StepSeconds > 0 && store.Time().Delta > 0) {
				if (!store.Has<PlayerInputClock>(record.Subject))
					store.Set(record.Subject, PlayerInputClock{});
				auto *clock = store.GetMutable<PlayerInputClock>(record.Subject);
				if (clock->Root != rig->Root || clock->Humanoid != rig->Humanoid) *clock = {};
				if (!clock->Native && record.Move.InputTick > clock->AnchorInputTick) {
					clock->Root = rig->Root;
					clock->Humanoid = rig->Humanoid;
					clock->AnchorWorldTick = store.Time().Tick;
					clock->AnchorInputTick = record.Move.InputTick;
					clock->InputStep = record.Move.StepSeconds;
					clock->WorldStep = store.Time().Delta;
					clock->AppliedInputTick = record.Move.InputTick;
					clock->LastQueuedInputTick = record.Move.InputTick;
				}
			}
			static const core::LogCategory controlTrace("portal-input");
			if (controlTrace.Enabled(core::LogLevel::Trace)) {
				const auto *root = store.Get<scene::Transform>(rig->Root);
				const auto &direction = humanoid->MoveDirection;
				ENGINE_LOG(
					core::LogLevel::Trace,
					"portal-input",
					"route=forwarded incarnation={} player={} world_tick={} input_tick={} delta={} "
					"input_step={} "
					"direction={},{},{} position={},{},{}",
					PortalTransferIncarnation(store),
					record.Subject.Id,
					store.Time().Tick,
					record.AppliedInputTick,
					store.Time().Delta,
					record.Move.StepSeconds,
					direction.X,
					direction.Y,
					direction.Z,
					root ? root->Frame.Position.X : 0,
					root ? root->Frame.Position.Y : 0,
					root ? root->Frame.Position.Z : 0
				);
			}
			core::Metrics::Count("world.portal.move.applied", 1);
		}
		bool AcknowledgeMove(ecs::Store &store, const TransferState &state, const Incoming &record) {
			if (record.Move.Sequence == 0 && !record.InputClosed) return false;
			Message ack;
			ack.Kind = MessageKind::MoveAck;
			ack.Id = record.Id;
			ack.DestinationIncarnation = state.Incarnation;
			ByteWriter writer;
			writer.WriteUInt64(record.AppliedMove);
			writer.WriteUInt64(record.AppliedInputTick);
			writer.WriteBool(record.InputClosed);
			WriteMotion(writer, record.Motion);
			ack.Body.assign(writer.Bytes().begin(), writer.Bytes().end());
			return Send(store, record.Id.SourceWorld, ack).Expected();
		}
		void Process(ecs::Store &store, TransferState &state, std::string_view from, const Message &message) {
			if (message.Kind == MessageKind::MoveAck) {
				const auto found = std::find_if(state.Out.begin(), state.Out.end(), [&](const auto &record) {
					return record.Receipt.Id == message.Id && record.Receipt.DestinationWorld == from &&
						   record.DestinationIncarnation != 0 &&
						   record.DestinationIncarnation == message.DestinationIncarnation;
				});
				if (found == state.Out.end()) return;
				ByteReader reader(message.Body);
				const auto applied = reader.ReadUInt64();
				const auto inputTick = reader.ReadUInt64();
				const auto closed = reader.ReadUInt8();
				const auto motion = ReadMotion(reader);
				if (reader.Failed() || !reader.AtEnd() || closed > 1 || applied > found->Move.Sequence ||
					inputTick > found->Move.InputTick || (applied == 0 && inputTick != 0) ||
					(applied == found->Move.Sequence && inputTick != found->Move.InputTick))
					return;
				if (motion && (motion->DestinationIncarnation != message.DestinationIncarnation ||
							   motion->InputTick > inputTick))
					return;
				if (motion && (!found->Receipt.Motion ||
							   (motion->DestinationTick > found->Receipt.Motion->DestinationTick &&
								motion->InputTick >= found->Receipt.Motion->InputTick)))
					found->Receipt.Motion = motion;
				found->AcknowledgedMove = std::max(found->AcknowledgedMove, applied);
				found->Receipt.AcknowledgedInputTick =
					std::max(found->Receipt.AcknowledgedInputTick, inputTick);
				found->InputClosed |= closed != 0;
				return;
			}
			if (message.Kind == MessageKind::Move) {
				if (from != message.Id.SourceWorld || message.DestinationIncarnation != state.Incarnation)
					return;
				const auto found = std::find_if(state.In.begin(), state.In.end(), [&](const auto &record) {
					return record.Id == message.Id && record.Kind == scene::PortalBodyKind::Player &&
						   !record.Cancelled;
				});
				ByteReader reader(message.Body);
				const auto move = ReadMove(reader);
				if (reader.Failed() || !reader.AtEnd() || move.Sequence == 0) return;
				if (found == state.In.end()) {
					const auto peer =
						std::find_if(state.Peers.begin(), state.Peers.end(), [&](const auto &entry) {
							return entry.World == from && entry.Incarnation == message.Id.SourceIncarnation &&
								   message.Id.Sequence <= entry.RetiredThrough;
						});
					if (peer == state.Peers.end()) return;
					Incoming retired;
					retired.Id = message.Id;
					retired.InputClosed = true;
					AcknowledgeMove(store, state, retired);
					return;
				}
				if (move.Sequence > found->Move.Sequence && move.JumpSequence >= found->Move.JumpSequence &&
					move.InputTick >= found->Move.InputTick)
					found->Move = move;
				ApplyForwardMove(store, *found);
				found->MotionReplyPending = true;
				if (found->InputClosed) AcknowledgeMove(store, state, *found);
				return;
			}
			if (message.Kind == MessageKind::Ready || message.Kind == MessageKind::Done ||
				message.Kind == MessageKind::Refuse || message.Kind == MessageKind::Cancelled) {
				const auto found = std::find_if(state.Out.begin(), state.Out.end(), [&](const auto &record) {
					return record.Receipt.Id == message.Id && record.Receipt.DestinationWorld == from;
				});
				if (found == state.Out.end()) return;
				auto &record = *found;
				if (message.Kind == MessageKind::Cancelled) {
					if (record.Receipt.Stage == PortalTransferStage::Cancelling)
						Restore(store, record, record.Receipt.Diagnostic);
					return;
				}
				if (record.Receipt.Stage == PortalTransferStage::Cancelling) return;
				if (message.Kind == MessageKind::Refuse) {
					Restore(store, record, message.Diagnostic);
					return;
				}
				if (message.Kind == MessageKind::Done) {
					if (record.Receipt.Stage == PortalTransferStage::Committing &&
						message.DestinationIncarnation == record.DestinationIncarnation) {
						record.Receipt.Stage = PortalTransferStage::Committed;
						record.Body.clear();
					}
					return;
				}
				if (record.Receipt.Stage != PortalTransferStage::Preparing ||
					message.DestinationIncarnation == 0)
					return;
				Message commit = message;
				if (state.RequirePlayerAdmission && record.Receipt.Kind == scene::PortalBodyKind::Player) {
					if (record.AdmittedIncarnation == 0) return;
					if (record.AdmittedIncarnation != message.DestinationIncarnation) {
						record.Receipt.Diagnostic = "destination changed after player admission";
						return;
					}
				}
				commit.Kind = MessageKind::Commit;
				const auto ticket = Send(store, from, commit);
				if (!ticket.Expected()) return;
				record.DestinationIncarnation = message.DestinationIncarnation;
				record.Receipt.Stage = PortalTransferStage::Committing;
				record.Ticket = ticket;
				record.RetryAt = store.Time().Tick + RETRY_TICKS;
				if (record.Receipt.Kind == scene::PortalBodyKind::Player)
					(void)scene::RemoveCharacter(store, record.Subject);
				store.DestroyInstance(record.Subject);
				record.Animation.clear();
				return;
			}
			if (from != message.Id.SourceWorld) return;
			auto peer = std::find_if(state.Peers.begin(), state.Peers.end(), [&](const auto &p) {
				return p.World == from;
			});
			if (peer == state.Peers.end()) {
				if (state.Peers.size() == MAXIMUM_RECORDS) {
					Send(
						store,
						from,
						Response(state, message, MessageKind::Refuse, "portal peer bound reached")
					);
					return;
				}
				state.Peers.push_back({std::string(from), message.Id.SourceIncarnation, 0, {}});
				peer = state.Peers.end() - 1;
			}
			if (message.Id.SourceIncarnation != peer->Incarnation) {
				if (std::ranges::find(peer->RetiredIncarnations, message.Id.SourceIncarnation) !=
					peer->RetiredIncarnations.end())
					return;
				if (peer->RetiredIncarnations.size() == MAXIMUM_RECORDS) {
					Send(
						store,
						from,
						Response(
							state, message, MessageKind::Refuse, "portal incarnation history bound reached"
						)
					);
					return;
				}
				peer->RetiredIncarnations.push_back(peer->Incarnation);
				state.In.erase(
					std::remove_if(
						state.In.begin(),
						state.In.end(),
						[&](const auto &r) { return !r.Committed && r.Id.SourceWorld == from; }
					),
					state.In.end()
				);
				peer->Incarnation = message.Id.SourceIncarnation;
				peer->RetiredThrough = 0;
			}
			auto found = std::find_if(state.In.begin(), state.In.end(), [&](const auto &r) {
				return r.Id == message.Id;
			});
			if (message.Kind == MessageKind::Cancel) {
				if (found != state.In.end() && found->Committed) {
					Send(store, from, Response(state, message, MessageKind::Done));
					return;
				}
				if (found == state.In.end()) {
					// Retired history no longer proves whether this receipt committed.
					if (message.Id.Sequence <= peer->RetiredThrough) return;
					if (!RoomForIncoming(store, state)) return;
					Incoming cancelled;
					cancelled.Id = message.Id;
					cancelled.Cancelled = true;
					state.In.push_back(std::move(cancelled));
				} else {
					found->Cancelled = true;
					found->Body = std::vector<std::byte>{};
				}
				Send(store, from, Response(state, message, MessageKind::Cancelled));
				return;
			}
			if (found != state.In.end()) {
				if (found->Cancelled) {
					Send(store, from, Response(state, message, MessageKind::Cancelled));
					return;
				}
				if (message.Kind == MessageKind::Offer) {
					if (!found->Committed && found->Body != message.Body) {
						Send(
							store,
							from,
							Response(state, message, MessageKind::Refuse, "portal receipt payload changed")
						);
						return;
					}
					Send(
						store,
						from,
						Response(state, message, found->Committed ? MessageKind::Done : MessageKind::Ready)
					);
					return;
				}
				if (message.Kind != MessageKind::Commit ||
					message.DestinationIncarnation != state.Incarnation)
					return;
				if (found->Committed) {
					Send(store, from, Response(state, message, MessageKind::Done));
					return;
				}
				scene::PortalBodyCopy body;
				ByteReader reader(found->Body);
				std::string failure;
				scene::PortalBodyArrival arrival;
				if (!scene::ReadPortalBody(reader, body) || !reader.AtEnd() ||
					!ResolveSuffix(store, body, failure) ||
					!scene::AdmitPortalBody(store, body, arrival, failure)) {
					ENGINE_WARN("portal commit remains pending: {}", failure);
					return;
				}
				found->Subject = body.Kind == scene::PortalBodyKind::Player ? arrival.Player : arrival.Root;
				found->Committed = true;
				found->Body.clear();
				ApplyForwardMove(store, *found);
				AcknowledgeMove(store, state, *found);
				Send(store, from, Response(state, message, MessageKind::Done));
				return;
			}
			if (message.Kind != MessageKind::Offer || message.Id.Sequence <= peer->RetiredThrough) return;
			if (std::count_if(
					state.In.begin(),
					state.In.end(),
					[](const auto &r) { return !r.Committed && !r.Cancelled; }
				) >= static_cast<std::ptrdiff_t>(MAXIMUM_ACTIVE) ||
				message.Body.size() > MAXIMUM_BYTES - std::min(MAXIMUM_BYTES, HeldBytes(state))) {
				Send(
					store,
					from,
					Response(state, message, MessageKind::Refuse, "portal reservation bound reached")
				);
				return;
			}
			scene::PortalBodyCopy body;
			ByteReader reader(message.Body);
			std::string failure;
			if (!scene::ReadPortalBody(reader, body) || !reader.AtEnd() ||
				(body.Kind == scene::PortalBodyKind::Player && scene::PlayersOf(store) == NULL_ENTITY) ||
				scene::WorkspaceOf(store) == NULL_ENTITY || !ResolveSuffix(store, body, failure)) {
				Send(
					store,
					from,
					Response(
						state,
						message,
						MessageKind::Refuse,
						"destination refuses malformed rig or missing services"
					)
				);
				return;
			}
			const auto *settings = store.Get<scene::PlayersServiceComponent>(scene::PlayersOf(store));
			const auto reservations =
				static_cast<size_t>(std::count_if(state.In.begin(), state.In.end(), [](const auto &r) {
					return !r.Committed && !r.Cancelled && r.Kind == scene::PortalBodyKind::Player;
				}));
			if (body.Kind == scene::PortalBodyKind::Player && settings != nullptr &&
				scene::PlayerCount(store) + reservations >=
					static_cast<size_t>(std::max(0, settings->MaxPlayers))) {
				Send(
					store,
					from,
					Response(state, message, MessageKind::Refuse, "destination player capacity reached")
				);
				return;
			}
			if (!RoomForIncoming(store, state)) return;
			Incoming incoming;
			incoming.Kind = body.Kind;
			incoming.Id = message.Id;
			incoming.Body = message.Body;
			incoming.Through = message.Through;
			state.In.push_back(std::move(incoming));
			Send(store, from, Response(state, message, MessageKind::Ready));
		}

		void PumpNativeMoves(ecs::Store &store) {
			ENGINE_PROFILE("portal native input");
			if (store.AdoptOnly()) return;
			store.Each<PlayerInputClock>([&](Entity player, PlayerInputClock &clock) {
				if (!clock.Native) return;
				const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
				if (!rig || rig->Root != clock.Root || rig->Humanoid != clock.Humanoid) {
					clock = {};
					return;
				}
				auto *humanoid = store.GetMutable<scene::Humanoid>(clock.Humanoid);
				if (!humanoid) return;
				while (clock.Count != 0) {
					const auto &move = clock.Pending[clock.Begin];
					if (move.PhysicsTick > store.Time().Tick) break;
					const auto previousDirection = humanoid->MoveDirection;
					humanoid->MoveDirection = move.Direction;
					humanoid->JumpRequested |= move.Jump;
					clock.AppliedInputTick = move.InputTick;
					clock.Begin = (clock.Begin + 1) % MAXIMUM_NATIVE_INPUTS;
					--clock.Count;
					if (previousDirection != core::Vector3{} && humanoid->MoveDirection == core::Vector3{}) {
						ENGINE_LOG(
							core::LogLevel::Trace,
							"portal-input-stop",
							"route=scheduled incarnation={} player={} humanoid={} world_tick={} "
							"input_tick={} direction=0,0,0",
							PortalTransferIncarnation(store),
							player.Id,
							clock.Humanoid.Id,
							store.Time().Tick,
							clock.AppliedInputTick
						);
					}
					ENGINE_LOG(
						core::LogLevel::Trace,
						"portal-input",
						"route=scheduled incarnation={} player={} world_tick={} input_tick={} delta={} "
						"input_step={} humanoid={} direction={},{},{}",
						PortalTransferIncarnation(store),
						player.Id,
						store.Time().Tick,
						clock.AppliedInputTick,
						store.Time().Delta,
						clock.InputStep,
						clock.Humanoid.Id,
						humanoid->MoveDirection.X,
						humanoid->MoveDirection.Y,
						humanoid->MoveDirection.Z
					);
					core::Metrics::Count("world.portal.native.applied", 1);
				}
			});
		}

		void WriteInputClocks(ByteWriter &writer, const void *values, size_t count) {
			const auto *clocks = static_cast<const PlayerInputClock *>(values);
			for (size_t index = 0; index < count; ++index) {
				const auto &clock = clocks[index];
				writer.WriteUInt64(clock.Root.Id);
				writer.WriteUInt64(clock.Humanoid.Id);
				writer.WriteUInt64(clock.AnchorWorldTick);
				writer.WriteUInt64(clock.AnchorInputTick);
				writer.WriteDouble(clock.InputStep);
				writer.WriteDouble(clock.WorldStep);
				writer.WriteUInt64(clock.AppliedInputTick);
				writer.WriteUInt64(clock.LastQueuedInputTick);
				writer.WriteBool(clock.Native);
				writer.WriteUInt32(static_cast<uint32_t>(clock.Count));
				for (size_t offset = 0; offset < clock.Count; ++offset) {
					const auto &move = clock.Pending[(clock.Begin + offset) % MAXIMUM_NATIVE_INPUTS];
					writer.WriteUInt64(move.InputTick);
					writer.WriteUInt64(move.PhysicsTick);
					writer.WriteFloat(move.Direction.X);
					writer.WriteFloat(move.Direction.Y);
					writer.WriteFloat(move.Direction.Z);
					writer.WriteBool(move.Jump);
				}
			}
		}
		void ReadInputClocks(ByteReader &reader, void *values, size_t count) {
			auto *clocks = static_cast<PlayerInputClock *>(values);
			for (size_t index = 0; index < count; ++index) {
				PlayerInputClock clock;
				clock.Root = Entity(reader.ReadUInt64());
				clock.Humanoid = Entity(reader.ReadUInt64());
				clock.AnchorWorldTick = reader.ReadUInt64();
				clock.AnchorInputTick = reader.ReadUInt64();
				clock.InputStep = reader.ReadDouble();
				clock.WorldStep = reader.ReadDouble();
				clock.AppliedInputTick = reader.ReadUInt64();
				clock.LastQueuedInputTick = reader.ReadUInt64();
				clock.Native = StrictBool(reader);
				clock.Count = reader.ReadUInt32();
				if (clock.Count > MAXIMUM_NATIVE_INPUTS || !std::isfinite(clock.InputStep) ||
					!std::isfinite(clock.WorldStep) || clock.InputStep < 0 || clock.WorldStep < 0 ||
					(clock.Root != NULL_ENTITY &&
					 (clock.Humanoid == NULL_ENTITY || clock.InputStep == 0 || clock.WorldStep == 0)) ||
					(clock.Native && clock.Root == NULL_ENTITY) ||
					clock.AppliedInputTick > clock.LastQueuedInputTick) {
					reader.Fail();
					return;
				}
				uint64_t previousInput = clock.AppliedInputTick, previousPhysics = 0;
				for (size_t offset = 0; offset < clock.Count; ++offset) {
					auto &move = clock.Pending[offset];
					move.InputTick = reader.ReadUInt64();
					move.PhysicsTick = reader.ReadUInt64();
					move.Direction = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
					move.Jump = StrictBool(reader);
					if (!clock.Native || move.InputTick <= previousInput ||
						move.InputTick > clock.LastQueuedInputTick || move.PhysicsTick < previousPhysics ||
						!ValidDirection(move.Direction))
						reader.Fail();
					previousInput = move.InputTick;
					previousPhysics = move.PhysicsTick;
				}
				if (reader.Failed()) return;
				clocks[index] = clock;
			}
		}

		void WriteStates(ByteWriter &writer, const void *values, size_t count) {
			const auto *states = static_cast<const TransferState *>(values);
			for (size_t index = 0; index < count; ++index) {
				const auto &state = states[index];
				writer.WriteUInt64(state.Incarnation);
				writer.WriteUInt64(state.NextSequence);
				writer.WriteUInt64(state.Opening.Value);
				writer.WriteBool(state.Open);
				writer.WriteBool(state.RequirePlayerAdmission);
				writer.WriteUInt32(static_cast<uint32_t>(state.Out.size()));
				for (const auto &record : state.Out) {
					WriteAnimationHolds(writer, record.Animation);
					writer.WriteString(scene::Describe(record.Receipt.Kind));
					WriteId(writer, record.Receipt.Id);
					writer.WriteString(record.Receipt.DestinationWorld);
					writer.WriteUInt8(static_cast<uint8_t>(record.Receipt.Stage));
					WriteThrough(writer, record.Receipt.Through);
					writer.WriteString(record.Receipt.Diagnostic);
					writer.WriteUInt64(record.Receipt.AcknowledgedInputTick);
					WriteMotion(writer, record.Receipt.Motion);
					writer.WriteUInt64(record.Subject.Id);
					writer.WriteUInt64(record.Root.Id);
					writer.WriteUInt64(record.Humanoid.Id);
					writer.WriteRaw(&record.OriginalMotion, sizeof(record.OriginalMotion));
					writer.WriteRaw(&record.OriginalHumanoid, sizeof(record.OriginalHumanoid));
					writer.WriteBool(record.WasSimulated);
					writer.WriteBool(record.HadOwner);
					writer.WriteUInt64(record.OriginalOwner.Player.Id);
					writer.WriteUInt64(record.DestinationIncarnation);
					writer.WriteUInt64(record.RetryAt);
					writer.WriteUInt64(record.AdmittedIncarnation);
					writer.WriteUInt64(record.Ticket.Value);
					WriteMove(writer, record.Move);
					writer.WriteUInt64(record.AcknowledgedMove);
					writer.WriteBool(record.InputClosed);
					WriteBytes(writer, record.Body);
				}
				writer.WriteUInt32(static_cast<uint32_t>(state.In.size()));
				for (const auto &record : state.In) {
					writer.WriteString(scene::Describe(record.Kind));
					WriteId(writer, record.Id);
					WriteBytes(writer, record.Body);
					WriteThrough(writer, record.Through);
					writer.WriteUInt64(record.Subject.Id);
					writer.WriteBool(record.Committed);
					writer.WriteBool(record.Cancelled);
					WriteMove(writer, record.Move);
					writer.WriteUInt64(record.AppliedMove);
					writer.WriteUInt64(record.AppliedJump);
					writer.WriteUInt64(record.AppliedInputTick);
					WriteMotion(writer, record.Motion);
					writer.WriteBool(record.MotionReplyPending);
					writer.WriteBool(record.InputClosed);
				}
				writer.WriteUInt32(static_cast<uint32_t>(state.Peers.size()));
				for (const auto &peer : state.Peers) {
					writer.WriteString(peer.World);
					writer.WriteUInt64(peer.Incarnation);
					writer.WriteUInt64(peer.RetiredThrough);
					writer.WriteUInt32(static_cast<uint32_t>(peer.RetiredIncarnations.size()));
					for (const auto incarnation : peer.RetiredIncarnations)
						writer.WriteUInt64(incarnation);
				}
			}
		}
		void ReadStates(ByteReader &reader, void *values, size_t count) {
			auto *states = static_cast<TransferState *>(values);
			for (size_t index = 0; index < count; ++index) {
				TransferState state;
				size_t remainingBytes = MAXIMUM_BYTES;
				state.Incarnation = reader.ReadUInt64();
				state.NextSequence = reader.ReadUInt64();
				state.Opening = world::Ticket{reader.ReadUInt64()};
				state.Open = reader.ReadBool();
				state.RequirePlayerAdmission = reader.ReadBool();
				const uint32_t outgoing = reader.ReadUInt32();
				if (outgoing > MAXIMUM_RECORDS) {
					reader.Fail();
					return;
				}
				for (uint32_t at = 0; at < outgoing; ++at) {
					Outgoing record;
					record.Animation = ReadAnimationHolds(reader);
					const auto kind = scene::PortalBodyKindOf(Text(reader));
					if (!kind) {
						reader.Fail();
						return;
					}
					record.Receipt.Kind = *kind;
					record.Receipt.Id = ReadId(reader);
					record.Receipt.DestinationWorld = Text(reader);
					const auto stage = reader.ReadUInt8();
					if (stage > static_cast<uint8_t>(PortalTransferStage::Cancelling)) {
						reader.Fail();
						return;
					}
					record.Receipt.Stage = static_cast<PortalTransferStage>(stage);
					record.Receipt.Through = ReadThrough(reader);
					record.Receipt.Diagnostic = Text(reader, true);
					record.Receipt.AcknowledgedInputTick = reader.ReadUInt64();
					record.Receipt.Motion = ReadMotion(reader);
					record.Subject = Entity{reader.ReadUInt64()};
					record.Root = Entity{reader.ReadUInt64()};
					record.Humanoid = Entity{reader.ReadUInt64()};
					reader.ReadRaw(&record.OriginalMotion, sizeof(record.OriginalMotion));
					reader.ReadRaw(&record.OriginalHumanoid, sizeof(record.OriginalHumanoid));
					record.WasSimulated = reader.ReadBool();
					record.HadOwner = reader.ReadBool();
					record.OriginalOwner.Player = Entity{reader.ReadUInt64()};
					record.DestinationIncarnation = reader.ReadUInt64();
					record.RetryAt = reader.ReadUInt64();
					record.AdmittedIncarnation = reader.ReadUInt64();
					record.Ticket = world::Ticket{reader.ReadUInt64()};
					record.Move = ReadMove(reader);
					record.AcknowledgedMove = reader.ReadUInt64();
					record.InputClosed = StrictBool(reader);
					if (record.AcknowledgedMove > record.Move.Sequence ||
						record.Receipt.AcknowledgedInputTick > record.Move.InputTick ||
						(record.Receipt.Motion &&
						 (record.Receipt.Motion->DestinationIncarnation != record.DestinationIncarnation ||
						  record.Receipt.Motion->InputTick > record.Receipt.AcknowledgedInputTick)))
						reader.Fail();
					record.Body = ReadBytes(reader, remainingBytes);
					remainingBytes -= record.Body.size();
					state.Out.push_back(std::move(record));
				}
				const uint32_t incoming = reader.ReadUInt32();
				if (incoming > MAXIMUM_RECORDS) {
					reader.Fail();
					return;
				}
				for (uint32_t at = 0; at < incoming; ++at) {
					Incoming record;
					const auto kind = scene::PortalBodyKindOf(Text(reader));
					if (!kind) {
						reader.Fail();
						return;
					}
					record.Kind = *kind;
					record.Id = ReadId(reader);
					record.Body = ReadBytes(reader, remainingBytes);
					remainingBytes -= record.Body.size();
					record.Through = ReadThrough(reader);
					record.Subject = Entity{reader.ReadUInt64()};
					record.Committed = reader.ReadBool();
					record.Cancelled = reader.ReadBool();
					record.Move = ReadMove(reader);
					record.AppliedMove = reader.ReadUInt64();
					record.AppliedJump = reader.ReadUInt64();
					record.AppliedInputTick = reader.ReadUInt64();
					record.Motion = ReadMotion(reader);
					record.MotionReplyPending = StrictBool(reader);
					record.InputClosed = StrictBool(reader);
					if (record.AppliedMove > record.Move.Sequence ||
						record.AppliedJump > record.Move.JumpSequence ||
						record.AppliedJump > record.AppliedMove ||
						record.AppliedInputTick > record.Move.InputTick ||
						(record.Motion && (record.Motion->DestinationIncarnation != state.Incarnation ||
										   record.Motion->InputTick > record.AppliedInputTick)))
						reader.Fail();
					if (record.Cancelled &&
						(record.Committed || record.Subject != NULL_ENTITY || !record.Body.empty())) {
						reader.Fail();
						return;
					}
					state.In.push_back(std::move(record));
				}
				const uint32_t peers = reader.ReadUInt32();
				if (peers > MAXIMUM_RECORDS) {
					reader.Fail();
					return;
				}
				for (uint32_t at = 0; at < peers; ++at) {
					Peer peer;
					peer.World = Text(reader);
					peer.Incarnation = reader.ReadUInt64();
					peer.RetiredThrough = reader.ReadUInt64();
					const uint32_t retired = reader.ReadUInt32();
					if (retired > MAXIMUM_RECORDS) {
						reader.Fail();
						return;
					}
					for (uint32_t previous = 0; previous < retired; ++previous) {
						const auto incarnation = reader.ReadUInt64();
						if (incarnation == 0 || incarnation == peer.Incarnation ||
							std::ranges::find(peer.RetiredIncarnations, incarnation) !=
								peer.RetiredIncarnations.end()) {
							reader.Fail();
							return;
						}
						peer.RetiredIncarnations.push_back(incarnation);
					}
					state.Peers.push_back(std::move(peer));
				}
				// An unconfigured resource is a valid empty snapshot, but cannot own
				// transfers or an open channel until the host assigns its incarnation.
				const bool unconfigured = state.Incarnation == 0 && state.NextSequence == 1 &&
										  state.Opening.Value == 0 && !state.Open && state.Out.empty() &&
										  state.In.empty() && state.Peers.empty();
				if (reader.Failed() || HeldBytes(state) > MAXIMUM_BYTES ||
					(state.Incarnation == 0 && !unconfigured) || state.NextSequence == 0) {
					reader.Fail();
					return;
				}
				states[index] = std::move(state);
			}
		}
	}

	bool ValidPortalTransferMotion(const PortalTransferMotion &motion) {
		return ValidMotion(motion);
	}
	bool WritePortalTransferMotion(core::ByteWriter &writer, const PortalTransferMotion &motion) {
		if (!ValidMotion(motion)) return false;
		WriteMotion(writer, motion);
		return true;
	}
	bool ReadPortalTransferMotion(core::ByteReader &reader, PortalTransferMotion &motion) {
		const auto decoded = ReadMotion(reader);
		if (reader.Failed() || !decoded) return false;
		motion = *decoded;
		return true;
	}

	void RegisterPortalTransferComponents() {
		// Snapshot restoration needs the contact codec and channel callbacks
		// before a configured world's state is read.
		(void)RegisterPortalContacts();
		ecs::Components::Register<TransferState>("script.PortalTransfers", WriteStates, ReadStates);
		ecs::Components::Register<PlayerInputClock>(
			"script.PortalPlayerInput", WriteInputClocks, ReadInputClocks
		);
	}
	bool ConfigurePortalTransfers(ecs::Store &store, uint64_t incarnation, bool requirePlayerAdmission) {
		if (incarnation == 0 || store.AdoptOnly() || world::Postbox(store).IsReplica()) return false;
		RegisterPortalTransferComponents();
		if (const auto *previous = State(store); previous != nullptr)
			return previous->Incarnation == incarnation &&
				   previous->RequirePlayerAdmission == requirePlayerAdmission &&
				   ConfigurePortalContacts(store, incarnation);
		if (!ConfigurePortalContacts(store, incarnation)) return false;
		TransferState state;
		state.Incarnation = incarnation;
		state.RequirePlayerAdmission = requirePlayerAdmission;
		store.SetResource(state);
		return true;
	}
	uint64_t PortalTransferIncarnation(const ecs::Store &store) {
		const auto *state = State(store);
		return state == nullptr ? 0 : state->Incarnation;
	}
	bool AdmitPortalPlayerTransfer(
		ecs::Store &store, const PortalTransferId &id, uint64_t destinationIncarnation
	) {
		auto *state = State(store);
		if (!state || store.AdoptOnly() || world::Postbox(store).IsReplica() || destinationIncarnation == 0)
			return false;
		for (auto &record : state->Out) {
			if (record.Receipt.Id != id || record.Receipt.Kind != scene::PortalBodyKind::Player ||
				record.Receipt.Stage != PortalTransferStage::Preparing)
				continue;
			if (record.AdmittedIncarnation != 0 && record.AdmittedIncarnation != destinationIncarnation)
				return false;
			record.AdmittedIncarnation = destinationIncarnation;
			record.RetryAt = store.Time().Tick;
			return true;
		}
		return false;
	}
	bool
	CancelPortalPlayerTransfer(ecs::Store &store, const PortalTransferId &id, std::string_view diagnostic) {
		auto *state = State(store);
		if (!state || store.AdoptOnly() || world::Postbox(store).IsReplica() || !ValidText(diagnostic))
			return false;
		for (auto &record : state->Out) {
			if (record.Receipt.Id != id || record.Receipt.Kind != scene::PortalBodyKind::Player) continue;
			if (record.Receipt.Stage == PortalTransferStage::Cancelling) return true;
			if (record.Receipt.Stage != PortalTransferStage::Preparing) return false;
			record.Receipt.Stage = PortalTransferStage::Cancelling;
			record.Receipt.Diagnostic = diagnostic;
			record.Body = std::vector<std::byte>{};
			record.RetryAt = store.Time().Tick;
			return true;
		}
		return false;
	}

	namespace {
		bool BeginTransfer(
			ecs::Store &store,
			Entity subject,
			scene::PortalBodyKind kind,
			std::string_view destination,
			const scene::SeamTransform &through,
			PortalTransferId &out,
			std::string &failure,
			std::optional<scene::PortalBodySweep> sweep
		) {
			ENGINE_PROFILE("begin portal transfer");
			auto *state = State(store);
			if (state == nullptr || !state->Open || store.AdoptOnly() || world::Postbox(store).IsReplica() ||
				!ValidText(destination) || destination == store.Name() || !Similarity(through)) {
				failure = "portal transfer endpoint is not ready or is not authoritative";
				return false;
			}
			if ((state->Out.size() == MAXIMUM_RECORDS &&
				 std::none_of(state->Out.begin(), state->Out.end(), Retired)) ||
				state->NextSequence == std::numeric_limits<uint64_t>::max() ||
				std::count_if(state->Out.begin(), state->Out.end(), Active) >=
					static_cast<std::ptrdiff_t>(MAXIMUM_ACTIVE) ||
				std::any_of(state->Out.begin(), state->Out.end(), [&](const auto &r) {
					return r.Subject == subject && Active(r);
				})) {
				failure = "portal source transfer bound reached";
				return false;
			}
			scene::PortalBodyCopy copy;
			const ecs::ComponentId localComponents[]{ecs::Components::Assigned<PlayerInputClock>()};
			const bool captured =
				kind == scene::PortalBodyKind::Player
					? scene::CapturePortalBody(store, subject, copy, failure, localComponents)
					: scene::CapturePortalObject(store, subject, copy, failure);
			if (!captured) return false;
			copy.Sweep = sweep;
			if (!scene::MapPortalBody(copy, through, failure)) return false;
			ByteWriter writer;
			if (!scene::WritePortalBody(writer, copy) ||
				writer.Size() > MAXIMUM_BYTES - std::min(MAXIMUM_BYTES, HeldBytes(*state))) {
				failure = "portal source byte bound reached";
				return false;
			}
			const auto *rig = kind == scene::PortalBodyKind::Player
								  ? store.Get<scene::Character>(scene::CharacterOf(store, subject))
								  : nullptr;
			const Entity root =
				kind == scene::PortalBodyKind::Object ? subject : (rig ? rig->Root : NULL_ENTITY);
			const Entity humanoid = rig ? rig->Humanoid : NULL_ENTITY;
			if (store.Get<scene::Motion>(root) == nullptr ||
				(kind == scene::PortalBodyKind::Player && store.Get<scene::Humanoid>(humanoid) == nullptr)) {
				failure = "portal body has no moving root";
				return false;
			}
			Outgoing record;
			record.Receipt.Id = {std::string(store.Name()), state->Incarnation, state->NextSequence};
			record.Receipt.DestinationWorld = destination;
			record.Receipt.Through = through;
			record.Receipt.Kind = kind;
			record.Animation = HoldAnimation(store, subject, kind);
			record.Subject = subject;
			record.Root = root;
			record.Humanoid = humanoid;
			record.OriginalMotion = *store.Get<scene::Motion>(record.Root);
			if (humanoid != NULL_ENTITY) record.OriginalHumanoid = *store.Get<scene::Humanoid>(humanoid);
			record.WasSimulated = store.Has<scene::Simulated>(record.Root);
			if (const auto *owner = store.Get<scene::NetworkOwner>(record.Root)) {
				record.HadOwner = true;
				record.OriginalOwner = *owner;
			}
			record.Body.assign(writer.Bytes().begin(), writer.Bytes().end());
			Message offer;
			offer.Id = record.Receipt.Id;
			offer.Body = record.Body;
			offer.Through = through;
			record.Ticket = Send(store, destination, offer);
			if (!record.Ticket.Expected()) {
				failure = "portal offer exceeds the world's bus budget";
				return false;
			}
			record.RetryAt = store.Time().Tick + RETRY_TICKS;
			if (state->Out.size() == MAXIMUM_RECORDS) {
				const auto old = std::find_if(state->Out.begin(), state->Out.end(), Retired);
				state->Out.erase(old);
			}
			out = record.Receipt.Id;
			state->NextSequence++;
			state->Out.push_back(std::move(record));
			const auto &held = state->Out.back();
			// Keep the visible pose and playhead fixed while the copied body waits
			// for admission. A precommit refusal restores these exact rows.
			for (const auto &animation : held.Animation) {
				if (animation.Animator) store.Remove<scene::Animator>(animation.Owner);
				if (animation.Track) store.Remove<scene::AnimationTrack>(animation.Owner);
			}
			store.Remove<scene::Motion>(held.Root);
			store.Remove<scene::Simulated>(held.Root);
			store.Remove<scene::NetworkOwner>(held.Root);
			if (auto *steering = store.GetMutable<scene::Humanoid>(held.Humanoid)) steering->Enabled = false;
			return true;
		}

	}

	bool BeginPortalTransfer(
		ecs::Store &store,
		Entity subject,
		std::string_view destination,
		const scene::SeamTransform &through,
		PortalTransferId &out,
		std::string &failure,
		std::optional<scene::PortalBodySweep> sweep
	) {
		return BeginTransfer(
			store, subject, scene::PortalBodyKind::Player, destination, through, out, failure, sweep
		);
	}

	bool BeginPortalObjectTransfer(
		ecs::Store &store,
		Entity subject,
		std::string_view destination,
		const scene::SeamTransform &through,
		PortalTransferId &out,
		std::string &failure,
		std::optional<scene::PortalBodySweep> sweep
	) {
		return BeginTransfer(
			store, subject, scene::PortalBodyKind::Object, destination, through, out, failure, sweep
		);
	}

	void PumpPortalTransfers(ecs::Store &store) {
		ENGINE_PROFILE("pump portal transfers");
		auto *state = State(store);
		if (state == nullptr || store.AdoptOnly() || world::Postbox(store).IsReplica()) return;
		if (!state->Open && !state->Opening.Expected())
			state->Opening = world::Postbox(store).OpenChannel(CHANNEL);
		std::vector<world::Delivery> deliveries;
		if (auto *inbox = store.ResourceMutable<world::Inbox>()) {
			for (auto it = inbox->Arrived.begin(); it != inbox->Arrived.end();) {
				bool owned =
					it->Bus == world::BusKind::Channel && it->Key.Text() == CHANNEL && !it->Reply.Expected();
				owned |= it->Reply.Expected() &&
						 (it->Reply == state->Opening ||
						  std::any_of(state->Out.begin(), state->Out.end(), [&](const auto &r) {
							  return r.Ticket == it->Reply;
						  }));
				if (owned) {
					deliveries.push_back(std::move(*it));
					it = inbox->Arrived.erase(it);
				} else
					++it;
			}
		}
		for (const auto &delivery : deliveries) {
			if (delivery.Reply.Expected()) {
				if (delivery.Reply == state->Opening) {
					state->Open = delivery.Status == world::BusStatus::Ok;
					state->Opening = {};
					continue;
				}
				if (delivery.Status != world::BusStatus::Ok)
					for (auto &record : state->Out)
						if (record.Ticket == delivery.Reply)
							if (record.Receipt.Stage != PortalTransferStage::Cancelling)
								Restore(store, record, std::string(world::Describe(delivery.Status)));
				continue;
			}
			Message message;
			core::Metrics::Count("world.portal.received.messages", 1);
			core::Metrics::Count("world.portal.received.bytes", delivery.Payload.size());
			if (Decode(delivery.Payload, message)) Process(store, *state, delivery.From.Text(), message);
		}
		for (auto &record : state->Out) {
			if (!Active(record) || store.Time().Tick < record.RetryAt) continue;
			Message message;
			message.Id = record.Receipt.Id;
			message.Through = record.Receipt.Through;
			message.DestinationIncarnation = record.DestinationIncarnation;
			message.Kind =
				record.Receipt.Stage == PortalTransferStage::Cancelling
					? MessageKind::Cancel
					: (record.Receipt.Stage == PortalTransferStage::Preparing ? MessageKind::Offer
																			  : MessageKind::Commit);
			if (message.Kind == MessageKind::Offer) message.Body = record.Body;
			const auto sent = Send(store, record.Receipt.DestinationWorld, message);
			if (sent.Expected()) {
				record.Ticket = sent;
				record.RetryAt = store.Time().Tick + RETRY_TICKS;
			}
		}
		for (const auto &record : state->Out) {
			if (record.InputClosed || record.Move.Sequence <= record.AcknowledgedMove ||
				record.DestinationIncarnation == 0 ||
				(record.Receipt.Stage != PortalTransferStage::Committing &&
				 record.Receipt.Stage != PortalTransferStage::Committed))
				continue;
			Message message;
			message.Kind = MessageKind::Move;
			message.Id = record.Receipt.Id;
			message.DestinationIncarnation = record.DestinationIncarnation;
			ByteWriter writer;
			WriteMove(writer, record.Move);
			message.Body.assign(writer.Bytes().begin(), writer.Bytes().end());
			Send(store, record.Receipt.DestinationWorld, message);
		}
	}
	void RegisterPortalTransferSystems(ecs::Scheduler &scheduler) {
		RegisterPortalTransferComponents();
		if (!scheduler.HasSystem("portal.transfer.pump", ecs::Phase::PreSimulation))
			scheduler.Add("portal.transfer.pump", ecs::Phase::PreSimulation, [](ecs::Store &store) {
				PumpPortalTransfers(store);
				PumpNativeMoves(store);
			});
		if (!scheduler.HasSystem("portal.transfer.motion", ecs::Phase::Replication))
			scheduler.Add("portal.transfer.motion", ecs::Phase::Replication, [](ecs::Store &store) {
				auto *state = State(store);
				if (!state || store.AdoptOnly() || world::Postbox(store).IsReplica()) return;
				ENGINE_PROFILE("portal completed motion");
				for (auto &record : state->In) {
					if (!record.Committed || record.InputClosed || record.AppliedMove == 0) continue;
					const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, record.Subject));
					if (!rig) continue;
					const auto *root = store.Get<scene::Transform>(rig->Root);
					const auto *velocity = store.Get<scene::Motion>(rig->Root);
					const auto *humanoid = store.Get<scene::Humanoid>(rig->Humanoid);
					if (!root || !velocity || !humanoid) continue;
					PortalTransferMotion motion{
						state->Incarnation,
						store.Time().Tick,
						record.AppliedInputTick,
						root->Frame,
						velocity->Linear,
						velocity->Angular,
						humanoid->WalkSpeed,
						humanoid->JumpSpeed,
						humanoid->Grounded,
						store.Time().Elapsed
					};
					if (!ValidMotion(motion)) continue;
					if (!record.MotionReplyPending && record.Motion && SameMotion(*record.Motion, motion))
						continue;
					record.Motion = motion;
					record.MotionReplyPending = false;
					AcknowledgeMove(store, *state, record);
					core::Metrics::Count("world.portal.motion.samples", 1);
				}
			});
		if (!scheduler.HasSystem("portal.transfer.cross", ecs::Phase::PostSimulation))
			scheduler.Add(
				"portal.transfer.cross",
				ecs::Phase::PostSimulation,
				[](ecs::Store &store) {
					auto *state = State(store);
					if (state == nullptr || !state->Open || store.AdoptOnly() ||
						world::Postbox(store).IsReplica())
						return;
					std::vector<scene::PortalSeam> seams;
					scene::GatherPortalSeams(store, seams);
					if (seams.empty()) return;
					struct Crossing {
						Entity Subject;
						scene::PortalBodyKind Kind;
						std::string Destination;
						scene::SeamTransform Through;
						scene::PortalBodySweep Sweep;
					};
					std::vector<Crossing> crossings;
					const auto gather = [&](Entity subject, Entity root, scene::PortalBodyKind kind) {
						const auto *before = store.Get<scene::PreviousTransform>(root);
						const auto *now = store.Get<scene::Transform>(root);
						const auto *motion = store.Get<scene::Motion>(root);
						if (subject == NULL_ENTITY || before == nullptr || now == nullptr ||
							motion == nullptr)
							return;
						scene::PortalHop hop;
						size_t index = 0;
						if (!scene::NearestPortalCrossing(
								seams, before->Frame.Position, now->Frame.Position, true, hop, index
							) ||
							!seams[index].Crosses)
							return;
						const float remaining = store.Time().Delta * (1 - hop.Share);
						auto start = physics::Advanced(now->Frame, {}, -motion->Angular, remaining);
						start.Position = before->Frame.Position +
										 (now->Frame.Position - before->Frame.Position) * hop.Share;
						crossings.push_back(
							{subject,
							 kind,
							 std::string(seams[index].DestinationWorld.Text()),
							 hop.Through,
							 {start, now->Frame.Position - start.Position, motion->Angular * remaining}}
						);
					};
					std::vector<Entity> characterRoots;
					store.Each<const scene::Character>([&](Entity, const scene::Character &rig) {
						characterRoots.push_back(rig.Root);
						gather(rig.Owner, rig.Root, scene::PortalBodyKind::Player);
					});
					std::sort(characterRoots.begin(), characterRoots.end(), [](Entity left, Entity right) {
						return left.Id < right.Id;
					});
					store.Each<const scene::Motion>([&](Entity object, const scene::Motion &) {
						if (std::binary_search(
								characterRoots.begin(),
								characterRoots.end(),
								object,
								[](Entity left, Entity right) { return left.Id < right.Id; }
							) ||
							store.Has<scene::CharacterLimb>(object) || store.Has<scene::Humanoid>(object))
							return;
						gather(object, object, scene::PortalBodyKind::Object);
					});
					for (const auto &crossing : crossings) {
						PortalTransferId id;
						std::string failure;
						if (!BeginTransfer(
								store,
								crossing.Subject,
								crossing.Kind,
								crossing.Destination,
								crossing.Through,
								id,
								failure,
								crossing.Sweep
							))
							ENGINE_WARN("portal crossing refused: {}", failure);
					}
				},
				ecs::SystemOrder{{}, {}, {"character.portal"}, {"physics.contacts"}}
			);
	}
	std::vector<PortalTransferReceipt> PortalTransferReceipts(const ecs::Store &store) {
		std::vector<PortalTransferReceipt> receipts;
		if (const auto *state = State(store))
			for (const auto &record : state->Out)
				receipts.push_back(record.Receipt);
		return receipts;
	}
	std::optional<PortalTransferReceipt>
	PortalTransferOfPlayer(const ecs::Store &store, Entity sourcePlayer) {
		if (const auto *state = State(store))
			for (auto it = state->Out.rbegin(); it != state->Out.rend(); ++it)
				if (it->Receipt.Kind == scene::PortalBodyKind::Player && it->Subject == sourcePlayer)
					return it->Receipt;
		return std::nullopt;
	}
	Entity PortalTransferPlayer(const ecs::Store &store, const PortalTransferId &id) {
		if (const auto *state = State(store))
			for (const auto &record : state->In)
				if (record.Kind == scene::PortalBodyKind::Player && record.Id == id && record.Committed &&
					store.Alive(record.Subject))
					return record.Subject;
		return NULL_ENTITY;
	}
	bool ForwardPortalPlayerMove(
		ecs::Store &store,
		Entity sourcePlayer,
		const core::Vector3 &direction,
		bool jump,
		uint64_t inputTick,
		double stepSeconds
	) {
		if (store.AdoptOnly() || world::Postbox(store).IsReplica() || !ValidDirection(direction) ||
			!std::isfinite(stepSeconds) || stepSeconds < 0)
			return false;
		auto *state = State(store);
		if (!state) return false;
		for (auto it = state->Out.rbegin(); it != state->Out.rend(); ++it) {
			if (it->Subject != sourcePlayer || it->Receipt.Kind != scene::PortalBodyKind::Player) continue;
			if (it->InputClosed || (it->Receipt.Stage != PortalTransferStage::Preparing &&
									it->Receipt.Stage != PortalTransferStage::Committing &&
									it->Receipt.Stage != PortalTransferStage::Committed))
				return false;
			if (inputTick != 0 && inputTick <= it->Move.InputTick) return true;
			if (it->Move.Sequence != 0 && it->Move.Direction == direction && !jump && inputTick == 0)
				return true;
			if (it->Move.Sequence == std::numeric_limits<uint64_t>::max()) return false;
			++it->Move.Sequence;
			it->Move.JumpSequence += jump;
			it->Move.Direction = direction;
			it->Move.InputTick = std::max(it->Move.InputTick, inputTick);
			it->Move.StepSeconds = stepSeconds;
			return true;
		}
		return false;
	}
	PortalInputDisposition SchedulePortalPlayerMove(
		ecs::Store &store,
		Entity player,
		const core::Vector3 &direction,
		bool jump,
		uint64_t inputTick,
		double stepSeconds
	) {
		using Result = PortalInputDisposition;
		if (store.AdoptOnly() || !ValidDirection(direction) || !std::isfinite(stepSeconds) || stepSeconds < 0)
			return Result::Refused;
		if (!ecs::Components::Assigned<PlayerInputClock>().IsValid()) return Result::Immediate;
		auto *clock = store.GetMutable<PlayerInputClock>(player);
		if (!clock) return Result::Immediate;
		const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
		if (!rig || clock->Root != rig->Root || clock->Humanoid != rig->Humanoid || inputTick == 0) {
			*clock = {};
			return Result::Immediate;
		}
		if (clock->AnchorInputTick == 0 || clock->InputStep <= 0 || clock->WorldStep <= 0)
			return Result::Immediate;
		auto refuse = [] {
			core::Metrics::Count("world.portal.native.refused", 1);
			return Result::Refused;
		};
		if (stepSeconds != clock->InputStep || store.Time().Delta != clock->WorldStep) return refuse();
		if (inputTick <= clock->LastQueuedInputTick) return clock->Native ? Result::Queued : refuse();
		const auto now = store.Time().Tick;
		if (now == std::numeric_limits<uint64_t>::max()) return refuse();
		// Subtract integer epochs before converting, preserving precision on long sessions.
		const long double offset = std::ceil(
			static_cast<long double>(inputTick - clock->AnchorInputTick) * clock->InputStep /
				clock->WorldStep -
			1e-6L
		);
		if (!std::isfinite(offset) || offset < 0 ||
			offset >= static_cast<long double>(std::numeric_limits<uint64_t>::max() - clock->AnchorWorldTick))
			return refuse();
		const uint64_t due = std::max(now + 1, clock->AnchorWorldTick + static_cast<uint64_t>(offset));
		// A corrupt or incompatible input clock cannot reserve an unbounded future.
		if (static_cast<long double>(due - now) * clock->WorldStep >
			std::max(1.0L, static_cast<long double>(clock->WorldStep)) + 1e-6L)
			return refuse();
		if (clock->Count != 0 &&
			clock->Pending[(clock->Begin + clock->Count - 1) % MAXIMUM_NATIVE_INPUTS].PhysicsTick == due) {
			auto &pending = clock->Pending[(clock->Begin + clock->Count - 1) % MAXIMUM_NATIVE_INPUTS];
			pending.InputTick = inputTick;
			pending.Direction = direction;
			pending.Jump |= jump;
			core::Metrics::Count("world.portal.native.coalesced", 1);
		} else {
			if (clock->Count == MAXIMUM_NATIVE_INPUTS) return refuse();
			clock->Pending[(clock->Begin + clock->Count) % MAXIMUM_NATIVE_INPUTS] = {
				inputTick, due, direction, jump
			};
			++clock->Count;
		}
		clock->LastQueuedInputTick = inputTick;
		clock->Native = true;
		ClosePortalPlayerMoveForwarding(store, player);
		core::Metrics::Count("world.portal.native.queued", 1);
		return Result::Queued;
	}

	std::optional<uint64_t> AppliedPortalPlayerInput(const ecs::Store &store, Entity player) {
		if (!ecs::Components::Assigned<PlayerInputClock>().IsValid()) return {};
		const auto *clock = store.Get<PlayerInputClock>(player);
		if (!clock || clock->AppliedInputTick == 0) return {};
		const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
		if (!rig || rig->Root != clock->Root || rig->Humanoid != clock->Humanoid) return {};
		return clock->AppliedInputTick;
	}

	void ClosePortalPlayerMoveForwarding(ecs::Store &store, Entity destinationPlayer) {
		if (store.AdoptOnly() || world::Postbox(store).IsReplica()) return;
		auto *state = State(store);
		if (!state) return;
		for (auto &record : state->In) {
			if (record.Subject != destinationPlayer || !record.Committed || record.InputClosed ||
				record.Kind != scene::PortalBodyKind::Player)
				continue;
			record.InputClosed = true;
			AcknowledgeMove(store, *state, record);
		}
	}
	std::optional<PortalTransferReceipt>
	PortalTransferOfObject(const ecs::Store &store, Entity sourceObject) {
		if (const auto *state = State(store))
			for (auto it = state->Out.rbegin(); it != state->Out.rend(); ++it)
				if (it->Receipt.Kind == scene::PortalBodyKind::Object && it->Subject == sourceObject)
					return it->Receipt;
		return std::nullopt;
	}
	Entity PortalTransferObject(const ecs::Store &store, const PortalTransferId &id) {
		if (const auto *state = State(store))
			for (const auto &record : state->In)
				if (record.Kind == scene::PortalBodyKind::Object && record.Id == id && record.Committed &&
					store.Alive(record.Subject))
					return record.Subject;
		return NULL_ENTITY;
	}

}
