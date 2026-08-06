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
	Semisolid collision audit (-semisolid).
-----------------------------------------------------------------------------*/

// PF_Semisolid means "does not cut the BSP, but still blocks". Walk every BSP
// node whose surface carries that flag, step a short way along -Normal (i.e.
// into the material the surface faces away from) and ask the world whether
// that point is solid. A correctly-collidable semisolid answers "solid"; one
// that renders but does not block answers "open" and is walk-through.
//
// PF_NotSolid surfaces are audited alongside as a control: those are SUPPOSED
// to read open, so if they behave differently from the semisolids the test
// itself is sound.
static void RunSemisolidAudit( ULevel* Level, const char* MapName )
{
	guard(RunSemisolidAudit);
	UModel* M = Level->Model;
	if( !M || !M->Nodes || !M->Surfs || !M->Verts || !M->Points )
	{
		debugf( NAME_Log, "SEMISOLID %-16s no model", MapName );
		return;
	}

	FLOAT Depth = 6.f;
	Parse( appCmdLine(), "SSDEPTH=", Depth );
	INT Limit = 8;
	Parse( appCmdLine(), "SSLIMIT=", Limit );

	INT SemiTotal=0, SemiOpen=0, NotTotal=0, NotOpen=0, SolidTotal=0, SolidOpen=0;
	INT Reported=0;
	for( INT i=0; i<M->Nodes->Num(); i++ )
	{
		const FBspNode& N = M->Nodes->Element(i);
		if( N.NumVertices < 3 || N.iSurf==INDEX_NONE )
			continue;
		const FBspSurf& S = M->Surfs->Element( N.iSurf );

		INT Kind;	// 0 = plain solid, 1 = semisolid, 2 = not-solid
		if     ( S.PolyFlags & PF_NotSolid  ) Kind = 2;
		else if( S.PolyFlags & PF_Semisolid ) Kind = 1;
		else                                  Kind = 0;

		// Centroid of the node polygon.
		FVector C(0,0,0);
		for( INT v=0; v<N.NumVertices; v++ )
			C += M->Points->Element( M->Verts->Element( N.iVertPool + v ).pVertex );
		C /= (FLOAT)N.NumVertices;

		// Step behind the face. FBspNode::Plane points out of the solid.
		// Sample two depths and require BOTH open before calling it
		// walk-through: a single deep sample punches out the far side of a
		// thin brush and reports every thin semisolid as broken.
		FVector Nrm( N.Plane.X, N.Plane.Y, N.Plane.Z );
		FCheckResult H1(1.f), H2(1.f);
		UBOOL Free
			=  Level->SinglePointCheck( H1, C - Nrm*(Depth*0.25f), FVector(0,0,0), 0, Level->GetLevelInfo(), 0 )
			&& Level->SinglePointCheck( H2, C - Nrm*Depth,         FVector(0,0,0), 0, Level->GetLevelInfo(), 0 );

		if( Kind==1 ) { SemiTotal++;  if(Free) SemiOpen++; }
		else if( Kind==2 ) { NotTotal++; if(Free) NotOpen++; }
		else { SolidTotal++; if(Free) SolidOpen++; }

		if( Kind==1 && Free && Reported<Limit )
		{
			debugf( NAME_Log, "SEMISOLID %-16s   walk-through at (%.0f,%.0f,%.0f) node %i surf %i",
				MapName, C.X, C.Y, C.Z, i, N.iSurf );
			Reported++;
		}
	}
	debugf
	(
		NAME_Log,
		"SEMISOLID %-16s semisolid %i/%i open (%.0f%%) | plain-solid %i/%i open (%.0f%%) | notsolid %i/%i open",
		MapName,
		SemiOpen, SemiTotal, SemiTotal ? 100.0*SemiOpen/SemiTotal : 0.0,
		SolidOpen, SolidTotal, SolidTotal ? 100.0*SolidOpen/SolidTotal : 0.0,
		NotOpen, NotTotal
	);
	unguard;
}

/*-----------------------------------------------------------------------------
	Collision probe (-mapprobe).
-----------------------------------------------------------------------------*/

// Ask the *live* level the same question the player's movement asks it:
// ULevel::SinglePointCheck with the pawn extent, walked along a line. This
// goes through FCollisionHash -> AActor::IsBlockedBy -> UPrimitive::PointCheck,
// so it sees exactly what a walking player would hit -- including movers that
// are solid in isolation but never registered in the collision hash.
//
//   -mapprobe=x0:y0:z0:x1:y1:z1  (world-space segment, 32 samples). Colons,
// not commas: Parse() truncates values at the first comma.
static void RunMapProbe( UEngine* Engine )
{
	guard(RunMapProbe);
	UGameEngine* Game = Cast<UGameEngine>( Engine );
	if( !Game || !Game->GLevel )
	{
		debugf( NAME_Log, "MAPPROBE: no level" );
		return;
	}
	ULevel* Level = Game->GLevel;

	char Spec[256]="";
	Parse( appCmdLine(), "MAPPROBE=", Spec, ARRAY_COUNT(Spec) );
	FVector A(0,0,0), B(0,0,0);
	if( sscanf( Spec, "%f:%f:%f:%f:%f:%f", &A.X,&A.Y,&A.Z, &B.X,&B.Y,&B.Z )!=6 )
	{
		debugf( NAME_Log, "MAPPROBE: need -mapprobe=x0:y0:z0:x1:y1:z1 (got '%s')", Spec );
		return;
	}

	FLOAT ER=17.f, EH=39.f;
	Parse( appCmdLine(), "PROBER=", ER );
	Parse( appCmdLine(), "PROBEH=", EH );
	FVector Extent( ER, ER, EH );

	debugf( NAME_Log, "MAPPROBE: (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f) extent %.0fx%.0f",
		A.X,A.Y,A.Z, B.X,B.Y,B.Z, ER, EH );

	// -probetrigger=<tag> fires that event first and ticks the engine for
	// -probesecs seconds, so the sweep sees the world after a mover has moved.
	char TrigTag[NAME_SIZE]="";
	if( Parse( appCmdLine(), "PROBETRIGGER=", TrigTag, ARRAY_COUNT(TrigTag) ) )
	{
		FName Want( TrigTag, FNAME_Find );
		INT Fired=0;
		for( INT i=0; i<Level->Num(); i++ )
		{
			AActor* Act = Level->Element(i);
			if( Act && Act->Tag==Want )
			{
				Act->eventTrigger( Act, NULL );
				Fired++;
			}
		}
		FLOAT Secs=4.f;
		Parse( appCmdLine(), "PROBESECS=", Secs );
		debugf( NAME_Log, "MAPPROBE: triggered '%s' on %i actor(s), ticking %.1fs", TrigTag, Fired, Secs );
		for( FLOAT T=0.f; T<Secs; T+=0.05f )
		{
			Engine->Tick( 0.05f );
			for( INT i=0; i<Level->Num(); i++ )
			{
				AMover* M = Cast<AMover>( Level->Element(i) );
				if( M && M->Tag==Want )
					debugf( NAME_Log, "MAPPROBE   t=%4.2f %s key=%i loc=(%.0f,%.0f,%.0f)",
						T+0.05f, M->GetName(), (INT)M->KeyNum, M->Location.X, M->Location.Y, M->Location.Z );
			}
		}
	}

	// -probemodel counts level-BSP points inside the probe segment's endpoints
	// boxes, split into static points and the moving-brush points the
	// FMovingBrushTracker injects past Points->Num(). Movers are drawn ONLY
	// through that tracker (UnSprite.cpp: IsMovingBrush -> BrushTracker->
	// Update), so this is where a mover would render at a stale position.
	if( ParseParam( appCmdLine(), "PROBEMODEL" ) )
	{
		UVectors* Pts = Level->Model->Points;
		FLOAT Rad = 96.f;
		Parse( appCmdLine(), "PROBEBOX=", Rad );
		INT SA=0, DA=0, SB=0, DB=0;
		for( INT i=0; i<Pts->GetMax(); i++ )
		{
			const FVector& V = Pts->Element(i);
			UBOOL Dyn = ( i >= Pts->GetNum() );
			if( Abs(V.X-A.X)<Rad && Abs(V.Y-A.Y)<Rad && Abs(V.Z-A.Z)<Rad ) { if(Dyn) DA++; else SA++; }
			if( Abs(V.X-B.X)<Rad && Abs(V.Y-B.Y)<Rad && Abs(V.Z-B.Z)<Rad ) { if(Dyn) DB++; else SB++; }
		}
		debugf( NAME_Log, "MAPPROBE model: points num=%i max=%i", Pts->GetNum(), Pts->GetMax() );
		debugf( NAME_Log, "MAPPROBE model: near A (%.0f,%.0f,%.0f) static=%i moving=%i", A.X,A.Y,A.Z, SA, DA );
		debugf( NAME_Log, "MAPPROBE model: near B (%.0f,%.0f,%.0f) static=%i moving=%i", B.X,B.Y,B.Z, SB, DB );
	}

	// -probegrid prints an ASCII occupancy map of the box A..B, one plate per
	// Z slice: '.' = a pawn-sized box fits, '#' = world BSP blocks, a digit =
	// blocked by a mover. Reveals leaks and pockets that line sweeps miss.
	if( ParseParam( appCmdLine(), "PROBEGRID" ) )
	{
		FLOAT Step = 16.f;
		Parse( appCmdLine(), "PROBESTEP=", Step );
		Step = Clamp( Step, 4.f, 256.f );
		for( FLOAT Z=A.Z; Z<=B.Z+0.01f; Z+=Step )
		{
			debugf( NAME_Log, "MAPGRID Z=%.0f   (rows = Y %.0f..%.0f, cols = X %.0f..%.0f, step %.0f)",
				Z, A.Y, B.Y, A.X, B.X, Step );
			for( FLOAT Y=A.Y; Y<=B.Y+0.01f; Y+=Step )
			{
				char Row[160]; INT n=0;
				for( FLOAT X=A.X; X<=B.X+0.01f && n<ARRAY_COUNT(Row)-1; X+=Step )
				{
					FMemMark Mark(GMem);
					char C='.';
					for( FCheckResult* H = Level->MultiPointCheck( GMem, FVector(X,Y,Z), Extent, 0, Level->GetLevelInfo(), 1 ); H; H=H->GetNext() )
					{
						UBOOL IsWorld = ( H->Actor==NULL || H->Actor==(AActor*)Level->GetLevelInfo() );
						if( IsWorld )                       { C='#'; break; }
						else if( H->Actor->bBlockPlayers )  { C='M'; }
					}
					Mark.Pop();
					Row[n++]=C;
				}
				Row[n]=0;
				debugf( NAME_Log, "MAPGRID Y=%6.0f |%s|", Y, Row );
			}
		}
	}

	INT Steps = 32;
	Parse( appCmdLine(), "PROBESTEPS=", Steps );
	Steps = Clamp( Steps, 1, 256 );
	for( INT i=0; i<=Steps; i++ )
	{
		FVector P = A + (B-A) * ((FLOAT)i/Steps);

		// MultiPointCheck, not SinglePointCheck: the latter stops at the first
		// overlap, which is usually a non-blocking Trigger cylinder and would
		// hide whatever is behind it. Report only hits that would actually
		// stop a player (world BSP, or an actor with bBlockPlayers).
		FMemMark Mark(GMem);
		char Blockers[256]=""; INT NumBlock=0, NumTouch=0;
		for( FCheckResult* H = Level->MultiPointCheck( GMem, P, Extent, 0, Level->GetLevelInfo(), 1 ); H; H=H->GetNext() )
		{
			UBOOL IsWorld = ( H->Actor==NULL || H->Actor==(AActor*)Level->GetLevelInfo() );
			if( IsWorld || H->Actor->bBlockPlayers )
			{
				NumBlock++;
				if( appStrlen(Blockers) < 200 )
				{
					if( Blockers[0] ) appStrcat( Blockers, ", " );
					appStrcat( Blockers, IsWorld ? "world BSP" : H->Actor->GetName() );
				}
			}
			else NumTouch++;
		}
		Mark.Pop();

		debugf( NAME_Log, "MAPPROBE %3i (%7.0f,%8.0f,%7.0f)  %-7s blockers=%i [%s] touches=%i",
			i, P.X, P.Y, P.Z, NumBlock ? "SOLID" : "*OPEN*", NumBlock, Blockers, NumTouch );
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Actor probe (-probeactors).
-----------------------------------------------------------------------------*/

// -probeactors[=<substr>] dumps every placed actor: class, name, location and
// the render/occlusion facts that matter when reproducing a reported spot
// (corona flag + skin on lights, mesh + collision on decorations). Optional
// substring filters by class name. Non-interactive: log, then exit.
static void RunActorProbe( UEngine* Engine )
{
	guard(RunActorProbe);
	UGameEngine* Game = Cast<UGameEngine>( Engine );
	if( !Game || !Game->GLevel )
	{
		debugf( NAME_Log, "ACTORPROBE: no level" );
		return;
	}
	ULevel* Level = Game->GLevel;
	char Filter[64]="";
	Parse( appCmdLine(), "PROBEACTORS=", Filter, ARRAY_COUNT(Filter) );
	INT Logged=0;
	for( INT i=0; i<Level->Num(); i++ )
	{
		AActor* Act = Level->Element(i);
		if( !Act )
			continue;
		if( Filter[0] && !appStrfind( const_cast<char*>(Act->GetClass()->GetName()), Filter ) )
			continue;
		debugf( NAME_Log, "ACTORPROBE %-22s %-26s (%7.0f,%8.0f,%7.0f) corona=%i skin=%s mesh=%s collide=%i",
			Act->GetClass()->GetName(), Act->GetName(),
			Act->Location.X, Act->Location.Y, Act->Location.Z,
			(INT)Act->bCorona,
			Act->Skin ? Act->Skin->GetName() : "None",
			Act->Mesh ? Act->Mesh->GetName() : "None",
			(INT)Act->bCollideActors );
		Logged++;
	}
	debugf( NAME_Log, "ACTORPROBE: %i actor(s) logged (filter '%s')", Logged, Filter );
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
	// -probeview=x:y:z:pitch:yaw pins the view actor at a world position for
	// -probeframes frames, screenshots it (viewport "SHOT" exec) and quits.
	// Non-interactive repro of a reported spot: no walking required.
	FVector	ViewLoc(0,0,0);
	FRotator ViewRot(0,0,0);
	UBOOL	PinView = 0;
	INT		ProbeFrames = 60, FrameNum = 0;
	{
		char Spec[256]="";
		if( Parse( appCmdLine(), "PROBEVIEW=", Spec, ARRAY_COUNT(Spec) ) )
		{
			FLOAT X,Y,Z; INT Pitch,Yaw;
			if( sscanf( Spec, "%f:%f:%f:%i:%i", &X,&Y,&Z, &Pitch, &Yaw )==5 )
			{
				ViewLoc = FVector(X,Y,Z);
				ViewRot = FRotator(Pitch,Yaw,0);
				PinView = 1;
				Parse( appCmdLine(), "PROBEFRAMES=", ProbeFrames );
				debugf( NAME_Log, "PROBEVIEW: pinning view at (%.0f,%.0f,%.0f) pitch=%i yaw=%i for %i frames",
					X,Y,Z, Pitch, Yaw, ProbeFrames );
			}
			else debugf( NAME_Log, "PROBEVIEW: need -probeview=x:y:z:pitch:yaw (got '%s')", Spec );
		}
	}

	// -probewalk=x:y:z:yaw teleports the player there (FarMoveActor keeps the
	// collision hash coherent) and then WALKS it in the yaw direction with
	// collision fully live -- Velocity/Acceleration are re-primed before every
	// tick, so movement runs through the real player path (PlayerMove ->
	// physWalking -> MultiLineCheck). Position is logged as it goes; where it
	// stops is where the game actually blocks. Screenshot + exit at the end.
	FVector	WalkStart(0,0,0);
	INT		WalkYaw = 0;
	UBOOL	ProbeWalk = 0, WalkPlaced = 0;
	{
		char Spec[256]="";
		if( Parse( appCmdLine(), "PROBEWALK=", Spec, ARRAY_COUNT(Spec) ) )
		{
			FLOAT X,Y,Z;
			if( sscanf( Spec, "%f:%f:%f:%i", &X,&Y,&Z, &WalkYaw )==4 )
			{
				WalkStart = FVector(X,Y,Z);
				ProbeWalk = 1;
				Parse( appCmdLine(), "PROBEFRAMES=", ProbeFrames );
				debugf( NAME_Log, "PROBEWALK: start (%.0f,%.0f,%.0f) yaw=%i for %i frames",
					X,Y,Z, WalkYaw, ProbeFrames );
			}
			else debugf( NAME_Log, "PROBEWALK: need -probewalk=x:y:z:yaw (got '%s')", Spec );
		}
	}

	// -probelight=x:y:z spawns a corona-flagged test light there (steady,
	// bright, GenFX.LensFlar skin). Deterministic repro for flare-occlusion
	// bugs: place it behind a decoration/masked sheet, pin the view with
	// -probeview, and the shot shows whether the corona is (in)correctly
	// occluded -- no dependence on where a mapper happened to put one.
	FVector	ProbeLightLoc(0,0,0);
	UBOOL	ProbeLight = 0, ProbeLightPlaced = 0;
	{
		char Spec[256]="";
		if( Parse( appCmdLine(), "PROBELIGHT=", Spec, ARRAY_COUNT(Spec) ) )
		{
			FLOAT X,Y,Z;
			if( sscanf( Spec, "%f:%f:%f", &X,&Y,&Z )==3 )
			{
				ProbeLightLoc = FVector(X,Y,Z);
				ProbeLight = 1;
				debugf( NAME_Log, "PROBELIGHT: corona light at (%.0f,%.0f,%.0f)", X,Y,Z );
			}
			else debugf( NAME_Log, "PROBELIGHT: need -probelight=x:y:z (got '%s')", Spec );
		}
	}

	// -probemesh=x:y:z:MeshName spawns a visible, non-colliding mesh actor
	// there (an Effects actor wearing the mesh). Repro tool for "renders but
	// doesn't collide" occlusion bugs: decorations and masked sheets never
	// block collision traces, so this is how to put foliage in front of a
	// corona light deterministically.
	FVector	ProbeMeshLoc(0,0,0);
	char	ProbeMeshName[64]="";
	UBOOL	ProbeMesh = 0, ProbeMeshPlaced = 0;
	{
		char Spec[256]="";
		if( Parse( appCmdLine(), "PROBEMESH=", Spec, ARRAY_COUNT(Spec) ) )
		{
			FLOAT X,Y,Z;
			if( sscanf( Spec, "%f:%f:%f:%63s", &X,&Y,&Z, ProbeMeshName )==4 )
			{
				ProbeMeshLoc = FVector(X,Y,Z);
				ProbeMesh = 1;
				debugf( NAME_Log, "PROBEMESH: %s at (%.0f,%.0f,%.0f)", ProbeMeshName, X,Y,Z );
			}
			else debugf( NAME_Log, "PROBEMESH: need -probemesh=x:y:z:MeshName (got '%s')", Spec );
		}
	}

	DOUBLE StatWindowStart = OldTime;
	INT    StatFrames = 0, StatSpikes = 0;
	DOUBLE StatMax = 0.0, StatSum = 0.0, TickSum = 0.0, PrevTick = 0.0;
	while( GIsRunning && !GIsRequestingExit )
	{
		if( ProbeLight && !ProbeLightPlaced && Engine->Client )
		{
			UGameEngine* G = Cast<UGameEngine>( Engine );
			if( G && G->GLevel )
			{
				ProbeLightPlaced = 1;
				UClass* EC = FindObject<UClass>( ANY_PACKAGE, "Effects" );
				UTexture* Flare = NULL;
				for( TObjectIterator<UTexture> It; It; ++It )
					if( appStrfind( const_cast<char*>(It->GetPathName()), "LensFlar" ) )
						{ Flare = *It; break; }
				AActor* L = EC ? G->GLevel->SpawnActor( EC, NAME_None, NULL, NULL, ProbeLightLoc, FRotator(0,0,0), NULL, 0, 1 ) : NULL;
				if( L )
				{
					L->bHidden         = 1;
					L->bDynamicLight   = 1;
					L->bCorona         = 1;
					L->LightType       = LT_Steady;
					L->LightBrightness = 255;
					L->LightHue        = 0;
					L->LightSaturation = 255;
					L->LightRadius     = 64;
					L->LifeSpan        = 0.f;	// Effects subclasses default short-lived; keep it alive
					L->DrawScale       = 0.25f;
					L->Skin            = Flare;
					debugf( NAME_Log, "PROBELIGHT: spawned %s at (%.0f,%.0f,%.0f) skin=%s",
						L->GetName(), L->Location.X, L->Location.Y, L->Location.Z,
						Flare ? Flare->GetName() : "None" );
				}
				else debugf( NAME_Log, "PROBELIGHT: spawn FAILED (class %s)", EC ? EC->GetName() : "'Effects' not found" );
			}
		}
		if( ProbeMesh && !ProbeMeshPlaced && Engine->Client )
		{
			UGameEngine* G = Cast<UGameEngine>( Engine );
			if( G && G->GLevel )
			{
				ProbeMeshPlaced = 1;
				UClass* EC = FindObject<UClass>( ANY_PACKAGE, "Effects" );
				UMesh* M = FindObject<UMesh>( ANY_PACKAGE, ProbeMeshName );
				AActor* A = (EC && M) ? G->GLevel->SpawnActor( EC, NAME_None, NULL, NULL, ProbeMeshLoc, FRotator(0,0,0), NULL, 0, 1 ) : NULL;
				if( A )
				{
					A->DrawType  = DT_Mesh;
					A->Mesh      = M;
					A->LifeSpan  = 0.f;
					A->DrawScale = 1.f;
					debugf( NAME_Log, "PROBEMESH: spawned %s mesh=%s at (%.0f,%.0f,%.0f)",
						A->GetName(), M->GetName(), A->Location.X, A->Location.Y, A->Location.Z );
				}
				else debugf( NAME_Log, "PROBEMESH: spawn FAILED (class=%s mesh=%s)",
					EC ? "ok" : "'Effects' missing", M ? "ok" : ProbeMeshName );
			}
		}
		if( ProbeWalk && Engine->Client )
		{
			UGameEngine* G = Cast<UGameEngine>( Engine );
			for( INT v=0; G && G->GLevel && v<Engine->Client->Viewports.Num(); v++ )
			{
				APlayerPawn* P = Engine->Client->Viewports(v)->Actor;
				if( !P )
					continue;
				if( !WalkPlaced )
				{
					WalkPlaced = 1;
					G->GLevel->FarMoveActor( P, WalkStart, 0, 1 );
					debugf( NAME_Log, "PROBEWALK: placed at (%.0f,%.0f,%.0f)", P->Location.X, P->Location.Y, P->Location.Z );
				}
				FLOAT A = WalkYaw * (2.f*PI/65536.f);
				FVector Dir( appCos(A), appSin(A), 0.f );
				P->Physics      = PHYS_Walking;
				P->Rotation.Yaw = P->ViewRotation.Yaw = WalkYaw;
				P->Velocity     = Dir * P->GroundSpeed + FVector(0,0,P->Velocity.Z);
				P->Acceleration = Dir * P->AccelRate;
				if( ( FrameNum % 25 )==0 || FrameNum==ProbeFrames-1 )
					debugf( NAME_Log, "PROBEWALK: frame %3i at (%.0f,%.0f,%.0f)", FrameNum, P->Location.X, P->Location.Y, P->Location.Z );
			}
			if( ++FrameNum >= ProbeFrames )
			{
				for( INT v=0; v<Engine->Client->Viewports.Num(); v++ )
					Engine->Client->Viewports(v)->Exec( "SHOT", GSystem );
				debugf( NAME_Log, "PROBEWALK: done, exiting" );
				GIsRequestingExit = 1;
			}
		}

		char ProbeExec[128]="";
		UBOOL HasExec = Parse( appCmdLine(), "PROBEEXEC=", ProbeExec, ARRAY_COUNT(ProbeExec) );
		if( !ProbeWalk && (PinView || HasExec) && Engine->Client )
		{
			// Freeze the view actor there: physics off and no world collision,
			// otherwise gravity//pushout move it before the shot. Skipped when
			// only -probeexec is given, so the actor stays wherever the map or
			// save game put it (that is the point when inspecting a save).
			UGameEngine* PinG = Cast<UGameEngine>( Engine );
			if( PinView )
			for( INT v=0; v<Engine->Client->Viewports.Num(); v++ )
			{
				APlayerPawn* P = Engine->Client->Viewports(v)->Actor;
				if( !P )
					continue;
				P->Physics       = PHYS_None;
				P->bCollideWorld = 0;
				P->SetCollision( 0, 0, 0 );
				// FarMoveActor, not a bare Location write: it updates the
				// actor's Region (zone/leaf), which leaf-driven rendering --
				// the corona light list above all -- reads. With a stale
				// Region the shot silently loses every corona/flare.
				if( PinG && PinG->GLevel )
					PinG->GLevel->FarMoveActor( P, ViewLoc, 0, 1 );
				else
					P->Location  = ViewLoc;
				P->Rotation      = ViewRot;
				P->ViewRotation  = ViewRot;
				P->Velocity      = FVector(0,0,0);
				P->BaseEyeHeight = 0.f;
				P->EyeHeight     = 0.f;
			}
			if( ++FrameNum == ProbeFrames )
			{
				if( HasExec )
					Engine->Exec( ProbeExec, GSystem );
				for( INT v=0; v<Engine->Client->Viewports.Num(); v++ )
					Engine->Client->Viewports(v)->Exec( "SHOT", GSystem );
				debugf( NAME_Log, "PROBEVIEW: screenshot taken, exiting" );
				GIsRequestingExit = 1;
			}
		}

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
			if( !GIsRequestingExit && ParseParam( appCmdLine(), "SEMISOLID" ) )
			{
				UGameEngine* G = Cast<UGameEngine>( Engine );
				if( G && G->GLevel )
				{
					char Name[128]="";
					Parse( appCmdLine(), "SSNAME=", Name, ARRAY_COUNT(Name) );
					RunSemisolidAudit( G->GLevel, Name[0] ? Name : "level" );
				}
				GIsRequestingExit = 1;
			}
			if( !GIsRequestingExit && ParseParam( appCmdLine(), "MAPPROBE" ) )
			{
				RunMapProbe( Engine );
				GIsRequestingExit = 1;
			}
			if( !GIsRequestingExit && ParseParam( appCmdLine(), "PROBEACTORS" ) )
			{
				RunActorProbe( Engine );
				GIsRequestingExit = 1;
			}
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
