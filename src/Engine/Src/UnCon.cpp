/*=============================================================================
	UnCon.cpp: Implementation of UConsole class
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.
=============================================================================*/

#include "EnginePrivate.h"
#include "UnRender.h"

/*------------------------------------------------------------------------------
	UConsole object implementation.
------------------------------------------------------------------------------*/

IMPLEMENT_CLASS(UConsole);

// x64 port: how much of the screen the pulled-down console may cover.
#define CONSOLE_MAX_FRACTION 0.25f

/*------------------------------------------------------------------------------
	Console.
------------------------------------------------------------------------------*/

//
// Constructor.
//
UConsole::UConsole()
{}

//
// Init console.
//
void UConsole::_Init( UViewport* InViewport )
{
	guard(UConsole::_Init);
	check(sizeof(UConsole)==UConsole::StaticClass->GetPropertiesSize());

	// Set properties.
	Viewport		= InViewport;
	TopLine			= MAX_LINES-1;
	BorderSize		= 1; 

	// Init scripting.
	InitExecution();

	// Start console log.
	Logf(LocalizeGeneral("Engine","Core"));
	Logf(LocalizeGeneral("Copyright","Core"));
	Logf(" ");
	Logf(" ");

	unguard;
}

/*------------------------------------------------------------------------------
	Viewport console output.
------------------------------------------------------------------------------*/

//
// Print a message on the playing screen.
// Time = time to keep message going, or 0=until next message arrives, in 60ths sec
//
void UConsole::WriteBinary( const void* Data, INT Length, EName ThisType )
{
	guard(UConsole::WriteBinary);
	eventMessage( (const char*)Data, ThisType );
	unguard;
}

//
// x64 port: an output device that forwards to the console and remembers
// whether anything came through. Most exec handlers report only to the log,
// so a command typed at the console produced no visible reply at all and there
// was no way to tell "done" from "silently did nothing" -- see execConsoleCommand.
//
class FConsoleEcho : public FOutputDevice
{
public:
	FConsoleEcho( UConsole* InConsole ) : Console(InConsole), Wrote(0) {}
	void WriteBinary( const void* Data, INT Length, EName MsgType=NAME_None )
	{
		if( Length > 0 )
			Wrote = 1;
		Console->WriteBinary( Data, Length, MsgType );
	}
	UConsole*	Console;
	UBOOL		Wrote;
};

void UConsole::execConsoleCommand( FFrame& Stack, BYTE*& Result )
{
	guardSlow(UConsole::execLog);

	P_GET_STRING(S);
	P_FINISH;

	// x64 port: always answer a typed command. Handlers that print their own
	// result (stat readouts, the screenshot's filename) are left to speak for
	// themselves; the rest get a plain acknowledgement so the console confirms
	// the command ran. Failures stay with the script, which prints the
	// localized "unrecognized command" -- echoing here too would double it.
	FConsoleEcho Echo( this );
	UBOOL Ok = Viewport->Exec( S, &Echo );
	if( Ok && !Echo.Wrote )
		Echo.Logf( "%s: ok", S );
	*(DWORD*)Result = Ok;

	unguardexecSlow;
}
AUTOREGISTER_INTRINSIC( UConsole, INDEX_NONE, execConsoleCommand );

/*------------------------------------------------------------------------------
	Rendering.
------------------------------------------------------------------------------*/

//
// Called before rendering the world view.  Here, the
// Viewport console code can affect the screen's Viewport,
// for example by shrinking the view according to the
// size of the status bar.
//
FSceneNode SavedFrame;
void UConsole::PreRender( FSceneNode* Frame )
{
	guard(UConsole::PreRender);

	// Prevent status redraw due to changing.
	eventTick( Viewport->CurrentTime - Viewport->LastUpdateTime );

	// Save the Viewport.
	SavedFrame = *Frame;

	// Compute new status info.
	BorderLines		= 0;
	BorderPixels	= 0;
	ConsoleLines	= 0;

	// Compute sizing of all visible status bar components.
	if( ConsolePos > 0.0 )
	{
		// Show console. x64 port: capped at a quarter of the screen. The script
		// slides ConsolePos to its own target (0.9 in the shipped Engine.u),
		// which buries all but a strip of the game under the panel -- useless
		// when the console is being used to look AT something, e.g. typing
		// "shot" to photograph an artifact. Capping the pixel height here
		// rather than editing the script keeps this working with the retail
		// Engine.u, which is not rebuilt from source: the panel simply stops
		// growing a quarter of the way down, and the text, which is laid out
		// upward from the panel's bottom edge, follows it.
		ConsoleLines = Min(ConsolePos * (FLOAT)Frame->Y, CONSOLE_MAX_FRACTION * (FLOAT)Frame->Y);
		Frame->Y -= ConsoleLines;
	}
	if( BorderSize>=2 )
	{
		// Encroach on screen area.
		FLOAT Fraction = (FLOAT)(BorderSize-1) / (FLOAT)(MAX_BORDER-1);

		BorderLines = (int)Min((FLOAT)Frame->Y * 0.25f * Fraction,(FLOAT)Frame->Y);
		BorderLines = ::Max(0,BorderLines - ConsoleLines);
		Frame->Y -= 2 * BorderLines;

		BorderPixels = (int)Min((FLOAT)Frame->X * 0.25f * Fraction,(FLOAT)Frame->X) & ~3;
		Frame->X -= 2 * BorderPixels;
	}
	Frame->XB += BorderPixels;
	Frame->YB += ConsoleLines + BorderLines;
	Frame->ComputeRenderSize();

	unguard;
}

//
// Refresh the player console on the specified Viewport.  This is called after
// all in-game graphics are drawn in the rendering loop, and it overdraws stuff
// with the status bar, menus, and chat text.
//
void UConsole::PostRender( FSceneNode* Frame )
{
	guard(UConsole::PostRender);
	check(Viewport->Client->Engine->Client);
	*Frame = SavedFrame;

	// x64 port: the canvas exposes a VIRTUAL size (raw px / UIScale) and scales
	// blits up; the console draws through the canvas, so it must position in the
	// same virtual space (else at 4K the panel/text land at 3x their coords).
	// UIScale==1 at/below 720p, so this is a no-op there.
	FLOAT UIScale  = Max( 1.f, (FLOAT)Frame->Y / 720.f );
	FLOAT VClipX   = Frame->X / UIScale;
	FLOAT VClipY   = Frame->Y / UIScale;
	// x64 port: FLOOR the console height to a whole virtual pixel. The panel is
	// blitted at float coords (smooth) but the text funnels through Printf's INT
	// params, so it snaps to UIScale-sized steps -- at high res the text juddered
	// against the smoothly-sliding panel. Flooring puts panel and text on the same
	// integer grid so they move together (no relative jitter).
	FLOAT VConsole = appFloor( ConsoleLines / UIScale );
	FLOAT VBorderL = BorderLines  / UIScale;
	FLOAT VBorderP = BorderPixels / UIScale;

	// Big status message.
	UFont* LargeFont = Viewport->Canvas->LargeFont;
	char BigMessage[256]="";
	if( Viewport->Actor->bShowMenu )
		appStrcpy( BigMessage, "" );		
	else if( Viewport->Actor->Level->LevelAction==LEVACT_Loading )
		appStrcpy( BigMessage, LocalizeProgress("Loading") );
	else if( Viewport->Actor->Level->LevelAction==LEVACT_Saving )
		appStrcpy( BigMessage, LocalizeProgress("Saving") );
	else if( Viewport->Actor->Level->LevelAction==LEVACT_Connecting )
		appStrcpy( BigMessage, LocalizeProgress("Connecting") );
	else if( Viewport->Actor->Level->Pauser[0] )
	{
		LargeFont = Viewport->Canvas->MedFont;
		appSprintf( BigMessage, LocalizeProgress("Paused"), Viewport->Actor->Level->Pauser );
	}
	if( BigMessage[0] )
	{
		appStrupr( BigMessage );
		INT XL, YL;
		Viewport->Canvas->StrLen( LargeFont, XL, YL, BigMessage );
		Viewport->Canvas->Printf( LargeFont, VClipX/2-XL/2, VClipY/2-YL/2, "%s", BigMessage );
	}

	// If the console has changed since the previous frame, draw it (virtual coords).
	FLOAT YStart   = VBorderL;
	FLOAT YEnd	   = VClipY - VBorderL;
	if( ConsoleLines > 0 )
		Viewport->Canvas->DrawPattern( ConBackground, 0.0, 0.0, VClipX, VConsole, 1.0, 0.0, VConsole, NULL, 1.0, FPlane(0.7,0.7,0.7,0), FPlane(0,0,0,0), 0 );

	// Draw border.
	if( BorderLines>0 || BorderPixels>0 )
	{
		YStart += VConsole;
		if( BorderLines > 0 )
		{
			Viewport->Canvas->DrawPattern( Border, 0, 0, VClipX, VBorderL, 1.0, 0.0, 0.0, NULL, 1.0, FPlane(1,1,1,0), FPlane(0,0,0,0), 0 );
			Viewport->Canvas->DrawPattern( Border, 0, YEnd, VClipX, VBorderL, 1.0, 0.0, 0.0, NULL, 1.0, FPlane(1,1,1,0), FPlane(0,0,0,0), 0 );
		}
		if( BorderPixels > 0 )
		{
			Viewport->Canvas->DrawPattern( Border, 0, YStart, VBorderP, YEnd-YStart, 1.0, 0.0, 0.0, NULL, 1.0, FPlane(1,1,1,0), FPlane(0,0,0,0), 0 );
			Viewport->Canvas->DrawPattern( Border, VClipX-VBorderP, YStart, VBorderP, YEnd-YStart, 1.0, 0.0, 0.0, NULL, 1.0, FPlane(1,1,1,0), FPlane(0,0,0,0), 0 );
		}
	}

	// Draw console text.
	if( ConsoleLines )
	{
		// Console is visible; display console view.
		FLOAT Y = VConsole-1;
		appSprintf(MsgText[(TopLine + 1 + MAX_LINES) % MAX_LINES],"(> %s_",TypedStr);
		for( INT i=Scrollback; i<(NumLines+1); i++ )
		{
			// Display all text in the buffer.
			INT Line = (TopLine + MAX_LINES*2 - (i-1)) % MAX_LINES;

			INT XL,YL;
			Viewport->Canvas->WrappedStrLen( Viewport->Canvas->MedFont, XL, YL, VClipX-8, MsgText[Line] );

			// Half-space blank lines.
			if( YL == 0 )
				YL = 5;

			Y -= YL;
			if( (Y+YL)<0 )
				break;
			Viewport->Canvas->CurX = 4;
			Viewport->Canvas->CurY = Y;
			Viewport->Canvas->WrappedPrintf( Viewport->Canvas->MedFont, 0, "%s", MsgText[Line] );
		}
	}
	else
	{
		// Console is hidden; display single-line view.
		if( TextLines>0 && MsgType!=NAME_None && (!Viewport->Actor->bShowMenu || Viewport->Actor->bShowScores) )
		{
			int iLine=TopLine;
			for( int i=0; i<NumLines; i++ )
			{
				if( *MsgText[iLine] )
					break;
				iLine = (iLine-1+MAX_LINES)%MAX_LINES;
			}
			Viewport->Canvas->CurX = 4;
			Viewport->Canvas->CurY = 2;
			Viewport->Canvas->WrappedPrintf( Viewport->Canvas->MedFont, 1, "%s", MsgText[iLine] );
			if ( TextLines > 1 )
			{
				iLine = (iLine-1+MAX_LINES)%MAX_LINES;
				for ( int j=0; j<i; j++ )
				{
					if( *MsgText[iLine] )
						break;
					iLine = (iLine-1+MAX_LINES)%MAX_LINES;
				}
				Viewport->Canvas->CurY = 12;
				Viewport->Canvas->WrappedPrintf( Viewport->Canvas->MedFont, 1, "%s", MsgText[iLine] );
			}
		}
		if( GetMainFrame()->Node && GetMainFrame()->Node->GetFName()=="Typing" )
		{
			// Draw stuff being typed.
			int XL,YL;
			char S[256];
			appSprintf( S, "(> %s_", TypedStr );
			Viewport->Canvas->WrappedStrLen( Viewport->Canvas->MedFont, XL, YL, VClipX-8, S );
			Viewport->Canvas->CurX = 2;
			Viewport->Canvas->CurY = VClipY - VConsole - YL - 1;
			Viewport->Canvas->WrappedPrintf( Viewport->Canvas->MedFont, 0, "%s", S );
		}
	}
	unguard;
}

/*------------------------------------------------------------------------------
	The End.
------------------------------------------------------------------------------*/
