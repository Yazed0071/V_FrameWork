local this = {}

this.registerIvars={
	"V_FrameWork_Welcome_Message",
	"V_FrameWork_Version_1_4_Message",
}

this.V_FrameWork_Welcome_Message={
	save=IvarProc.CATEGORY_EXTERNAL,
	default=0,
	range={min=0,max=1,},
}

this.V_FrameWork_Version_1_4_Message={
	save=IvarProc.CATEGORY_EXTERNAL,
	default=0,
	range={min=0,max=1,},
}



function this.Messages()
	return
	Tpp.StrCode32Table {
		Terminal = {
			{
				msg = "MbDvcActOpenTop",
				func = function()
                    if TppMission.IsHelicopterSpace(vars.missionCode) then
                        if Ivars.V_FrameWork_Welcome_Message:Get() == 0 then
                            Ivars.V_FrameWork_Welcome_Message:Set(1)
                            V_TppUiCommand.ShowMbDvcAnnouncePopupRewardLangId("MbDvcPopup_Title_FirstTimeInstall",  "MbDvcPopup_Text_FirstTimeInstall")
                        end
                        if Ivars.V_FrameWork_Version_1_4_Message:Get() == 0 then
                            Ivars.V_FrameWork_Version_1_4_Message:Set(1)
                            V_TppUiCommand.ShowMbDvcAnnouncePopupReportLangId("MbDvcPopup_Title_V_FrameWork_Version_1_4_Message",  "MbDvcPopup_Text_V_FrameWork_Version_1_4_Message")
                        end
                    end
				end
			},
		},
	}
end

function this.Init(missionTable)
  this.messageExecTable=Tpp.MakeMessageExecTable(this.Messages())
end

function this.OnMessage(sender,messageId,arg0,arg1,arg2,arg3,strLogText)
  Tpp.DoMessage(this.messageExecTable,TppMission.CheckMessageOption,sender,messageId,arg0,arg1,arg2,arg3,strLogText)
end

return this
