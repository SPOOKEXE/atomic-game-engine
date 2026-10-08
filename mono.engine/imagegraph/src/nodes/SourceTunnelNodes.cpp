#include "../SourceRetainedImageOutputs.hpp"
#include "../SourceTunnel.hpp"
#include "ArraySource.hpp"
#include "Families.hpp"

#include <array>

namespace engine::imagegraph::detail {
	namespace {
		constexpr std::array DisplayNames{
			std::pair{SourceValueDisplay::Default, std::string_view{"Default"}},
			std::pair{SourceValueDisplay::Range, std::string_view{"Range"}},
			std::pair{SourceValueDisplay::Rotation, std::string_view{"Rotation"}},
			std::pair{SourceValueDisplay::RotationRange, std::string_view{"RotationRange"}},
			std::pair{SourceValueDisplay::Slider, std::string_view{"Slider"}},
			std::pair{SourceValueDisplay::SliderRange, std::string_view{"SliderRange"}},
			std::pair{SourceValueDisplay::Padding, std::string_view{"Padding"}},
			std::pair{SourceValueDisplay::Vector, std::string_view{"Vector"}},
			std::pair{SourceValueDisplay::VectorRange, std::string_view{"VectorRange"}},
			std::pair{SourceValueDisplay::Area, std::string_view{"Area"}},
			std::pair{SourceValueDisplay::EnumButton, std::string_view{"EnumButton"}},
			std::pair{SourceValueDisplay::EnumScroll, std::string_view{"EnumScroll"}},
			std::pair{SourceValueDisplay::Seed, std::string_view{"Seed"}},
			std::pair{SourceValueDisplay::Palette, std::string_view{"Palette"}},
			std::pair{SourceValueDisplay::Curve, std::string_view{"Curve"}}
		};
		constexpr std::array KindNames{
			std::pair{SourceSocketKind::Integer, std::string_view{"Integer"}},
			std::pair{SourceSocketKind::Float, std::string_view{"Float"}},
			std::pair{SourceSocketKind::Boolean, std::string_view{"Boolean"}},
			std::pair{SourceSocketKind::Colour, std::string_view{"Colour"}},
			std::pair{SourceSocketKind::Surface, std::string_view{"Surface"}},
			std::pair{SourceSocketKind::FilePath, std::string_view{"FilePath"}},
			std::pair{SourceSocketKind::Curve, std::string_view{"Curve"}},
			std::pair{SourceSocketKind::Text, std::string_view{"Text"}},
			std::pair{SourceSocketKind::Object, std::string_view{"Object"}},
			std::pair{SourceSocketKind::Node, std::string_view{"Node"}},
			std::pair{SourceSocketKind::Any, std::string_view{"Any"}},
			std::pair{SourceSocketKind::Path, std::string_view{"Path"}},
			std::pair{SourceSocketKind::Particle, std::string_view{"Particle"}},
			std::pair{SourceSocketKind::Rigid, std::string_view{"Rigid"}},
			std::pair{SourceSocketKind::SmokeDomain, std::string_view{"SmokeDomain"}},
			std::pair{SourceSocketKind::Struct, std::string_view{"Struct"}},
			std::pair{SourceSocketKind::Strand, std::string_view{"Strand"}},
			std::pair{SourceSocketKind::Mesh2D, std::string_view{"Mesh2D"}},
			std::pair{SourceSocketKind::Trigger, std::string_view{"Trigger"}},
			std::pair{SourceSocketKind::Mesh3D, std::string_view{"Mesh3D"}},
			std::pair{SourceSocketKind::Light3D, std::string_view{"Light3D"}},
			std::pair{SourceSocketKind::Camera3D, std::string_view{"Camera3D"}},
			std::pair{SourceSocketKind::Scene3D, std::string_view{"Scene3D"}},
			std::pair{SourceSocketKind::Material3D, std::string_view{"Material3D"}},
			std::pair{SourceSocketKind::PcxNode, std::string_view{"PcxNode"}},
			std::pair{SourceSocketKind::Audio, std::string_view{"Audio"}},
			std::pair{SourceSocketKind::FluidDomain, std::string_view{"FluidDomain"}},
			std::pair{SourceSocketKind::Sdf, std::string_view{"Sdf"}},
			std::pair{SourceSocketKind::Gradient, std::string_view{"Gradient"}},
			std::pair{SourceSocketKind::Font, std::string_view{"Font"}}
		};
		template <class Table, class Enum> std::string_view DomainName(const Table &table, Enum value) {
			for (const auto &[kind, name] : table)
				if (kind == value) return name;
			return {};
		}
		template <class Table, class Enum>
		bool DomainEnum(const Table &table, std::string_view text, std::optional<Enum> &value) {
			value.reset();
			if (text.empty()) return true;
			for (const auto &[kind, name] : table)
				if (name == text) {
					value = kind;
					return true;
				}
			return false;
		}
		const Link *SenderLink(const NodeContext &c) {
			const Link *link = nullptr;
			if (c.EvaluationDocument)
				for (const auto &candidate : c.EvaluationDocument->Links)
					if (candidate.ToNode == c.Authored.Id && candidate.ToPort == "value_in")
						link = &candidate;
			return link;
		}
		bool PublishTunnelValue(NodeContext &c, const Value &value, bool imageArray) {
			constexpr std::string_view port = "value_out";
			if (const auto *surface = std::get_if<SurfaceValue>(&value)) {
				auto *output =
					c.NewImage(port, surface->Data.Width, surface->Data.Height, surface->Data.Format);
				if (!output) return false;
				output->Pixels = surface->Data.Pixels;
				output->Hash = surface->Data.Hash;
				return true;
			}
			if (imageArray) {
				const auto *array = std::get_if<ArrayValue>(&value);
				source_array::TreeCost cost;
				if (!array || !array->Elements.empty() || !array->Nested.empty() ||
					!source_array::Measure(array->Items, cost) || !source_array::AllImages(array->Items))
					return c.Fail(Status::InvalidValue, "tunnel image array snapshot is invalid", port);
				const uint64_t bytes = cost.Bytes + cost.Nodes * (sizeof(Image) + sizeof(ImageArrayItem) +
																  sizeof(std::vector<ImageArrayItem>));
				if (!c.ReserveOutput(bytes, port)) return false;
				ImageArray output;
				output.Images.reserve(cost.Nodes);
				auto items = array->Items;
				source_array::ExportImages(items, output, output.Items);
				c.OutputImageArrays.emplace_back(port, std::move(output));
				return true;
			}
			const auto bytes = ValueClonePayloadBytes(value);
			if (!bytes || !c.ReserveOutput(*bytes, port))
				return c.Fail(Status::LimitExceeded, "tunnel value exceeds its clone budget", port);
			c.SetValue(port, value);
			return c.FailureCode == Status::Ok;
		}
	} // namespace
	bool SourceTunnelSenderDomain(NodeContext &c, bool callback, SourceSocketDomain &domain) {
		domain = {};
		const auto *prior = SourceRetainedField(c, "domain");
		if (c.FailureCode != Status::Ok) return false;
		if (prior) {
			const auto *encoded = std::get_if<ArrayValue>(prior);
			if (!encoded || encoded->ElementType != ValueType::Text || encoded->Elements.size() != 3 ||
				!encoded->Nested.empty() || !encoded->Items.empty() ||
				!std::holds_alternative<std::string>(encoded->Elements[0]) ||
				!std::holds_alternative<std::string>(encoded->Elements[1]) ||
				!std::holds_alternative<std::string>(encoded->Elements[2]))
				return c.Fail(Status::InvalidValue, "tunnel sender domain metadata is invalid", "value_in");
			const auto type = ParseValueTypeName(std::get<std::string>(encoded->Elements[0]));
			if (!type ||
				!DomainEnum(DisplayNames, std::get<std::string>(encoded->Elements[1]), domain.Display) ||
				!DomainEnum(KindNames, std::get<std::string>(encoded->Elements[2]), domain.Kind))
				return c.Fail(Status::InvalidValue, "tunnel sender domain names are invalid", "value_in");
			domain.Type = *type;
		}
		if (!callback) return true;
		const Link *link = SenderLink(c);
		const auto *identity = SourceRetainedField(c, "link");
		if (c.FailureCode != Status::Ok) return false;
		bool unchanged = !identity && !link;
		if (identity) {
			const auto *array = std::get_if<ArrayValue>(identity);
			if (!array || array->ElementType != ValueType::Text || array->Elements.size() != 2 ||
				!array->Nested.empty() || !array->Items.empty() ||
				!std::holds_alternative<std::string>(array->Elements[0]) ||
				!std::holds_alternative<std::string>(array->Elements[1]))
				return c.Fail(Status::InvalidValue, "tunnel sender link metadata is invalid", "value_in");
			unchanged = std::get<std::string>(array->Elements[0]) == (link ? link->FromNode : "") &&
						std::get<std::string>(array->Elements[1]) == (link ? link->FromPort : "");
		}
		if (!unchanged)
			domain = link ? c.InputDomain("value_in").value_or(SourceSocketDomain{}) : SourceSocketDomain{};
		return true;
	}
	bool ExecuteSourceTunnel(NodeContext &c) {
		if (c.Entry.Type == "pc.tunnel_in") {
			SourceSocketDomain domain;
			if (!SourceTunnelSenderDomain(c, true, domain)) return false;
			const Link *link = SenderLink(c);
			auto identityCharge = c.ReserveWorkspace(
				sizeof(ArrayValue) + 2 * sizeof(ElementValue) +
					(link ? link->FromNode.size() + link->FromPort.size() : 0) + 2 * std::string{}.capacity(),
				"value_in"
			);
			if (!identityCharge) return false;
			const Value identity = ArrayValue{
				ValueType::Text,
				{std::string{link ? link->FromNode : ""}, std::string{link ? link->FromPort : ""}}
			};
			const auto typeName = ValueTypeName(domain.Type);
			const auto displayName =
				domain.Display ? DomainName(DisplayNames, *domain.Display) : std::string_view{};
			const auto kindName = domain.Kind ? DomainName(KindNames, *domain.Kind) : std::string_view{};
			if (typeName.empty() || (domain.Display && displayName.empty()) ||
				(domain.Kind && kindName.empty()))
				return c.Fail(Status::InvalidValue, "tunnel sender domain cannot be named", "value_in");
			auto domainCharge = c.ReserveWorkspace(
				sizeof(ArrayValue) + 3 * sizeof(ElementValue) + typeName.size() + displayName.size() +
					kindName.size() + 3 * std::string{}.capacity(),
				"value_in"
			);
			if (!domainCharge) return false;
			const Value encoded = ArrayValue{
				ValueType::Text, {std::string{typeName}, std::string{displayName}, std::string{kindName}}
			};
			const std::array fields{
				std::pair<std::string_view, const Value *>{"link", &identity},
				std::pair<std::string_view, const Value *>{"domain", &encoded}
			};
			return StoreSourceRetainedMetadata(c, fields);
		}
		const auto &input = c.TunnelInput;
		const Value cold = int64_t{-4};
		Value snapshot;
		const Value *value = &cold;
		bool imageArray = false;
		auto snapshotCharge = c.ReserveWorkspace(0, "value_out");
		if (!snapshotCharge) return false;
		if (input.Matched) {
			if (input.Surface) {
				const uint64_t bytes = sizeof(Value) + input.Surface->Pixels.size();
				auto admitted = c.ReserveWorkspace(bytes, "value_out");
				if (!admitted) return false;
				*snapshotCharge = std::move(*admitted);
				snapshot = SurfaceValue{*input.Surface};
				value = &snapshot;
			} else if (input.Surfaces) {
				source_array::TreeCost cost;
				if (!source_array::ImageCost(*input.Surfaces, input.Surfaces->Items, cost, 1))
					return c.Fail(
						Status::LimitExceeded, "tunnel image array exceeds bounded payload", "value_out"
					);
				auto admitted = c.ReserveWorkspace(cost.Bytes + sizeof(ArrayValue), "value_out");
				if (!admitted) return false;
				*snapshotCharge = std::move(*admitted);
				ArrayValue array{ValueType::Any, {}};
				array.Items = source_array::FromImages(*input.Surfaces, input.Surfaces->Items);
				snapshot = std::move(array);
				value = &snapshot;
				imageArray = true;
			} else if (input.Data) {
				value = input.Data;
				imageArray = input.DataImageArray;
			}
		} else {
			if (const auto *prior = SourceRetainedField(c, "value")) {
				value = prior;
				const auto *marker = SourceRetainedField(c, "image_array");
				const auto *flag = marker ? std::get_if<bool>(marker) : nullptr;
				if (!flag)
					return c.Fail(
						Status::InvalidValue, "tunnel retained image-array marker is invalid", "value_out"
					);
				imageArray = *flag;
			}
			if (c.FailureCode != Status::Ok) return false;
		}
		if (!PublishTunnelValue(c, *value, imageArray)) return false;
		const auto domain = input.Matched && input.Domain ? *input.Domain : SourceSocketDomain{};
		if (!c.SetOutputDomain("value_out", domain)) return false;
		const Value marker = imageArray;
		const Value emptyKey = std::string{};
		const Value *key = c.Find("name");
		if (!key) key = &emptyKey;
		if (!std::holds_alternative<std::string>(*key))
			return c.Fail(Status::TypeMismatch, "current source tunnel key must be text", "name");
		const std::array fields{
			std::pair<std::string_view, const Value *>{"value", value},
			std::pair<std::string_view, const Value *>{"image_array", &marker},
			std::pair<std::string_view, const Value *>{"key", key}
		};
		return StoreSourceRetainedMetadata(c, fields);
	}
	std::span<const ExecutorEntry> SourceTunnelExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.tunnel_in", ExecuteSourceTunnel, true}, {"pc.tunnel_out", ExecuteSourceTunnel, true}
		};
		return entries;
	}
} // namespace engine::imagegraph::detail
