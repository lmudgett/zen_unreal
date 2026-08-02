//=============================================================================
// UnrealSlotMenu
//=============================================================================
class UnrealSlotMenu expands UnrealMenu
	config
	localized;

var globalconfig string[100] SlotNames[9];
var globalconfig int SaveTimes[9];	// minutes-resolution save stamp, 0 = never saved
var int SlotOrder[9];				// display order: newest save first, empties last
var localized string[16] MonthNames[12];

function BeginPlay()
{
	Super.BeginPlay();
	BuildSlotOrder();
}

// Effective ordering stamp for a slot: the recorded SaveTimes when present,
// else reconstructed by parsing the display string ("Title H:MM Month Day",
// as UnrealSaveMenu writes it) so saves from before stamps existed still
// sort. The string has no year; assume the current one.
function int SlotStamp( int i )
{
	local string[100] Rest;
	local string[32] Tok2, Tok3, T;
	local int j, Day, Mon, Hour, Min;

	if ( SaveTimes[i] != 0 )
		return SaveTimes[i];
	if ( SlotNames[i] ~= "..Empty.." )
		return 0;

	Rest = SlotNames[i];
	if ( Left(Rest, 4) ~= "Net:" )
		Rest = Mid(Rest, 4);
	j = InStr(Rest, " ");
	While ( j != -1 )
	{
		T = Left(Rest, j);
		Rest = Mid(Rest, j + 1);
		Tok2 = Tok3;
		Tok3 = T;
		j = InStr(Rest, " ");
	}
	// Now Rest = day, Tok3 = month name, Tok2 = "H:MM".
	Day = int(Rest);
	For ( j=0; j<12; j++ )
		if ( MonthNames[j] ~= Tok3 )
			Mon = j + 1;
	j = InStr(Tok2, ":");
	if ( j == -1 || Mon == 0 || Day == 0 )
		return 1;	// unparseable but real save: sorts as oldest, above empties
	Hour = int(Left(Tok2, j));
	Min  = int(Mid(Tok2, j + 1));
	return ((((Max(Level.Year - 2020, 0) * 12 + Mon) * 31 + Day) * 24 + Hour) * 60 + Min) * 60;
}

// Sort the display order newest-save-first; empty slots (stamp 0) sink to the
// end. Stable, so equal-stamp saves keep their slot order.
function BuildSlotOrder()
{
	local int i, j, t;
	local int Stamps[9];

	For ( i=0; i<9; i++ )
	{
		SlotOrder[i] = i;
		Stamps[i] = SlotStamp(i);
	}
	For ( i=1; i<9; i++ )
	{
		t = SlotOrder[i];
		j = i - 1;
		While ( (j >= 0) && (Stamps[SlotOrder[j]] < Stamps[t]) )
		{
			SlotOrder[j+1] = SlotOrder[j];
			j--;
		}
		SlotOrder[j+1] = t;
	}
}

function DrawSlots(canvas Canvas)
{
	local int StartX, StartY, Spacing, i;
			
	Spacing = Clamp(0.05 * Canvas.ClipY, 12, 32);
	StartX = Max(20, 0.5 * (Canvas.ClipX - 206));
	StartY = Max(40, 0.5 * (Canvas.ClipY - MenuLength * Spacing-40));
	Canvas.Font = Canvas.MedFont;

	For ( i=1; i<10; i++ )
	{
		Canvas.SetPos(StartX, StartY + i * Spacing );
		Canvas.DrawText(SlotNames[SlotOrder[i-1]], False);
	}

	// show selection
	Canvas.SetPos( StartX - 20, StartY + Spacing * Selection);
	Canvas.DrawText("[]", false);	
}

defaultproperties
{
     SlotNames(0)="..Empty.."
     SlotNames(1)="..Empty.."
     SlotNames(2)="..Empty.."
     SlotNames(3)="..Empty.."
     SlotNames(4)="..Empty.."
     SlotNames(5)="..Empty.."
     SlotNames(6)="..Empty.."
     SlotNames(7)="..Empty.."
     SlotNames(8)="..Empty.."
     MonthNames(0)="January"
     MonthNames(1)="February"
     MonthNames(2)="March"
     MonthNames(3)="April"
     MonthNames(4)="May"
     MonthNames(5)="June"
     MonthNames(6)="July"
     MonthNames(7)="August"
     MonthNames(8)="September"
     MonthNames(9)="October"
     MonthNames(10)="November"
     MonthNames(11)="December"
     MenuLength=9
}
