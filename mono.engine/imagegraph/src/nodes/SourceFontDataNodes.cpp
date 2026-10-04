#include "../FontPath.hpp"
#include "../FontPayload.hpp"
#include "Families.hpp"

namespace engine::imagegraph::detail {
	namespace {
		bool FontDataForward(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.font_data");
			const Value empty = std::string{};
			const auto *input = c.Find("font");
			if (!input) input = &empty;
			if (c.ProcessorRow == 0 && c.Request.SourceFonts && !c.Request.SourceFonts->Aliases.empty()) {
				const Value *original = input;
				for (const auto &[port, value] : c.ProcessorOriginalValues)
					if (port == "font" && value) original = value;
				uint64_t textBytes = 0;
				const auto rootText = [&](const auto &leaf) {
					if (const auto *text = std::get_if<std::string>(&leaf)) textBytes += text->size();
				};
				if (const auto *whole = std::get_if<ArrayValue>(original)) {
					for (const auto &leaf : whole->Elements)
						rootText(leaf);
					for (const auto &item : whole->Items)
						if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) rootText(*leaf);
				} else
					rootText(*original);
				if (textBytes > (16 * 1024 * 1024) / c.Request.SourceFonts->Aliases.size())
					return c.Fail(
						Status::LimitExceeded, "whole font batch alias comparisons exceed work bound", "font"
					);
			}

			if (const auto *font = std::get_if<FontValue>(input)) {
				const auto cloned = ValueClonePayloadBytes(*input);
				if (!cloned || !c.ReserveOutput(*cloned, "font"))
					return c.FailureCode == Status::Ok
							   ? c.Fail(
									 Status::InvalidValue, "font input is not a bounded owned font", "font"
								 )
							   : false;
				c.SetValue("font", *font);
				if (!c.SetOutputDomain("font", {ValueType::Font, std::nullopt, SourceSocketKind::Font}))
					return false;
				return c.FailureCode == Status::Ok;
			}

			if (const auto *array = std::get_if<ArrayValue>(input)) {
				// With processing disabled the source Font getter maps only root strings.
				// Nested arrays remain complete values rather than becoming font-path lists.
				const auto clone = ValueClonePayloadBytes(*input);
				if (!clone || !ValidRuntimeValue(*input) || !c.ReserveOutput(*clone, "font"))
					return c.FailureCode == Status::Ok
							   ? c.Fail(Status::InvalidValue, "font array is not bounded", "font")
							   : false;
				const auto *namespaceData = c.Request.SourceFonts;
				uint64_t stringBytes = 0;
				const auto count = [&](const ElementValue &leaf) {
					if (const auto *text = std::get_if<std::string>(&leaf)) stringBytes += text->size();
				};
				for (const auto &leaf : array->Elements)
					count(leaf);
				for (const auto &item : array->Items)
					if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) count(*leaf);
				if (namespaceData && !namespaceData->Aliases.empty() &&
					stringBytes > (16 * 1024 * 1024) / namespaceData->Aliases.size())
					return c.Fail(
						Status::LimitExceeded, "font array alias comparisons exceed work bound", "font"
					);
				const uint64_t workspace = 3 * (Limits::MaximumTextBytes + std::string{}.capacity());
				auto charge = c.ReserveWorkspace(workspace, "font");
				if (!charge) return false;
				ArrayValue result = *array;
				const auto resolve = [&](ElementValue &leaf) -> bool {
					if (auto *text = std::get_if<std::string>(&leaf)) {
						std::string resolved, failure;
						const auto status =
							ResolveSourceFontPath(*text, namespaceData, workspace, resolved, failure);
						if (status != Status::Ok) return c.Fail(status, std::move(failure), "font");
						if (!c.ReserveOutput(resolved.capacity(), "font")) return false;
						*text = std::move(resolved);
					}
					return true;
				};
				for (auto &leaf : result.Elements)
					if (!resolve(leaf)) return false;
				for (auto &item : result.Items)
					if (auto *leaf = std::get_if<ElementValue>(&item.Data))
						if (!resolve(*leaf)) return false;
				if (!ValidPayload(result, true))
					return c.Fail(
						Status::LimitExceeded, "resolved font array exceeds payload limits", "font"
					);
				c.SetValue("font", std::move(result));
				if (!c.SetOutputDomain("font", {ValueType::Array, std::nullopt, SourceSocketKind::Font}))
					return false;
				return c.FailureCode == Status::Ok;
			}
			const auto *path = std::get_if<std::string>(input);
			if (!path)
				return c.Fail(
					Status::UnsupportedExecution, "font input has no native Text or Font transport", "font"
				);
			// All three lexical replacement strings can coexist. Resolve only after admission.
			const uint64_t workspace = 3 * (Limits::MaximumTextBytes + std::string{}.capacity());
			auto charge = c.ReserveWorkspace(workspace, "font");
			if (!charge) return false;
			std::string result, failure;
			const auto status =
				ResolveSourceFontPath(*path, c.Request.SourceFonts, workspace, result, failure);
			if (status != Status::Ok) return c.Fail(status, std::move(failure), "font");
			if (!c.ReserveOutput(result.capacity(), "font")) return false;
			c.SetValue("font", std::move(result));
			if (!c.SetOutputDomain("font", {ValueType::Text, std::nullopt, std::nullopt})) return false;
			return c.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceFontDataExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.font_data", FontDataForward, true}};
		return entries;
	}
}
