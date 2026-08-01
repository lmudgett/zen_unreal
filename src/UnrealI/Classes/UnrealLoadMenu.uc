//=============================================================================
// UnrealLoadMenu
//=============================================================================
class UnrealLoadMenu expands UnrealSlotMenu
	localized
	config;

var() localized string[32] RestartString;

function bool ProcessSelection()
{
	local int Slot;

	if ( Selection == 1 )
	{
		PlayerOwner.ReStartLevel();
		return true;
	}
	// Selection is a display position (newest first); resolve the real slot.
	Slot = SlotOrder[Selection - 2];
	if ( SlotNames[Slot] ~= "..Empty.." )
		return false;
	bExitAllMenus = true;
	PlayerOwner.ClientMessage("");
	if ( Left(SlotNames[Slot], 4) == "Net:" )
		Level.ServerTravel( "?load=" $ Slot, false);
	else
		PlayerOwner.ClientTravel( "?load=" $ Slot, TRAVEL_Absolute, false);
	return true;
}

function DrawMenu(canvas Canvas)
{
	DrawBackGround(Canvas, (Canvas.ClipY < 320));

	DrawTitle(Canvas);
	DrawSlots(Canvas);	
}

function DrawSlots(canvas Canvas)
{
	local int StartX, StartY, Spacing, i;
			
	Spacing = Clamp(0.05 * Canvas.ClipY, 12, 32);
	StartX = Max(20, 0.5 * (Canvas.ClipX - 206));
	StartY = Max(40, 0.5 * (Canvas.ClipY - MenuLength * Spacing-40));
	Canvas.Font = Canvas.MedFont;

	Canvas.SetPos(StartX, StartY);
	Canvas.DrawText(RestartString$Level.Title, False);

	For ( i=1; i<10; i++ )
	{
		Canvas.SetPos(StartX, StartY + i * Spacing );
		Canvas.DrawText(SlotNames[SlotOrder[i-1]], False);
	}

	// show selection
	Canvas.SetPos( StartX - 20, StartY + Spacing * (Selection - 1));
	Canvas.DrawText("[]", false);	
}

defaultproperties
{
     RestartString="Restart "
     MenuLength=10
     MenuTitle="LOAD GAME"
}
