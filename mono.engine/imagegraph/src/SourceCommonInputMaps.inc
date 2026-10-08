namespace detail {
	Status CaptureSourceCommonInputMap(
		NodeContext &context,
		const SourceCommonOwnerRecord &owner,
		GroupRenderSession &candidate,
		EvaluationBudget &budget,
		AllocationReservation &candidateCharge,
		Diagnostic &diagnostic
	) {
		auto row = std::find_if(
			candidate.SourceCommonInputs.begin(), candidate.SourceCommonInputs.end(), [&](const auto &map) {
				return map.OwnerId == owner.SourceOwnerId && map.OwnerType == owner.SourceType;
			}
		);
		if (row == candidate.SourceCommonInputs.end()) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidValue,
				"getInputs map requires explicit owner initialization",
				owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		std::vector<EvaluationInputValue> values;
		std::vector<EvaluationInputImage> images;
		std::vector<SnapshotImageArray> arrays;
		std::optional<SurfaceFormat> policy;
		int64_t interpolation = 1;
		auto snapshotCharge = budget.Reserve(0);
		NodeInputCapture capture{
			context.Authored.Id, &values, &images, &policy, &*snapshotCharge, &arrays, &interpolation
		};
		SourceGetterProjection projection(context);
		if (!projection.Prepare() || !CaptureNodeInputs(context, capture, {}, {})) {
			SetDiagnostic(
				diagnostic,
				context.FailureCode,
				context.FailureMessage,
				context.Authored.Id,
				context.FailurePort
			);
			return diagnostic.Code;
		}
		const uint64_t oldPayload = row->Inputs ? RetainedPayloadBytes(*row->Inputs) : 0;
		const size_t oldFields = row->Inputs && row->Inputs->Data ? row->Inputs->Data->Fields.size() : 0;
		const size_t added = values.size() + images.size() + arrays.size();
		if (oldFields > Limits::MaximumArrayElements || added > Limits::MaximumArrayElements - oldFields) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "getInputs map exceeds field bounds", owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		uint64_t bytes = oldPayload;
		if (!AddBytes(bytes, sizeof(StructData)) ||
			!AddArrayBytes(bytes, oldFields + added, sizeof(std::pair<std::string, Value>)) ||
			!AddBytes(bytes, snapshotCharge->Bytes()) || !AddBytes(bytes, snapshotCharge->Bytes())) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "getInputs map clone size overflows", owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		auto mapCharge = budget.Reserve(bytes);
		if (!mapCharge) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "getInputs map exceeds live bytes", owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		StructValue map = row->Inputs ? *row->Inputs : StructValue{};
		if (!map.Data) map.Data.emplace();
		map.Data->Fields.reserve(oldFields + added);
		const auto set = [&](std::string key, Value value) {
			const auto found =
				std::find_if(map.Data->Fields.begin(), map.Data->Fields.end(), [&](const auto &field) {
					return field.first == key;
				});
			if (found == map.Data->Fields.end())
				map.Data->Fields.emplace_back(std::move(key), std::move(value));
			else
				found->second = std::move(value);
		};
		for (auto &value : values)
			set(std::move(value.Port), std::move(value.Data));
		for (auto &image : images)
			set(std::move(image.Port), SurfaceValue{std::move(image.Data)});
		for (auto &images : arrays) {
			ArrayValue array;
			array.ElementType = ValueType::Any;
			if (!images.Data.Items.empty()) {
				source_array::TreeCost cost;
				if (!source_array::ImageCost(images.Data, images.Data.Items, cost, 1)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"getInputs image array exceeds source bounds",
						owner.SourceOwnerId,
						images.Port
					);
					return diagnostic.Code;
				}
				array.Items = source_array::FromImages(images.Data, images.Data.Items);
			} else {
				array.Items.reserve(images.Data.Images.size());
				for (auto &image : images.Data.Images)
					array.Items.push_back({std::move(image)});
			}
			set(std::move(images.Port), std::move(array));
		}
		if (!mapCharge->Resize(RetainedPayloadBytes(map))) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"getInputs map retained copy exceeds live bytes",
				owner.SourceOwnerId
			);
			return diagnostic.Code;
		}
		row->Inputs = std::move(map);
		auto displaced = candidateCharge.Split(oldPayload);
		if (!displaced || !candidateCharge.Merge(std::move(*mapCharge))) std::terminate();
		diagnostic = {};
		return Status::Ok;
	}
}
