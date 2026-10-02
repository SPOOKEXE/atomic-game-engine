#include "Families.hpp"
#include "Path.hpp"
#include "SourceBuiltinRandomContext.hpp"
#include "SourceInterpret.hpp"
#include "SourceRigidControls.hpp"
#include "SourceRigidFracture.hpp"

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
				// The provider owns only this synchronous snapshot. Its pixels are never
				// retained here.
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
			const auto append = [&](const RigidValue &handle) {
				if (out.size() == 256)
					return context.Fail(Status::LimitExceeded, "rigid selected objects exceed 256 handles");
				out.push_back(handle);
				return true;
			};
			if (const auto *handle = std::get_if<RigidValue>(value)) return append(*handle);
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (!array->Nested.empty() || array->Elements.size() + array->Items.size() > 256 - out.size())
					return context.Fail(
						Status::LimitExceeded, "rigid selected objects exceed bounded flat list"
					);
				for (const auto &element : array->Elements) {
					const auto *handle = std::get_if<RigidValue>(&element);
					if (!handle)
						return context.Fail(
							Status::InvalidValue, "rigid object list contains a foreign value"
						);
					if (!append(*handle)) return false;
				}
				for (const auto &item : array->Items) {
					const auto *leaf = std::get_if<ElementValue>(&item.Data);
					const auto *handle = leaf ? std::get_if<RigidValue>(leaf) : nullptr;
					if (!handle)
						return context.Fail(
							Status::InvalidValue, "rigid source object list contains a non-handle leaf"
						);
					if (!append(*handle)) return false;
				}
				return true;
			}
			return context.Fail(Status::InvalidValue, "rigid input is not an object handle");
		}
		bool Inline(NodeContext &) {
			return true;
		}
		const RigidVisualState *LatestVisual(const RigidExecution &execution, std::string_view id) {
			const RigidVisualState *visual = nullptr;
			for (const auto &frame : execution.Owner->VisualFrames)
				for (const auto &mutation : frame.Mutations)
					if (mutation.Data.BodyId == id) visual = &mutation.Data;
			return visual;
		}
		bool Selected(RigidExecution &execution, std::string_view port, std::vector<RigidValue> &handles) {
			if (!Handles(execution.Context.Find(port), handles, execution.Context)) return false;
			for (const auto &handle : handles)
				if (!handle.Data || handle.Data->OwnerId != execution.Owner->History.OwnerId)
					return execution.Context.Fail(
						Status::InvalidValue, "rigid actor received a foreign collection object", port
					);
			return true;
		}
		bool PassObjects(NodeContext &context, std::string_view port = "object") {
			if (const auto *objects = context.Find(port))
				context.SetValue("object", *objects);
			else {
				ArrayValue empty;
				empty.ElementType = ValueType::Rigid;
				context.SetValue("object", std::move(empty));
			}
			return context.FailureCode == Status::Ok;
		}
		bool Force(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto charge =
				context.ReserveWorkspace(256 * (sizeof(RigidValue) + sizeof(RigidObjectData) + 1024));
			if (!charge) return false;
			std::vector<RigidValue> handles;
			if (!Selected(execution, "object", handles)) return false;
			const auto kind = context.Integer("force_type");
			const bool apply =
				(kind == 0 || kind == 2) ? context.Boolean("apply", true) : context.Boolean("trigger");
			if (kind < 0 || kind > 4)
				return context.Fail(
					Status::InvalidValue, "rigid force type is outside source choices", "force_type"
				);
			if (apply) {
				if (kind == 4) {
					if (!execution.Charge(handles.size() * 512)) return false;
					std::vector<std::string> ids;
					for (const auto &handle : handles)
						ids.push_back(handle.Data->BodyId);
					if (!execution.Event(
							RigidControlReader{context, execution.Dimension, execution.Scale}.Explosion(
								ids, true
							)
						))
						return false;
				} else
					for (const auto &handle : handles)
						if (!execution.Event(
								RigidControlReader{context, execution.Dimension, execution.Scale}.Force(
									handle.Data->BodyId
								)
							))
							return false;
			}
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
			return PassObjects(context);
		}
		bool Activate(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto charge =
				context.ReserveWorkspace(256 * (sizeof(RigidValue) + sizeof(RigidObjectData) + 1024));
			if (!charge) return false;
			std::vector<RigidValue> handles;
			if (!Selected(execution, "object", handles)) return false;
			if (context.Request.RigidPlaying && context.Request.RigidFrameProgress)
				for (const auto &handle : handles) {
					SourceRigidChange change;
					change.BodyId = handle.Data->BodyId;
					change.Enabled = context.Boolean("activated", true);
					if (!execution.Event(std::move(change))) return false;
				}
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
			return PassObjects(context);
		}
		bool Explode(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto charge =
				context.ReserveWorkspace(256 * (sizeof(RigidValue) + sizeof(RigidObjectData) + 1024));
			if (!charge) return false;
			std::vector<RigidValue> handles;
			if (!Selected(execution, "object", handles)) return false;
			if (context.Boolean("trigger")) {
				if (!execution.Charge(handles.size() * 512)) return false;
				std::vector<std::string> ids;
				for (const auto &handle : handles)
					ids.push_back(handle.Data->BodyId);
				if (!execution.Event(
						RigidControlReader{context, execution.Dimension, execution.Scale}.Explosion(
							ids, false
						)
					))
					return false;
			}
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
			return PassObjects(context);
		}
		bool StaticGeometry(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto *state = execution.NodeState();
			if (!state) return false;
			if (context.Request.Tick == 0) {
				RigidControlReader reader{context, execution.Dimension, execution.Scale};
				const bool wall = context.Authored.Type == "pc.rigid_wall";
				const int64_t sides = wall ? context.Integer("sides", 2) : 1;
				if (sides < 0 || sides > 15)
					return context.Fail(
						Status::InvalidValue, "rigid wall bitmask is outside four sides", "sides"
					);
				for (uint32_t side = 0; side < (wall ? 4u : 1u); ++side)
					if (sides & (1 << side)) {
						if (!execution.Charge(2048)) return false;
						const std::string id = context.Authored.Id + "/" +
											   std::to_string(context.ProcessorRow) + "/" +
											   std::to_string(side);
						auto body =
							wall
								? reader.Wall(id, side)
								: reader.Segment(
									  id, reader.Pixels("segment_start"), reader.Pixels("segment_end", {1, 0})
								  );
						if (!execution.Event(std::move(body))) return false;
						state->OutputBodyIds.push_back(id);
					}
			}
			if (!context.ReserveOutput(
					state->OutputBodyIds.size() * (sizeof(ElementValue) + sizeof(RigidObjectData) + 512)
				))
				return false;
			ArrayValue output;
			output.ElementType = ValueType::Rigid;
			for (const auto &id : state->OutputBodyIds)
				AppendHandle(output, execution.Owner->History.OwnerId, id);
			context.SetValue("object", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool Joint(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto charge =
				context.ReserveWorkspace(512 * (sizeof(RigidValue) + sizeof(RigidObjectData) + 1024));
			if (!charge) return false;
			std::vector<RigidValue> a, b;
			if (!Selected(execution, "object_a", a) || !Selected(execution, "object_b", b)) return false;
			const bool motor = context.Authored.Type == "pc.rigid_joint_rotate";
			if (a.size() == 1 && b.size() == 1 && (!motor || context.Request.Tick == 0)) {
				if (!execution.Charge(2048)) return false;
				const std::string id = context.Authored.Id + "/" + std::to_string(context.ProcessorRow) +
									   "/" + std::to_string(context.Request.Tick);
				if (!execution.Event(
						RigidControlReader{context, execution.Dimension, execution.Scale}.Joint(
							id, a[0].Data->BodyId, b[0].Data->BodyId, motor
						)
					))
					return false;
			}
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
			return PassObjects(context, "object_a");
		}
		bool Variable(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto charge =
				context.ReserveWorkspace(256 * (sizeof(RigidValue) + sizeof(RigidObjectData) + 1024));
			if (!charge) return false;
			std::vector<RigidValue> handles;
			if (!Selected(execution, "objects", handles)) return false;
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
			SourceRigidSnapshot snapshot;
			if (!execution.Snapshot(snapshot)) return false;
			std::array<ArrayValue, 8> values;
			constexpr std::array types{
				ValueType::Vector2,
				ValueType::Vector2,
				ValueType::Scalar,
				ValueType::Colour,
				ValueType::Scalar,
				ValueType::Vector2,
				ValueType::Vector2,
				ValueType::Scalar
			};
			constexpr std::array<std::string_view, 8> ports{
				"positions",
				"scales",
				"rotations",
				"blends",
				"alpha",
				"velocity",
				"center_of_mass",
				"velocity_magnitude"
			};
			if (!context.ReserveOutput(handles.size() * 8 * sizeof(ElementValue))) return false;
			for (size_t index = 0; index < values.size(); ++index) {
				values[index].ElementType = types[index];
				values[index].Elements.reserve(handles.size());
			}
			for (const auto &handle : handles) {
				const auto body =
					std::find_if(snapshot.Bodies.begin(), snapshot.Bodies.end(), [&](const auto &item) {
						return item.Id == handle.Data->BodyId;
					});
				if (body == snapshot.Bodies.end())
					return context.Fail(Status::InvalidValue, "rigid variable references an absent body");
				const auto *visual = LatestVisual(execution, body->Id);
				values[0].Elements.emplace_back(body->Position);
				values[1].Elements.emplace_back(
					visual ? Vector2{visual->XScale, visual->YScale} : Vector2{1, 1}
				);
				values[2].Elements.emplace_back(-body->RotationDegrees);
				values[3].Elements.emplace_back(visual ? visual->BlendColour : Colour{255, 255, 255, 255});
				values[4].Elements.emplace_back(visual ? visual->Alpha : 1.);
				values[5].Elements.emplace_back(body->LinearVelocity);
				values[6].Elements.emplace_back(body->WorldCentreOfMass);
				values[7].Elements.emplace_back(std::hypot(body->LinearVelocity.X, body->LinearVelocity.Y));
			}
			for (size_t index = 0; index < values.size(); ++index)
				context.SetValue(ports[index], std::move(values[index]));
			return context.FailureCode == Status::Ok;
		}

		// Source Override performs its own per-object array lookup instead of processor batching.
		bool OverrideRow(
			NodeContext &source,
			NodeContext &row,
			size_t index,
			double scale,
			std::array<Value, 10> &selected,
			size_t &count
		) {
			row.ValueViews.reserve(source.Values.size() + source.ValueViews.size() + selected.size());
			for (const auto &[port, value] : source.Values)
				row.ValueViews.emplace_back(port, &value);
			row.ValueViews.insert(row.ValueViews.end(), source.ValueViews.begin(), source.ValueViews.end());
			row.Images.reserve(source.Images.size() + 1);
			row.Images = source.Images;
			const auto element = [](const ArrayValue &array, size_t at) -> const ElementValue * {
				if (!array.Items.empty())
					return at < array.Items.size() ? std::get_if<ElementValue>(&array.Items[at].Data)
												   : nullptr;
				return at < array.Elements.size() ? &array.Elements[at] : nullptr;
			};
			const auto numeric = [](const ElementValue &value) -> std::optional<double> {
				if (const auto *v = std::get_if<double>(&value)) return *v;
				if (const auto *v = std::get_if<int64_t>(&value)) return double(*v);
				return {};
			};
			for (const auto &[port, flag] : std::array<std::pair<std::string_view, std::string_view>, 9>{
					 {{"positions", "set_positions"},
					  {"scales", "set_scales"},
					  {"rotations", "set_rotations"},
					  {"blends", "set_blends"},
					  {"alpha", "set_alpha"},
					  {"mass", "set_mass"},
					  {"friction", "set_friction"},
					  {"bounciness", "set_bounciness"},
					  {"gravity_scale", "set_gravity_scale"}}
				 }) {
				if (!source.Boolean(flag)) continue;
				const auto *raw = source.Find(port);
				const auto *array = raw ? std::get_if<ArrayValue>(raw) : nullptr;
				if (!array) continue;
				if (!array->Nested.empty())
					return source.Fail(
						Status::UnsupportedExecution,
						"rigid Override requires source arrays, not native nested batching",
						port
					);
				Value value{0.};
				if (port == "positions" || port == "scales") {
					// A native Vec2 is the source scalar pair; an array of Vec2 is the source pair list.
					const size_t length =
						!array->Items.empty() ? array->Items.size() : array->Elements.size();
					const auto *first = element(*array, 0);
					if (first && numeric(*first)) {
						const auto *second = element(*array, 1);
						const auto x = numeric(*first),
								   y = second ? numeric(*second) : std::optional<double>{};
						if (!y)
							return source.Fail(
								Status::InvalidValue,
								"rigid Override vector pair lacks its second coordinate",
								port
							);
						value = Vector2{*x, *y};
					} else {
						if (length < 2 || index >= length)
							return source.Fail(
								Status::UnsupportedExecution,
								"source rigid Override indexes an absent vector row",
								port
							);
						Vector2 vector{};
						if (const auto *leaf = element(*array, index);
							leaf && std::holds_alternative<Vector2>(*leaf))
							vector = std::get<Vector2>(*leaf);
						else {
							const auto *items =
								index < array->Items.size()
									? std::get_if<std::vector<SourceArrayItem>>(&array->Items[index].Data)
									: nullptr;
							if (!items || items->size() != 2)
								return source.Fail(
									Status::InvalidValue,
									"rigid Override vector row requires two coordinates",
									port
								);
							const auto *xleaf = std::get_if<ElementValue>(&(*items)[0].Data),
									   *yleaf = std::get_if<ElementValue>(&(*items)[1].Data);
							const auto x = xleaf ? numeric(*xleaf) : std::optional<double>{},
									   y = yleaf ? numeric(*yleaf) : std::optional<double>{};
							if (!x || !y)
								return source.Fail(
									Status::InvalidValue, "rigid Override vector row is not numeric", port
								);
							vector = {*x, *y};
						}
						if (port == "positions") {
							vector.X /= scale;
							vector.Y /= scale;
						}
						value = vector;
					}
				} else if (const auto *leaf = element(*array, index)) {
					if (port == "blends") {
						const auto colour =
							std::visit([](const auto &item) { return InterpretPackedColour(item); }, *leaf);
						if (!colour)
							return source.Fail(
								Status::InvalidValue, "rigid Override colour row is not packed colour", port
							);
						value = *colour;
					} else {
						const auto scalar = numeric(*leaf);
						if (!scalar)
							return source.Fail(
								Status::InvalidValue, "rigid Override scalar row is not numeric", port
							);
						value = *scalar;
					}
				}
				selected[count] = std::move(value);
				row.ValueViews.emplace_back(port, &selected[count++]);
			}
			if (source.Boolean("set_surfaces"))
				if (const auto *raw = source.Find("surfaces"))
					if (const auto *array = std::get_if<ArrayValue>(raw)) {
						const auto *leaf = element(*array, index);
						const Image *image = nullptr;
						if (leaf) {
							if (const auto *surface = std::get_if<SurfaceValue>(leaf)) image = &surface->Data;
							if (const auto *atlas = std::get_if<AtlasValue>(leaf); atlas && atlas->Data)
								image = &atlas->Data->Surface.Data;
						} else if (index < array->Items.size())
							image = std::get_if<Image>(&array->Items[index].Data);
						if (image)
							row.Images.emplace_back("surfaces", image);
						else if (index <
								 (!array->Items.empty() ? array->Items.size() : array->Elements.size()))
							return source.Fail(
								Status::InvalidValue,
								"rigid Override surface row is not an owned image",
								"surfaces"
							);
						else {
							selected[count] = false;
							row.ValueViews.emplace_back("set_surfaces", &selected[count++]);
						}
					}
			return true;
		}
		bool Override(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto charge =
				context.ReserveWorkspace(256 * (sizeof(RigidValue) + sizeof(RigidObjectData) + 1024));
			if (!charge) return false;
			std::vector<RigidValue> handles;
			if (!Selected(execution, "object", handles)) return false;
			for (size_t index = 0; index < handles.size(); ++index) {
				const auto &handle = handles[index];
				auto rowCharge = context.ReserveWorkspace(
					(context.Values.size() + context.ValueViews.size() + 10) *
						sizeof(std::pair<std::string_view, const Value *>) +
					(context.Images.size() + 1) * sizeof(std::pair<std::string_view, const Image *>)
				);
				if (!rowCharge) return false;
				NodeContext row(context.Authored, context.Entry, context.Request, context.AllocationBudget());
				std::array<Value, 10> selected;
				size_t count = 0;
				if (!OverrideRow(context, row, index, execution.Scale, selected, count)) return false;
				RigidControlReader reader{row, execution.Dimension, execution.Scale};
				auto change = reader.Override(handle.Data->BodyId);
				if (row.FailureCode != Status::Ok)
					return context.Fail(row.FailureCode, row.FailureMessage, row.FailurePort);
				if (!execution.Event(std::move(change))) return false;
				const auto *previous = LatestVisual(execution, handle.Data->BodyId);
				if (!previous) continue;
				if (!execution.Charge(
						sizeof(RigidVisualState) +
						(previous->Texture ? previous->Texture->Data.Pixels.size() : 0) + 512
					))
					return false;
				RigidVisualState visual = *previous;
				if (row.Boolean("set_surfaces")) {
					const auto *texture = row.Input("surfaces");
					if (!texture)
						return context.Fail(
							Status::InvalidValue,
							"rigid visual override requires a captured image",
							"surfaces"
						);
					if (!execution.Charge(texture->Pixels.size())) return false;
					visual.Texture = SurfaceValue{*texture};
				}
				if (row.Boolean("set_scales")) {
					const auto scale = row.Vec2("scales", {1, 1});
					const auto mode = row.Integer("mode");
					const double priorX = visual.XScale;
					visual.XScale = mode == 1 ? scale.X + priorX : mode == 2 ? scale.X * priorX : scale.X;
					visual.YScale = mode == 1 ? scale.Y + priorX : mode == 2 ? scale.Y * priorX : scale.Y;
				}
				if (row.Boolean("set_blends")) {
					const auto *value = row.Find("blends");
					const auto colour =
						value
							? std::visit([](const auto &leaf) { return InterpretPackedColour(leaf); }, *value)
							: std::optional<Colour>{};
					if (!colour)
						return context.Fail(
							Status::InvalidValue, "rigid blend override is not a packed colour", "blends"
						);
					visual.BlendColour = *colour;
				}
				if (row.Boolean("set_alpha")) {
					const auto alpha = row.Scalar("alpha", 1);
					visual.Alpha = row.Integer("mode_4") == 1 ? visual.Alpha + alpha : alpha;
				}
				if (row.FailureCode != Status::Ok)
					return context.Fail(row.FailureCode, row.FailureMessage, row.FailurePort);
				if (!execution.Visual(std::move(visual))) return false;
			}
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
			return PassObjects(context);
		}
		bool Sensor(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto charge =
				context.ReserveWorkspace(256 * (sizeof(RigidValue) + sizeof(RigidObjectData) + 1024));
			if (!charge) return false;
			std::vector<RigidValue> handles;
			if (!Selected(execution, "detect_objects", handles)) return false;
			auto *state = execution.NodeState();
			if (!state) return false;
			RigidControlReader reader{context, execution.Dimension, execution.Scale};
			if (context.Request.Tick == 0) {
				if (!execution.Charge(1024)) return false;
				const std::string id =
					context.Authored.Id + "/sensor/" + std::to_string(context.ProcessorRow);
				if (!execution.Event(reader.Sensor(id))) return false;
				state->OutputBodyIds.push_back(id);
			}
			if (state->OutputBodyIds.size() != 1)
				return context.Fail(Status::InvalidValue, "rigid sensor history lacks its first-frame body");
			SourceRigidChange move;
			move.BodyId = state->OutputBodyIds[0];
			const auto position = reader.Pixels("position", {.5, .5});
			move.PositionWorld = Vector2{position.X / execution.Scale, position.Y / execution.Scale};
			if (!execution.Event(std::move(move))) return false;
			SourceRigidSnapshot snapshot;
			if (!execution.Snapshot(snapshot)) return false;
			ArrayValue detected;
			detected.ElementType = ValueType::Rigid;
			if (!context.ReserveOutput(
					handles.size() * (sizeof(ElementValue) + sizeof(RigidObjectData) + 512)
				))
				return false;
			for (const auto &overlap : snapshot.Overlaps)
				if (overlap.Sensor == state->OutputBodyIds[0]) {
					if (std::any_of(handles.begin(), handles.end(), [&](const auto &handle) {
							return handle.Data->BodyId == overlap.Visitor;
						}))
						AppendHandle(detected, execution.Owner->History.OwnerId, overlap.Visitor);
				}
			context.SetValue("detected_objects", std::move(detected));
			return context.FailureCode == Status::Ok;
		}

		StructValue PointData(const SourceRigidContactPoint &point) {
			StructValue value;
			auto &fields = value.Data.emplace().Fields;
			const auto vector = [](Vector2 vector) {
				StructValue result;
				result.Data.emplace().Fields = {{"x", vector.X}, {"y", vector.Y}};
				return result;
			};
			fields = {
				{"point", vector(point.Point)},
				{"anchorA", vector(point.AnchorA)},
				{"anchorB", vector(point.AnchorB)},
				{"separation", point.Separation},
				{"normalImpulse", point.NormalImpulse},
				{"tangentImpulse", point.TangentImpulse},
				{"totalNormalImpulse", point.TotalNormalImpulse},
				{"normalVelocity", point.NormalVelocity},
				{"id", int64_t(point.Id)},
				{"persisted", point.Persisted}
			};
			return value;
		}
		StructValue ContactData(const SourceRigidContact &contact) {
			StructValue value;
			auto &fields = value.Data.emplace().Fields;
			StructValue normal;
			normal.Data.emplace().Fields = {{"x", contact.Normal.X}, {"y", contact.Normal.Y}};
			fields = {
				{"shapeA_id", contact.A},
				{"shapeB_id", contact.B},
				{"mani_normal", std::move(normal)},
				{"point_count", int64_t(contact.PointCount)},
				{"mani_points_0", PointData(contact.Points[0])},
				{"mani_points_1", PointData(contact.Points[1])},
				{"rolling_impulse", contact.RollingImpulse}
			};
			return value;
		}
		bool Collision(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto charge =
				context.ReserveWorkspace(512 * (sizeof(RigidValue) + sizeof(RigidObjectData) + 1024));
			if (!charge) return false;
			std::vector<RigidValue> handles, filters;
			if (!Selected(execution, "objects", handles) || !Selected(execution, "filter_object", filters))
				return false;
			auto *state = execution.NodeState();
			if (!state) return false;
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
			SourceRigidSnapshot snapshot;
			if (!execution.Snapshot(snapshot)) return false;
			const uint64_t maximum = handles.size() * snapshot.Contacts.size();
			if (maximum > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "rigid collision records exceed the bounded source list"
				);
			if (!context.ReserveOutput(maximum * (8192 + 5 * sizeof(ElementValue)))) return false;
			ArrayValue data, points, normals, newPoints, newNormals;
			data.ElementType = ValueType::Struct;
			points.ElementType = normals.ElementType = newPoints.ElementType = newNormals.ElementType =
				ValueType::Vector2;
			bool anyNew = false;
			for (const auto &handle : handles)
				for (const auto &contact : snapshot.Contacts) {
					if (contact.A != handle.Data->BodyId && contact.B != handle.Data->BodyId) continue;
					if (!filters.empty() &&
						!std::any_of(filters.begin(), filters.end(), [&](const auto &filter) {
							return filter.Data->BodyId == contact.A || filter.Data->BodyId == contact.B;
						}))
						continue;
					auto pair = std::find_if(
						state->CollisionPairs.begin(), state->CollisionPairs.end(), [&](const auto &item) {
							return item.A == contact.A && item.B == contact.B;
						}
					);
					const bool isNew = pair == state->CollisionPairs.end();
					anyNew = anyNew || isNew;
					if (isNew) {
						if (!execution.Charge(sizeof(RigidCollisionPairState) * 2 + 1024)) return false;
						state->CollisionPairs.push_back({contact.A, contact.B, context.Request.Tick});
					} else
						pair->LastObservedTick = context.Request.Tick;
					data.Elements.emplace_back(ContactData(contact));
					if (!contact.PointCount) continue;
					Vector2 position = contact.Points[0].Point;
					if (contact.PointCount == 2) {
						position.X = (position.X + contact.Points[1].Point.X) * .5;
						position.Y = (position.Y + contact.Points[1].Point.Y) * .5;
					}
					position.X *= execution.Scale;
					position.Y *= execution.Scale;
					points.Elements.emplace_back(position);
					normals.Elements.emplace_back(contact.Normal);
					if (isNew) {
						newPoints.Elements.emplace_back(position);
						newNormals.Elements.emplace_back(contact.Normal);
					}
				}
			std::erase_if(state->CollisionPairs, [&](const auto &pair) {
				return pair.LastObservedTick != context.Request.Tick;
			});
			context.SetValue("collision_data", std::move(data));
			context.SetValue("collision_points", std::move(points));
			context.SetValue("collision_normals", std::move(normals));
			context.SetValue("new_collision_trigger", anyNew);
			context.SetValue("new_collision_points", std::move(newPoints));
			context.SetValue("new_collision_normals", std::move(newNormals));
			return context.FailureCode == Status::Ok;
		}

		bool Fracture(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto *state = execution.NodeState();
			if (!state) return false;
			if (context.Request.Tick == 0) {
				const auto *base = context.Input("base_texture"), *map = context.Input("fracture_map");
				if (!base || !map)
					return context.Fail(
						Status::InvalidValue, "rigid fracture requires captured base texture and map"
					);
				const uint64_t pixels = uint64_t(base->Width) * base->Height;
				const uint64_t admission = pixels * 2400 + 256 * sizeof(RigidFracturePiece);
				auto workspace = context.ReserveWorkspace(admission);
				if (!workspace) return false;
				std::vector<RigidFracturePiece> pieces;
				Diagnostic diagnostic;
				const auto status = SourceRigidFracture(
					*base,
					*map,
					{context.Scalar("fracture_threshold", .1), context.Scalar("mesh_expansion")},
					admission,
					pieces,
					diagnostic
				);
				if (status != Status::Ok) return context.Fail(diagnostic);
				RigidControlReader reader{context, execution.Dimension, execution.Scale};
				const auto position = reader.Pixels("position", {.5, .5});
				std::vector<Vector2> origins;
				origins.reserve(pieces.size());
				for (size_t index = 0; index < pieces.size(); ++index) {
					if (!execution.Charge(
							sizeof(SourceRigidBody) + sizeof(RigidVisualState) +
							pieces[index].Texture.Pixels.size() + 2048
						))
						return false;
					const std::string id = context.Authored.Id + "/" + std::to_string(context.ProcessorRow) +
										   "/" + std::to_string(index);
					const auto origin = Vector2{
						position.X - base->Width * .5 + pieces[index].Origin.X,
						position.Y - base->Height * .5 + pieces[index].Origin.Y
					};
					auto body = reader.Fragment(id, origin, pieces[index].Points);
					RigidVisualState visual;
					visual.BodyId = id;
					visual.XOffset = pieces[index].Texture.Width * .5;
					visual.YOffset = pieces[index].Texture.Height * .5;
					visual.Texture = SurfaceValue{std::move(pieces[index].Texture)};
					if (!execution.Event(std::move(body)) || !execution.Visual(std::move(visual)))
						return false;
					state->OutputBodyIds.push_back(id);
					origins.push_back(origin);
				}
				if (context.Boolean("use_joint") && origins.size() > 1) {
					struct Edge {
						size_t A, B;
						double Distance;
					};
					if (!execution.Charge(origins.size() * origins.size() * sizeof(Edge))) return false;
					std::vector<Edge> edges;
					edges.reserve(origins.size() * (origins.size() - 1) / 2);
					std::vector<size_t> roots(origins.size());
					for (size_t i = 0; i < origins.size(); ++i) {
						roots[i] = i;
						for (size_t j = i + 1; j < origins.size(); ++j)
							edges.push_back(
								{i, j, std::hypot(origins[i].X - origins[j].X, origins[i].Y - origins[j].Y)}
							);
					}
					std::stable_sort(edges.begin(), edges.end(), [](const auto &a, const auto &b) {
						return a.Distance < b.Distance;
					});
					const auto root = [&](size_t index) {
						while (roots[index] != index)
							index = roots[index];
						return index;
					};
					size_t jointIndex = 0;
					for (const auto &edge : edges) {
						const size_t a = root(edge.A), b = root(edge.B);
						if (a == b) continue;
						roots[a] = b;
						SourceRigidJoint joint;
						joint.Id = context.Authored.Id + "/weld/" + std::to_string(context.ProcessorRow) +
								   "/" + std::to_string(jointIndex++);
						joint.BodyA = state->OutputBodyIds[edge.A];
						joint.BodyB = state->OutputBodyIds[edge.B];
						joint.Stiffness = context.Scalar("stiffness", 10);
						joint.Damping = context.Scalar("damping", .5);
						joint.BreakForce = context.Scalar("breaking_force");
						if (!execution.Event(std::move(joint))) return false;
					}
				}
			}
			if (!context.ReserveOutput(
					state->OutputBodyIds.size() * (sizeof(ElementValue) + sizeof(RigidObjectData) + 512)
				))
				return false;
			ArrayValue output;
			output.ElementType = ValueType::Rigid;
			for (const auto &id : state->OutputBodyIds)
				AppendHandle(output, execution.Owner->History.OwnerId, id);
			context.SetValue("object", std::move(output));
			return context.FailureCode == Status::Ok;
		}

		bool PathCollider(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto *state = execution.NodeState();
			if (!state) return false;
			if (context.Request.Tick == 0) {
				const auto *raw = context.Find("path");
				const auto *path = raw ? std::get_if<Path2D>(raw) : nullptr;
				if (!path)
					return context.Fail(
						Status::InvalidValue, "rigid path collider requires a source path", "path"
					);
				const auto samples = context.Integer("samples", 8);
				if (samples < 2 || samples > 256)
					return context.Fail(
						Status::InvalidValue, "rigid path samples must be 2 through256", "samples"
					);
				PathRuntime runtime;
				if (!runtime.Init(context, *path)) return false;
				auto point = runtime.PointRatio(0);
				Vector2 previous{point.X, point.Y};
				RigidControlReader reader{context, execution.Dimension, execution.Scale};
				for (int64_t index = 1; index <= samples; ++index) {
					point = runtime.PointRatio(std::clamp(double(index) / double(samples - 1), 0., .999));
					if (context.FailureCode != Status::Ok) return false;
					const Vector2 next{point.X, point.Y};
					if (!execution.Charge(sizeof(SourceRigidBody) + sizeof(ElementValue) + 1024))
						return false;
					const std::string id = context.Authored.Id + "/" + std::to_string(context.ProcessorRow) +
										   "/" + std::to_string(index - 1);
					auto body = reader.Segment(id, previous, next);
					// Preserve the source list entry as a native static identity when its
					// endpoints coincide.
					if (previous == next) {
						body.Shape = SourceRigidShape::Empty;
						body.Points.clear();
					}
					if (!execution.Event(std::move(body))) return false;
					state->OutputBodyIds.push_back(id);
					previous = next;
				}
			}
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
			ArrayValue objects;
			objects.ElementType = ValueType::Rigid;
			if (!context.ReserveOutput(
					state->OutputBodyIds.size() * (sizeof(ElementValue) + sizeof(RigidObjectData) + 256)
				))
				return false;
			for (const auto &id : state->OutputBodyIds)
				AppendHandle(objects, execution.Owner->History.OwnerId, id);
			context.SetValue("object", std::move(objects));
			return context.FailureCode == Status::Ok;
		}
		bool Object(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto *state = execution.NodeState();
			if (!state) return false;
			auto workspace =
				context.ReserveWorkspace(256 * (sizeof(const Image *) + sizeof(const AtlasData *)));
			if (!workspace) return false;
			std::vector<std::pair<const Image *, const AtlasData *>> textures;
			textures.reserve(256);
			const auto addLeaf = [&](const ElementValue &leaf) -> bool {
				if (const auto *atlas = std::get_if<AtlasValue>(&leaf); atlas && atlas->Data) {
					textures.emplace_back(&atlas->Data->Surface.Data, &*atlas->Data);
					return true;
				}
				if (const auto *surface = std::get_if<SurfaceValue>(&leaf)) {
					textures.emplace_back(&surface->Data, nullptr);
					return true;
				}
				return context.Fail(
					Status::InvalidValue, "rigid texture array has a non-surface leaf", "texture"
				);
			};
			if (const auto *raw = context.Find("texture")) {
				if (const auto *array = std::get_if<ArrayValue>(raw)) {
					if (array->Elements.size() + array->Items.size() > 256)
						return context.Fail(
							Status::LimitExceeded,
							"rigid texture list exceeds 256 before allocation",
							"texture"
						);
					if (!array->Nested.empty())
						return context.Fail(
							Status::UnsupportedExecution, "rigid texture list must be flat", "texture"
						);
					for (const auto &leaf : array->Elements)
						if (!addLeaf(leaf)) return false;
					for (const auto &item : array->Items) {
						if (const auto *image = std::get_if<Image>(&item.Data))
							textures.emplace_back(image, nullptr);
						else if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
							if (!addLeaf(*leaf)) return false;
						} else
							return context.Fail(
								Status::UnsupportedExecution, "rigid texture list must be flat", "texture"
							);
					}
				} else if (const auto *atlas = std::get_if<AtlasValue>(raw); atlas && atlas->Data)
					textures.emplace_back(&atlas->Data->Surface.Data, &*atlas->Data);
				else if (const auto *surface = std::get_if<SurfaceValue>(raw))
					textures.emplace_back(&surface->Data, nullptr);
			}
			if (textures.empty())
				for (const auto &[port, array] : context.ImageArrays)
					if (port == "texture" && array) {
						if (array->Items.size() > 256)
							return context.Fail(
								Status::LimitExceeded,
								"rigid captured texture array exceeds 256 before allocation",
								"texture"
							);
						for (const auto &item : array->Items) {
							const auto *index = std::get_if<size_t>(&item.Data);
							if (!index || *index >= array->Images.size())
								return context.Fail(
									Status::InvalidValue,
									"rigid image array texture reference is invalid",
									"texture"
								);
							textures.emplace_back(&array->Images[*index], nullptr);
						}
					}
			if (textures.empty())
				if (const auto *image = context.Input("texture")) textures.emplace_back(image, nullptr);
			if (textures.size() > 256)
				return context.Fail(
					Status::LimitExceeded, "rigid texture list exceeds native body budget", "texture"
				);
			if (!execution.Charge(sizeof(RigidSpawnRecipe) * 2 + context.Authored.Id.size() + 256))
				return false;
			auto &recipes = execution.Owner->NodeFrames.at(context.Request.Tick).Recipes;
			recipes.emplace_back();
			auto &recipe = recipes.back();
			recipe.NodeId = context.Authored.Id;
			recipe.ProcessorRow = context.ProcessorRow;
			recipe.Tick = context.Request.Tick;
			const RigidControlReader reader{context, execution.Dimension, execution.Scale};
			for (size_t index = 0; index < textures.size(); ++index) {
				const auto &[texture, atlas] = textures[index];
				if (!ValidSurfaceLayout(*texture, Limits::MaximumDimension, Limits::MaximumArrayBytes))
					return context.Fail(Status::InvalidValue, "rigid object texture is invalid", "texture");
				if (!execution.Charge(
						sizeof(SourceRigidBody) * 2 + sizeof(RigidVisualState) * 2 + texture->Pixels.size() +
						2048
					))
					return false;
				auto body = reader.Object(
					context.Authored.Id + "/prototype/" + std::to_string(index),
					texture->Width,
					texture->Height
				);
				body.Position = {};
				RigidVisualState visual;
				visual.BodyId = body.Id;
				visual.Texture = SurfaceValue{*texture};
				if (atlas) {
					visual.XScale = atlas->Scale.X;
					visual.YScale = atlas->Scale.Y;
					visual.BlendColour = atlas->Blend;
					visual.Alpha = atlas->Alpha;
					body.Size.X *= atlas->Scale.X;
					body.Size.Y *= atlas->Scale.Y;
					if (context.Boolean("offset_atlas", true)) body.Position = atlas->Position;
				}
				if (context.Integer("shape") == 2) {
					const auto *raw = context.Find("attribute_mesh");
					const auto *meshes = raw ? std::get_if<ArrayValue>(raw) : nullptr;
					if (!meshes || index >= meshes->Items.size())
						return context.Fail(
							Status::UnsupportedExecution,
							"rigid custom object requires owned captured mesh action",
							"attribute_mesh"
						);
					const auto *points =
						std::get_if<std::vector<SourceArrayItem>>(&meshes->Items[index].Data);
					if (!points || points->size() < 3 || points->size() > 8)
						return context.Fail(
							Status::InvalidValue,
							"rigid polygon requires3through8 captured points",
							"attribute_mesh"
						);
					for (const auto &item : *points) {
						Vector2 point{};
						if (const auto *leaf = std::get_if<ElementValue>(&item.Data);
							leaf && std::holds_alternative<Vector2>(*leaf))
							point = std::get<Vector2>(*leaf);
						else {
							const auto *coordinates = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
							if (!coordinates || coordinates->size() != 2)
								return context.Fail(
									Status::InvalidValue,
									"rigid mesh point requires two coordinates",
									"attribute_mesh"
								);
							const auto coordinate = [&](size_t component) -> std::optional<double> {
								const auto *v = std::get_if<ElementValue>(&(*coordinates)[component].Data);
								if (!v) return {};
								if (const auto *d = std::get_if<double>(v)) return *d;
								if (const auto *i = std::get_if<int64_t>(v)) return double(*i);
								return {};
							};
							const auto x = coordinate(0), y = coordinate(1);
							if (!x || !y)
								return context.Fail(
									Status::InvalidValue,
									"rigid mesh coordinates must be numeric",
									"attribute_mesh"
								);
							point = {*x, *y};
						}
						body.Points.push_back({point.X - body.Size.X * .5, point.Y - body.Size.Y * .5});
					}
				}
				recipe.Prototypes.push_back(std::move(body));
				recipe.Visuals.push_back(std::move(visual));
			}
			if (context.Boolean("spawn", true) &&
				context.Integer("spawn_frame", 0) == static_cast<int64_t>(context.Request.Tick) &&
				!recipe.Prototypes.empty()) {
				state->OutputBodyIds.clear();
				for (size_t slot = 0; slot < recipe.Prototypes.size(); ++slot) {
					const uint64_t index = state->SpawnIndex++;
					if (!execution.Charge(
							sizeof(SourceRigidBody) + sizeof(RigidVisualState) +
							recipe.Visuals[slot].Texture->Data.Pixels.size() + 2048
						))
						return false;
					auto body = recipe.Prototypes[slot];
					auto visual = recipe.Visuals[slot];
					body.Id = context.Authored.Id + "/" + std::to_string(context.ProcessorRow) + "/" +
							  std::to_string(index);
					visual.BodyId = body.Id;
					const auto position = reader.Pixels("spawn_position", {.5, .5});
					body.Position = {position.X + body.Position.X, position.Y + body.Position.Y};
					const auto id = body.Id;
					if (!execution.Event(std::move(body)) || !execution.Visual(std::move(visual)))
						return false;
					state->OutputBodyIds.push_back(id);
				}
			}
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
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
		bool SourceRigidSampleColor(NodeContext &context, double position, Colour &output) {
			const auto *entry = FindCatalogueEntry("pc.gradient_out");
			Node node{context.Authored.Id + "/gradient", "pc.gradient_out", "", {}, {}};
			NodeContext sample(node, *entry, context.Request, context.AllocationBudget());
			sample.ByteBudget = context.AvailableBytes();
			sample.ValueViews.emplace_back("gradient", context.Find("random_color"));
			sample.Values.emplace_back("sample", position);
			const auto executor = FindExecutor(node.Type);
			if (!executor || !executor(sample))
				return context.Fail(sample.FailureCode, sample.FailureMessage, "random_color");
			for (const auto &value : sample.OutputValues)
				if (value.Port == "color")
					if (const auto *color = std::get_if<Colour>(&value.Data)) {
						output = *color;
						return true;
					}
			return context.Fail(
				Status::InvalidValue, "source gradient returned no packed color", "random_color"
			);
		}
		bool Spawner(NodeContext &context) {
			RigidExecution execution{context};
			if (!execution.Begin()) return false;
			auto *state = execution.NodeState();
			if (!state) return false;
			const auto type = context.Integer("spawn_type");
			if (type < 0 || type > 1)
				return context.Fail(
					Status::InvalidValue, "rigid spawn type is outside source choices", "spawn_type"
				);
			const auto delay = context.Integer("spawn_delay", 4), amount = context.Integer("spawn_amount", 1);
			if (amount > 256)
				return context.Fail(
					Status::LimitExceeded, "rigid stream amount exceeds native256body budget", "spawn_amount"
				);
			const bool spawn = context.Boolean("spawn", true) && amount > 0 &&
							   (type == 1 || delay == 0 || int64_t(context.Request.Tick) % delay == 0);
			if (spawn) {
				if (type == 1)
					return context.Fail(
						Status::UnsupportedExecution,
						"source Burst compares current frame with a junction reference; exact source "
						"comparison is unverified",
						"spawn_type"
					);
				const auto *producer = context.InputProducer("object");
				if (!producer)
					return context.Fail(
						Status::UnsupportedExecution,
						"rigid spawner requires an effective source producer even for an empty object list",
						"object"
					);
				const std::string_view producerId = producer->FromNode;
				const RigidSpawnRecipe *recipe = nullptr;
				for (const auto &candidate : execution.Owner->NodeFrames.at(context.Request.Tick).Recipes)
					if (candidate.NodeId == producerId && candidate.ProcessorRow == 0) recipe = &candidate;
				if (!recipe)
					return context.Fail(
						Status::UnsupportedExecution,
						"rigid source producer has no current captured spawn recipe",
						"object"
					);
				if (recipe->Prototypes.size() != recipe->Visuals.size())
					return context.Fail(
						Status::InvalidValue, "rigid spawn recipe visuals disagree with its bodies", "object"
					);
				if (recipe->Prototypes.empty())
					return context.Fail(
						Status::UnsupportedExecution,
						"rigid source producer has no valid source texture",
						"object"
					);
				const auto *raw = context.Find("spawn_area");
				const auto *authoredArea = raw ? std::get_if<Area>(raw) : nullptr;
				if (!authoredArea)
					return context.Fail(
						Status::InvalidValue,
						"rigid spawn area must be one resolved source area",
						"spawn_area"
					);
				Area area = *authoredArea;
				const bool linked =
					std::find(context.LinkedValues.begin(), context.LinkedValues.end(), "spawn_area") !=
					context.LinkedValues.end();
				if (!linked && context.Integer("spawn_area_unit", 1) == 1) {
					area.CenterX *= execution.Dimension.X;
					area.CenterY *= execution.Dimension.Y;
					area.HalfWidth *= execution.Dimension.X;
					area.HalfHeight *= execution.Dimension.Y;
				}
				if (area.Shape > 1)
					return context.Fail(
						Status::InvalidValue, "rigid spawn area has an unknown source shape", "spawn_area"
					);
				const auto alpha = context.Vec2("alpha", {1, 1});
				if (!std::isfinite(alpha.X) || !std::isfinite(alpha.Y) || alpha.X > alpha.Y)
					return context.Fail(Status::InvalidValue, "rigid spawn alpha range is invalid", "alpha");
				const auto *colorRaw = context.Find("random_color");
				const auto *gradient = colorRaw ? std::get_if<Gradient>(colorRaw) : nullptr;
				if (!gradient)
					return context.Fail(
						Status::InvalidValue, "rigid spawn requires one resolved gradient", "random_color"
					);
				const SourceBuiltinRandomCapture *capture = nullptr;
				if (!FindSourceBuiltinRandomCapture(context, capture)) return false;
				size_t cursor = 0;
				const auto draw = [&](SourceBuiltinRandomOperation operation,
									  double lower,
									  double upper) -> std::optional<double> {
					if (cursor >= capture->Draws.size()) {
						context.Fail(
							Status::InvalidValue, "rigid spawner recording omits source RNG calls", "seed"
						);
						return {};
					}
					const auto &item = capture->Draws[cursor++];
					if (item.Operation != operation || item.Lower != lower || item.Upper != upper ||
						!std::isfinite(item.Result) ||
						(operation != SourceBuiltinRandomOperation::SeedObservation &&
						 (item.Result < lower || item.Result > upper))) {
						context.Fail(
							Status::InvalidValue, "rigid spawner RNG call signature is stale", "seed"
						);
						return {};
					}
					return item.Result;
				};
				for (int64_t piece = 0; piece < amount; ++piece) {
					if (state->OutputBodyIds.size() >= 256)
						return context.Fail(
							Status::LimitExceeded, "rigid spawner body list exceeds native budget"
						);
					const auto seed = draw(SourceBuiltinRandomOperation::SeedObservation, 0, 0);
					if (!seed) return false;
					const double fraction = *seed - std::floor(*seed);
					Vector2 position{};
					if (area.Shape == 0) {
						const auto x0 = draw(SourceBuiltinRandomOperation::Random, 0, 1),
								   x1 = draw(SourceBuiltinRandomOperation::Random, 0, 1),
								   y0 = draw(SourceBuiltinRandomOperation::Random, 0, 1),
								   y1 = draw(SourceBuiltinRandomOperation::Random, 0, 1);
						if (!x0 || !x1 || !y0 || !y1) return false;
						position = {
							area.CenterX +
								(-area.HalfWidth + 2 * area.HalfWidth * (*x0 + (*x1 - *x0) * fraction)),
							area.CenterY +
								(-area.HalfHeight + 2 * area.HalfHeight * (*y0 + (*y1 - *y0) * fraction))
						};
					} else {
						const auto angle = draw(SourceBuiltinRandomOperation::Random, 0, 360),
								   x0 = draw(SourceBuiltinRandomOperation::Random, 0, 1),
								   x1 = draw(SourceBuiltinRandomOperation::Random, 0, 1),
								   y0 = draw(SourceBuiltinRandomOperation::Random, 0, 1),
								   y1 = draw(SourceBuiltinRandomOperation::Random, 0, 1);
						if (!angle || !x0 || !x1 || !y0 || !y1) return false;
						const auto radians = *angle * std::numbers::pi / 180;
						position = {
							area.CenterX +
								std::cos(radians) * area.HalfWidth * (*x0 + (*x1 - *x0) * fraction),
							area.CenterY -
								std::sin(radians) * area.HalfHeight * (*y0 + (*y1 - *y0) * fraction)
						};
					}
					const auto slot = state->SpawnIndex % recipe->Prototypes.size();
					if (!execution.Charge(
							sizeof(SourceRigidBody) + sizeof(RigidVisualState) +
							recipe->Visuals[slot].Texture->Data.Pixels.size() + 2048
						))
						return false;
					auto body = recipe->Prototypes[slot];
					auto visual = recipe->Visuals[slot];
					body.Id = context.Authored.Id + "/" + std::to_string(context.ProcessorRow) + "/" +
							  std::to_string(state->SpawnIndex++);
					visual.BodyId = body.Id;
					body.Position = {position.X + body.Position.X, position.Y + body.Position.Y};
					const auto progress = draw(SourceBuiltinRandomOperation::Random, 0, 1),
							   opacity = draw(SourceBuiltinRandomOperation::RandomRange, alpha.X, alpha.Y);
					if (!progress || !opacity) return false;
					if (!SourceRigidSampleColor(context, *progress, visual.BlendColour)) return false;
					visual.Alpha = *opacity;
					const auto id = body.Id;
					if (!execution.Event(std::move(body)) || !execution.Visual(std::move(visual)))
						return false;
					state->OutputBodyIds.push_back(id);
				}
				if (cursor != capture->Draws.size())
					return context.Fail(
						Status::InvalidValue, "rigid spawner recording contains surplus RNG calls", "seed"
					);
			}
			if (!execution.Event(SourceRigidEvent::Checkpoint{})) return false;
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
		bool RigidRasterWork(NodeContext &context, Vector2 dimension, size_t objects) {
			constexpr uint64_t maximumPixels = 64000000;
			const uint64_t width = static_cast<uint32_t>(dimension.X),
						   height = static_cast<uint32_t>(dimension.Y);
			const uint64_t rows = std::max<size_t>(1, context.ProcessorCount),
						   count = std::max<size_t>(1, objects);
			if (width * height > maximumPixels / count / rows)
				return context.Fail(
					Status::LimitExceeded,
					"rigid CPU raster exceeds 64 million pixels across processor rows",
					"surface_out"
				);
			return true;
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
			if (!RigidRasterWork(context, execution.Dimension, handles.size())) return false;
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
		bool RenderId(NodeContext &context) {
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
			std::vector<size_t> normalizers;
			const auto collect = [&](const Value *raw) -> bool {
				const auto *array = raw ? std::get_if<ArrayValue>(raw) : nullptr;
				if (!array) return true;
				const auto before = handles.size();
				if (!Handles(raw, handles, context)) return false;
				normalizers.insert(
					normalizers.end(),
					handles.size() - before,
					array->Items.empty() ? array->Elements.size() : array->Items.size()
				);
				return true;
			};
			for (const auto &input : context.Values)
				if (input.first.starts_with("object"))
					if (!collect(&input.second)) return false;
			for (const auto &input : context.ValueViews)
				if (input.first.starts_with("object"))
					if (!collect(input.second)) return false;
			if (!RigidRasterWork(context, execution.Dimension, handles.size())) return false;
			auto *image = context.NewImage(
				"surface_out",
				static_cast<uint32_t>(execution.Dimension.X),
				static_cast<uint32_t>(execution.Dimension.Y),
				context.Boolean("normalize_output", true) ? SurfaceFormat::RGBA8Unorm
														  : SurfaceFormat::R16Float
			);
			if (!image) return false;
			const SourceBuiltinRandomCapture *capture = nullptr;
			size_t drawIndex = 0, handleIndex = 0;
			int64_t nextIndex = 1;
			for (const auto &handle : handles) {
				const auto groupSize = normalizers.at(handleIndex++);
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
				if (!capture && !FindSourceBuiltinRandomCapture(context, capture)) return false;
				if (drawIndex >= capture->Draws.size())
					return context.Fail(Status::InvalidValue, "rigid ID RNG recording omits a draw", "seed");
				const auto &draw = capture->Draws[drawIndex++];
				if (draw.Operation != SourceBuiltinRandomOperation::Random || draw.Lower != 0 ||
					draw.Upper != 1 || !std::isfinite(draw.Result) || draw.Result < 0 || draw.Result > 1)
					return context.Fail(
						Status::InvalidValue, "rigid ID random draw does not match source random(1)", "seed"
					);
				const double scaled = draw.Result * 255, lower = std::floor(scaled),
							 fraction = scaled - lower;
				const double grey =
					(lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.) != 0))) / 255.;
				const double colorIndex = context.Boolean("normalize_output", true)
											  ? double(nextIndex) / groupSize
											  : double(nextIndex);
				++nextIndex;
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
						for (size_t channel = 0; channel < 3; ++channel)
							source[channel] = colorIndex * source[3] * grey;
						for (size_t channel = 0; channel < 3; ++channel)
							destination[channel] =
								source[channel] * alpha + destination[channel] * (1 - alpha);
						destination[3] = alpha + destination[3] * (1 - alpha);
						if (!StoreSurfacePixel(*image, x, y, destination))
							return context.Fail(Status::InvalidValue, "rigid visual blend is nonfinite");
					}
			}

			image->Hash = SurfaceHash(*image);
			if (capture && drawIndex != capture->Draws.size())
				return context.Fail(
					Status::InvalidValue, "rigid ID RNG recording contains surplus draws", "seed"
				);
			context.SetValue("amounts", nextIndex);
			return context.FailureCode == Status::Ok;
		}
	} // namespace
	std::span<const ExecutorEntry> SourceRigidExecutors() {
		static const std::array entries{
			ExecutorEntry{"pc.rigid_group_inline", Inline},
			ExecutorEntry{"pc.rigid_object", Object},
			ExecutorEntry{"pc.rigid_render", Render},
			ExecutorEntry{"pc.rigid_force_apply", Force},
			ExecutorEntry{"pc.rigid_explode", Explode},
			ExecutorEntry{"pc.rigid_activate", Activate},
			ExecutorEntry{"pc.rigid_object_segment", StaticGeometry},
			ExecutorEntry{"pc.rigid_wall", StaticGeometry},
			ExecutorEntry{"pc.rigid_joint_fix", Joint},
			ExecutorEntry{"pc.rigid_joint_rotate", Joint},
			ExecutorEntry{"pc.rigid_variable", Variable},
			ExecutorEntry{"pc.rigid_override", Override},
			ExecutorEntry{"pc.rigid_sensor", Sensor},
			ExecutorEntry{"pc.rigid_object_get_collision", Collision},
			ExecutorEntry{"pc.rigid_fracture", Fracture},
			ExecutorEntry{"pc.rigid_path_collider", PathCollider},
			ExecutorEntry{"pc.rigid_render_id", RenderId},
			ExecutorEntry{"pc.rigid_object_spawner", Spawner}
		};
		return entries;
	}
} // namespace engine::imagegraph::detail
