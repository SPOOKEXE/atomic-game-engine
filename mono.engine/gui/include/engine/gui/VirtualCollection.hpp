#pragma once

// Bounded typed records for one scrolling interface collection.
//
// A UIVirtualCollection is a child modifier of a ScrollingFrame. Its data
// lives here, while one hidden ordinary GUI child supplies the structural row
// template. Layout and compile turn a visible record range into facets; they
// never manufacture entities or retain a presentation row pool.
//
// @tier L7 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::gui {

	struct VirtualFocusState {
		ecs::Entity Collection;
		std::string Key;
		uint32_t Index = UINT32_MAX;
	};

	// Viewer-local scroll anchor retained across a source page revision.
	struct VirtualAnchorState {
		uint64_t Revision = UINT64_MAX;
		std::string Key;
		float OffsetY = 0.0f;
	};

	// How a collection obtains one record's vertical extent.
	enum class VirtualExtentPolicy : uint8_t {
		// Every record uses VirtualCollection::FixedExtent.
		Fixed,

		// Every record supplies VirtualRecord::MeasuredExtent.
		Measured,
	};

	// How the published records occupy the scrolling canvas.
	enum class VirtualLayoutPolicy : uint8_t {
		// One record per vertical slot.
		List,

		// Row-major cells with a fixed stride and a bounded column count.
		Grid,
	};

	// One typed, named value in a virtual record.
	struct VirtualField {
		// The UTF-8 field name, unique within its record.
		std::string Name;

		// The field's explicitly tagged value.
		ecs::AttributeValue Value;
	};

	// One keyed collection record.
	struct VirtualRecord {
		// The stable UTF-8 identity, unique within the published page.
		std::string Key;

		// The record's bounded typed values.
		std::vector<VirtualField> Fields;

		// The vertical extent when the collection uses Measured policy.
		float MeasuredExtent = 0.0f;
	};

	// The bounded source page presently published for a collection.
	struct VirtualPage {
		// The global item index held by Records.front().
		uint32_t First = 0;

		// Cumulative vertical extent before `First`, published by the source
		// provider so a measured page can anchor without scanning missing items.
		float ExtentBefore = 0.0f;

		// The contiguous source records currently available for presentation.
		std::vector<VirtualRecord> Records;
	};

	// An authored, bounded data source for one virtual collection.
	struct VirtualCollection {
		// The largest page this source accepts, independent of ItemCount.
		static constexpr uint32_t MAXIMUM_PAGE_RECORDS = 4096;

		// The largest number of values in one record.
		static constexpr uint32_t MAXIMUM_FIELDS_PER_RECORD = 64;

		// The largest UTF-8 key, field name, or String value in bytes.
		static constexpr uint32_t MAXIMUM_TEXT_BYTES = 1024;

		// The largest encoded source page, including its typed values.
		static constexpr uint32_t MAXIMUM_PAGE_BYTES = 1024 * 1024;

		// The largest authored off-screen range on either side of the viewport.
		static constexpr uint32_t MAXIMUM_OVERSCAN = 64;

		// A bounded scroll extent keeps layout and hit-test arithmetic finite.
		static constexpr float MAXIMUM_CANVAS_EXTENT = 1000000000.0f;

		// The full number of source items, including those outside Page.
		uint32_t ItemCount = 0;

		// The bounded source records currently published by the provider.
		VirtualPage Page;

		// Whether fixed or record-specific extents decide vertical placement.
		VirtualExtentPolicy ExtentPolicy = VirtualExtentPolicy::Fixed;

		// The vertical extent every record uses under Fixed policy.
		float FixedExtent = 24.0f;

		// The presentation arrangement for this source page.
		VirtualLayoutPolicy LayoutPolicy = VirtualLayoutPolicy::List;

		// The number of row-major cells in each grid row.
		uint32_t GridColumns = 1;

		// The grid cell's canvas stride. Grid collections use fixed extents.
		core::Vector2 GridCellStride{100.0f, 24.0f};

		// Extra records requested before and after the visible interval.
		uint32_t Overscan = 2;

		// The authored source revision that produced this page.
		uint64_t Revision = 0;
	};

	// Finds a published record by its stable source key, or returns null when it is absent.
	const VirtualRecord *FindVirtualRecord(const VirtualCollection &collection, std::string_view key);

	// Returns the published record at `index`, or null when its page does not hold it.
	const VirtualRecord *VirtualRecordAt(const VirtualCollection &collection, uint32_t index);

	// Finds one typed value in a record, or returns null when it is absent.
	const ecs::AttributeValue *FindVirtualField(const VirtualRecord &record, std::string_view name);

	// Returns the extent selected by the collection's declared policy.
	float VirtualExtentOf(const VirtualCollection &collection, const VirtualRecord &record);

	// Returns the cumulative extent before an index. Missing measured records use
	// FixedExtent as the deterministic provider estimate and never trigger a scan.
	float VirtualExtentBefore(const VirtualCollection &collection, uint32_t index);

	// Uses the source's page anchor and measured records, and FixedExtent for
	// unavailable records. This is the scrollable extent without materializing
	// the full source.
	float VirtualEstimatedTotalExtent(const VirtualCollection &collection);

	// Returns the estimated canvas extent without materializing missing records.
	core::Vector2 VirtualCanvasExtent(const VirtualCollection &collection);

	// Returns one record's row-major canvas origin.
	core::Vector2 VirtualCellPosition(const VirtualCollection &collection, uint32_t index);

	// Checks record bounds, UTF-8 names, uniqueness, value types, and extents.
	bool ValidateVirtualCollection(const VirtualCollection &collection);

	// Writes one valid collection using a stable, field-by-field wire format.
	// Returns false without writing when validation fails.
	bool WriteVirtualCollection(core::ByteWriter &writer, const VirtualCollection &collection);

	// Reads one valid collection. A malformed value fails `reader` and leaves
	// `collection` untouched.
	bool ReadVirtualCollection(core::ByteReader &reader, VirtualCollection &collection);

	// Registers the collection component under its stable schema name.
	void RegisterVirtualCollectionComponents();
}
