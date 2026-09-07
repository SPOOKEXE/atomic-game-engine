#include <engine/core/Profiling.hpp>
#include <engine/ecs/ChangeChannel.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Accessories.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/Tools.hpp>
#include <engine/scene/Visibility.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <type_traits>

namespace engine::scene {
	namespace {
		using core::ByteReader;
		using core::ByteWriter;
		using core::CFrame;
		using core::Vector3;
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		constexpr uint32_t MAGIC = 0x31425450;
		constexpr size_t MAXIMUM_COMPONENT_BYTES = 8192;
		size_t ComponentByteLimit(std::string_view type) {
			return type == "scene.AnimationBuffer" ? MAXIMUM_PORTAL_ANIMATION_BYTES + 8
												   : MAXIMUM_COMPONENT_BYTES;
		}
		constexpr size_t MAXIMUM_COMPONENTS = 32;
		constexpr size_t MAXIMUM_NAME = 256;

		bool Finite(float value) {
			return std::isfinite(value);
		}
		bool Finite(const Vector3 &value) {
			return Finite(value.X) && Finite(value.Y) && Finite(value.Z);
		}
		bool Finite(const CFrame &value) {
			const auto rotation = value.Rotation();
			const float norm = rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z +
							   rotation.w * rotation.w;
			return Finite(value.Position) && Finite(norm) && std::abs(norm - 1.0f) < 0.001f;
		}
		bool Text(std::string_view text, bool empty = false) {
			return (empty || !text.empty()) && text.size() <= MAXIMUM_NAME &&
				   text.find('\0') == std::string_view::npos;
		}
		std::string ReadText(ByteReader &reader, bool empty = false) {
			const uint32_t size = reader.ReadUInt32();
			if (size > MAXIMUM_NAME || size > reader.Remaining()) {
				reader.Fail();
				return {};
			}
			std::string value(size, '\0');
			reader.ReadRaw(value.data(), size);
			if (!Text(value, empty)) reader.Fail();
			return value;
		}

		// Only these concrete scene types may be copied. Reference-bearing types have
		// an explicit visitor below; arbitrary component object bytes never qualify.
		using CopyTypes = std::tuple<
			Transform,
			PreviousTransform,
			Pivot,
			Bounds,
			Motion,
			RigidBody,
			Simulated,
			Collider,
			Surface,
			Visual,
			PhysicsProperties,
			SurfaceAppearance,
			Tags,
			Character,
			CharacterLimb,
			PlayerCharacter,
			Humanoid,
			NetworkOwner,
			PlayerIdentity,
			PlayerNetworkComponent,
			ServiceComponent,
			PortalTransit,
			TextContent,
			Skeleton,
			Bone,
			Animator,
			AnimationTrack,
			AnimationClip,
			AnimationBuffer,
			Tool,
			Attachment,
			Accessory,
			ecs::NotArchivable>;

		template <class T, class Visit> void References(T &value, Visit &&visit) {
			if constexpr (std::is_same_v<T, Character>) {
				visit(value.Root);
				visit(value.Humanoid);
				visit(value.Owner);
			} else if constexpr (std::is_same_v<T, CharacterLimb>)
				visit(value.Root);
			else if constexpr (std::is_same_v<T, PlayerCharacter>)
				visit(value.Model);
			else if constexpr (std::is_same_v<T, Humanoid>)
				visit(value.RootPart);
			else if constexpr (std::is_same_v<T, NetworkOwner>)
				visit(value.Player);
			else if constexpr (std::is_same_v<T, Animator>)
				visit(value.Rig);
			else if constexpr (std::is_same_v<T, AnimationTrack>)
				visit(value.Clip);
			else if constexpr (std::is_same_v<T, AnimationClip>)
				visit(value.Buffer);
			else if constexpr (std::is_same_v<T, Accessory>) {
				visit(value.HandleAttachment);
				visit(value.CharacterAttachment);
			}
		}

		template <class T> bool Valid(const T &value) {
			if constexpr (std::is_same_v<T, Transform> || std::is_same_v<T, PreviousTransform>)
				return Finite(value.Frame);
			else if constexpr (std::is_same_v<T, Pivot>)
				return Finite(value.Offset);
			else if constexpr (std::is_same_v<T, Attachment>)
				return Finite(value.Frame) && Finite(value.WorldFrame);
			else if constexpr (std::is_same_v<T, Bounds>)
				return Finite(value.HalfExtent) && value.HalfExtent.X >= 0 && value.HalfExtent.Y >= 0 &&
					   value.HalfExtent.Z >= 0;
			else if constexpr (std::is_same_v<T, Motion>)
				return Finite(value.Linear) && Finite(value.Angular);
			else if constexpr (std::is_same_v<T, RigidBody>)
				return Finite(value.Mass) && value.Mass >= 0 && Finite(value.LinearDamping) &&
					   Finite(value.AngularDamping) &&
					   static_cast<unsigned>(value.Kind) <= static_cast<unsigned>(BodyKind::Dynamic);
			else if constexpr (std::is_same_v<T, Collider>)
				return Finite(value.Extent) && value.Extent.X >= 0 && value.Extent.Y >= 0 &&
					   value.Extent.Z >= 0;
			else if constexpr (std::is_same_v<T, CharacterLimb>)
				return Finite(value.Offset);
			else if constexpr (std::is_same_v<T, Humanoid>)
				return Finite(value.MoveDirection) && Finite(value.WalkSpeed) && Finite(value.JumpSpeed) &&
					   Finite(value.Height) && Finite(value.GroundTolerance) && Finite(value.Health) &&
					   Finite(value.MaxHealth) && value.Height > 0;
			else if constexpr (std::is_same_v<T, PhysicsProperties>)
				return Finite(value.Density) && Finite(value.Friction) && Finite(value.Elasticity);
			else if constexpr (std::is_same_v<T, Visual>)
				return Finite(value.Tint.R) && Finite(value.Tint.G) && Finite(value.Tint.B) &&
					   Finite(value.Transparency) && value.Surface == -1;
			else if constexpr (std::is_same_v<T, Tags>)
				return value.Mask == 0; // Nonempty masks require the world's tag-name table.
			else if constexpr (std::is_same_v<T, PlayerIdentity>)
				return Finite(value.RespawnTime);
			else if constexpr (std::is_same_v<T, PlayerNetworkComponent>)
				return Finite(value.LocalSimulatedNetworkLatency);
			else if constexpr (std::is_same_v<T, Skeleton>)
				return value.JointCount <= MAX_JOINTS && Finite(value.PoseScale) && value.PoseScale > 0;
			else if constexpr (std::is_same_v<T, Bone>)
				return Finite(value.Rest) && Finite(value.Transform) && Finite(value.InverseBind) &&
					   Finite(value.WorldFrame) && value.Joint < MAX_JOINTS &&
					   (value.ParentJoint == NO_JOINT || value.ParentJoint < value.Joint);
			else if constexpr (std::is_same_v<T, Animator>)
				return Finite(value.RootMotionWeight) && value.RootMotionWeight >= 0 &&
					   value.RootMotionWeight <= 1;
			else if constexpr (std::is_same_v<T, AnimationTrack>)
				return Finite(value.TimePosition) && Finite(value.Speed) && Finite(value.Weight) &&
					   Finite(value.WeightTarget) && Finite(value.FadeTime) && value.FadeTime >= 0 &&
					   value.Weight >= 0 && value.Weight <= 1 && value.WeightTarget >= 0 &&
					   value.WeightTarget <= 1 && value.Priority <= AnimationPriority::Override;
			else if constexpr (std::is_same_v<T, AnimationBuffer>)
				return value.Data.size() <= MAXIMUM_PORTAL_ANIMATION_BYTES;
			else if constexpr (std::is_same_v<T, Tool>)
				return Finite(value.Grip);
			else if constexpr (std::is_same_v<T, TextContent>)
				return value.Value.size() <= MAXIMUM_COMPONENT_BYTES - 4;
			else if constexpr (std::is_same_v<T, PortalTransit>)
				return Finite(value.Frame) && Finite(value.Scale) && value.Scale > 0;
			return true;
		}

		// POD readers copy bool object bytes, so reject invalid representations before
		// invoking the existing lossless codec. Custom codecs use ByteReader::ReadBool.
		template <class T> bool ValidBoolBytes(std::span<const std::byte> bytes) {
			const auto valid = [&](size_t offset) {
				return offset < bytes.size() && std::to_integer<unsigned>(bytes[offset]) <= 1;
			};
			if constexpr (std::is_same_v<T, Humanoid>)
				return valid(offsetof(T, Grounded)) && valid(offsetof(T, JumpRequested)) &&
					   valid(offsetof(T, Enabled)) && valid(offsetof(T, AutoRotate));
			else if constexpr (std::is_same_v<T, PhysicsProperties>)
				return valid(offsetof(T, Custom));
			else if constexpr (std::is_same_v<T, Animator>)
				return valid(offsetof(T, RootMotion)) && valid(offsetof(T, EvaluationThrottled));
			else if constexpr (std::is_same_v<T, AnimationTrack>)
				return valid(offsetof(T, Looped)) && valid(offsetof(T, Playing));
			else if constexpr (std::is_same_v<T, ServiceComponent>)
				return valid(offsetof(T, Fixture));
			return true;
		}

		template <class T> bool Decode(const PortalComponentCopy &copy, T &value) {
			const auto &descriptor = ecs::Components::Describe(ecs::Components::Assigned<T>());
			if (descriptor.Size == 0) {
				return copy.Bytes.empty() && copy.References.empty();
			}
			if (!descriptor.Serialisable ||
				(descriptor.RawSerialisation &&
				 (copy.Bytes.size() != descriptor.Size || !ValidBoolBytes<T>(copy.Bytes))))
				return false;
			ByteReader reader(copy.Bytes);
			descriptor.Read(reader, &value, 1);
			if (reader.Failed() || !reader.AtEnd() || !Valid(value)) return false;
			ByteWriter canonical;
			descriptor.Write(canonical, &value, 1);
			if (!std::ranges::equal(canonical.Bytes(), copy.Bytes)) return false;
			bool clear = true;
			size_t count = 0;
			References(value, [&](Entity &reference) {
				clear &= reference == NULL_ENTITY;
				++count;
			});
			return clear && count == copy.References.size();
		}

		template <class Callback, size_t... Index>
		bool Dispatch(std::string_view type, Callback &&callback, std::index_sequence<Index...>) {
			bool found = false;
			(
				[&] {
					using T = std::tuple_element_t<Index, CopyTypes>;
					const auto id = ecs::Components::Assigned<T>();
					if (id.IsValid() && ecs::Components::Describe(id).Name.Text() == type) {
						callback.template operator()<T>();
						found = true;
					}
				}(),
				...
			);
			return found;
		}
		template <class Callback> bool Dispatch(std::string_view type, Callback &&callback) {
			return Dispatch(
				type,
				std::forward<Callback>(callback),
				std::make_index_sequence<std::tuple_size_v<CopyTypes>>{}
			);
		}

		bool StructuralOrDerived(ecs::ComponentId id) {
			return id == ecs::Components::Assigned<ecs::Hierarchy>() ||
				   id == ecs::Components::Assigned<ecs::InstanceName>() ||
				   id == ecs::Components::Assigned<ecs::InstanceClass>() ||
				   id == ecs::Components::Assigned<ecs::DirtyBits>() ||
				   id == ecs::Components::Assigned<Rendered>() ||
				   id == ecs::Components::Assigned<RenderedSignature>() ||
				   id == ecs::Components::Assigned<LocalTransparency>() ||
				   id == ecs::Components::Assigned<PortalTransitSeen>();
		}

		const PortalNodeCopy *Find(const PortalBodyCopy &body, std::string_view key) {
			const auto found = std::find_if(body.Nodes.begin(), body.Nodes.end(), [&](const auto &node) {
				return node.Key == key;
			});
			return found == body.Nodes.end() ? nullptr : &*found;
		}

		ecs::ClassId FindClass(std::string_view name) {
			for (size_t index = 0; index < ecs::Classes::Count(); ++index) {
				const ecs::ClassId id(static_cast<uint32_t>(index));
				if (ecs::Classes::Describe(id).Name.Text() == name) return id;
			}
			return {};
		}

		const PortalComponentCopy *Component(const PortalNodeCopy &node, std::string_view name) {
			const auto found =
				std::find_if(node.Components.begin(), node.Components.end(), [&](const auto &component) {
					return component.Type == name;
				});
			return found == node.Components.end() ? nullptr : &*found;
		}

		bool CoherentAccessories(const PortalBodyCopy &body) {
			for (const auto &node : body.Nodes) {
				const auto *points = Component(node, "scene.Accessory");
				if (!points) continue;
				if (points->References[0].empty() && points->References[1].empty()) {
					if (node.Parent == body.Character && !body.Character.empty()) return false;
					continue;
				}
				const auto *handlePoint = Find(body, points->References[0]);
				const auto *characterPoint = Find(body, points->References[1]);
				if (body.Kind != PortalBodyKind::Player || node.Parent != body.Character || !handlePoint ||
					!characterPoint || !Component(*handlePoint, "scene.Attachment") ||
					!Component(*characterPoint, "scene.Attachment") ||
					handlePoint->Name != characterPoint->Name)
					return false;
				const auto *handle = Find(body, handlePoint->Parent);
				const auto *target = Find(body, characterPoint->Parent);
				if (!handle || !target || handle->Parent != node.Key || handle->Name != "Handle" ||
					!ecs::Classes::IsA(FindClass(handle->Class), FindClass("BasePart")))
					return false;
				const auto *limb = Component(*handle, "scene.CharacterLimb");
				if (!limb || limb->References[0] != body.Root) return false;
				if (target->Key == body.Root) continue;
				const auto *carried = Component(*target, "scene.CharacterLimb");
				if (target->Parent != body.Character || !carried || carried->References[0] != body.Root)
					return false;
			}
			return true;
		}

		bool CoherentAnimation(const PortalBodyCopy &body) {
			struct Joint {
				size_t Rig;
				uint16_t Slot;
				uint16_t Parent;
			};
			std::vector<uint16_t> counts(body.Nodes.size());
			std::vector<Joint> joints;
			for (size_t index = 0; index < body.Nodes.size(); ++index) {
				if (const auto *encoded = Component(body.Nodes[index], "scene.Skeleton")) {
					Skeleton skeleton;
					Decode(*encoded, skeleton);
					counts[index] = skeleton.JointCount;
				}
			}
			for (const auto &node : body.Nodes) {
				const auto linked = [&](const char *type, const char *target) {
					const auto *value = Component(node, type);
					if (!value || value->References[0].empty()) return true;
					const auto *other = Find(body, value->References[0]);
					return other && Component(*other, target);
				};
				if (!linked("scene.Animator", "scene.Skeleton") ||
					!linked("scene.AnimationTrack", "scene.AnimationClip") ||
					!linked("scene.AnimationClip", "scene.AnimationBuffer"))
					return false;
				const auto *encoded = Component(node, "scene.Bone");
				if (!encoded) continue;
				const PortalNodeCopy *rig = Find(body, node.Parent);
				for (size_t depth = 0; rig && depth < body.Nodes.size(); ++depth) {
					if (Component(*rig, "scene.Skeleton")) break;
					rig = Find(body, rig->Parent);
				}
				if (!rig) return false;
				const size_t rigIndex = static_cast<size_t>(rig - body.Nodes.data());
				Bone bone;
				Decode(*encoded, bone);
				if (bone.Joint >= counts[rigIndex]) return false;
				joints.push_back({rigIndex, bone.Joint, bone.ParentJoint});
			}
			const auto order = [](const Joint &left, const Joint &right) {
				return left.Rig < right.Rig || (left.Rig == right.Rig && left.Slot < right.Slot);
			};
			std::sort(joints.begin(), joints.end(), order);
			for (size_t index = 0; index < joints.size(); ++index) {
				const auto &joint = joints[index];
				if (index && joint.Rig == joints[index - 1].Rig && joint.Slot == joints[index - 1].Slot)
					return false;
				if (joint.Parent != NO_JOINT &&
					!std::binary_search(
						joints.begin(), joints.end(), Joint{joint.Rig, joint.Parent, NO_JOINT}, order
					))
					return false;
			}
			return true;
		}

		bool CoherentRoles(const PortalBodyCopy &body) {
			if (body.Kind == PortalBodyKind::Object) {
				const auto &root = *Find(body, body.Root);
				if (!root.Parent.empty() ||
					!ecs::Classes::IsA(FindClass(root.Class), FindClass("BasePart")) ||
					Component(root, "scene.Transform") == nullptr ||
					Component(root, "scene.Motion") == nullptr ||
					Component(root, "scene.Collider") == nullptr)
					return false;
				for (const auto &node : body.Nodes) {
					if (Component(node, "scene.Character") || Component(node, "scene.Humanoid") ||
						Component(node, "scene.PlayerCharacter") || Component(node, "scene.CharacterLimb"))
						return false;
					if (node.Key != body.Root &&
						(Component(node, "scene.Motion") || Component(node, "scene.Simulated")))
						return false;
					if (const auto *owner = Component(node, "scene.NetworkOwner");
						owner && owner->References != std::vector<std::string>{""})
						return false;
				}
				return true;
			}
			const auto &player = *Find(body, body.Player);
			const auto &character = *Find(body, body.Character);
			const auto &root = *Find(body, body.Root);
			const auto &humanoid = *Find(body, body.Humanoid);
			const auto *playerLink = Component(player, "scene.PlayerCharacter");
			const auto *characterLink = Component(character, "scene.Character");
			const auto *steering = Component(humanoid, "scene.Humanoid");
			const auto *ownership = Component(root, "scene.NetworkOwner");
			if (!player.Parent.empty() || !character.Parent.empty() ||
				!ecs::Classes::IsA(FindClass(character.Class), ModelClass()) ||
				!ecs::Classes::IsA(FindClass(root.Class), FindClass("BasePart")) ||
				(body.Humanoid != body.Root &&
				 !ecs::Classes::IsA(FindClass(humanoid.Class), HumanoidClass())) ||
				!ecs::Classes::IsA(FindClass(player.Class), PlayerClass()) || playerLink == nullptr ||
				playerLink->References != std::vector<std::string>{body.Character} ||
				characterLink == nullptr ||
				characterLink->References !=
					std::vector<std::string>{body.Root, body.Humanoid, body.Player} ||
				steering == nullptr ||
				steering->References !=
					std::vector<std::string>{body.Root == body.Humanoid ? std::string() : body.Root} ||
				Component(root, "scene.Transform") == nullptr || Component(root, "scene.Motion") == nullptr ||
				(ownership != nullptr && ownership->References != std::vector<std::string>{body.Player}))
				return false;
			const auto inCharacter = [&](const PortalNodeCopy *node) {
				for (size_t depth = 0; node != nullptr && depth < body.Nodes.size(); ++depth) {
					if (node->Key == body.Character) return true;
					node = Find(body, node->Parent);
				}
				return false;
			};
			for (const auto &node : body.Nodes) {
				if (node.Key != body.Root && Component(node, "scene.Motion") != nullptr) return false;
				if (node.Key != body.Humanoid && Component(node, "scene.Humanoid") != nullptr) return false;
				if (node.Key != body.Character && Component(node, "scene.Character") != nullptr) return false;
				if (node.Key != body.Player && Component(node, "scene.PlayerCharacter") != nullptr)
					return false;
				if (const auto *limb = Component(node, "scene.CharacterLimb");
					limb && limb->References != std::vector<std::string>{body.Root})
					return false;
				if (const auto *owner = Component(node, "scene.NetworkOwner");
					owner && owner->References != std::vector<std::string>{body.Player})
					return false;
			}
			return inCharacter(&root) && inCharacter(&humanoid);
		}

		void EncodeBody(ByteWriter &writer, const PortalBodyCopy &body) {
			writer.WriteUInt32(MAGIC);
			writer.WriteString(Describe(body.Kind));
			writer.WriteString(body.Player);
			writer.WriteString(body.Character);
			writer.WriteString(body.Root);
			writer.WriteString(body.Humanoid);
			writer.WriteBool(body.Sweep.has_value());
			if (body.Sweep) {
				writer.WriteRaw(&body.Sweep->From, sizeof(body.Sweep->From));
				writer.WriteRaw(&body.Sweep->Displacement, sizeof(body.Sweep->Displacement));
				writer.WriteRaw(&body.Sweep->AngularDisplacement, sizeof(body.Sweep->AngularDisplacement));
			}
			writer.WriteUInt32(static_cast<uint32_t>(body.Nodes.size()));
			for (const auto &node : body.Nodes) {
				writer.WriteString(node.Key);
				writer.WriteString(node.Parent);
				writer.WriteString(node.Class);
				writer.WriteString(node.Name);
				writer.WriteUInt32(static_cast<uint32_t>(node.Components.size()));
				for (const auto &component : node.Components) {
					writer.WriteString(component.Type);
					writer.WriteUInt32(static_cast<uint32_t>(component.Bytes.size()));
					writer.WriteRaw(component.Bytes.data(), component.Bytes.size());
					writer.WriteUInt32(static_cast<uint32_t>(component.References.size()));
					for (const auto &reference : component.References)
						writer.WriteString(reference);
				}
			}
		}
	}

	namespace {
		bool CaptureBody(
			const ecs::Store &store,
			Entity subject,
			PortalBodyKind kind,
			PortalBodyCopy &out,
			std::string &failure,
			std::span<const ecs::ComponentId> localComponents = {}
		) {
			ENGINE_PROFILE("capture portal body");
			const bool object = kind == PortalBodyKind::Object;
			if (store.AdoptOnly() || !store.Alive(subject) ||
				(!object && !store.IsA(subject, PlayerClass())) ||
				(object && !store.IsA(subject, FindClass("BasePart")))) {
				failure = "portal transfer requires an authoritative supported subject";
				return false;
			}
			const Entity player = object ? NULL_ENTITY : subject;
			const Entity model = object ? subject : CharacterOf(store, player);
			const auto *rig = object ? nullptr : store.Get<Character>(model);
			if (!object && (rig == nullptr || rig->Owner != player)) {
				failure = "portal transfer requires the player's actual character";
				return false;
			}
			const Entity root = object ? subject : rig->Root;
			const Entity humanoid = object ? NULL_ENTITY : rig->Humanoid;
			PortalBodyCopy body;
			body.Kind = kind;
			std::vector<Entity> entities;
			const auto gather = [&](auto &&self, Entity entity, std::string key, std::string parent) -> bool {
				if (entities.size() >= MAXIMUM_PORTAL_NODES || !store.Alive(entity)) return false;
				if (std::find(entities.begin(), entities.end(), entity) != entities.end()) return false;
				const auto &klass = ecs::Classes::Describe(store.ClassOf(entity));
				body.Nodes.push_back(
					{std::move(key),
					 std::move(parent),
					 std::string(klass.Name.Text()),
					 std::string(store.InstanceNameOf(entity).Text()),
					 {}}
				);
				entities.push_back(entity);
				const std::string held = body.Nodes.back().Key;
				bool valid = true;
				size_t childIndex = 0;
				store.EachChild(entity, [&](Entity child) {
					if (valid) valid = self(self, child, held + "/" + std::to_string(childIndex++), held);
				});
				return valid;
			};
			if ((!object && !gather(gather, player, "player", "")) ||
				!gather(gather, model, object ? "object" : "character", "")) {
				failure = "portal rig exceeds its node bound or overlaps the player subtree";
				return false;
			}
			// Clip definitions may be shared outside the rig. Copy their bounded
			// dependency closure under the transferred owner, without moving or
			// retiring the original world's shared definitions.
			for (size_t index = 0; index < entities.size(); ++index) {
				Entity dependency;
				bool clip = false;
				if (const auto *track = store.Get<AnimationTrack>(entities[index])) {
					dependency = track->Clip;
					clip = true;
				} else if (const auto *definition = store.Get<AnimationClip>(entities[index]))
					dependency = definition->Buffer;
				if (dependency == NULL_ENTITY || std::ranges::find(entities, dependency) != entities.end())
					continue;
				if ((clip && !store.Get<AnimationClip>(dependency)) ||
					(!clip && !store.Get<AnimationBuffer>(dependency)) ||
					!gather(
						gather,
						dependency,
						"dependency/" + std::to_string(index),
						object ? "object" : "character"
					)) {
					failure = "portal animation dependency is unavailable or exceeds the owned body bound";
					return false;
				}
			}
			const auto key = [&](Entity entity) -> std::string {
				const auto found = std::find(entities.begin(), entities.end(), entity);
				return found == entities.end()
						   ? std::string()
						   : body.Nodes[static_cast<size_t>(found - entities.begin())].Key;
			};
			body.Player = key(player);
			body.Character = object ? std::string() : key(model);
			body.Root = key(root);
			body.Humanoid = key(humanoid);
			for (size_t index = 0; index < entities.size(); ++index) {
				const Entity entity = entities[index];
				for (const auto id : store.ComponentsOf(entity)) {
					if (StructuralOrDerived(id) ||
						std::ranges::find(localComponents, id) != localComponents.end())
						continue;
					const auto &descriptor = ecs::Components::Describe(id);
					PortalComponentCopy copy;
					copy.Type = std::string(descriptor.Name.Text());
					bool valid = true;
					const bool supported = Dispatch(copy.Type, [&]<class T> {
						T value{};
						if constexpr (!std::is_empty_v<T>) {
							const auto *source = store.Get<T>(entity);
							valid = source && Valid(*source);
							if (!valid) return;
							value = *source;
						}
						References(value, [&](Entity &reference) {
							const std::string target =
								reference == NULL_ENTITY ? std::string() : key(reference);
							if (reference != NULL_ENTITY && target.empty()) valid = false;
							copy.References.push_back(target);
							reference = NULL_ENTITY;
						});
						if (valid) {
							ByteWriter writer;
							if (descriptor.Size != 0) descriptor.Write(writer, &value, 1);
							copy.Bytes.assign(writer.Bytes().begin(), writer.Bytes().end());
						}
					});
					if (!supported || !valid) {
						failure =
							"portal transfer refuses unsupported component, value or external reference: " +
							copy.Type;
						return false;
					}
					body.Nodes[index].Components.push_back(std::move(copy));
				}
				auto &components = body.Nodes[index].Components;
				std::sort(components.begin(), components.end(), [](const auto &left, const auto &right) {
					return left.Type < right.Type;
				});
			}
			if (!ValidatePortalBody(body, failure)) return false;
			out = std::move(body);
			return true;
		}

	}
	const char *Describe(PortalBodyKind kind) {
		switch (kind) {
		case PortalBodyKind::Player:
			return "player";
		case PortalBodyKind::Object:
			return "object";
		}
		return "invalid";
	}
	std::optional<PortalBodyKind> PortalBodyKindOf(std::string_view name) {
		if (name == "player") return PortalBodyKind::Player;
		if (name == "object") return PortalBodyKind::Object;
		return std::nullopt;
	}
	bool CapturePortalBody(
		const ecs::Store &store,
		Entity player,
		PortalBodyCopy &out,
		std::string &failure,
		std::span<const ecs::ComponentId> localComponents
	) {
		return CaptureBody(store, player, PortalBodyKind::Player, out, failure, localComponents);
	}
	bool
	CapturePortalObject(const ecs::Store &store, Entity object, PortalBodyCopy &out, std::string &failure) {
		return CaptureBody(store, object, PortalBodyKind::Object, out, failure);
	}

	bool ValidatePortalBody(const PortalBodyCopy &body, std::string &failure) {
		if (body.Sweep && (!Finite(body.Sweep->From) || !Finite(body.Sweep->Displacement) ||
						   !Finite(body.Sweep->AngularDisplacement))) {
			failure = "portal suffix is not finite";
			return false;
		}
		const bool player = body.Kind == PortalBodyKind::Player;
		if ((!player && body.Kind != PortalBodyKind::Object) || body.Nodes.empty() ||
			body.Nodes.size() > MAXIMUM_PORTAL_NODES || !Find(body, body.Root) ||
			(player && (!Find(body, body.Player) || !Find(body, body.Character) ||
						!Find(body, body.Humanoid) || body.Player == body.Character)) ||
			(!player && (!body.Player.empty() || !body.Character.empty() || !body.Humanoid.empty()))) {
			failure = "portal rig has invalid role references or node count";
			return false;
		}
		// Resolve class text before any component codec can intern asset names.
		// Every class default must be represented, apart from explicit local
		// derived state and the tree rebuilt from packet-local references.
		for (const auto &node : body.Nodes) {
			const auto klass = FindClass(node.Class);
			if (!klass.IsValid()) {
				failure = "portal rig has unknown class " + node.Class;
				return false;
			}
			const auto &info = ecs::Classes::Describe(klass);
			if (info.Set == nullptr) {
				failure = "portal rig class has no component set";
				return false;
			}
			for (const auto id : info.Set->Ids()) {
				if (!StructuralOrDerived(id) &&
					Component(node, ecs::Components::Describe(id).Name.Text()) == nullptr) {
					failure = "portal rig omits a required class component";
					return false;
				}
			}
		}

		size_t bytes =
			32 + body.Player.size() + body.Character.size() + body.Root.size() + body.Humanoid.size();
		for (size_t index = 0; index < body.Nodes.size(); ++index) {
			const auto &node = body.Nodes[index];
			if (!Text(node.Key) || !Text(node.Parent, true) || !Text(node.Class) || !Text(node.Name, true) ||
				node.Components.size() > MAXIMUM_COMPONENTS || Find(body, node.Key) != &node) {
				failure = "portal rig has invalid or repeated node identity";
				return false;
			}
			const auto *parent = node.Parent.empty() ? nullptr : Find(body, node.Parent);
			if ((!node.Parent.empty() && (parent == nullptr || parent >= &node)) ||
				(node.Parent.empty() &&
				 (player ? node.Key != body.Player && node.Key != body.Character : node.Key != body.Root))) {
				failure = "portal rig parent must precede its child";
				return false;
			}
			bytes += 20 + node.Key.size() + node.Parent.size() + node.Class.size() + node.Name.size();
			std::string_view previous;
			for (const auto &component : node.Components) {
				if (!Text(component.Type) || component.Type <= previous ||
					component.Bytes.size() > ComponentByteLimit(component.Type) ||
					component.References.size() > 3) {
					failure = "portal rig component bounds or canonical order are invalid";
					return false;
				}
				previous = component.Type;
				bytes += 12 + component.Type.size() + component.Bytes.size();
				for (const auto &reference : component.References) {
					if (!Text(reference, true) || (!reference.empty() && Find(body, reference) == nullptr)) {
						failure = "portal rig reference leaves the copied tree";
						return false;
					}
					bytes += 4 + reference.size();
				}
				bool valid = false;
				const bool known = Dispatch(component.Type, [&]<class T> {
					T value{};
					valid = Decode(component, value);
				});
				if (!known || !valid) {
					failure = "portal rig component is unsupported or malformed: " + component.Type;
					return false;
				}
				if (bytes > MAXIMUM_PORTAL_BODY_BYTES) {
					failure = "portal rig exceeds its byte bound";
					return false;
				}
			}
		}
		if (!CoherentRoles(body) || !CoherentAnimation(body) || !CoherentAccessories(body)) {
			failure = "portal rig role links disagree with its declared roles";
			return false;
		}
		return true;
	}

	bool WritePortalBody(ByteWriter &writer, const PortalBodyCopy &body) {
		std::string failure;
		if (!ValidatePortalBody(body, failure)) return false;
		ByteWriter encoded;
		EncodeBody(encoded, body);
		if (encoded.Size() > MAXIMUM_PORTAL_BODY_BYTES) return false;
		writer.WriteUInt32(static_cast<uint32_t>(encoded.Size()));
		writer.WriteRaw(encoded.Bytes().data(), encoded.Size());
		return true;
	}

	bool ReadPortalBody(ByteReader &reader, PortalBodyCopy &out) {
		const uint32_t size = reader.ReadUInt32();
		if (size > MAXIMUM_PORTAL_BODY_BYTES || size > reader.Remaining()) {
			reader.Fail();
			return false;
		}
		const auto bytes = reader.ReadRawView(size);
		ByteReader input(bytes);
		if (input.ReadUInt32() != MAGIC) {
			reader.Fail();
			return false;
		}
		PortalBodyCopy body;
		const auto kind = PortalBodyKindOf(ReadText(input));
		if (!kind) {
			reader.Fail();
			return false;
		}
		body.Kind = *kind;
		body.Player = ReadText(input, true);
		body.Character = ReadText(input, true);
		body.Root = ReadText(input);
		body.Humanoid = ReadText(input, true);
		const uint8_t swept = input.ReadUInt8();
		if (swept > 1) input.Fail();
		if (swept == 1) {
			body.Sweep.emplace();
			input.ReadRaw(&body.Sweep->From, sizeof(body.Sweep->From));
			input.ReadRaw(&body.Sweep->Displacement, sizeof(body.Sweep->Displacement));
			input.ReadRaw(&body.Sweep->AngularDisplacement, sizeof(body.Sweep->AngularDisplacement));
		}
		const uint32_t nodes = input.ReadUInt32();
		if (input.Failed() || nodes > MAXIMUM_PORTAL_NODES) {
			reader.Fail();
			return false;
		}
		body.Nodes.reserve(nodes);
		for (uint32_t index = 0; index < nodes; ++index) {
			PortalNodeCopy node;
			node.Key = ReadText(input);
			node.Parent = ReadText(input, true);
			node.Class = ReadText(input);
			node.Name = ReadText(input, true);
			const uint32_t components = input.ReadUInt32();
			if (input.Failed() || components > MAXIMUM_COMPONENTS) {
				reader.Fail();
				return false;
			}
			node.Components.reserve(components);
			for (uint32_t at = 0; at < components; ++at) {
				PortalComponentCopy component;
				component.Type = ReadText(input);
				const uint32_t count = input.ReadUInt32();
				if (input.Failed() || count > ComponentByteLimit(component.Type) ||
					count > input.Remaining()) {
					reader.Fail();
					return false;
				}
				component.Bytes.resize(count);
				input.ReadRaw(component.Bytes.data(), count);
				const uint32_t references = input.ReadUInt32();
				if (input.Failed() || references > 3) {
					reader.Fail();
					return false;
				}
				for (uint32_t ref = 0; ref < references; ++ref)
					component.References.push_back(ReadText(input, true));
				node.Components.push_back(std::move(component));
			}
			body.Nodes.push_back(std::move(node));
		}
		std::string failure;
		if (input.Failed() || !input.AtEnd() || !ValidatePortalBody(body, failure)) {
			reader.Fail();
			return false;
		}
		out = std::move(body);
		return true;
	}

	bool MapPortalBody(PortalBodyCopy &body, const SeamTransform &through, std::string &failure) {
		if (!Finite(through.Frame) || !Finite(through.Origin) || !Finite(through.Scale) ||
			through.Scale <= 0 || !ValidatePortalBody(body, failure)) {
			failure = "invalid portal similarity or body";
			return false;
		}
		PortalBodyCopy mapped = body;
		if (mapped.Sweep) {
			mapped.Sweep->From = through.Place(mapped.Sweep->From);
			mapped.Sweep->Displacement = through.Carry(mapped.Sweep->Displacement);
			mapped.Sweep->AngularDisplacement = through.Rotate(mapped.Sweep->AngularDisplacement);
		}
		for (auto &node : mapped.Nodes)
			for (auto &component : node.Components) {
				Dispatch(component.Type, [&]<class T> {
					T value{};
					Decode(component, value);
					if constexpr (std::is_same_v<T, Transform> || std::is_same_v<T, PreviousTransform>)
						value.Frame = through.Place(value.Frame);
					else if constexpr (std::is_same_v<T, Attachment>) {
						value.Frame.Position = value.Frame.Position * through.Scale;
						value.WorldFrame = through.Place(value.WorldFrame);
					} else if constexpr (std::is_same_v<T, Motion>) {
						value.Linear = through.Carry(value.Linear);
						value.Angular = through.Rotate(value.Angular);
					} else if constexpr (std::is_same_v<T, Bounds>)
						value.HalfExtent = value.HalfExtent * through.Scale;
					else if constexpr (std::is_same_v<T, Collider>)
						value.Extent = value.Extent * through.Scale;
					else if constexpr (std::is_same_v<T, CharacterLimb>)
						value.Offset.Position = value.Offset.Position * through.Scale;
					else if constexpr (std::is_same_v<T, Pivot>)
						value.Offset.Position = value.Offset.Position * through.Scale;
					else if constexpr (std::is_same_v<T, Skeleton>)
						value.PoseScale *= through.Scale;
					else if constexpr (std::is_same_v<T, Bone>) {
						value.Rest.Position = value.Rest.Position * through.Scale;
						value.Transform.Position = value.Transform.Position * through.Scale;
						value.InverseBind.Position = value.InverseBind.Position * through.Scale;
						value.WorldFrame = through.Place(value.WorldFrame);
					} else if constexpr (std::is_same_v<T, Tool>)
						value.Grip.Position = value.Grip.Position * through.Scale;
					else if constexpr (std::is_same_v<T, Humanoid>) {
						value.MoveDirection = through.Rotate(value.MoveDirection);
						value.WalkSpeed *= through.Scale;
						value.JumpSpeed *= through.Scale;
						value.Height *= through.Scale;
						value.GroundTolerance *= through.Scale;
					}
					const auto &descriptor = ecs::Components::Describe(ecs::Components::Assigned<T>());
					ByteWriter writer;
					if (descriptor.Size != 0) descriptor.Write(writer, &value, 1);
					component.Bytes.assign(writer.Bytes().begin(), writer.Bytes().end());
				});
			}
		if (!ValidatePortalBody(mapped, failure)) return false;
		body = std::move(mapped);
		return true;
	}

	bool AdmitPortalBody(
		ecs::Store &store, const PortalBodyCopy &body, PortalBodyArrival &out, std::string &failure
	) {
		ENGINE_PROFILE("admit portal body");
		if (store.AdoptOnly() || (body.Kind == PortalBodyKind::Player && PlayersOf(store) == NULL_ENTITY) ||
			WorkspaceOf(store) == NULL_ENTITY || !ValidatePortalBody(body, failure)) {
			failure = "destination cannot admit this portal body";
			return false;
		}
		const auto *settings = store.Get<PlayersServiceComponent>(PlayersOf(store));
		if (body.Kind == PortalBodyKind::Player && settings != nullptr &&
			PlayerCount(store) >= static_cast<size_t>(std::max(0, settings->MaxPlayers))) {
			failure = "destination player capacity reached";
			return false;
		}
		std::vector<ecs::ClassId> classes;
		for (const auto &node : body.Nodes) {
			const auto klass = FindClass(node.Class);
			if (!klass.IsValid()) {
				failure = "destination does not know class " + node.Class;
				return false;
			}
			classes.push_back(klass);
		}
		std::vector<Entity> made;
		const auto resolve = [&](std::string_view key) -> Entity {
			const auto *node = Find(body, key);
			return node == nullptr ? NULL_ENTITY : made[static_cast<size_t>(node - body.Nodes.data())];
		};
		const auto rollback = [&] {
			for (const auto entity : made)
				if (store.Alive(entity)) store.DestroyInstance(entity);
		};
		for (size_t index = 0; index < body.Nodes.size(); ++index) {
			const Entity entity = store.CreateInstance(classes[index], body.Nodes[index].Name);
			if (entity == NULL_ENTITY) {
				rollback();
				failure = "destination refused a rig instance";
				return false;
			}
			made.push_back(entity);
		}
		for (size_t index = 0; index < body.Nodes.size(); ++index) {
			const auto &node = body.Nodes[index];
			const Entity entity = made[index];
			std::vector<ecs::ComponentId> defaults(
				store.ComponentsOf(entity).begin(), store.ComponentsOf(entity).end()
			);
			for (const auto id : defaults)
				if (!StructuralOrDerived(id)) store.RemoveComponent(entity, id);
			for (const auto &component : node.Components) {
				Dispatch(component.Type, [&]<class T> {
					T value{};
					Decode(component, value);
					size_t at = 0;
					References(value, [&](Entity &reference) {
						reference = resolve(component.References[at++]);
					});
					store.Set(entity, value);
				});
			}
			if (!node.Parent.empty()) store.SetParent(entity, resolve(node.Parent));
		}
		// The source segment has already been consumed, including its destination
		// suffix. Replaying it here can immediately enter a reciprocal portal.
		for (const auto entity : made)
			if (const auto *pose = store.Get<Transform>(entity); pose && store.Has<PreviousTransform>(entity))
				store.Set(entity, PreviousTransform{pose->Frame});
		for (const auto entity : made)
			if (store.Has<Attachment>(entity)) {
				const auto frame = ResolveAttachment(store, entity);
				store.GetMutable<Attachment>(entity)->WorldFrame = frame;
			}
		PortalBodyArrival arrival{
			resolve(body.Player), resolve(body.Character), resolve(body.Root), resolve(body.Humanoid)
		};
		if (body.Kind == PortalBodyKind::Object) {
			store.SetParent(arrival.Root, WorkspaceOf(store));
			out = arrival;
			return true;
		}
		store.SetParent(arrival.Character, WorkspaceOf(store));
		store.Remove<PlayerCharacter>(arrival.Player);
		if (!SetPlayerCharacter(store, arrival.Player, arrival.Character)) {
			rollback();
			failure = "destination could not bind the transferred Humanoid";
			return false;
		}
		store.SetParent(arrival.Player, PlayersOf(store));
		out = arrival;
		return true;
	}
}
