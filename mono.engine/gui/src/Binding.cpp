#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Binding.hpp>

#include <utility>
#include <vector>

namespace engine::gui {
	namespace {
		constexpr size_t MAXIMUM_SOURCE_PATH_BYTES = 256;
		constexpr size_t MAXIMUM_SOURCE_PATH_DEPTH = 32;
		constexpr size_t MAXIMUM_BINDING_TEXT_BYTES = 4096;

		template <class T> void WriteTransientBindings(core::ByteWriter &, const void *, size_t) {}

		template <class T> void ReadTransientBindings(core::ByteReader &, void *destination, size_t count) {
			auto *values = static_cast<T *>(destination);
			for (size_t index = 0; index < count; index++)
				values[index] = {};
		}

		uint64_t ConfigurationStamp(const Binding &binding) {
			uint64_t stamp = 1469598103934665603ULL;
			const auto add = [&](std::string_view text) {
				for (const char character : text) {
					stamp ^= static_cast<uint8_t>(character);
					stamp *= 1099511628211ULL;
				}
			};
			add(binding.SourcePath);
			add(binding.Attribute.Text());
			add(binding.Target.Text());
			add(binding.Fallback);
			return stamp == 0 ? 1 : stamp;
		}

		enum class ResolveFailure { None, InvalidPath, MissingSource };

		ecs::Entity ResolveSource(const ecs::Store &store, std::string_view path, ResolveFailure &failure) {
			if (path.empty() || path.size() > MAXIMUM_SOURCE_PATH_BYTES) {
				failure = ResolveFailure::InvalidPath;
				return ecs::NULL_ENTITY;
			}
			ecs::Entity source = ecs::NULL_ENTITY;
			size_t position = 0;
			for (size_t depth = 0; position < path.size() && depth < MAXIMUM_SOURCE_PATH_DEPTH; depth++) {
				const size_t end = path.find('/', position);
				const std::string_view segment = path.substr(position, end - position);
				if (segment.empty()) {
					failure = ResolveFailure::InvalidPath;
					return ecs::NULL_ENTITY;
				}
				ecs::Entity match = ecs::NULL_ENTITY;
				bool ambiguous = false;
				const auto collect = [&](ecs::Entity candidate) {
					if (store.InstanceNameOf(candidate).Text() != segment) {
						return;
					}
					if (match != ecs::NULL_ENTITY) {
						ambiguous = true;
					} else {
						match = candidate;
					}
				};
				if (source == ecs::NULL_ENTITY) {
					store.EachRoot(collect);
				} else {
					store.EachChild(source, collect);
				}
				if (ambiguous) {
					failure = ResolveFailure::InvalidPath;
					return ecs::NULL_ENTITY;
				}
				if (match == ecs::NULL_ENTITY) {
					failure = ResolveFailure::MissingSource;
					return ecs::NULL_ENTITY;
				}
				source = match;
				if (end == std::string_view::npos) {
					failure = ResolveFailure::None;
					return source;
				}
				position = end + 1;
			}
			failure = ResolveFailure::InvalidPath;
			return ecs::NULL_ENTITY;
		}

		void WriteBindings(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *bindings = static_cast<const Binding *>(source);
			for (size_t index = 0; index < count; index++) {
				const Binding &binding = bindings[index];
				writer.WriteString(binding.SourcePath);
				writer.WriteName(binding.Attribute);
				writer.WriteName(binding.Target);
				writer.WriteString(binding.Fallback);
			}
		}

		void ReadBindings(core::ByteReader &reader, void *destination, size_t count) {
			auto *bindings = static_cast<Binding *>(destination);
			for (size_t index = 0; index < count; index++) {
				Binding &binding = bindings[index];
				const std::string_view source = reader.ReadString();
				if (source.size() > MAXIMUM_SOURCE_PATH_BYTES) {
					reader.Fail();
					return;
				}
				binding.SourcePath = std::string(source);
				binding.Attribute = reader.ReadName();
				binding.Target = reader.ReadName();
				const std::string_view fallback = reader.ReadString();
				if (fallback.size() > MAXIMUM_BINDING_TEXT_BYTES) {
					reader.Fail();
					return;
				}
				binding.Fallback = std::string(fallback);
			}
		}
	}

	void RegisterBindingComponents() {
		ecs::Components::Register<Binding>("gui.Binding", WriteBindings, ReadBindings);
		ecs::Components::Register<BindingOutput>(
			"gui.BindingOutput", WriteTransientBindings<BindingOutput>, ReadTransientBindings<BindingOutput>
		);
		ecs::Components::Register<BindingDependency>(
			"gui.BindingDependency",
			WriteTransientBindings<BindingDependency>,
			ReadTransientBindings<BindingDependency>
		);
	}

	size_t EvaluateBindings(ecs::Store &store) {
		std::vector<ecs::Entity> bindings;
		store.Each<const Binding>([&](ecs::Entity entity, const Binding &) { bindings.push_back(entity); });

		size_t changed = 0;
		for (const ecs::Entity entity : bindings) {
			const Binding *binding = store.Get<Binding>(entity);
			BindingOutput *output = store.GetMutable<BindingOutput>(entity);
			BindingDependency *dependency = store.GetMutable<BindingDependency>(entity);
			if (binding == nullptr || output == nullptr || dependency == nullptr) {
				continue;
			}

			BindingOutput resolved;
			const uint64_t stamp = ConfigurationStamp(*binding);
			ResolveFailure sourceFailure = ResolveFailure::None;
			const bool resolveSource =
				dependency->ConfigurationStamp != stamp || !store.Alive(dependency->Source);
			if (resolveSource) {
				dependency->Source = ResolveSource(store, binding->SourcePath, sourceFailure);
				dependency->ConfigurationStamp = stamp;
				dependency->SourceRevision = 0;
			}
			if (dependency->Source == ecs::NULL_ENTITY) {
				// A missing source may appear after the binding. Retry only that
				// unresolved path; a resolved dependency never walks the tree again.
				dependency->ConfigurationStamp = 0;
				resolved.Failure = sourceFailure == ResolveFailure::InvalidPath
									   ? BindingFailure::InvalidSourcePath
									   : BindingFailure::MissingSource;
				resolved.Value = binding->Fallback.substr(0, MAXIMUM_BINDING_TEXT_BYTES);
			} else if (binding->Target != core::Name("Text")) {
				if (!resolveSource && output->EvaluationCount != 0) {
					continue;
				}
				resolved.Failure = BindingFailure::UnsupportedTarget;
				resolved.Value = binding->Fallback.substr(0, MAXIMUM_BINDING_TEXT_BYTES);
			} else {
				const uint64_t sourceRevision =
					ecs::AttributeRevision(store, dependency->Source, binding->Attribute);
				if (dependency->SourceRevision == sourceRevision && output->EvaluationCount != 0) {
					continue;
				}
				dependency->SourceRevision = sourceRevision;
				resolved.SourceRevision = sourceRevision;
				ecs::AttributeValue value;
				if (!ecs::GetAttribute(store, dependency->Source, binding->Attribute, value)) {
					resolved.Failure = BindingFailure::MissingAttribute;
					resolved.Value = binding->Fallback.substr(0, MAXIMUM_BINDING_TEXT_BYTES);
				} else if (value.Type != ecs::PropertyType::String) {
					resolved.Failure = BindingFailure::TypeMismatch;
					resolved.Value = binding->Fallback.substr(0, MAXIMUM_BINDING_TEXT_BYTES);
				} else if (value.String.size() > MAXIMUM_BINDING_TEXT_BYTES) {
					resolved.Failure = BindingFailure::OutputTooLong;
					resolved.Value = binding->Fallback.substr(0, MAXIMUM_BINDING_TEXT_BYTES);
				} else {
					resolved.Value = std::move(value.String);
					resolved.Valid = true;
				}
			}

			resolved.EvaluationCount = output->EvaluationCount + 1;
			if (output->Value != resolved.Value || output->Valid != resolved.Valid ||
				output->Failure != resolved.Failure || output->SourceRevision != resolved.SourceRevision) {
				changed++;
			}
			*output = std::move(resolved);
		}
		return changed;
	}

	std::string_view BoundText(const ecs::Store &store, ecs::Entity instance, std::string_view fallback) {
		std::string_view text = fallback;
		bool found = false;
		store.EachChild(instance, [&](ecs::Entity child) {
			if (found) {
				return;
			}
			const Binding *binding = store.Get<Binding>(child);
			const BindingOutput *output = store.Get<BindingOutput>(child);
			if (binding != nullptr && output != nullptr && binding->Target == core::Name("Text")) {
				text = output->Value;
				found = true;
			}
		});
		return text;
	}
}
