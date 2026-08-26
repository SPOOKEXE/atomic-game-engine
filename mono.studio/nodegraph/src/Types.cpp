// The vocabulary: the type table, the value comparison, and the registration
// helpers that make a node type read as a declaration.

#include <algorithm>
#include <nodegraph/Types.hpp>
#include <unordered_map>

namespace nodegraph {

	namespace {
		// One vocabulary per process. A function-local static, so it exists
		// before the first registration and cannot be built twice.
		struct Table {
			std::vector<DataType> Order;
			std::unordered_map<std::string, size_t> ById;
		};

		Table &Types() {
			static Table table;
			return table;
		}
	}

	Colour Colour::Hex(uint32_t rgb) {
		return Colour{
			static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
			static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
			static_cast<float>(rgb & 0xFF) / 255.0f,
			1.0f,
		};
	}

	void DataTypes::Register(const DataType &type) {
		Table &table = Types();
		const auto found = table.ById.find(type.Id);
		if (found != table.ById.end()) {
			table.Order[found->second] = type;
			return;
		}
		table.ById.emplace(type.Id, table.Order.size());
		table.Order.push_back(type);
	}

	const DataType *DataTypes::Find(const std::string &id) {
		const Table &table = Types();
		const auto found = table.ById.find(id);
		return found == table.ById.end() ? nullptr : &table.Order[found->second];
	}

	bool DataTypes::CanConnect(const std::string &from, const std::string &to) {
		if (from.empty() || to.empty()) {
			return false;
		}
		return from == to || from == ANY_TYPE || to == ANY_TYPE;
	}

	const std::vector<DataType> &DataTypes::All() {
		return Types().Order;
	}

	bool Value::operator==(const Value &other) const {
		// **Compared field by field rather than by memory**, because a `Value`
		// holds a string: two values that agree on the fields their kind uses
		// are equal, and comparing the unused ones would make a slider unequal
		// to itself over a save and load.
		if (Kind != other.Kind) {
			return false;
		}
		switch (Kind) {
		case WidgetKind::Toggle:
			return Flag == other.Flag;
		case WidgetKind::Text:
		case WidgetKind::Select:
			return Text == other.Text;
		case WidgetKind::Colour:
			return Tint.R == other.Tint.R && Tint.G == other.Tint.G && Tint.B == other.Tint.B &&
				   Tint.A == other.Tint.A;
		case WidgetKind::Slider:
		case WidgetKind::Number:
			return Number == other.Number;
		}
		return false;
	}

	PortSpec Port(std::string name, std::string type) {
		return PortSpec{std::move(name), std::move(type)};
	}

	WidgetSpec Slider(std::string key, std::string label, double minimum, double maximum, double value) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Slider;
		spec.Minimum = minimum;
		spec.Maximum = maximum;
		spec.Step = (maximum - minimum) / 100.0;
		spec.Default.Kind = WidgetKind::Slider;
		spec.Default.Number = std::clamp(value, minimum, maximum);
		return spec;
	}

	WidgetSpec Toggle(std::string key, std::string label, bool value) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Toggle;
		spec.Default.Kind = WidgetKind::Toggle;
		spec.Default.Flag = value;
		return spec;
	}

	WidgetSpec Select(std::string key, std::string label, std::vector<std::string> options, int chosen) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Select;
		spec.Options = std::move(options);
		spec.Default.Kind = WidgetKind::Select;

		// **The chosen option is stored as its text, not its index.** An index
		// is a number that means something different the moment somebody
		// reorders the options, and reordering a list is the sort of edit
		// nobody expects to change a saved graph.
		if (chosen >= 0 && static_cast<size_t>(chosen) < spec.Options.size()) {
			spec.Default.Text = spec.Options[static_cast<size_t>(chosen)];
		} else if (!spec.Options.empty()) {
			spec.Default.Text = spec.Options.front();
		}
		return spec;
	}

	WidgetSpec Number(std::string key, std::string label, double value) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Number;
		spec.Minimum = 0.0;
		spec.Maximum = 0.0;
		spec.Step = 0.1;
		spec.Default.Kind = WidgetKind::Number;
		spec.Default.Number = value;
		return spec;
	}

	WidgetSpec Text(std::string key, std::string label, std::string value) {
		WidgetSpec spec;
		spec.Key = std::move(key);
		spec.Label = std::move(label);
		spec.Kind = WidgetKind::Text;
		spec.Default.Kind = WidgetKind::Text;
		spec.Default.Text = std::move(value);
		return spec;
	}

	Chrome &HostChrome() {
		static Chrome table;
		return table;
	}
}
