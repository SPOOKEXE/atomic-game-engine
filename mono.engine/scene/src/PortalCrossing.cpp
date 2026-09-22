#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/PortalCrossing.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

namespace engine::scene {
	namespace {
		uint64_t NamespaceOf(const ecs::Store &store) {
			// FNV-1a is only a deterministic namespace derivation for an authority
			// that was not explicitly configured. Sequence supplies uniqueness.
			uint64_t value = 1469598103934665603ull;
			for (const unsigned char character : store.Name()) {
				value ^= character;
				value *= 1099511628211ull;
			}
			return value == 0 ? 1 : value;
		}

		BodyIdentityAuthority *Authority(ecs::Store &store) {
			if (auto *authority = store.ResourceMutable<BodyIdentityAuthority>()) return authority;
			store.SetResource(BodyIdentityAuthority{});
			return store.ResourceMutable<BodyIdentityAuthority>();
		}

		uint64_t TopologyOf(const PortalSeam &seam) {
			auto mix = [](uint64_t state, uint64_t value) {
				state ^= value;
				return state * 1099511628211ull;
			};
			uint64_t value = 1469598103934665603ull;
			value = mix(value, seam.Pane.Id);
			value = mix(value, seam.Far.Id);
			for (const float coordinate :
				 {seam.Centre.X,
				  seam.Centre.Y,
				  seam.Centre.Z,
				  seam.Normal.X,
				  seam.Normal.Y,
				  seam.Normal.Z,
				  seam.First.X,
				  seam.First.Y,
				  seam.First.Z,
				  seam.Second.X,
				  seam.Second.Y,
				  seam.Second.Z,
				  seam.Scale})
				value = mix(value, std::bit_cast<uint32_t>(coordinate));
			return value == 0 ? 1 : value;
		}

		PortalSeamPin PinOf(const PortalSeam &seam) {
			PortalSeamPin pin;
			pin.CentreFrame = core::CFrame(seam.Centre);
			pin.Destination = seam.Destination;
			pin.Normal = seam.Normal;
			pin.First = seam.First;
			pin.Second = seam.Second;
			pin.Pane = seam.Pane;
			pin.Far = seam.Far;
			pin.DestinationWorld = seam.DestinationWorld;
			pin.Scale = seam.Scale;
			pin.TagFilter = seam.TagFilter;
			pin.Surface = seam.Surface;
			pin.Crosses = seam.Crosses;
			pin.Bidirectional = seam.Bidirectional;
			return pin;
		}

		PortalSeam SeamOf(const PortalSeamPin &pin) {
			PortalSeam seam;
			seam.Centre = pin.CentreFrame.Position;
			seam.Normal = pin.Normal;
			seam.First = pin.First;
			seam.Second = pin.Second;
			seam.Destination = pin.Destination;
			seam.Pane = pin.Pane;
			seam.Far = pin.Far;
			seam.DestinationWorld = pin.DestinationWorld;
			seam.Scale = pin.Scale;
			seam.TagFilter = pin.TagFilter;
			seam.Surface = pin.Surface;
			seam.Crosses = pin.Crosses;
			seam.Bidirectional = pin.Bidirectional;
			return seam;
		}

		PortalSeam LivePinnedSeam(const PortalSeamPin &pin, const PortalSeam &live) {
			PortalSeam seam = live;
			// Endpoint poses are sampled every tick, but the route and scale are
			// the decision the body entered under. Changing either during overlap
			// would map one physical body through two incompatible seams.
			seam.Scale = pin.Scale;
			seam.DestinationWorld = pin.DestinationWorld;
			seam.TagFilter = pin.TagFilter;
			seam.Surface = pin.Surface;
			seam.Crosses = pin.Crosses;
			seam.Bidirectional = pin.Bidirectional;
			return seam;
		}

		int8_t SideOf(float offset, int8_t stable) {
			if (offset > PORTAL_CROSSING_HYSTERESIS) return 1;
			if (offset < -PORTAL_CROSSING_HYSTERESIS) return -1;
			return stable;
		}

		const PortalSeam *FindActive(std::span<const PortalSeam> seams, const PortalCrossingState &crossing) {
			for (const PortalSeam &seam : seams)
				if (seam.Pane == crossing.Pane && seam.Far == crossing.Far) return &seam;
			return nullptr;
		}

		void Clear(PortalCrossingState &crossing) {
			crossing.Pane = ecs::NULL_ENTITY;
			crossing.Far = ecs::NULL_ENTITY;
			crossing.Pin = {};
			crossing.TopologyRevision = 0;
			crossing.Phase = PortalCrossingPhase::Idle;
			crossing.PresentationRevision++;
		}
	}

	bool ConfigureBodyIdentityAuthority(ecs::Store &store, uint64_t nameSpace, uint64_t nextSequence) {
		if (nameSpace == 0 || nextSequence == 0) return false;
		auto *authority = Authority(store);
		if (authority == nullptr) return false;
		if (authority->Namespace == nameSpace) {
			authority->NextSequence = std::max(authority->NextSequence, nextSequence);
			return true;
		}
		if (authority->Namespace != 0 && authority->Namespace != nameSpace && authority->NextSequence != 1)
			return false;
		authority->Namespace = nameSpace;
		authority->NextSequence = nextSequence;
		return true;
	}

	BodyIdentity MintBodyIdentity(ecs::Store &store) {
		auto *authority = Authority(store);
		if (authority == nullptr) return {};
		if (authority->Namespace == 0) authority->Namespace = NamespaceOf(store);
		if (authority->NextSequence == 0 || authority->NextSequence == std::numeric_limits<uint64_t>::max())
			return {};
		return BodyIdentity{BodyKey{authority->Namespace, authority->NextSequence++}, 1};
	}

	bool AssignBodyIdentity(ecs::Store &store, ecs::Entity body, const BodyIdentity &identity) {
		if (!store.Alive(body) || !identity.Key.IsValid() || identity.Generation == 0) return false;
		bool collision = false;
		store.Each<const BodyIdentity>([&](ecs::Entity other, const BodyIdentity &live) {
			collision = collision || (other != body && live.Key == identity.Key);
		});
		if (collision) return false;
		store.Set(body, identity);
		return true;
	}

	bool EnsureBodyIdentity(ecs::Store &store, ecs::Entity body, BodyIdentity &out) {
		if (!store.Alive(body) || store.AdoptOnly()) return false;
		if (const BodyIdentity *identity = store.Get<BodyIdentity>(body)) {
			bool collision = !identity->Key.IsValid() || identity->Generation == 0;
			store.Each<const BodyIdentity>([&](ecs::Entity other, const BodyIdentity &live) {
				collision = collision || (other != body && live.Key == identity->Key);
			});
			if (!collision) {
				out = *identity;
				return true;
			}
		}
		out = MintBodyIdentity(store);
		return AssignBodyIdentity(store, body, out);
	}

	float PortalBodyReach(const ecs::Store &store, ecs::Entity body, ecs::Entity reference) {
		if (!store.Alive(body)) return 0.0f;
		if (reference == ecs::NULL_ENTITY) reference = body;
		const Transform *anchor = store.Get<Transform>(reference);
		if (anchor == nullptr) return 0.0f;
		float reach = 0.0f;
		std::vector<ecs::Entity> pending{body};
		for (size_t index = 0; index < pending.size(); ++index) {
			const ecs::Entity entity = pending[index];
			if (const Transform *transform = store.Get<Transform>(entity)) {
				const Bounds *bounds = store.Get<Bounds>(entity);
				const float extent = bounds ? bounds->HalfExtent.Magnitude() : 0.0f;
				reach = std::max(
					reach, (transform->Frame.Position - anchor->Frame.Position).Magnitude() + extent
				);
			}
			store.EachChild(entity, [&](ecs::Entity child) { pending.push_back(child); });
		}
		return reach;
	}

	bool AdvancePortalCrossing(
		ecs::Store &store, ecs::Entity body, std::span<const PortalSeam> seams, float reach
	) {
		PortalCrossingState *crossing = store.GetMutable<PortalCrossingState>(body);
		if (crossing == nullptr || !(reach > 0.0f) || !std::isfinite(reach)) return false;
		const ecs::Entity reference = crossing->Reference == ecs::NULL_ENTITY ? body : crossing->Reference;
		const Transform *transform = store.Get<Transform>(reference);
		if (transform == nullptr) return false;

		const PortalSeam *current =
			crossing->Phase == PortalCrossingPhase::Idle ? nullptr : FindActive(seams, *crossing);
		PortalSeam pinned;
		const PortalSeam *seam = nullptr;
		if (crossing->Phase != PortalCrossingPhase::Idle) {
			pinned = current != nullptr ? LivePinnedSeam(crossing->Pin, *current) : SeamOf(crossing->Pin);
			seam = &pinned;
		}
		if (seam == nullptr) {
			for (const PortalSeam &candidate : seams) {
				if (!SeamStraddled(candidate, transform->Frame.Position, reach)) continue;
				crossing->Pane = candidate.Pane;
				crossing->Far = candidate.Far;
				crossing->TopologyRevision = TopologyOf(candidate);
				crossing->Pin = PinOf(candidate);
				crossing->StableSide =
					SideOf(SeamOffset(candidate, transform->Frame.Position), crossing->StableSide);
				crossing->ReferenceSide = crossing->StableSide;
				crossing->Phase = PortalCrossingPhase::Overlapping;
				crossing->PresentationRevision++;
				seam = &candidate;
				break;
			}
		}
		if (seam == nullptr) return false;

		const bool occupied = SeamStraddled(*seam, transform->Frame.Position, reach);
		const int8_t side = SideOf(SeamOffset(*seam, transform->Frame.Position), crossing->ReferenceSide);
		crossing->ReferenceSide = side;
		if (!occupied) {
			if (crossing->Phase == PortalCrossingPhase::Prepared)
				crossing->ReferenceSide = crossing->StableSide;
			Clear(*crossing);
			return true;
		}
		if (side != crossing->StableSide && crossing->Phase == PortalCrossingPhase::Overlapping) {
			crossing->Phase = PortalCrossingPhase::Prepared;
			crossing->PresentationRevision++;
		} else if (side == crossing->StableSide && crossing->Phase == PortalCrossingPhase::Prepared) {
			crossing->Phase = PortalCrossingPhase::Overlapping;
			crossing->PresentationRevision++;
		}
		return true;
	}

	bool PinnedPortalSeam(const PortalCrossingState &crossing, PortalSeam &out) {
		if (crossing.Phase == PortalCrossingPhase::Idle || crossing.Pin.Pane == ecs::NULL_ENTITY ||
			crossing.Pin.Far == ecs::NULL_ENTITY)
			return false;
		out = SeamOf(crossing.Pin);
		return true;
	}

	bool PinnedPortalSeam(
		const PortalCrossingState &crossing, std::span<const PortalSeam> seams, PortalSeam &out
	) {
		if (!PinnedPortalSeam(crossing, out)) return false;
		if (const PortalSeam *live = FindActive(seams, crossing)) out = LivePinnedSeam(crossing.Pin, *live);
		return true;
	}

	bool CommitPortalCrossing(ecs::Store &store, ecs::Entity body, uint64_t authorityEpoch) {
		PortalCrossingState *crossing = store.GetMutable<PortalCrossingState>(body);
		if (crossing == nullptr || crossing->Phase != PortalCrossingPhase::Prepared || authorityEpoch == 0)
			return false;
		crossing->StableSide = crossing->ReferenceSide;
		crossing->AuthorityEpoch = authorityEpoch;
		crossing->Phase = PortalCrossingPhase::Committed;
		crossing->PresentationRevision++;
		return true;
	}

	bool CancelPortalCrossing(ecs::Store &store, ecs::Entity body) {
		PortalCrossingState *crossing = store.GetMutable<PortalCrossingState>(body);
		if (crossing == nullptr || crossing->Phase == PortalCrossingPhase::Committed) return false;
		crossing->ReferenceSide = crossing->StableSide;
		crossing->Phase =
			crossing->Pane == ecs::NULL_ENTITY ? PortalCrossingPhase::Idle : PortalCrossingPhase::Overlapping;
		crossing->PresentationRevision++;
		return true;
	}

	void WriteBodyIdentities(core::ByteWriter &writer, const void *source, size_t count) {
		const auto *identities = static_cast<const BodyIdentity *>(source);
		for (size_t index = 0; index < count; ++index) {
			writer.WriteUInt64(identities[index].Key.High);
			writer.WriteUInt64(identities[index].Key.Low);
			writer.WriteUInt64(identities[index].Generation);
		}
	}

	void ReadBodyIdentities(core::ByteReader &reader, void *destination, size_t count) {
		auto *identities = static_cast<BodyIdentity *>(destination);
		for (size_t index = 0; index < count; ++index) {
			identities[index].Key.High = reader.ReadUInt64();
			identities[index].Key.Low = reader.ReadUInt64();
			identities[index].Generation = reader.ReadUInt64();
		}
	}
}
