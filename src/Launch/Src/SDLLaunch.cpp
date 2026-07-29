/*=============================================================================
	SDLLaunch.cpp: Portable Unreal launcher (cross-platform port, Phase 4).

	Replaces the Win32 launcher (WinMain + the Window package's log window +
	PeekMessage pump) for the SDL build with a plain main() and a portable
	engine tick loop. There is no message pump here: SDLClient::Tick() pumps
	SDL events inside Engine->Tick, and a dedicated server has no window at
	all. The log window is replaced by mirroring the log to stdout via
	GLogHook (Unreal.log still receives everything). Keeps the -selftest /
	-make / -server command paths and the 200fps frame-rate cap from the
	Windows launcher.

	Author: Len Mudgett
=============================================================================*/

#include "LaunchPrivate.h"
#if !_WIN32
	#include <time.h>
	#include <unistd.h>
	#include <signal.h>	// SIGINT/SIGTERM graceful-shutdown handler
#endif
#include <stdio.h>
#if UNREAL_WITH_EDGUI
	#include "EdGui.h"	// Phase 5: ImGui editor frame for -editor mode
#endif

extern CORE_API FGlobalPlatform GTempPlatform;
extern "C" {char GPackage[64]="Launch";}
#if _WIN32
// Core's Win32 appBaseDir reads this; the DLLs each set their own copy in
// DllMain, the executable sets it here.
extern "C" {HINSTANCE hInstance;}
#endif

/*-----------------------------------------------------------------------------
	Log to stdout.
-----------------------------------------------------------------------------*/

// Without the Win32 log window, mirror the log to the terminal — that is
// where dedicated servers and Linux/macOS users read it. FGlobalPlatform
// already applies event suppression and writes Unreal.log before calling
// the hook.
class FStdoutLogHook : public FOutputDevice
{
	void WriteBinary( const void* Data, INT Length, EName Event )
	{
		// Flush per line: when stdout is a pipe it is fully buffered, and the
		// -selftest path exits via TerminateProcess/_exit with no stdio flush.
		printf( "%s: %s\n", *FName(Event), (char*)Data );
		fflush( stdout );
	}
};
static FStdoutLogHook GStdoutLog;

/*-----------------------------------------------------------------------------
	Graceful shutdown on Ctrl+C / SIGTERM / console close.
-----------------------------------------------------------------------------*/

// POSIX dedicated servers are stopped by SIGTERM (systemd, docker) or Ctrl+C
// (SIGINT). Without a handler the C runtime tears down out of lifecycle -
// static destructors run while the object subsystem is still live, tripping
// the ~UObject IsValid() assert. Turn the signal into a graceful-exit request
// so MainLoop unwinds through ExitEngine()/GObj.Exit() in the normal order.
//
// Windows intentionally keeps its prior behavior: the GUI build shuts down via
// the message pump, and a force-killed dedicated server is a low-priority ops
// edge case. (A SetConsoleCtrlHandler here swallowed the Ctrl+C that external
// timeout/kill tooling relies on, so it is deliberately not installed.)
#if !_WIN32
static void SignalHandler( int )
{
	GIsRequestingExit = 1;
}
#endif

static void InstallShutdownHandler()
{
#if !_WIN32
	signal( SIGINT,  SignalHandler );
	signal( SIGTERM, SignalHandler );
#endif
}

/*-----------------------------------------------------------------------------
	Portable sleep.
-----------------------------------------------------------------------------*/

static void PortableSleep( DOUBLE Seconds )
{
#if _WIN32
	Sleep( (DWORD)(Seconds*1000.0) );
#else
	struct timespec Ts;
	Ts.tv_sec  = (time_t)Seconds;
	Ts.tv_nsec = (long)((Seconds - (DOUBLE)Ts.tv_sec) * 1.e9);
	nanosleep( &Ts, NULL );
#endif
}

/*-----------------------------------------------------------------------------
	Error handling.
-----------------------------------------------------------------------------*/

static void HandleError()
{
	GIsGuarded=0;
	GIsCriticalError=1;
	debugf( NAME_Exit, "Shutting down after catching exception" );
	GObj.ShutdownAfterError();
	debugf( NAME_Exit, "Exiting due to exception" );
	GErrorHist[ARRAY_COUNT(GErrorHist)-1]=0;
	fprintf( stderr, "\nCritical Error: %s\n", GErrorHist );
#if _WIN32
	MessageBox( NULL, GErrorHist, LocalizeError("Critical"), MB_OK|MB_ICONERROR|MB_TASKMODAL );
#endif
}

/*-----------------------------------------------------------------------------
	Engine init/exit.
-----------------------------------------------------------------------------*/

// Portable subset of the Window package's InitEngine: no running-instance
// mutex, no registry file-type registration, no first-run hardware wizard
// (1998 CPU/memory detection is moot on anything that runs this port).
static UEngine* InitEngine()
{
	guard(InitEngine);
	appInit();
	GDynMem.Init( 65536 );
	GSceneMem.Init( 32768 );

	// Create the global engine object.
	UClass* EngineClass;
	if( !GIsEditor )
		EngineClass = GObj.LoadClass( UGameEngine::StaticClass, NULL, "ini:Engine.Engine.GameEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
	else if( ParseParam( appCmdLine(),"MAKE" ) )
		EngineClass = GObj.LoadClass( UEngine::StaticClass, NULL, "ini:Engine.Engine.EditorEngine", NULL, LOAD_NoFail | LOAD_DisallowFiles | LOAD_KeepImports, NULL );
	else
		// Interactive editor. LOAD_DisallowFiles (same as -make): the engine
		// object must come from the intrinsic C++-registered class — the
		// retail Editor.u serialized its 1998 32-bit defaults, which copied
		// over the x64 object clobber the C++ TArray members (crashed in
		// Tools.AddItem with a garbage ArrayMax). Editor.u itself still loads
		// normally afterwards via EditPackages[] in UEditorEngine::Init.
		EngineClass = GObj.LoadClass( UEngine::StaticClass, NULL, "ini:Engine.Engine.EditorEngine", NULL, LOAD_NoFail | LOAD_DisallowFiles | LOAD_KeepImports, NULL );

	UEngine* Engine = ConstructClassObject<UEngine>( EngineClass );
	Engine->Init();
	return Engine;
	unguard;
}

static void ExitEngine( UEngine* Engine )
{
	guard(ExitEngine);
	GObj.Exit();
	GMem.Exit();
	GDynMem.Exit();
	GSceneMem.Exit();
	GCache.Exit(1);
	appDumpAllocs( &GTempPlatform );
	unguard;
}

/*-----------------------------------------------------------------------------
	Main loop.
-----------------------------------------------------------------------------*/

static void MainLoop( UEngine* Engine )
{
	guard(MainLoop);
	GIsRunning = 1;
	DOUBLE OldTime = appSeconds();

	// -framestats logs per-frame timing (spikes >25ms immediately,
	// distribution every 5s) to diagnose stutter reports.
	UBOOL FrameStats = ParseParam( appCmdLine(), "FRAMESTATS" );

	// Frame-rate cap, default 200: the 1998 physics float math degenerates
	// below ~2ms deltas (see the WinDrv launcher and [Engine.Engine]
	// FrameRateLimit; 0 uncaps).
	FLOAT MinFrameTime = 1.f/200.f;
	{
		char Limit[32]="";
		if( GetConfigString( "Engine.Engine", "FrameRateLimit", Limit, ARRAY_COUNT(Limit) ) )
		{
			INT Fps = appAtoi( Limit );
			if( Fps > 0 )
				MinFrameTime = 1.f/Fps;
			else
				MinFrameTime = 0.f;
		}
	}
#if _WIN32
	timeBeginPeriod( 1 ); // 1ms Sleep granularity for the cap below
#endif
	DOUBLE StatWindowStart = OldTime;
	INT    StatFrames = 0, StatSpikes = 0;
	DOUBLE StatMax = 0.0, StatSum = 0.0, TickSum = 0.0, PrevTick = 0.0;
	while( GIsRunning && !GIsRequestingExit )
	{
		// Update the world. SDL event pumping happens inside the tick:
		// UGameEngine::Tick -> Client->Tick -> SDLClient PumpEvents.
		DOUBLE NewTime = appSeconds();
		DOUBLE FrameDelta = NewTime - OldTime;
		if( FrameStats )
		{
			StatFrames++; StatSum += FrameDelta; StatMax = Max( StatMax, FrameDelta ); TickSum += PrevTick;
			if( FrameDelta > 0.025 )
			{
				StatSpikes++;
				debugf( "FrameStats: spike frame=%.1fms tick=%.1fms", FrameDelta*1000.0, PrevTick*1000.0 );
			}
			if( NewTime - StatWindowStart > 5.0 )
			{
				debugf( "FrameStats: %i frames avg=%.1fms tickavg=%.1fms max=%.1fms spikes(>25ms)=%i", StatFrames, StatFrames ? StatSum*1000.0/StatFrames : 0.0, StatFrames ? TickSum*1000.0/StatFrames : 0.0, StatMax*1000.0, StatSpikes );
				StatWindowStart = NewTime; StatFrames = 0; StatSpikes = 0; StatMax = 0.0; StatSum = 0.0; TickSum = 0.0;
			}
		}
		Engine->Tick( NewTime - OldTime );
		OldTime = NewTime;
		if( FrameStats )
			PrevTick = appSeconds() - NewTime;

		// Enforce optional maximum tick rate (dedicated server pacing).
		INT MaxTickRate = Engine->GetMaxTickRate();
		if( MaxTickRate )
		{
			DOUBLE Delta = (1.0/MaxTickRate) - (appSeconds()-OldTime);
			if( Delta > 0.0 )
				PortableSleep( Delta );
		}

		// Frame-rate cap — sleep the bulk at 1ms granularity, spin the
		// sub-2ms tail for accuracy.
		if( MinFrameTime > 0.f )
		{
			while( appSeconds() - OldTime < MinFrameTime )
			{
				if( MinFrameTime - (appSeconds() - OldTime) > 0.002 )
					PortableSleep( 0.001 );
			}
		}
	}
#if _WIN32
	timeEndPeriod( 1 );
#endif
	GIsRunning = 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	Main.
-----------------------------------------------------------------------------*/

int main( int argc, char** argv )
{
	GIsStarted = 1;
#if _WIN32
	hInstance = GetModuleHandle( NULL );
	// DPI awareness is not set here: SDL3 makes the process per-monitor
	// DPI-aware itself during video init.
#else
	// Core has no GetCommandLine on POSIX; record argc/argv (also resolves
	// appBaseDir via argv[0] when /proc/self/exe is unavailable).
	appSetCmdLine( argc, argv );
#endif

	// Request a graceful exit on Ctrl+C / SIGTERM / console close (see above).
	InstallShutdownHandler();

	// Set package name.
	appStrcpy( GPackage, appPackage() );

	INT ErrorLevel = 0; // -selftest failure count -> exit code

	// Init mode.
	GIsServer = 1;
	GIsClient = !ParseParam(appCmdLine(),"SERVER") && !ParseParam(appCmdLine(),"MAKE");
	GIsEditor = ParseParam(appCmdLine(),"EDITOR") || ParseParam(appCmdLine(),"MAKE");

	appChdir( appBaseDir() );
	GLogHook = &GStdoutLog;

	// Begin.
#ifndef _DEBUG
	try
	{
#endif
		GIsGuarded=1;
		GSystem = &GTempPlatform;
		// -selftest runs subsystem checks in isolation (no window, renderer,
		// audio device, or gameplay) and exits with the failure count as the
		// process exit code. See SelfTest.cpp.
		if( ParseParam( appCmdLine(), "SELFTEST" ) )
		{
			extern INT RunSelfTests();
			appInit();
			GDynMem.Init( 65536 );
			ErrorLevel = RunSelfTests();
			GIsGuarded=0;
			// The test run loaded every retail package outside the normal
			// engine lifecycle; skip ALL teardown including static
			// destructors (which expect that lifecycle) and exit directly
			// with the failure count. The log file is unbuffered, so nothing
			// is lost.
#if _WIN32
			TerminateProcess( GetCurrentProcess(), ErrorLevel );
#else
			_exit( ErrorLevel );
#endif
		}
		else
		{
			UEngine* Engine = InitEngine();
#if UNREAL_WITH_EDGUI
			// Phase 5: interactive editor gets the ImGui shell (the -make
			// batch-compile path sets GIsRequestingExit during Init instead).
			if( GIsEditor && !GIsRequestingExit )
				EdGuiInstall( Engine );
#endif
			if( !GIsRequestingExit )
				MainLoop( Engine );
#if UNREAL_WITH_EDGUI
			EdGuiShutdown();	// before ExitEngine: needs the GL context alive
#endif
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
	GLogHook = NULL;
	appExit();
	GIsStarted = 0;
	return ErrorLevel;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
