#include <engine/ecs/Store.hpp>
#include <engine/game/PortalIslandScene.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/PortalCrossing.hpp>
#include <engine/scene/SurfaceTable.hpp>

#include <algorithm>
#include <cmath>

namespace engine::game {
	void CollectPortalIslandBodies(ecs::Store &store, std::vector<physics::PortalIslandBody> &out) {
		out.clear();
		store.Each<
			const scene::BodyIdentity,
			const scene::PortalCrossingState,
			const scene::Transform,
			const scene::Motion,
			const scene::RigidBody,
			const scene::Collider>([&](ecs::Entity,
									   const auto &identity,
									   const auto &crossing,
									   const auto &transform,
									   const auto &motion,
									   const auto &rigid,
									   const auto &collider) {
			if (crossing.Phase == scene::PortalCrossingPhase::Idle ||
				rigid.Kind != scene::BodyKind::Dynamic || !(rigid.Mass > 0.0f))
				return;
			physics::CopiedDynamicContact body;
			body.Identity = identity;
			body.Frame = transform.Frame;
			body.Motion = motion;
			body.Extent = collider.Extent;
			body.Mass = rigid.Mass;
			body.Kind = collider.Shape;
			out.push_back(physics::MakePortalIslandBody(body));
		});
		std::sort(out.begin(), out.end(), [](const auto &left, const auto &right) {
			return left.Id < right.Id;
		});
	}

	void CollectPortalIslandContacts(ecs::Store &store, physics::PortalIslandPacket &packet) {
		const auto *surfaces = store.Resource<scene::SurfaceTable>();
		store.Each<
			const scene::BodyIdentity,
			const scene::PortalCrossingState,
			const scene::Transform,
			const scene::Motion,
			const scene::RigidBody,
			const scene::Collider>([&](ecs::Entity root,
									   const auto &identity,
									   const auto &crossing,
									   const auto &transform,
									   const auto &motion,
									   const auto &rigid,
									   const auto &collider) {
			if (crossing.Phase == scene::PortalCrossingPhase::Idle ||
				rigid.Kind != scene::BodyKind::Dynamic || !(rigid.Mass > 0.0f))
				return;
			for (const physics::CopiedDynamicContact &far : physics::CopiedDynamicContactsFor(store, root)) {
				physics::CopiedDynamicContact near;
				near.Identity = identity;
				near.Frame = transform.Frame;
				near.Motion = motion;
				near.Extent = collider.Extent;
				near.Mass = rigid.Mass;
				scene::SurfaceProperties material;
				if (const auto *surface = store.Get<scene::Surface>(root); surface && surfaces) {
					if (const auto *found = surfaces->Find(surface->Material)) material = *found;
				}
				if (const auto *override = store.Get<scene::PhysicsProperties>(root);
					override && override->Custom) {
					material.Friction = override->Friction;
					material.Restitution = override->Elasticity;
				}
				near.Friction = material.Friction;
				near.Restitution = material.Restitution;
				near.Kind = collider.Shape;
				// The local row is never sent through the contact codec. The far-side
				// aperture is still a valid finite witness for Append's value checks.
				near.Window = far.Window;
				(void)AppendPortalIslandContact(near, far, packet);
			}
		});
	}

	bool ApplyPortalIslandResults(ecs::Store &store, std::span<const physics::PortalIslandResult> results) {
		bool valid = true;
		for (const auto &result : results) {
			bool applied = false;
			store.Each<const scene::BodyIdentity, scene::Motion>(
				[&](ecs::Entity, const auto &identity, auto &motion) {
					if (identity.Key.High != result.Id.KeyHigh || identity.Key.Low != result.Id.KeyLow ||
						identity.Generation != result.Id.Generation)
						return;
					motion.Linear = result.LinearVelocity;
					motion.Angular = result.AngularVelocity;
					applied = true;
				}
			);
			valid = valid && applied;
		}
		return valid;
	}

	bool AppendPortalIslandContact(
		const physics::CopiedDynamicContact &first,
		const physics::CopiedDynamicContact &second,
		physics::PortalIslandPacket &packet
	) {
		if (!physics::ValidContactWindow(first.Window) || !physics::ValidContactWindow(second.Window) ||
			(first.Identity.Key == second.Identity.Key &&
			 first.Identity.Generation == second.Identity.Generation) ||
			!(first.Mass > 0) || !(second.Mass > 0))
			return false;
		const auto identifier = [](const scene::BodyIdentity &identity) {
			return physics::PortalIslandBodyId{identity.Key.High, identity.Key.Low, identity.Generation, 0};
		};
		const auto add = [&](const physics::CopiedDynamicContact &body, bool owned) {
			const physics::PortalIslandBodyId id = identifier(body.Identity);
			if (std::any_of(packet.Bodies.begin(), packet.Bodies.end(), [&](const auto &existing) {
					return existing.Id == id;
				}))
				return;
			physics::PortalIslandBody copied = physics::MakePortalIslandBody(body);
			copied.Owned = owned;
			packet.Bodies.push_back(std::move(copied));
		};
		add(first, true);
		add(second, false);
		const core::Vector3 separation = second.Frame.Position - first.Frame.Position;
		const core::Vector3 overlap =
			first.Extent + second.Extent -
			core::Vector3{std::abs(separation.X), std::abs(separation.Y), std::abs(separation.Z)};
		if (!(overlap.X > 0 && overlap.Y > 0 && overlap.Z > 0)) return false;
		core::Vector3 normal{separation.X < 0 ? -1.0f : 1.0f, 0, 0};
		float penetration = overlap.X;
		if (overlap.Y < penetration) {
			normal = {0, separation.Y < 0 ? -1.0f : 1.0f, 0};
			penetration = overlap.Y;
		}
		if (overlap.Z < penetration) {
			normal = {0, 0, separation.Z < 0 ? -1.0f : 1.0f};
			penetration = overlap.Z;
		}
		packet.Contacts.push_back(
			{identifier(first.Identity),
			 identifier(second.Identity),
			 first.Frame.Position + separation * 0.5f,
			 normal,
			 penetration,
			 std::sqrt(first.Friction * second.Friction),
			 std::max(first.Restitution, second.Restitution)}
		);
		return true;
	}
}
