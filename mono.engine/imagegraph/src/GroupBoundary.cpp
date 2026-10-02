#include "GroupBoundary.hpp"

#include "ValuePayload.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	namespace {
		bool ShapeBytes(
			const ImageArrayItem &item, size_t imageCount, size_t &count, uint64_t &bytes, size_t depth = 0
		) {
			if (++count > Limits::MaximumArrayElements || depth > Limits::MaximumArrayElements) return false;
			if (const auto *leaf = std::get_if<size_t>(&item.Data)) return *leaf < imageCount;
			if (const auto *children = std::get_if<std::vector<ImageArrayItem>>(&item.Data)) {
				if (children->size() > Limits::MaximumArrayElements ||
					children->size() * sizeof(ImageArrayItem) > UINT64_MAX - bytes)
					return false;
				bytes += children->size() * sizeof(ImageArrayItem);
				for (const auto &child : *children)
					if (!ShapeBytes(child, imageCount, count, bytes, depth + 1)) return false;
			}
			return true;
		}
		bool Forward(NodeContext &context, std::string_view input) {
			if (const auto *image = context.Input(input)) {
				Image *out = context.NewImage("value", image->Width, image->Height, image->Format);
				if (!out) return false;
				std::copy(image->Pixels.begin(), image->Pixels.end(), out->Pixels.begin());
				return true;
			}
			for (const auto &[port, array] : context.ImageArrays) {
				if (port != input || !array) continue;
				if (array->Images.size() > Limits::MaximumArrayElements ||
					array->Items.size() > Limits::MaximumArrayElements)
					return context.Fail(
						Status::LimitExceeded, "group image array count exceeds the bounded domain", input
					);
				size_t count = 0;
				uint64_t bytes =
					array->Images.size() * sizeof(Image) + array->Items.size() * sizeof(ImageArrayItem);
				for (const auto &item : array->Items)
					if (!ShapeBytes(item, array->Images.size(), count, bytes))
						return context.Fail(
							Status::LimitExceeded, "group image array shape exceeds the bounded domain", input
						);
				for (const auto &image : array->Images) {
					if (!ValidSurfaceLayout(
							image, context.Request.MaximumImageDimension, Limits::MaximumEvaluationBytes
						))
						return context.Fail(
							Status::LimitExceeded, "group array surface exceeds the request cap", input
						);
					if (image.Pixels.size() > UINT64_MAX - bytes)
						return context.Fail(
							Status::LimitExceeded, "group array surface bytes overflow", input
						);
					bytes += image.Pixels.size();
				}
				const uint64_t nameBytes = std::max(size_t(5), std::string{}.capacity());
				if (nameBytes > UINT64_MAX - bytes || !context.ReserveOutput(bytes + nameBytes, "value"))
					return false;
				context.OutputImageArrays.emplace_back("value", *array);
				return true;
			}
			const Value *value = context.Find(input);
			if (!value || !ValidRuntimeValue(*value))
				return context.Fail(
					Status::InvalidValue, "group boundary requires a represented finite value", input
				);
			context.SetValue("value", *value);
			return context.FailureCode == Status::Ok;
		}
		bool NumericChoice(NodeContext &context, std::string_view port, int64_t &out) {
			const Value *value = context.Find(port);
			const auto choice = value ? SourceChoiceNumber(*value) : std::optional<double>{};
			if (!choice)
				return context.Fail(
					Status::UnsupportedExecution, "group domain control requires a finite numeric value", port
				);
			const double number = *choice;
			if (!std::isfinite(number) || number != std::trunc(number) || number < 0 || number > INT64_MAX)
				return context.Fail(
					Status::UnsupportedExecution, "group domain choice requires a supported exact index", port
				);
			out = static_cast<int64_t>(number);
			return context.FailureCode == Status::Ok;
		}
	}
	bool ResolveGroupSocketDomain(int64_t type, int64_t subtype, int64_t size, SourceSocketDomain &out) {
		if (type < 0 || subtype < 0 || size < 0 || type >= 32 || type == 10 || type == 20 || type == 26 ||
			size > 2)
			return false;
		const uint8_t displayCounts[] = {13, 11, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
										 1,	 1,	 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
		if (subtype >= displayCounts[type]) return false;
		static constexpr ValueType domains[] = {
			ValueType::Integer,	 ValueType::Scalar,		 ValueType::Boolean, ValueType::Colour,
			ValueType::Image,	 ValueType::Text,		 ValueType::Curve,	 ValueType::Text,
			ValueType::Object,	 ValueType::NodeRef,	 ValueType::Any,	 ValueType::Any,
			ValueType::Path2D,	 ValueType::Particle,	 ValueType::Rigid,	 ValueType::SmokeDomain,
			ValueType::Struct,	 ValueType::Strand,		 ValueType::Mesh2D,	 ValueType::Any,
			ValueType::Any,		 ValueType::Mesh,		 ValueType::Light3D, ValueType::Any,
			ValueType::Scene3D,	 ValueType::Material3D,	 ValueType::Any,	 ValueType::PcxNode,
			ValueType::AudioBit, ValueType::FluidDomain, ValueType::Sdf,	 ValueType::Gradient
		};
		// Slots 10, 20 and 26 are separators and were rejected above.
		static constexpr SourceSocketKind sourceKinds[] = {
			SourceSocketKind::Integer,	   SourceSocketKind::Float,		 SourceSocketKind::Boolean,
			SourceSocketKind::Colour,	   SourceSocketKind::Surface,	 SourceSocketKind::FilePath,
			SourceSocketKind::Curve,	   SourceSocketKind::Text,		 SourceSocketKind::Object,
			SourceSocketKind::Node,		   SourceSocketKind::Any,		 SourceSocketKind::Any,
			SourceSocketKind::Path,		   SourceSocketKind::Particle,	 SourceSocketKind::Rigid,
			SourceSocketKind::SmokeDomain, SourceSocketKind::Struct,	 SourceSocketKind::Strand,
			SourceSocketKind::Mesh2D,	   SourceSocketKind::Trigger,	 SourceSocketKind::Any,
			SourceSocketKind::Mesh3D,	   SourceSocketKind::Light3D,	 SourceSocketKind::Camera3D,
			SourceSocketKind::Scene3D,	   SourceSocketKind::Material3D, SourceSocketKind::Any,
			SourceSocketKind::PcxNode,	   SourceSocketKind::Audio,		 SourceSocketKind::FluidDomain,
			SourceSocketKind::Sdf,		   SourceSocketKind::Gradient
		};
		static constexpr SourceValueDisplay numericDisplays[] = {
			SourceValueDisplay::Default,
			SourceValueDisplay::Range,
			SourceValueDisplay::Rotation,
			SourceValueDisplay::RotationRange,
			SourceValueDisplay::Slider,
			SourceValueDisplay::SliderRange,
			SourceValueDisplay::Padding,
			SourceValueDisplay::Vector,
			SourceValueDisplay::VectorRange,
			SourceValueDisplay::Area,
			SourceValueDisplay::EnumButton,
			SourceValueDisplay::EnumScroll,
			SourceValueDisplay::Seed
		};
		SourceValueDisplay display = SourceValueDisplay::Default;
		if (type <= 1)
			display = type == 1 && subtype == 10 ? SourceValueDisplay::Seed : numericDisplays[subtype];
		else if (type == 3 && subtype == 1)
			display = SourceValueDisplay::Palette;
		else if (type == 6)
			display = SourceValueDisplay::Curve;
		out = {domains[type], display, sourceKinds[type]};
		return true;
	}
	bool ExecuteGroupBoundary(NodeContext &context) {
		if (context.Authored.Type == "pc.group_output") {
			const auto domain = context.InputDomain("value").value_or(
				SourceSocketDomain{ValueType::Any, SourceValueDisplay::Default, SourceSocketKind::Any}
			);
			return context.SetOutputDomain("value", domain) && Forward(context, "value");
		}
		int64_t type, subtype, size;
		if (!context.GroupReplay &&
			(!NumericChoice(context, "input_type", type) || !NumericChoice(context, "subtype", subtype) ||
			 !NumericChoice(context, "vector_size", size)))
			return false;
		SourceSocketDomain domain;
		if (context.GroupReplay) {
			type = context.GroupReplay->InputType;
			subtype = context.GroupReplay->Subtype;
			size = context.GroupReplay->VectorSize;
		}
		if (!ResolveGroupSocketDomain(type, subtype, size, domain))
			return context.Fail(
				Status::InvalidValue, "group boundary choices are outside the source domain", "input_type"
			);
		if (!context.SetOutputDomain("value", domain)) return false;
		if (type == 11) return Forward(context, "parent_value");
		if (type == 4) {
			if (subtype != 0)
				return context.Fail(Status::InvalidValue, "surface boundary subtype is invalid", "subtype");
			return Forward(context, "parent_value");
		}
		const Value *raw = context.Find("parent_value");
		if (!raw) return context.Fail(Status::InvalidValue, "group parent value is absent", "parent_value");
		// Generic NodeValue does not acquire subclass Bool/Int casts when setType changes metadata.
		// Concrete consuming nodes retain their own centralized source getter conversion.
		if (type <= 2) return Forward(context, "parent_value");
		const auto incoming = context.InputDomain("parent_value");
		if (type == 3 && incoming &&
			(incoming->Kind == SourceSocketKind::Float || incoming->Kind == SourceSocketKind::Integer ||
			 incoming->Kind == SourceSocketKind::Boolean))
			return Forward(context, "parent_value");
		if (domain.Display == SourceValueDisplay::Palette && !std::holds_alternative<ArrayValue>(*raw)) {
			if (!RepresentableArrayElementType(PayloadType(*raw)))
				return context.Fail(
					Status::UnsupportedExecution,
					"group palette scalar has no represented native array element",
					"parent_value"
				);
			const uint64_t bytes = sizeof(ElementValue) + RetainedPayloadBytes(*raw);
			if (!context.ReserveOutput(bytes + std::max(size_t(5), std::string{}.capacity()), "value"))
				return false;
			ArrayValue palette;
			palette.ElementType = PayloadType(*raw);
			palette.Elements.reserve(1);
			auto leaf = ArrayElement(*raw);
			palette.Elements.push_back(std::move(*leaf));
			context.SetValue("value", std::move(palette));
			return context.FailureCode == Status::Ok;
		}
		if (type == 31 && incoming && incoming->Kind == SourceSocketKind::Colour &&
			!std::holds_alternative<Gradient>(*raw)) {
			const auto *colour = std::get_if<Colour>(raw);
			const auto *palette = std::get_if<ArrayValue>(raw);
			if (!colour &&
				(!palette || palette->ElementType != ValueType::Colour || !palette->Nested.empty()))
				return context.Fail(
					Status::UnsupportedExecution,
					"source colour-to-gradient payload has no exact represented native colour",
					"parent_value"
				);
			const size_t count = colour ? 1 : palette->Elements.size();
			if (count > Limits::MaximumGradientKeys)
				return context.Fail(
					Status::LimitExceeded,
					"group palette-to-gradient exceeds the native key bound",
					"parent_value"
				);
			if (!context.ReserveOutput(
					count * sizeof(GradientKey) + std::max(size_t(5), std::string{}.capacity()), "value"
				))
				return false;
			Gradient gradient;
			gradient.Keys.reserve(count);
			if (colour)
				gradient.Keys.push_back({0, *colour});
			else
				for (size_t index = 0; index < count; ++index) {
					const auto *item = std::get_if<Colour>(&palette->Elements[index]);
					if (!item)
						return context.Fail(
							Status::TypeMismatch,
							"group palette contains a non-colour element",
							"parent_value"
						);
					gradient.Keys.push_back({static_cast<double>(index) / count, *item});
				}
			context.SetValue("value", std::move(gradient));
			return context.FailureCode == Status::Ok;
		}

		const ValueType expected = [&] {
			switch (type) {
			case 3:
				return subtype == 1 ? ValueType::Array : ValueType::Colour;
			case 5:
			case 7:
				return ValueType::Text;
			case 6:
				return ValueType::Curve;
			case 12:
				return ValueType::Path2D;
			case 21:
				return ValueType::Mesh;
			case 25:
				return ValueType::Material3D;
			case 28:
				return ValueType::AudioBit;
			case 31:
				return ValueType::Gradient;
			default:
				return ValueType::Any;
			}
		}();
		if (expected == ValueType::Any)
			return context.Fail(
				Status::UnsupportedExecution,
				"source group resource domain has no owned native payload",
				"input_type"
			);
		const auto *array = std::get_if<ArrayValue>(raw);
		if (PayloadType(*raw) != expected && (!array || array->ElementType != expected))
			return context.Fail(
				Status::TypeMismatch,
				"group parent payload does not match its declared source domain",
				"parent_value"
			);
		return Forward(context, "parent_value");
	}
}
