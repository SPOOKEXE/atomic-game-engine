#pragma once

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace skinning_fixture {
	using engine::core::CFrame;
	using engine::core::Name;
	using engine::core::Vector3;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::scene::Bone;

	inline void Require(bool condition, const char *reason) {
		if (!condition) throw std::runtime_error(reason);
	}

	// Half-turns have literal unit quaternions and signed diagonal matrices.
	// Expected affine arithmetic never calls the production frame composition.
	struct Affine {
		std::array<float, 3> Position{};
		std::array<float, 3> Signs{1, 1, 1};
	};
	inline Affine Product(const Affine &parent, const Affine &local) {
		Affine result;
		for (size_t axis = 0; axis < 3; ++axis) {
			result.Position[axis] = parent.Position[axis] + parent.Signs[axis] * local.Position[axis];
			result.Signs[axis] = parent.Signs[axis] * local.Signs[axis];
		}
		return result;
	}
	inline Affine Authored(size_t seed, float offset) {
		Affine result{{offset, static_cast<float>(seed % 5) * .125f, -.25f}};
		if (seed % 3 == 0)
			result.Signs = {-1, -1, 1};
		else if (seed % 3 == 1)
			result.Signs = {1, -1, -1};
		return result;
	}
	inline CFrame Frame(const Affine &affine) {
		CFrame result(Vector3(affine.Position[0], affine.Position[1], affine.Position[2]));
		if (affine.Signs[0] == -1 && affine.Signs[1] == -1) {
			result.QuaternionW = 0;
			result.QuaternionZ = 1;
		} else if (affine.Signs[1] == -1 && affine.Signs[2] == -1) {
			result.QuaternionW = 0;
			result.QuaternionX = 1;
		} else if (affine.Signs[0] == -1 && affine.Signs[2] == -1) {
			result.QuaternionW = 0;
			result.QuaternionY = 1;
		}
		return result;
	}
	inline void VerifyFrame(const CFrame &frame, const Affine &expected) {
		const std::array<Vector3, 4> points{Vector3{}, Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)};
		for (size_t point = 0; point < points.size(); ++point) {
			const auto actual = frame.PointToWorldSpace(points[point]);
			const std::array<float, 3> coordinates{actual.X, actual.Y, actual.Z};
			for (size_t axis = 0; axis < 3; ++axis)
				Require(
					std::isfinite(coordinates[axis]) &&
						coordinates[axis] ==
							expected.Position[axis] + (point == axis + 1 ? expected.Signs[axis] : 0),
					"skinning independent affine oracle"
				);
		}
	}
	inline std::array<uint32_t, 7> Words(const CFrame &frame) {
		return {
			std::bit_cast<uint32_t>(frame.Position.X),
			std::bit_cast<uint32_t>(frame.Position.Y),
			std::bit_cast<uint32_t>(frame.Position.Z),
			std::bit_cast<uint32_t>(frame.QuaternionX),
			std::bit_cast<uint32_t>(frame.QuaternionY),
			std::bit_cast<uint32_t>(frame.QuaternionZ),
			std::bit_cast<uint32_t>(frame.QuaternionW)
		};
	}
	inline void HashWord(uint64_t &hash, uint64_t word) {
		for (size_t byte = 0; byte < 8; ++byte) {
			hash ^= (word >> (byte * 8)) & 255;
			hash *= 1099511628211ULL;
		}
	}
	inline void HashFrame(uint64_t &hash, const CFrame &frame) {
		for (uint32_t word : Words(frame))
			HashWord(hash, word);
	}

	struct JointInput {
		Entity Instance;
		size_t Rig = 0;
		uint16_t Slot = 0;
		uint16_t Parent = engine::scene::NO_JOINT;
		Affine Rest;
		Affine InverseBind;
	};
	struct RigInput {
		Entity Instance;
		Entity Folder;
		Affine Base;
	};
	struct World {
		Store Storage{"skinning_fixture"};
		std::vector<RigInput> Rigs;
		std::vector<JointInput> Joints;
		std::vector<Affine> Expected;
		size_t JointCount = 0;
		size_t Clock = 0;
		bool Animated = false;
		size_t ExpectedWrites = 0;
		static constexpr std::array<float, 4> POSES{0, .25f, -.5f, .75f};

		World(size_t rigCount, size_t joints, bool animated) : JointCount(joints), Animated(animated) {
			Require(joints > 0 && joints <= engine::scene::MAX_JOINTS, "skinning fixture joint bound");
			engine::scene::RegisterSceneClasses();
			Rigs.reserve(rigCount);
			Joints.resize(rigCount * joints);
			Expected.resize(Joints.size());
			for (size_t rigIndex = 0; rigIndex < rigCount; ++rigIndex) {
				const Affine base = Authored(rigIndex, static_cast<float>(rigIndex) * 4 + 8);
				engine::scene::PartDesc description;
				description.Frame = Frame(base);
				const Entity rig = engine::scene::MakePart(Storage, description);
				Storage.Set(
					rig,
					engine::scene::Skeleton{Name("skinning_fixture.Rig"), static_cast<uint16_t>(joints), {}}
				);
				const Entity folder =
					Storage.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "Bones");
				Storage.SetParent(folder, rig);
				Rigs.push_back({rig, folder, base});
				// Reverse insertion deliberately disagrees with palette order.
				for (size_t reverse = joints; reverse > 0; --reverse) {
					const size_t slot = reverse - 1;
					const Entity instance = Storage.CreateInstance(engine::scene::BoneClass(), "Joint");
					Storage.SetParent(instance, folder);
					JointInput input{
						instance,
						rigIndex,
						static_cast<uint16_t>(slot),
						slot == 0 ? engine::scene::NO_JOINT : static_cast<uint16_t>((slot - 1) / 2),
						Authored(slot + rigIndex, .5f),
						Authored(slot + 1, -.125f)
					};
					Bone bone;
					bone.Rest = Frame(input.Rest);
					bone.InverseBind = Frame(input.InverseBind);
					bone.Joint = input.Slot;
					bone.ParentJoint = input.Parent;
					Storage.Set(instance, bone);
					Joints[rigIndex * joints + slot] = input;
				}
			}
			Prepare();
			engine::scene::ResolveBones(Storage);
			Verify();
		}
		Affine Pose(size_t slot) const {
			// Only translation varies; static and animated rows use the same tree.
			return {{Animated ? POSES[Clock % POSES.size()] : 0, static_cast<float>(slot % 3) * .125f, 0}};
		}
		void Prepare() {
			ExpectedWrites = 0;
			for (size_t index = 0; index < Joints.size(); ++index) {
				const auto &input = Joints[index];
				Bone bone = *Storage.Get<Bone>(input.Instance);
				bone.Transform = Frame(Pose(input.Slot));
				Storage.Set(input.Instance, bone);
				const Affine &parent = input.Parent == engine::scene::NO_JOINT
										   ? Rigs[input.Rig].Base
										   : Expected[input.Rig * JointCount + input.Parent];
				const Affine next = Product(Product(parent, input.Rest), Pose(input.Slot));
				if (next.Position != Expected[index].Position || next.Signs != Expected[index].Signs)
					++ExpectedWrites;
				Expected[index] = next;
			}
		}
		void Verify() const {
			for (size_t index = 0; index < Joints.size(); ++index) {
				const auto &input = Joints[index];
				const Bone *bone = Storage.Get<Bone>(input.Instance);
				Require(bone != nullptr, "skinning oracle missing bone");
				VerifyFrame(bone->WorldFrame, Expected[index]);
				VerifyFrame(
					engine::scene::SkinningFrameOf(*bone), Product(Expected[index], input.InverseBind)
				);
				Require(
					Words(bone->Rest) == Words(Frame(input.Rest)) &&
						Words(bone->Transform) == Words(Frame(Pose(input.Slot))) &&
						Words(bone->InverseBind) == Words(Frame(input.InverseBind)) &&
						bone->Joint == input.Slot && bone->ParentJoint == input.Parent,
					"skinning authored bone changed"
				);
			}
			for (const auto &rig : Rigs) {
				const auto *skeleton = Storage.Get<engine::scene::Skeleton>(rig.Instance);
				Require(
					skeleton && skeleton->Rig.Text() == "skinning_fixture.Rig" &&
						skeleton->JointCount == JointCount && skeleton->PoseScale == 1 &&
						skeleton->Reserved[0] == 0 && skeleton->Reserved[1] == 0,
					"skinning authored skeleton changed"
				);
			}
		}
		uint64_t InputHash() const {
			uint64_t hash = 14695981039346656037ULL;
			HashWord(hash, Rigs.size());
			HashWord(hash, JointCount);
			HashWord(hash, Animated);
			HashWord(hash, Clock);
			for (float pose : POSES)
				HashWord(hash, std::bit_cast<uint32_t>(pose));
			for (const auto &rig : Rigs) {
				HashWord(hash, rig.Instance.Id);
				HashWord(hash, Storage.ParentOf(rig.Instance).Id);
				for (char byte : Storage.NameOf(rig.Instance))
					HashWord(hash, static_cast<unsigned char>(byte));
				HashWord(hash, rig.Folder.Id);
				HashWord(hash, Storage.ParentOf(rig.Folder).Id);
				for (char byte : Storage.NameOf(rig.Folder))
					HashWord(hash, static_cast<unsigned char>(byte));
				Storage.EachDescendant(rig.Instance, [&](Entity descendant) {
					HashWord(hash, descendant.Id);
				});
				HashFrame(hash, Storage.Get<engine::scene::Transform>(rig.Instance)->Frame);
				const auto &skeleton = *Storage.Get<engine::scene::Skeleton>(rig.Instance);
				for (char byte : skeleton.Rig.Text())
					HashWord(hash, static_cast<unsigned char>(byte));
				HashWord(hash, skeleton.JointCount);
				HashWord(hash, std::bit_cast<uint32_t>(skeleton.PoseScale));
				HashWord(hash, skeleton.Reserved[0]);
				HashWord(hash, skeleton.Reserved[1]);
			}
			for (const auto &input : Joints) {
				const auto &bone = *Storage.Get<Bone>(input.Instance);
				HashWord(hash, input.Instance.Id);
				HashWord(hash, Storage.ParentOf(input.Instance).Id);
				for (char byte : Storage.NameOf(input.Instance))
					HashWord(hash, static_cast<unsigned char>(byte));
				HashWord(hash, bone.Joint);
				HashWord(hash, bone.ParentJoint);
				HashFrame(hash, bone.Rest);
				HashFrame(hash, bone.Transform);
				HashFrame(hash, bone.InverseBind);
			}
			return hash;
		}
		uint64_t OutputHash() const {
			uint64_t hash = 14695981039346656037ULL;
			for (const auto &input : Joints) {
				HashWord(hash, input.Instance.Id);
				HashFrame(hash, Storage.Get<Bone>(input.Instance)->WorldFrame);
			}
			return hash;
		}
		void Dump(size_t row, size_t call) const {
			// Correctness export only. The caller labels BENCH timings invalid.
			std::printf(
				"# skinning-canonical row=%zu call=%zu clock=%zu version=%llu words=",
				row,
				call,
				Clock,
				static_cast<unsigned long long>(Storage.ChangeVersion())
			);
			for (const auto &input : Joints) {
				std::printf("%016llx", static_cast<unsigned long long>(input.Instance.Id));
				for (uint32_t word : Words(Storage.Get<Bone>(input.Instance)->WorldFrame))
					std::printf("%08x", word);
			}
			std::printf("\n");
		}
	};
}
