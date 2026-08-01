//=============================================================================
// UnrealSaveMenu
//=============================================================================
class UnrealSaveMenu expands UnrealSlotMenu
	localized
	config;

var localized string[128] CantSave;

function BeginPlay()
{
	local int i;

	Super.BeginPlay();
	For (i=0; i<9; i++ )
		if (SlotNames[SlotOrder[i]] ~= "..Empty.." )
		{
			Selection = i + 1;
			break;
		}
}

function bool ProcessSelection()
{
	local int Slot;

	if ( PlayerOwner.Health <= 0 )
		return true;

	// Selection is a display position (newest first); resolve the real slot.
	Slot = SlotOrder[Selection - 1];
	if ( Level.Minute < 10 )
		SlotNames[Slot] = (Level.Title$" "$Level.Hour$"\:0"$Level.Minute$" "$MonthNames[Level.Month - 1]$" "$Level.Day);
	else
		SlotNames[Slot] = (Level.Title$" "$Level.Hour$"\:"$Level.Minute$" "$MonthNames[Level.Month - 1]$" "$Level.Day);

	if ( Level.NetMode != NM_Standalone )
		SlotNames[Slot] = "Net:"$SlotNames[Slot];
	SaveTimes[Slot] = (((Level.Year * 12 + Level.Month) * 31 + Level.Day) * 24 + Level.Hour) * 60 + Level.Minute;
	SaveConfig();
	bExitAllMenus = true;
	PlayerOwner.ClientMessage(" ");
	PlayerOwner.bDelayedCommand = true;
	PlayerOwner.DelayedCommand = "SaveGame "$Slot;
	return true;
}

function DrawMenu(canvas Canvas)
{

	if ( PlayerOwner.Health <= 0 )
	{
		MenuTitle = CantSave;
		DrawTitle(Canvas);
		return;
	}

	DrawBackGround(Canvas, (Canvas.ClipY < 320));
	DrawTitle(Canvas);
	DrawSlots(Canvas);	
}

defaultproperties
{
     CantSave="CAN'T SAVE WHEN DEAD"
     MenuTitle="SAVE GAME"
}
