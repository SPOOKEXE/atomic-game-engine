--!strict

local DataSceneService = game:GetService("DataSceneService")
local frame = script:GetAttribute("CFrame")
if typeof(frame) ~= "CFrame" then
	return
end

local camera = Instance.new("Camera")
local name = script:GetAttribute("Name")
camera.Name = if typeof(name) == "string" then name else "DataFactoryCamera"
local dataFactoryId = script:GetAttribute("CameraDataFactoryId")
if typeof(dataFactoryId) == "string" then
	camera:SetAttribute("DataFactoryId", dataFactoryId)
end
camera.CFrame = frame
local fieldOfView = script:GetAttribute("FieldOfView")
if typeof(fieldOfView) == "number" then camera.FieldOfView = fieldOfView end
local nearPlane = script:GetAttribute("NearPlaneZ")
if typeof(nearPlane) == "number" then camera.NearPlaneZ = nearPlane end
local farPlane = script:GetAttribute("FarPlaneZ")
if typeof(farPlane) == "number" then camera.FarPlaneZ = farPlane end
local imageWidth = script:GetAttribute("ImageWidth")
if typeof(imageWidth) == "number" then camera.ImageWidth = imageWidth end
local imageHeight = script:GetAttribute("ImageHeight")
if typeof(imageHeight) == "number" then camera.ImageHeight = imageHeight end
camera.Parent = workspace
workspace.CurrentCamera = camera
local publishedCamera = Instance.new("ObjectValue")
publishedCamera.Name = "PublishedCamera"
publishedCamera.Value = camera
publishedCamera.Parent = script

local cameraData = DataSceneService:GetCameraRenderingData(camera, 16)
print("DataFactory camera", cameraData.world_from_camera, cameraData.object_observations)
for _, observation in ipairs(cameraData.object_observations) do
	print(observation.id, observation.camera_from_object, observation.projected_bounds, observation.occlusion)
end
