/*=============================================================================
	Launch.cpp: Unreal launcher.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

Revision history:
	* Created by Tim Sweeney.
=============================================================================*/

#include "LaunchPrivate.h"
#include "Window.h"
#include "Res\LaunchRes.h"
#pragma warning( disable: 4355 )

extern CORE_API FGlobalPlatform GTempPlatform;
extern "C" {HINSTANCE hInstance;}
extern "C" {char GPackage[64]="Launch";}

/*-----------------------------------------------------------------------------
	WinMain.
-----------------------------------------------------------------------------*/

//
// Main window entry point.
//
INT WINAPI WinMain( HINSTANCE hInInstance, HINSTANCE hPrevInstance, char* InCmdLine, INT nCmdShow )
{
	// Remember instance.
	GIsStarted = 1;
	hInstance = hInInstance;

	// x64 port: become DPI-aware before any window is created or any monitor is
	// queried, so fullscreen placement and multi-monitor coordinates use real
	// physical pixels instead of the OS's scaled/virtualized values. Without
	// this, fullscreen on a high-DPI or mixed-DPI multi-monitor desktop is sized
	// and positioned wrongly. Prefer per-monitor-aware v2, then fall back to the
	// older system-DPI-aware API; both are loaded dynamically for portability.
	{
		typedef BOOL (WINAPI *FSetDpiCtx)( HANDLE );
		typedef BOOL (WINAPI *FSetDpiAware)( void );
		HMODULE User32 = LoadLibraryA( "user32.dll" );
		FSetDpiCtx SetDpiCtx = User32 ? (FSetDpiCtx)GetProcAddress( User32, "SetProcessDpiAwarenessContext" ) : NULL;
		// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4.
		if( !( SetDpiCtx && SetDpiCtx( (HANDLE)(INT_PTR)-4 ) ) )
		{
			FSetDpiAware SetDpiAware = User32 ? (FSetDpiAware)GetProcAddress( User32, "SetProcessDPIAware" ) : NULL;
			if( SetDpiAware )
				SetDpiAware();
		}
	}

	// Set package name.
	appStrcpy( GPackage, appPackage() );

	INT GErrorLevel = 0; // x64 port: -selftest failure count -> exit code

	// Init mode.
	GIsServer = 1;
	GIsClient = !ParseParam(appCmdLine(),"SERVER") && !ParseParam(appCmdLine(),"MAKE");
	GIsEditor = ParseParam(appCmdLine(),"EDITOR") || ParseParam(appCmdLine(),"MAKE");

	// Init windowing.
	appChdir( appBaseDir() );
	InitWindowing();

	// Init log window.
	GExecHook = GThisExecHook;
	GLog = new WLog( "" );
	GLog->OpenWindow( NULL, 1 );

	// Begin.
#ifndef _DEBUG
	try
	{
#endif
		// Start main loop.
		GIsGuarded=1;
		GSystem = &GTempPlatform;
		// x64 port: -selftest runs subsystem checks in isolation (no window,
		// renderer, audio device, or gameplay) and exits with the failure
		// count as the process exit code. See SelfTest.cpp.
		if( ParseParam( appCmdLine(), "SELFTEST" ) )
		{
			extern INT RunSelfTests();
			appInit();
			GDynMem.Init( 65536 );
			GErrorLevel = RunSelfTests();
			GIsGuarded=0;
			// The test run loaded every retail package outside the normal
			// engine lifecycle; skip ALL teardown including DLL-detach static
			// destructors (which expect that lifecycle) and exit directly
			// with the failure count. The log file is unbuffered, so nothing
			// is lost.
			TerminateProcess( GetCurrentProcess(), GErrorLevel );
		}
		else
		{
			UEngine* Engine = InitEngine();
			if( !GIsRequestingExit )
				MainLoop( Engine );
			ExitEngine( Engine );
			GIsGuarded=0;
		}
#ifndef _DEBUG
	}
	catch( ... )
	{
		// Crashed.
		try {HandleError();} catch( ... ) {}
	}
#endif

	// Shut down.
	GExecHook=NULL;
	if( GLog )
		delete GLog;
	appExit();
	GIsStarted = 0;
	return GErrorLevel; // x64 port: nonzero = -selftest failures
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
