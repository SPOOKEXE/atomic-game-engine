#pragma once
#include <engine/assets/Shader.hpp>

#include <span>
namespace engine::assets::detail {
	struct ShaderByteCounter {
		uint64_t Bytes = 0, Maximum = std::numeric_limits<uint64_t>::max();
		bool Add(uint64_t amount) {
			if (amount > Maximum - Bytes) return false;
			Bytes += amount;
			return true;
		}
		bool String(const std::string &value) {
			return value.capacity() != std::numeric_limits<size_t>::max() && Add(value.capacity() + 1);
		}
		template <class T> bool Vector(const std::vector<T> &values) {
			return values.capacity() <= (Maximum - Bytes) / sizeof(T) && Add(values.capacity() * sizeof(T));
		}
	};
	inline bool ShaderFields(const ShaderFeature &v, ShaderByteCounter &c) {
		return c.String(v.Name) && c.String(v.Value);
	}
	inline bool ShaderFields(const ShaderDependency &v, ShaderByteCounter &c) {
		return c.String(v.Name);
	}
	inline bool ShaderFields(const ShaderSpecialization &v, ShaderByteCounter &c) {
		return c.String(v.Name) && c.String(v.Type) && c.Vector(v.Value);
	}
	inline bool ShaderFields(const ShaderResource &v, ShaderByteCounter &c) {
		return c.String(v.Name) && c.String(v.Kind) && c.String(v.Access) && c.String(v.Dimension) &&
			   c.String(v.Format) && c.String(v.SampleType);
	}
	inline bool ShaderFields(const ShaderParameter &v, ShaderByteCounter &c) {
		return c.String(v.Name) && c.String(v.Resource) && c.String(v.Type);
	}
	inline bool ShaderFields(const ShaderInterfaceVariable &v, ShaderByteCounter &c) {
		return c.String(v.Name) && c.String(v.Type) && c.String(v.Interpolation);
	}
	inline bool ShaderFields(const ShaderOptimization &v, ShaderByteCounter &c) {
		return c.String(v.Name);
	}
	inline bool ShaderFields(const ShaderPayload &v, ShaderByteCounter &c) {
		return c.String(v.Backend) && c.String(v.EntryPoint) && c.String(v.Target) && c.Vector(v.Bytes);
	}
	inline bool ShaderFields(const std::string &v, ShaderByteCounter &c) {
		return c.String(v);
	}
	template <class T> bool ShaderRows(const std::vector<T> &values, ShaderByteCounter &c) {
		if (!c.Vector(values)) return false;
		for (const auto &v : values)
			if (!ShaderFields(v, c)) return false;
		return true;
	}
	inline bool ShaderFields(const ShaderVariant &v, ShaderByteCounter &c) {
		return c.String(v.Name) && c.String(v.Stage) && ShaderRows(v.Features, c) &&
			   ShaderRows(v.Specializations, c) && ShaderRows(v.Resources, c) &&
			   ShaderRows(v.Parameters, c) && ShaderRows(v.Inputs, c) && ShaderRows(v.Outputs, c) &&
			   ShaderRows(v.RequiredCapabilities, c) && ShaderRows(v.Optimizations, c) &&
			   ShaderRows(v.Payloads, c);
	}
	inline bool ShaderFields(const ShaderData &v, ShaderByteCounter &c) {
		for (const auto *text :
			 {&v.CompilerVersion,
			  &v.OptimizerVersion,
			  &v.TranslatorVersion,
			  &v.ShaderAbi,
			  &v.TargetEnvironment,
			  &v.SourceLanguage,
			  &v.CapabilityProfile,
			  &v.PolicyVersion,
			  &v.CookProfile})
			if (!c.String(*text)) return false;
		return ShaderRows(v.CompilerOptions, c) && ShaderRows(v.OptimizerOptions, c) &&
			   ShaderRows(v.TranslatorOptions, c) && ShaderRows(v.Dependencies, c) &&
			   ShaderRows(v.Variants, c);
	}
}
