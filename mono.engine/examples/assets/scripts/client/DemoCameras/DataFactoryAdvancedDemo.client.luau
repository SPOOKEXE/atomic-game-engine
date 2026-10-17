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

local cameraMetadata = DataSceneService:GetCameraRenderingData(camera)
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
