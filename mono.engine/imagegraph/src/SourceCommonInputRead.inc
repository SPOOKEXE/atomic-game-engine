namespace detail {
	Status ReadHeldSourceCommonInput(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		std::string_view port,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		Value &result,
		AllocationReservation &resultCharge,
		Diagnostic &diagnostic
	) {
		if (!request.GroupRender) {
			SetDiagnostic(
				diagnostic,
				Status::UnsupportedExecution,
				"named common input needs held producer sockets",
				std::string(nodeId),
				std::string(port)
			);
			return diagnostic.Code;
		}
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
			return n.Id == nodeId;
		});
		if (node == document.Nodes.end() || !FindPortType(*node, port, PortDirection::Input, &document)) {
			SetDiagnostic(
				diagnostic,
				Status::UnknownPort,
				"named common input is not declared",
				std::string(nodeId),
				std::string(port)
			);
			return diagnostic.Code;
		}
		std::vector<EvaluationInputValue> values;
		std::vector<EvaluationInputImage> images;
		std::vector<SnapshotImageArray> arrays;
		std::optional<SurfaceFormat> policy;
		int64_t interpolation = 1;
		AllocationReservation captureCharge;
		NodeInputCapture capture{nodeId, &values, &images, &policy, &captureCharge, &arrays, &interpolation};
		NodeResult ignored;
		AllocationReservation ignoredCharge;
		const auto status = EvaluateGraph(
			document,
			plan,
			{},
			request,
			ignored,
			diagnostic,
			budget,
			ignoredCharge,
			nullptr,
			nullptr,
			&capture,
			nullptr,
			nodeId,
			{},
			true
		);
		if (status != Status::Ok) return status;
		const auto value =
			std::find_if(values.begin(), values.end(), [&](const auto &v) { return v.Port == port; });
		if (value != values.end()) {
			auto charge = budget.Reserve(RetainedPayloadBytes(value->Data));
			if (!charge) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"named common input payload exceeds live bytes",
					std::string(nodeId),
					std::string(port)
				);
				return diagnostic.Code;
			}
			Value copied = value->Data;
			if (!charge->Resize(RetainedPayloadBytes(copied))) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "named common input copy exceeds live bytes"
				);
				return diagnostic.Code;
			}
			result = std::move(copied);
			resultCharge = std::move(*charge);
			return Status::Ok;
		}
		const auto image =
			std::find_if(images.begin(), images.end(), [&](const auto &v) { return v.Port == port; });
		if (image != images.end()) {
			const uint64_t bytes = image->Data.Pixels.capacity();
			auto charge = captureCharge.Split(bytes);
			if (!charge) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "named image input lacks retained admission"
				);
				return diagnostic.Code;
			}
			result = SurfaceValue{std::move(image->Data)};
			resultCharge = std::move(*charge);
			return Status::Ok;
		}
		const auto array =
			std::find_if(arrays.begin(), arrays.end(), [&](const auto &v) { return v.Port == port; });
		if (array != arrays.end()) {
			source_array::TreeCost cost;
			if (!array->Data.Items.empty() &&
				!source_array::ImageCost(array->Data, array->Data.Items, cost, 1)) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "named image array input exceeds source bounds"
				);
				return diagnostic.Code;
			}
			uint64_t bytes = captureCharge.Bytes();
			if (!AddBytes(bytes, cost.Bytes) ||
				!AddArrayBytes(bytes, array->Data.Images.size(), sizeof(SourceArrayItem))) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "named image array size overflows");
				return diagnostic.Code;
			}
			auto charge = budget.Reserve(bytes);
			if (!charge) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "named image array exceeds live bytes");
				return diagnostic.Code;
			}
			ArrayValue copied;
			copied.ElementType = ValueType::Any;
			if (!array->Data.Items.empty())
				copied.Items = source_array::FromImages(array->Data, array->Data.Items);
			else {
				copied.Items.reserve(array->Data.Images.size());
				for (auto &image : array->Data.Images)
					copied.Items.push_back({std::move(image)});
			}
			if (!charge->Resize(RetainedPayloadBytes(copied))) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "named array copy exceeds live bytes");
				return diagnostic.Code;
			}
			result = std::move(copied);
			resultCharge = std::move(*charge);
			return Status::Ok;
		}
		SetDiagnostic(
			diagnostic,
			Status::UnsupportedExecution,
			"named input getter produced no value",
			std::string(nodeId),
			std::string(port)
		);
		return diagnostic.Code;
	}
}
