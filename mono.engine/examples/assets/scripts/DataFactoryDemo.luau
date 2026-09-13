-- A compact data-factory scene: each exported entity declares a stable string
-- identity, then the script reads the ECS-backed metadata the MCP boundary uses.
-- Run `client --data-factory --mcp-port 8738 --script DataFactoryDemo.luau`.
-- The MCP owner pauses, snapshots, steps, polls and releases independently of
-- this world's scheduler. Do not use task.wait polling after all_systems pause:
-- a suspended scheduler cannot advance that waiter. The external sequence is
-- lifecycle_inspect -> pause -> snapshot -> capture -> poll_capture ->
-- get_resource(id, {instance_id, ticket, offset, max_bytes}) -> release_capture.
-- The factory profile is `Default PBR#1` and its capture node is `data-capture`.
-- Poll metadata carries calibrated world_from_camera and only declares a
-- projection when the renderer captured the exact resolved matrix.

local DataSceneService = game:GetService("DataSceneService")

type CapturePlane = {
	status: string,
	resource: string,
	byte_size: number,
}
type CaptureResult = {
	status: string,
	planes: {CapturePlane},
	copied_payload_bytes: number?,
}

local crate = Instance.new("Part")
crate.Name = "DataFactoryCrate"
crate:SetAttribute("DataFactoryId", "data-factory-demo/crate")
crate.Position = Vector3.new(0, 1, 0)
crate.Size = Vector3.new(2, 2, 2)
crate.Parent = workspace

local camera = Instance.new("Camera")
camera.Name = "DataFactoryCamera"
camera:SetAttribute("DataFactoryId", "data-factory-demo/camera")
camera.CFrame = CFrame.new(0, 3, 8)
camera.ImageWidth = 640
camera.ImageHeight = 360
camera.Parent = workspace
workspace.CurrentCamera = camera

local image = Instance.new("EditableImage")
image.Name = "DataFactoryLabel"
image:SetAttribute("DataFactoryId", "data-factory-demo/label")
image:DrawRectangle(Vector2.new(0, 0), Vector2.new(16, 16), Color3.fromRGB(255, 128, 0), 0)
local pixels = image:ToBuffer()
assert(image:FromBuffer(pixels))

local capabilities = DataSceneService:GetCapabilities()
local snapshot = DataSceneService:GetSceneSnapshot(128)
local cameraMetadata = DataSceneService:GetCameraRenderingData(camera)
local imageMetadata = DataSceneService:GetEditableImageMetadata(image)
local captureChannels = DataSceneService:GetCaptureChannels()

print(
	"DataFactory capabilities",
	capabilities.schema_version,
	capabilities.camera_metadata_schema_version,
	capabilities.physics_observation_schema_version,
	capabilities.scene_snapshot
)
print("DataFactory snapshot", snapshot.tick, #snapshot.entities, snapshot.coverage)
print(
	"DataFactory physics",
	snapshot.physics_observations.schema_version,
	snapshot.physics_observations.world_prepared,
	snapshot.physics_observations.contacts.available,
	snapshot.physics_observations.impulses.available,
	snapshot.physics_observations.forces.available,
	snapshot.physics_observations.forces.reason
)
print(
	"DataFactory camera",
	cameraMetadata.id,
	cameraMetadata.world_from_camera,
	cameraMetadata.camera_from_world,
	cameraMetadata.vertical_fov_radians,
	cameraMetadata.near_metres,
	cameraMetadata.far_metres,
	cameraMetadata.requested_width,
	cameraMetadata.requested_height,
	cameraMetadata.coordinate_convention
)
print(
	"DataFactory calibration",
	cameraMetadata.projection_available,
	cameraMetadata.projection_reason,
	cameraMetadata.crop.convention,
	cameraMetadata.lens_distortion.available,
	cameraMetadata.lens_distortion.reason,
	cameraMetadata.temporal_jitter.available,
	cameraMetadata.temporal_jitter.reason,
	cameraMetadata.units.world,
	cameraMetadata.units.image
)
print("DataFactory image", imageMetadata.width, imageMetadata.height, imageMetadata.format, imageMetadata.row_order)
print("DataFactory image bytes", buffer.len(pixels))
print("DataFactory capture", captureChannels.status, captureChannels.reason)

-- The host-owned evaluation path supplies the barriered snapshot ID and ticket
-- explicitly. Each host call does one request or poll, so it needs no scheduler
-- tick. The lifecycle sequence is pause -> snapshot -> capture -> poll_capture
-- -> get_resource -> release_capture. The current runtime evaluates host chunks
-- in isolated environments, so the host retains the ticket between calls.

DataSceneService:SetCaptureDriver(function(snapshotId, ticket)
	if ticket ~= nil then
		local result = DataSceneService:PollCapture(ticket) :: CaptureResult
		if result.status == "ready" or result.status == "partial" then
			local payloadBytes = 0
			local declaredBytes = 0
			for _, plane in ipairs(result.planes) do
				if plane.status == "ready" then
					assert(type(plane.byte_size) == "number" and plane.byte_size > 0)
					declaredBytes += plane.byte_size
					local offset = 0
					while offset < plane.byte_size do
						local remaining = plane.byte_size - offset
						local requested = math.min(remaining, 65536)
						local bytes = DataSceneService:GetCaptureBuffer(ticket, plane.resource, offset, requested)
						local copied = buffer.len(bytes)
						assert(copied > 0 and copied <= requested)
						offset += copied
						payloadBytes += copied
					end
				end
			end
			assert(payloadBytes == declaredBytes)
			result.copied_payload_bytes = payloadBytes
		end
		return result
	end
	return DataSceneService:Capture({
		snapshot_id = snapshotId,
		pipeline = "Default PBR#1",
		capture_node = "data-capture",
		view_slot = 0,
		channels = {"rgb_linear_hdr"},
		temporal_history = "preserve",
	})
end)
