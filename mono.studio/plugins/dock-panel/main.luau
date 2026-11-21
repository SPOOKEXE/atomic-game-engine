--!strict
-- A dock widget, a toggle button and a setting that survives a restart.
local wasOpen = plugin:GetSetting("open") == true

local info = DockWidgetPluginGuiInfo.new(Enum.InitialDockState.Right, wasOpen, false, 240, 160, 160, 120)
local widget = plugin:CreateDockWidgetPluginGui("DockPanel", info)
widget.Title = "Dock Panel"

local label = Instance.new("TextLabel")
label.Size = UDim2.fromScale(1, 1)
label.Text = "Hello from a dock widget"
label.Parent = widget

local toolbar = plugin:CreateToolbar("Dock Panel")
local button = toolbar:CreateButton("dock-panel-toggle", "Show or hide the panel", "", "Panel")
button:SetActive(widget.Enabled)

local function setOpen(open: boolean)
	widget.Enabled = open
	button:SetActive(open)
	plugin:SetSetting("open", open)
end

button.Click:Connect(function()
	setOpen(not widget.Enabled)
end)

widget:BindToClose(function()
	setOpen(false)
end)
