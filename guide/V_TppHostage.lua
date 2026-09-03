local this = {}

local StrCode32              = Fox.StrCode32
local IsTypeString           = Tpp.IsTypeString
local GetGameObjectId        = GameObject.GetGameObjectId
local GetGameObjectIdByIndex = GameObject.GetGameObjectIdByIndex
local SendCommand            = GameObject.SendCommand
local NULL_ID                = GameObject.NULL_ID

local HOSTAGE_OBJECT_TYPES = { "TppHostage2", "TppHostageUnique", "TppHostageUnique2" }

this.labels = {}


local function lookupCustomLabel(gameObjectId, gender, scenario)
    local label = this.labels[gameObjectId]

    if label == nil then
        for k, v in pairs(this.labels) do
            if type(k) == "string" and k ~= "male" and k ~= "female" and k ~= "child" then
                if GetGameObjectId(k) == gameObjectId then
                    label = v
                    break
                end
            end
        end
    end

    if label == nil then
        if gender == 0 then label = this.labels.male
        elseif gender == 1 then label = this.labels.female
        elseif gender == 2 then label = this.labels.child end
    end

    if type(label) == "table" then return label[scenario] end
    return label
end


local function pushEntry(hostage)
    hostage.customLabel      = lookupCustomLabel(hostage.gameObjectId, hostage.gender, "gone")
    hostage.customLabelTaken = lookupCustomLabel(hostage.gameObjectId, hostage.gender, "taken")
    GameObject.SendCommand(hostage.gameObjectId, {
        id                   = "SetLostHostage",
        hostageType          = hostage.gender,
        customLostLabel      = hostage.customLabel or 0,
        customLostLabelTaken = hostage.customLabelTaken or 0,
    })
end


-- gender: 0 = male, 1 = female, 2 = child.
function this.SetLostHostage(hostageNameOrId, gender, hostageLostLabel)
    if hostageNameOrId == nil then
        V_FrameWork.Log("V_TppHostage.SetLostHostage: hostageNameOrId is nil.")
        return
    end
    if IsTypeString(hostageNameOrId) then
        hostageNameOrId = GetGameObjectId(hostageNameOrId)
    end
    if hostageNameOrId == NULL_ID then
        V_FrameWork.Log("V_TppHostage.SetLostHostage: hostageId is NULL_ID.")
        return
    end
    if type(gender) ~= "number" then
        V_FrameWork.Log("V_TppHostage.SetLostHostage: gender is not a number (0 = male, 1 = female, 2 = child).")
        gender = 0
    end
    if type(hostageLostLabel) ~= "string" and type(hostageLostLabel) ~= "number" then
        V_FrameWork.Log("V_TppHostage.SetLostHostage: hostageLostLabel is not a string or number.")
        hostageLostLabel = 0
    end

    GameObject.SendCommand(hostageNameOrId, { id = "SetLostHostage", hostageType = gender, customLostLabel = hostageLostLabel or 0 })
end

function this.RemoveLostHostage(hostageNameOrId)
    if hostageNameOrId == nil then
        V_FrameWork.Log("V_TppHostage.RemoveLostHostage: hostageNameOrId is nil.")
        return
    end
    if IsTypeString(hostageNameOrId) then
        hostageNameOrId = GetGameObjectId(hostageNameOrId)
    end
    if hostageNameOrId == NULL_ID then
        V_FrameWork.Log("V_TppHostage.RemoveLostHostage: hostageId is NULL_ID.")
        return
    end
    GameObject.SendCommand(hostageNameOrId, { id = "RemoveLostHostage" })
end

function this.ClearLostHostages()
    GameObject.SendCommand({ type = "TppHostage2" }, { id = "ClearLostHostages" })
end


function this.GetHostageGender(hostageNameOrId)
    if hostageNameOrId == nil then
        V_FrameWork.Log("V_TppHostage.GetHostageGender: hostageNameOrId is nil.")
        return
    end
    if IsTypeString(hostageNameOrId) then
        hostageNameOrId = GetGameObjectId(hostageNameOrId)
    end
    if hostageNameOrId == NULL_ID then
        V_FrameWork.Log("V_TppHostage.GetHostageGender: hostageId is NULL_ID.")
        return
    end

    return SendCommand(hostageNameOrId, { id = "GetHostageGender" })
end

function this.IsHostageFemale(hostageNameOrId)
    return this.GetHostageGender(hostageNameOrId) == 1
end

function this.IsHostageChild(hostageNameOrId)
    return this.GetHostageGender(hostageNameOrId) == 2
end

function this.SetCustomLostLabel(key, value)
    this.labels[key] = value
    this.RefreshCustomLabels()
end

function this.ClearCustomLostLabel(key)
    this.labels[key] = nil
    this.RefreshCustomLabels()
end

function this.ClearAllCustomLostLabels()
    this.labels = {}
    this.RefreshCustomLabels()
end

function this.RegisterCustomLostLabels(t)
    if type(t) ~= "table" then
        V_FrameWork.Log("V_TppHostage.RegisterCustomLostLabels: argument is not a table.")
        return
    end
    for k, v in pairs(t) do
        this.labels[k] = v
    end
    this.RefreshCustomLabels()
end

function this.RefreshCustomLabels()
    if mvars.V_HostageList == nil then return end
    for _, hostage in ipairs(mvars.V_HostageList) do
        pushEntry(hostage)
    end
end


function this.BuildHostageList()
    mvars.V_HostageList = {}

    for _, hostageObjectType in ipairs(HOSTAGE_OBJECT_TYPES) do
        local hostageCount = SendCommand({ type = hostageObjectType }, { id = "GetMaxInstanceCount" })
        if hostageCount ~= nil then
            for i = 0, hostageCount - 1 do
                local hostageGameObjectId = GetGameObjectIdByIndex(hostageObjectType, i)
                if hostageGameObjectId ~= NULL_ID then
                    local gender = this.GetHostageGender(hostageGameObjectId) or 0
                    table.insert(mvars.V_HostageList, {
                        gameObjectId = hostageGameObjectId,
                        gender       = gender,
                        customLabel  = lookupCustomLabel(hostageGameObjectId, gender, "gone"),
                    })
                end
            end
        end
    end

    V_FrameWork.Log("[V_TppHostage]: Built V_HostageList with " .. tostring(#mvars.V_HostageList) .. " entries")
end

function this.AutoSetLostHostage()
    if mvars.V_HostageList == nil then
        this.BuildHostageList()
    end
    for _, hostage in ipairs(mvars.V_HostageList) do
        this.SetLostHostage(hostage.gameObjectId, hostage.gender, hostage.customLabel or 0)
    end
end



function this.Messages()
    return Tpp.StrCode32Table {
        UI = {
            {
                msg = "QuestAreaAnnounceText",
                func = function()
                    this.ClearLostHostages()
                    this.BuildHostageList()
                    this.AutoSetLostHostage()
                end,
            },
        },
        Mission = {
            {
                msg = "MissionStateReset",
                func = function(code)
                    this.ClearAllCustomLostLabels()
                end,
            },
        },
    }
end


function this.SetUpEnemy()
    this.ClearLostHostages()
    this.BuildHostageList()
    this.AutoSetLostHostage()
end


function this.Init(missionTable)
    this.messageExecTable = Tpp.MakeMessageExecTable(this.Messages())
end

function this.OnReload(missionTable)
    this.messageExecTable = Tpp.MakeMessageExecTable(this.Messages())
end

function this.OnMessage(sender, messageId, arg0, arg1, arg2, arg3, strLogText)
    Tpp.DoMessage(this.messageExecTable, TppMission.CheckMessageOption, sender, messageId, arg0, arg1, arg2, arg3, strLogText)
end


return this
