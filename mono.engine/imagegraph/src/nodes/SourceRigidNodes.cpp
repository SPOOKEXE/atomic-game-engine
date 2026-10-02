#include "../SourceRigidControls.hpp"
#include "Families.hpp"

#include <engine/imagegraph/RigidReplay.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		struct RigidExecution {
			NodeContext &Context;
			explicit RigidExecution(NodeContext &context) : Context(context) {}
			RigidOwnerReplayState *Owner = nullptr;
			Vector2 Dimension{};
			double Scale = 50;
			uint32_t Ordinal = 0;
			AllocationReservation SnapshotCharge;
			bool Charge(uint64_t bytes) {
				auto reserved = Context.ReserveWorkspace(bytes);
				return reserved && Context.CurrentRigidCharge &&
					   Context.CurrentRigidCharge->Merge(std::move(*reserved));
			}
			bool Begin() {
				if (!Context.CurrentRigid || !Context.CurrentRigidCharge || !Context.Request.RigidProvider)
					return Context.Fail(
						Status::UnsupportedExecution,
						"rigid graph requires a caller-owned replay and rigid provider"
					);
				if (Context.InlineOwnerType != "pc.rigid_group_inline")
					return Context.Fail(
						Status::UnsupportedExecution, "rigid actor requires its nearest RigidSim collection"
					);
				if (Context.Request.Tick >= 4096)
					return Context.Fail(Status::LimitExceeded, "rigid replay exceeds 4096 recorded frames");
				Node authored;
				authored.Type = "pc.rigid_group_inline";
				const auto *entry = FindCatalogueEntry(authored.Type);
				NodeContext controls(authored, *entry, Context.Request, Context.AllocationBudget());
				auto controlCharge = Context.ReserveWorkspace(
					Context.InlineOwnerValues.size() * sizeof(std::pair<std::string_view, const Value *>) +
					Context.InlineOwnerImages.size() * sizeof(std::pair<std::string_view, const Image *>)
				);
				if (!controlCharge) return false;
				controls.ValueViews.assign(
					Context.InlineOwnerValues.begin(), Context.InlineOwnerValues.end()
				);
				controls.Images.assign(Context.InlineOwnerImages.begin(), Context.InlineOwnerImages.end());
				Dimension = controls.Vec2("dimension", {1, 1});
				const bool linked = std::find(
										Context.InlineOwnerLinkedValues.begin(),
										Context.InlineOwnerLinkedValues.end(),
										"dimension"
									) != Context.InlineOwnerLinkedValues.end();
				if (!linked && controls.Integer("dimension_unit", 1) == 1) {
					Dimension.X *= Context.Project.SurfaceWidth;
					Dimension.Y *= Context.Project.SurfaceHeight;
				}
				if (const auto *mask = controls.Input("dimension"))
					Dimension = {double(mask->Width), double(mask->Height)};
				Scale = controls.Scalar("simulation_scale", 50);
				if (controls.FailureCode != Status::Ok)
					return Context.Fail(controls.FailureCode, controls.FailureMessage);
				if (!std::isfinite(Dimension.X) || !std::isfinite(Dimension.Y) || Dimension.X < 1 ||
					Dimension.Y < 1 || Dimension.X > Limits::MaximumDimension ||
					Dimension.Y > Limits::MaximumDimension)
					return Context.Fail(
						Status::InvalidValue, "rigid collection dimension is outside native bounds"
					);
				for (auto &owner : Context.CurrentRigid->Owners)
					if (owner.History.OwnerId == Context.InlineOwnerId) {
						Owner = &owner;
						break;
					}
				if (!Owner) {
					if (Context.Request.Tick != 0)
						return Context.Fail(
							Status::UnsupportedExecution, "rigid seek requires captured frames from tick zero"
						);
					if (!Charge(sizeof(RigidOwnerReplayState) * 2 + Context.InlineOwnerId.size() * 2 + 64))
						return false;
					Context.CurrentRigid->Owners.emplace_back();
					Owner = &Context.CurrentRigid->Owners.back();
					Owner->History.OwnerId = Context.InlineOwnerId;
					Owner->AuthoringRevision = Context.Request.RigidAuthoringRevision;
					Owner->History.World = RigidControlReader{controls, Dimension, Scale}.World();
				}
				if (Owner->AuthoringRevision != Context.Request.RigidAuthoringRevision)
					return Context.Fail(
						Status::UnsupportedExecution,
						"rigid replay authoring revision changed; reset at tick zero"
					);
				const size_t tick = Context.Request.Tick;
				if (Context.Request.RigidReplay)
					for (const auto &previous : Context.Request.RigidReplay->Owners) {
						if (previous.History.OwnerId == Owner->History.OwnerId && *Owner == previous &&
							Owner->History.Frames.size() > tick) {
							Owner->History.Frames.resize(tick);
							Owner->VisualFrames.resize(tick);
							Owner->NodeFrames.resize(tick);
							break;
						}
					}
				if (Owner->History.Frames.size() == tick) {
					uint64_t copiedNodeBytes = 0;
					if (tick) {
						const auto &previousNodes = Owner->NodeFrames.back().Nodes;
						copiedNodeBytes = previousNodes.size() * sizeof(RigidNodeReplayState);
						for (const auto &node : previousNodes) {
							copiedNodeBytes += node.NodeId.capacity();
							copiedNodeBytes += node.OutputBodyIds.size() * sizeof(std::string);
							for (const auto &id : node.OutputBodyIds)
								copiedNodeBytes += id.capacity();
							copiedNodeBytes += node.CollisionPairs.size() * sizeof(RigidCollisionPairState);
							for (const auto &pair : node.CollisionPairs)
								copiedNodeBytes += pair.A.capacity() + pair.B.capacity();
						}
					}
					if (!Charge(
							sizeof(SourceRigidFrame) * 2 + sizeof(RigidVisualFrame) * 2 +
							sizeof(RigidNodeFrame) * 2 + copiedNodeBytes
						))
						return false;
					RigidNodeFrame frame;
					if (tick) frame.Nodes = Owner->NodeFrames.back().Nodes;
					Owner->History.Frames.push_back(RigidControlReader{controls, Dimension, Scale}.Frame());
					Owner->VisualFrames.emplace_back();
					Owner->NodeFrames.push_back(std::move(frame));
				} else if (Owner->History.Frames.size() != tick + 1)
					return Context.Fail(
						Status::UnsupportedExecution, "rigid replay requires contiguous captured frames"
					);
				return true;
			}
			RigidNodeReplayState *NodeState() {
				auto &nodes = Owner->NodeFrames[Context.Request.Tick].Nodes;
				for (auto &node : nodes)
					if (node.NodeId == Context.Authored.Id && node.ProcessorRow == Context.ProcessorRow)
						return &node;
				if (!Charge(sizeof(RigidNodeReplayState) * 2 + Context.Authored.Id.size() * 2 + 32))
					return nullptr;
				nodes.push_back(
					{Context.Authored.Id, static_cast<uint32_t>(Context.ProcessorRow), 0, {}, {}}
				);
				return &nodes.back();
			}
			template <class T> bool Event(T command) {
				if (!Charge(sizeof(SourceRigidEvent) * 2 + 1024)) return false;
				Owner->History.Frames[Context.Request.Tick].Events.push_back(
					{{Context.Authored.Id, Ordinal++, static_cast<uint32_t>(Context.ProcessorRow)},
					 std::move(command)}
				);
				return true;
			}
			bool Visual(RigidVisualState visual) {
				const uint64_t pixels = visual.Texture ? visual.Texture->Data.Pixels.size() : 0;
				if (!Charge(sizeof(RigidVisualMutation) * 2 + pixels + visual.BodyId.size() * 2 + 64))
					return false;
				const SourceRigidEventPosition position{
					Context.Authored.Id, Ordinal, static_cast<uint32_t>(Context.ProcessorRow)
				};
				if (!Event(SourceRigidEvent::Checkpoint{})) return false;
				Owner->VisualFrames[Context.Request.Tick].Mutations.push_back({position, std::move(visual)});
				return true;
			}
			bool Snapshot(SourceRigidSnapshot &snapshot) {
				const SourceRigidEventPosition position{
					Context.Authored.Id, Ordinal - 1, static_cast<uint32_t>(Context.ProcessorRow)
				};
				Diagnostic diagnostic;
				const auto maximum =
					std::min<uint64_t>(Context.AvailableBytes(), Limits::MaximumEvaluationBytes);
				auto workspace = Context.ReserveWorkspace(maximum);
				if (!workspace) return false;
				const Status status = Context.Request.RigidProvider->Replay(
					Owner->History, Context.Request.Tick, position, maximum, snapshot, diagnostic
				);
				if (status != Status::Ok) return Context.Fail(diagnostic);
				uint64_t bytes = sizeof(SourceRigidSnapshot) +
								 snapshot.Bodies.capacity() * sizeof(SourceRigidBodyState) +
								 snapshot.Contacts.capacity() * sizeof(SourceRigidContact) +
								 snapshot.Overlaps.capacity() * sizeof(SourceRigidOverlap);
				for (const auto &body : snapshot.Bodies)
					bytes += body.Id.capacity();
				for (const auto &contact : snapshot.Contacts)
					bytes += contact.A.capacity() + contact.B.capacity();
				for (const auto &overlap : snapshot.Overlaps)
					bytes += overlap.Sensor.capacity() + overlap.Visitor.capacity();
				if (!workspace->Resize(bytes))
					return Context.Fail(Status::LimitExceeded, "rigid snapshot retained bytes exceed budget");
				SnapshotCharge = std::move(*workspace);
				// The provider owns only this synchronous snapshot. Its pixels are never retained here.
				return true;
			}
		};
		void AppendHandle(ArrayValue &array, std::string_view owner, std::string_view body) {
			RigidValue handle;
			handle.Data.emplace() = {std::string(owner), std::string(body)};
			array.Elements.emplace_back(std::move(handle));
		}
		bool Handles(const Value *value, std::vector<RigidValue> &out, NodeContext &context) {
			if (!value) return true;
			if (const auto *handle = std::get_if<RigidValue>(value)) {
				out.push_back(*handle);
				return true;
			}
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (array->Elements.size() > 256 || !array->Items.empty() || !array->Nested.empty())
					return context.Fail(
						Status::UnsupportedExecution, "rigid render requires a bounded flat object list"
					);
				for (const auto &element : array->Elements) {
					const auto *handle = std::get_if<RigidValue>(&element);
					if (!handle)
						return context.Fail(
							Status::InvalidValue, "rigid object list contains a foreign value"
						);
					out.push_back(*handle);
				}
				return true;
			}
			return context.Fail(Status::InvalidValue, "rigid input is not an object handle");
		}
		bool Inline(NodeContext &) {
			return true;
		}
		bool Object(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto *state = execution.NodeState();
			if (!state) return false;
			const Image *texture = context.Input("texture");
			const AtlasData *atlas = nullptr;
			if (const auto *value = context.Find("texture"))
				if (const auto *typed = std::get_if<AtlasValue>(value); typed && typed->Data) {
					atlas = &*typed->Data;
					texture = &atlas->Surface.Data;
				}
			if (!texture ||
				!ValidSurfaceLayout(*texture, Limits::MaximumDimension, Limits::MaximumArrayBytes))
				return context.Fail(
					Status::InvalidValue, "rigid object requires a valid captured texture", "texture"
				);
			if (context.Integer("shape") == 2)
				return context.Fail(
					Status::UnsupportedExecution,
					"rigid custom object requires a captured source mesh action",
					"attribute_mesh"
				);
			const bool spawn =
				context.Boolean("spawn", true) &&
				context.Integer("spawn_frame", 0) == static_cast<int64_t>(context.Request.Tick);
			if (spawn) {
				if (state->OutputBodyIds.size() >= 256)
					return context.Fail(Status::LimitExceeded, "rigid body count exceeds native profile");
				const std::string id = context.Authored.Id + "/" + std::to_string(context.ProcessorRow) +
									   "/" + std::to_string(state->SpawnIndex++);
				auto body = RigidControlReader{context, execution.Dimension, execution.Scale}.Object(
					id, texture->Width, texture->Height
				);
				RigidVisualState visual;
				visual.BodyId = id;
				if (!execution.Charge(texture->Pixels.size() + 128)) return false;
				visual.Texture = SurfaceValue{*texture};
				if (atlas) {
					visual.XScale = atlas->Scale.X;
					visual.YScale = atlas->Scale.Y;
					visual.BlendColour = atlas->Blend;
					visual.Alpha = atlas->Alpha;
					body.Size.X *= atlas->Scale.X;
					body.Size.Y *= atlas->Scale.Y;
					if (context.Boolean("offset_atlas", true)) {
						body.Position.X += atlas->Position.X;
						body.Position.Y += atlas->Position.Y;
					}
				}
				if (!execution.Event(std::move(body)) || !execution.Visual(std::move(visual))) return false;
				if (!execution.Charge(id.size() * 2 + 64)) return false;
				state->OutputBodyIds.push_back(id);
			}
			ArrayValue output;
			output.ElementType = ValueType::Rigid;
			if (!context.ReserveOutput(
					state->OutputBodyIds.size() * (sizeof(ElementValue) + sizeof(RigidObjectData) + 256)
				))
				return false;
			for (const auto &id : state->OutputBodyIds)
				AppendHandle(output, execution.Owner->History.OwnerId, id);
			context.SetValue("object", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool Render(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			if (!execution.Event(
					RigidControlReader{context, execution.Dimension, execution.Scale}.Step(
						context.Request.RigidPlaying
					)
				))
				return false;
			SourceRigidSnapshot snapshot;
			if (!execution.Snapshot(snapshot)) return false;
			auto inputCharge =
				context.ReserveWorkspace(256 * (sizeof(RigidValue) + sizeof(RigidObjectData) + 256));
			if (!inputCharge) return false;
			std::vector<RigidValue> handles;
			for (const auto &input : context.Values)
				if (input.first.starts_with("object"))
					if (!Handles(&input.second, handles, context)) return false;
			for (const auto &input : context.ValueViews)
				if (input.first.starts_with("object"))
					if (!Handles(input.second, handles, context)) return false;
			auto *image = context.NewImage(
				"surface_out",
				static_cast<uint32_t>(execution.Dimension.X),
				static_cast<uint32_t>(execution.Dimension.Y)
			);
			if (!image) return false;
			ArrayValue atlases;
			atlases.ElementType = ValueType::Atlas;
			for (const auto &handle : handles) {
				if (!handle.Data || handle.Data->OwnerId != execution.Owner->History.OwnerId)
					return context.Fail(
						Status::InvalidValue, "rigid render received an object from another collection"
					);
				const SourceRigidBodyState *body = nullptr;
				for (const auto &candidate : snapshot.Bodies)
					if (candidate.Id == handle.Data->BodyId) {
						body = &candidate;
						break;
					}
				if (!body) continue;
				const RigidVisualState *visual = nullptr;
				for (const auto &frame : execution.Owner->VisualFrames)
					for (const auto &mutation : frame.Mutations)
						if (mutation.Data.BodyId == body->Id) visual = &mutation.Data;
				if (!visual || !visual->Texture) continue;
				const auto &texture = visual->Texture->Data;
				if (visual->XScale == 0 || visual->YScale == 0) continue;
				const double radians = body->RotationDegrees * std::numbers::pi / 180, c = std::cos(radians),
							 s = std::sin(radians);
				const double ox = -.5 * texture.Width * visual->XScale + visual->XOffset,
							 oy = -.5 * texture.Height * visual->YScale + visual->YOffset;
				double dx = body->Position.X + ox * c - oy * s, dy = body->Position.Y + ox * s + oy * c;
				if (context.Boolean("round_position")) {
					dx = std::round(dx);
					dy = std::round(dy);
				}
				for (uint32_t y = 0; y < image->Height; ++y)
					for (uint32_t x = 0; x < image->Width; ++x) {
						const double px = x + .5 - dx, py = y + .5 - dy;
						const double u = (px * c + py * s) / visual->XScale,
									 v = (-px * s + py * c) / visual->YScale;
						if (u < 0 || v < 0 || u >= texture.Width || v >= texture.Height) continue;
						SurfacePixel source{}, destination{};
						if (!LoadSurfacePixel(
								texture, static_cast<uint32_t>(u), static_cast<uint32_t>(v), source
							) ||
							!LoadSurfacePixel(*image, x, y, destination))
							return context.Fail(
								Status::InvalidValue, "rigid visual surface cannot be sampled"
							);
						const double alpha = std::clamp(source[3] * visual->Alpha, 0., 1.);
						source[0] *= visual->BlendColour.Red / 255.;
						source[1] *= visual->BlendColour.Green / 255.;
						source[2] *= visual->BlendColour.Blue / 255.;
						for (size_t channel = 0; channel < 3; ++channel)
							destination[channel] =
								source[channel] * alpha + destination[channel] * (1 - alpha);
						destination[3] = alpha + destination[3] * (1 - alpha);
						if (!StoreSurfacePixel(*image, x, y, destination))
							return context.Fail(Status::InvalidValue, "rigid visual blend is nonfinite");
					}
				if (!context.ReserveOutput(sizeof(AtlasData) + texture.Pixels.size() + sizeof(ElementValue)))
					return false;
				AtlasValue atlas;
				auto &data = atlas.Data.emplace();
				data.Kind = AtlasKind::SurfaceAtlas;
				data.Surface = *visual->Texture;
				data.Dimension = {double(texture.Width), double(texture.Height)};
				data.Position = {dx, dy};
				data.Scale = {visual->XScale, visual->YScale};
				data.RotationDegrees = -body->RotationDegrees;
				data.Blend = visual->BlendColour;
				data.Alpha = visual->Alpha;
				atlases.Elements.emplace_back(std::move(atlas));
			}
			image->Hash = SurfaceHash(*image);
			context.SetValue("atlas_out", std::move(atlases));
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceRigidExecutors() {
		static const std::array entries{
			ExecutorEntry{"pc.rigid_group_inline", Inline},
			ExecutorEntry{"pc.rigid_object", Object},
			ExecutorEntry{"pc.rigid_render", Render}
		};
		return entries;
	}
}
