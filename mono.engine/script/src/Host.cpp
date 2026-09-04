// The VM-independent values passed across a script-host call.

#include <engine/script/Host.hpp>

#include <utility>

namespace engine::script {

	const char *Describe(HostTag tag) {
		switch (tag) {
		case HostTag::Nil:
			return "nil";
		case HostTag::Boolean:
			return "a boolean";
		case HostTag::Number:
			return "a number";
		case HostTag::String:
			return "a string";
		case HostTag::Array:
			return "an array";
		case HostTag::Map:
			return "a table";
		case HostTag::Instance:
			return "an Instance";
		case HostTag::Callback:
			return "a function";
		case HostTag::Vector3:
			return "a Vector3";
		case HostTag::Color3:
			return "a Color3";
		case HostTag::CFrame:
			return "a CFrame";
		}
		return "a value";
	}

	HostValue HostValue::Of(bool value) {
		HostValue out(HostTag::Boolean);
		out.Boolean = value;
		return out;
	}

	HostValue HostValue::Of(double value) {
		HostValue out(HostTag::Number);
		out.Number = value;
		return out;
	}

	HostValue HostValue::Of(std::string_view value) {
		HostValue out(HostTag::String);
		out.Text = value;
		return out;
	}

	HostValue HostValue::Of(ecs::Entity value) {
		HostValue out(HostTag::Instance);
		out.Instance = value;
		return out;
	}

	HostValue HostValue::List(std::vector<HostValue> items) {
		HostValue out(HostTag::Array);
		out.Items = std::move(items);
		return out;
	}
}
