-- A compact spatial-observation scene for DataSceneService.
-- Run `client --data-factory --mcp-port 8738 --script DataFactoryDemo.luau`.
local DataSceneService = game:GetService("DataSceneService")

local function box(name, id, position, size)
	local part = Instance.new("Part")
	part.Name = name
	part:SetAttribute("DataFactoryId", id)
	part.Position = position
	part.Size = size
	part.Parent = workspace
	return part
end

local camera = Instance.new("Camera")
camera.Name = "DataFactoryCamera"
camera:SetAttribute("DataFactoryId", "data-factory-demo/camera")
camera.CFrame = CFrame.new(0, 2, 8)
camera.ImageWidth = 640
camera.ImageHeight = 360
camera.Parent = workspace
workspace.CurrentCamera = camera

box("VisibleBox", "data-factory-demo/visible", Vector3.new(0, 1, 0), Vector3.new(2, 2, 2))
box("PartlyOffscreenBox", "data-factory-demo/partly-offscreen", Vector3.new(8, 1, 0), Vector3.new(4, 2, 2))
box("BehindBox", "data-factory-demo/behind", Vector3.new(0, 1, 12), Vector3.new(2, 2, 2))

local snapshot = DataSceneService:GetSceneSnapshot(16)
local cameraData = DataSceneService:GetCameraRenderingData(camera, 16)
local captureOptions = DataSceneService:CreateOptions()
captureOptions.Channels = {"rgb_linear_hdr", "linear_depth", "mesh_uv", "object_ids", "semantic_ids", "part_ids"}
print("DataFactory snapshot", snapshot.entities)
print("DataFactory camera", cameraData.world_from_camera, cameraData.object_observations)
-- This scene has no retained lifecycle snapshot or render bridge. It checks
-- typed option defaults only. The external Python example owns capture submission.
print("DataFactory capture options", captureOptions.CameraId, captureOptions.Pipeline, captureOptions.StorageProfile)
for _, observation in ipairs(cameraData.object_observations) do
	print(observation.id, observation.camera_from_object, observation.projected_bounds, observation.occlusion)
end
