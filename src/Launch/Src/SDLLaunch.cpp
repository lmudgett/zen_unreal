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
extern ENGINE_API UBOOL GTouchTrace;	// -touchtrace: touch/untouch handoff log
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
		// Zone facts too: an actor whose zone has a SkyZone is OUTDOORS, which is
		// how to find an open-sky camera spot without guessing yaws (fog matters
		// for effect rendering as well, so report it alongside).
		AZoneInfo* Zone = Act->Region.Zone;
		debugf( NAME_Log, "ACTORPROBE %-22s %-26s (%7.0f,%8.0f,%7.0f) rot=%i:%i corona=%i skin=%s mesh=%s collide=%i zone=%s sky=%i fog=%i fogcol=%i,%i,%i fogdist=%.0f",
			Act->GetClass()->GetName(), Act->GetName(),
			Act->Location.X, Act->Location.Y, Act->Location.Z,
			(INT)Act->Rotation.Pitch, (INT)Act->Rotation.Yaw,
			(INT)Act->bCorona,
			Act->Skin ? Act->Skin->GetName() : "None",
			Act->Mesh ? Act->Mesh->GetName() : "None",
			(INT)Act->bCollideActors,
			Zone ? Zone->GetName() : "None",
			(INT)( Zone && Zone->SkyZone ),
			(INT)( Zone && Zone->bFogZone ),
			Zone ? (INT)Zone->FogColor.R : 0, Zone ? (INT)Zone->FogColor.G : 0, Zone ? (INT)Zone->FogColor.B : 0,
			Zone ? Zone->FogDistance : 0.f );
		// Mover facts: a mover is lit from its OWN brush's baked light data
		// (Brush->Lights/LightBits, indexed per poly), so a mover whose brush
		// carries none draws black in any zone with zero ambient.
		if( AMover* M = Cast<AMover>(Act) )
			debugf( NAME_Log, "ACTORPROBE     ^ mover brush=%s polys=%i lightmap=%i lights=%i lightbits=%i dynlight=%i leaf=%i leaves=%i keys=%i",
				M->Brush ? M->Brush->GetName() : "None",
				M->Brush && M->Brush->Polys ? M->Brush->Polys->Num() : -1,
				M->Brush ? M->Brush->LightMap.Num() : -1,
				M->Brush ? M->Brush->Lights.Num() : -1,
				M->Brush ? M->Brush->LightBits.Num() : -1,
				(INT)M->bDynamicLightMover, (INT)M->Region.iLeaf,
				Level->Model ? Level->Model->Leaves.Num() : -1, (INT)M->NumKeys );
		// Light facts: which lights SHOULD reach a surface is the first
		// question when one draws black.
		if( Act->LightType!=LT_None )
			debugf( NAME_Log, "ACTORPROBE     ^ light type=%i effect=%i bright=%i hue=%i sat=%i radius=%i period=%i special=%i dynamic=%i hidden=%i",
				(INT)Act->LightType, (INT)Act->LightEffect, (INT)Act->LightBrightness, (INT)Act->LightHue,
				(INT)Act->LightSaturation, (INT)Act->LightRadius, (INT)Act->LightPeriod,
				(INT)Act->bSpecialLit, (INT)Act->bDynamicLight, (INT)Act->bHidden );
		// Second line for anything that draws as a sprite: DrawType/Style/Texture
		// are what decide the blend mode, and a sprite rendering as a flat opaque
		// rectangle is exactly a Style that resolved to "no blend".
		if( Act->DrawType==DT_Sprite || Act->DrawType==DT_SpriteAnimOnce )
			debugf( NAME_Log, "ACTORPROBE     ^ sprite draw=%i style=%i scale=%.2f hidden=%i tex=%s",
				(INT)Act->DrawType, (INT)Act->Style, Act->DrawScale, (INT)Act->bHidden,
				Act->Texture ? Act->Texture->GetPathName() : "None" );
		Logged++;
	}
	debugf( NAME_Log, "ACTORPROBE: %i actor(s) logged (filter '%s')", Logged, Filter );
	unguard;
}

/*-----------------------------------------------------------------------------
	Door probe (-probedoor).
-----------------------------------------------------------------------------*/

// Dumps the editable properties a class adds on top of Actor. Triggers,
// Dispatchers and SpecialEvents are script-only classes with no C++ struct, so
// reflection is the only way to read what they were authored with.
static void DumpAddedProps( AActor* Act, const char* Indent )
{
	guard(DumpAddedProps);
	char Line[1024];
	Line[0] = 0;
	for( TFieldIterator<UProperty> It( Act->GetClass() ); It; ++It )
	{
		UProperty* Prop = *It;
		if( !(Prop->PropertyFlags & CPF_Edit) || Prop->ArrayDim!=1 )
			continue;
		// Skip everything Actor itself declares: the door-specific facts are
		// what the subclass added, and Actor's ~200 properties bury them.
		UClass* Owner = Prop->GetOwnerClass();
		if( !Owner || AActor::StaticClass->IsChildOf(Owner) )
			continue;
		char Value[256]="";
		Prop->ExportText( 0, Value, (BYTE*)Act, (BYTE*)Act, 1 );
		if( appStrlen(Line)+appStrlen(Value)+appStrlen(Prop->GetName())+4 > ARRAY_COUNT(Line) )
			break;
		appSprintf( Line+appStrlen(Line), "%s=%s ", Prop->GetName(), Value );
	}
	if( Line[0] )
		debugf( NAME_Log, "DOORPROBE %s%s", Indent, Line );
	unguard;
}

// True if this actor can fire event E. Actor.Event is the usual channel, but
// it is not the only one: a Dispatcher fires from OutEvents[8], a mover from
// PlayerBumpEvent/BumpEvent. Scanning every name property except the ones that
// name the actor itself catches all of them, which matters because "nothing
// triggers this door" is only a real finding if the search was exhaustive.
static UBOOL FiresEvent( AActor* Act, FName E )
{
	guard(FiresEvent);
	if( E==NAME_None )
		return 0;
	for( TFieldIterator<UProperty> It( Act->GetClass() ); It; ++It )
	{
		UNameProperty* NP = Cast<UNameProperty>( *It );
		if( !NP )
			continue;
		const char* N = NP->GetName();
		// These name the actor or its script state, not something it fires.
		if( !appStricmp(N,"Tag") || !appStricmp(N,"Group") || !appStricmp(N,"InitialState")
		||  !appStricmp(N,"State") || !appStricmp(N,"AttachTag") || !appStricmp(N,"ReturnGroup") )
			continue;
		for( INT k=0; k<NP->ArrayDim; k++ )
			if( *(FName*)((BYTE*)Act + NP->Offset + k*sizeof(FName)) == E )
				return 1;
	}
	return 0;
	unguard;
}

// One line of "who is this actor and what state is it sitting in".
static void DumpActorIdentity( AActor* Act, const char* Indent, const char* Role, FVector From )
{
	// A proximity trigger only fires on Touch, so its reach -- and whether the
	// player is already standing inside it -- is the fact that decides whether
	// a door can still be made to open from where the save left the player.
	FLOAT Dist2D = FVector( Act->Location.X-From.X, Act->Location.Y-From.Y, 0 ).Size();
	UBOOL Inside = Act->bCollideActors
		&& Dist2D <= Act->CollisionRadius
		&& Abs(Act->Location.Z-From.Z) <= Act->CollisionHeight;
	char Touch[128]="";
	for( INT t=0; t<ARRAY_COUNT(Act->Touching); t++ )
		if( Act->Touching[t] )
			appSprintf( Touch+appStrlen(Touch), "%s ", Act->Touching[t]->GetName() );
	debugf( NAME_Log, "DOORPROBE %s%s %s '%s' tag=%s event=%s state=%s(init=%s) dist=%.0f loc=(%.0f,%.0f,%.0f) collide=%i hidden=%i radius=%.0f height=%.0f playerInside=%i touching=[%s]",
		Indent, Role, Act->GetClass()->GetName(), Act->GetName(),
		*Act->Tag, *Act->Event,
		(Act->GetMainFrame() && Act->GetMainFrame()->StateNode) ? Act->GetMainFrame()->StateNode->GetName() : "None",
		*Act->InitialState,
		(Act->Location-From).Size(),
		Act->Location.X, Act->Location.Y, Act->Location.Z,
		(INT)Act->bCollideActors, (INT)Act->bHidden,
		Act->CollisionRadius, Act->CollisionHeight, (INT)Inside, Touch );
}

// -probedoor[=<substr>] explains why a door is not opening. A mover opens only
// when something Trigger()s its Tag, so the answer is never in the mover alone:
// it is in the chain of actors whose Event matches that Tag, and in whether
// those actors are still alive and still in a state that can fire. This walks
// that chain two levels deep for every mover near the player (or every mover
// whose name/tag matches the filter) and prints what each link is holding.
// Non-interactive: log, then exit.
static void RunDoorProbe( UEngine* Engine )
{
	guard(RunDoorProbe);
	UGameEngine* Game = Cast<UGameEngine>( Engine );
	if( !Game || !Game->GLevel )
	{
		debugf( NAME_Log, "DOORPROBE: no level" );
		return;
	}
	ULevel* Level = Game->GLevel;
	char Filter[64]="";
	Parse( appCmdLine(), "PROBEDOOR=", Filter, ARRAY_COUNT(Filter) );
	FLOAT Radius = 1200.f;
	Parse( appCmdLine(), "DOORRADIUS=", Radius );

	// The player's position is the whole point of the default filter: a
	// savegame is a report of "I am stuck HERE", and the door in question is
	// the one being stared at.
	FVector From(0,0,0);
	AActor* Player = NULL;
	for( INT i=0; i<Level->Num(); i++ )
	{
		APawn* P = Cast<APawn>( Level->Element(i) );
		if( P && P->IsA(APlayerPawn::StaticClass) )
		{
			Player = P;
			From   = P->Location;
			break;
		}
	}
	debugf( NAME_Log, "DOORPROBE player=%s at (%.0f,%.0f,%.0f) filter='%s' radius=%.0f",
		Player ? Player->GetName() : "NONE", From.X, From.Y, From.Z, Filter, Radius );

	// Which door is "the" door: trace out of the saved eye along the saved view,
	// then fan the whole yaw circle. Distance to a mover's pivot is a poor proxy
	// -- the pivot can sit far from the brush -- so let the collision code say
	// what is actually in front of the player.
	if( Player )
	{
		APawn* PP = Cast<APawn>( Player );
		FVector Eye = From + FVector( 0, 0, PP ? PP->EyeHeight : 40.f );
		FRotator View = PP && PP->IsA(APlayerPawn::StaticClass)
			? ((APlayerPawn*)PP)->ViewRotation : Player->Rotation;
		for( INT Step=-1; Step<24; Step++ )
		{
			FRotator R = View;
			if( Step>=0 )
				R = FRotator( 0, Step*(65536/24), 0 );
			FCheckResult Hit(1.f);
			FVector Dir = R.Vector();
			if( !Level->SingleLineCheck( Hit, Player, Eye+Dir*4000.f, Eye, TRACE_VisBlocking ) )
			{
				AMover* HitMover = Cast<AMover>( Hit.Actor );
				if( Step<0 || HitMover )
					debugf( NAME_Log, "DOORPROBE look yaw=%i(%.0fdeg) -> %s '%s' at %.0f units%s",
						(INT)R.Yaw, R.Yaw*360.f/65536.f,
						Hit.Actor ? Hit.Actor->GetClass()->GetName() : "BSP",
						Hit.Actor ? Hit.Actor->GetName() : "world",
						(Hit.Location-Eye).Size(),
						Step<0 ? "  <== SAVED VIEW DIRECTION" : "" );
			}
			else if( Step<0 )
				debugf( NAME_Log, "DOORPROBE look yaw=%i -> nothing within 4000 units  <== SAVED VIEW DIRECTION", (INT)R.Yaw );
		}
	}

	INT Doors = 0;
	for( INT i=0; i<Level->Num(); i++ )
	{
		AMover* M = Cast<AMover>( Level->Element(i) );
		if( !M )
			continue;
		if( Filter[0] )
		{
			if( !appStrfind( const_cast<char*>(M->GetName()), Filter )
			&&  !appStrfind( const_cast<char*>(*M->Tag), Filter ) )
				continue;
		}
		else if( !Player || (M->Location-From).Size() > Radius )
			continue;
		Doors++;

		DumpActorIdentity( M, "", "MOVER", From );
		debugf( NAME_Log, "DOORPROBE   keys=%i/%i prev=%i triggerOnce=%i slave=%i useTriggered=%i damageTriggered=%i bump=%i encroach=%i numTriggerEvents=%i stayOpen=%.1f moveTime=%.1f",
			(INT)M->KeyNum, (INT)M->NumKeys, (INT)M->PrevKeyNum,
			(INT)M->bTriggerOnceOnly, (INT)M->bSlave, (INT)M->bUseTriggered, (INT)M->bDamageTriggered,
			(INT)M->BumpType, (INT)M->MoverEncroachType, M->numTriggerEvents, M->StayOpenTime, M->MoveTime );
		debugf( NAME_Log, "DOORPROBE   playerBumpEvent=%s bumpEvent=%s returnGroup=%s savedTrigger=%s triggerActor=%s leader=%s follower=%s",
			*M->PlayerBumpEvent, *M->BumpEvent, *M->ReturnGroup,
			M->SavedTrigger  ? M->SavedTrigger->GetName()  : "None",
			M->TriggerActor  ? M->TriggerActor->GetName()  : "None",
			M->Leader        ? M->Leader->GetName()        : "None",
			M->Follower      ? M->Follower->GetName()      : "None" );

		// Level one: everything that fires this mover's Tag.
		INT Sources = 0;
		for( INT j=0; j<Level->Num(); j++ )
		{
			AActor* T = Level->Element(j);
			if( !T || T==M || !FiresEvent( T, M->Tag ) )
				continue;
			Sources++;
			DumpActorIdentity( T, "  ", "<-fired-by", From );
			DumpAddedProps( T, "    " );
			// Level two: what fires THAT. A dead or dormant link here is the
			// usual reason a door stays shut with nothing obviously wrong.
			for( INT k=0; k<Level->Num(); k++ )
			{
				AActor* T2 = Level->Element(k);
				if( !T2 || T2==T || T->Tag==NAME_None || !FiresEvent( T2, T->Tag ) )
					continue;
				DumpActorIdentity( T2, "    ", "<-fired-by", From );
				DumpAddedProps( T2, "      " );
			}
		}
		if( !Sources )
			debugf( NAME_Log, "DOORPROBE   <-fired-by NOTHING (no actor in the level has Event=%s)", *M->Tag );
	}
	debugf( NAME_Log, "DOORPROBE: %i mover(s) reported", Doors );
	unguard;
}

/*-----------------------------------------------------------------------------
	Mesh animation probe (-probemeshanim).
-----------------------------------------------------------------------------*/

// -probemeshanim[=<substr>] dumps the animation sequences a mesh actually
// carries: which frames each sequence covers, its rate, and its notifies.
// Sequences are declared by #exec MESH SEQUENCE in script and BAKED INTO THE
// PACKAGE, so a package rebuilt from source can disagree with what the script
// says -- and a sequence pointing at the wrong frames is an animation that
// plays the wrong thing while every line of code involved looks correct.
// Comparing this against the #exec lines is the only way to tell.
static void RunMeshAnimProbe()
{
	guard(RunMeshAnimProbe);
	char Filter[64]="";
	Parse( appCmdLine(), "PROBEMESHANIM=", Filter, ARRAY_COUNT(Filter) );
	// Meshes load on demand with the classes that reference them, so a map that
	// happens not to contain the weapon has none of its meshes in memory. Load
	// the package outright (-probemeshpkg, default UnrealI) so the dump covers
	// what the package actually shipped rather than what this level touched.
	char Pkg[64]="UnrealI";
	Parse( appCmdLine(), "PROBEMESHPKG=", Pkg, ARRAY_COUNT(Pkg) );
	if( !GObj.LoadPackage( NULL, Pkg, LOAD_NoWarn ) )
		debugf( NAME_Log, "MESHANIM: could not load package '%s'", Pkg );
	INT Logged=0;
	for( TObjectIterator<UMesh> It; It; ++It )
	{
		UMesh* M = *It;
		if( Filter[0] && !appStrfind( const_cast<char*>(M->GetName()), Filter ) )
			continue;
		debugf( NAME_Log, "MESHANIM %s: %i sequence(s), %i frames total",
			M->GetPathName(), M->AnimSeqs.Num(), M->FrameVerts ? M->AnimFrames : 0 );
		for( INT i=0; i<M->AnimSeqs.Num(); i++ )
		{
			FMeshAnimSeq& S = M->AnimSeqs(i);
			debugf( NAME_Log, "MESHANIM    %-12s start=%-4i frames=%-4i rate=%-6.1f group=%-10s notifies=%i",
				*S.Name, S.StartFrame, S.NumFrames, S.Rate, *S.Group, S.Notifys.Num() );
			for( INT n=0; n<S.Notifys.Num(); n++ )
				debugf( NAME_Log, "MESHANIM       notify time=%.3f -> %s",
					S.Notifys(n).Time, *S.Notifys(n).Function );
		}
		Logged++;
	}
	debugf( NAME_Log, "MESHANIM: %i mesh(es) logged (filter '%s')", Logged, Filter );
	unguard;
}

/*-----------------------------------------------------------------------------
	Creature AI audit (-probeai).
-----------------------------------------------------------------------------*/

// -probeai watches every non-player pawn in a running level -- Nali, Skaarj,
// Krall, Titans, the lot -- and reports what its AI actually did, so "are the
// creatures working?" is answered by evidence rather than by walking the map
// and forming an impression. Sampled while the game ticks (see MainLoop) and
// summarized at exit.
//
// What it records per creature, and why each matters:
//   state    -- the UnrealScript state it is executing. A creature that never
//               leaves its initial state is asleep; one cycling states is
//               running its script.
//   enemy    -- did it ever acquire one? This is perception (sight/hearing)
//               working, and it is the difference between hostile AI and
//               scenery.
//   moved    -- total distance travelled. Movement means pathing and physics
//               are carrying it; a creature that wants an enemy but never
//               moves is stuck.
//   health   -- fell = it took damage (combat is resolving both ways).
//   NaN      -- a non-finite location. The 1998 physics is float-fragile and
//               a NaN here is the pathing crash signature, so it is called
//               out loudly rather than averaged away.
struct FAIWatch
{
	// Identity is COPIED at first sight, never read back off the pointer:
	// creatures die (and their memory is reused), so a report that
	// dereferences a stored actor at the end of the run crashes on exactly
	// the maps where the AI was busiest.
	AActor*		Actor;			// identity only -- compared, never dereferenced after Alive=0
	char		ClassName[48];
	char		Name[48];
	FLOAT		SightRadius, PeripheralVision;
	INT			Vis;
	UBOOL		Alive;
	FName		FirstState, LastState;
	INT			StateChanges;
	FVector		FirstLoc, LastLoc;
	FLOAT		Moved;
	FLOAT		StartHealth, MinHealth;
	UBOOL		EverEnemy, EverBadLoc;
	// Where the perception chain stands, sampled the same way the engine
	// walks it (UnPawn.cpp ShowSelf): the player shows itself to every pawn,
	// each pawn is asked whether its current state listens for SeePlayer, and
	// if so whether it has line of sight. Recording all three separates "the
	// creature cannot see the player" from "the creature is not listening"
	// from "the creature saw and ignored it".
	FLOAT		MinDist;
	UBOOL		EverProbing, EverLOS;
	// Did it ever run an alarm? A friendly Nali that spots the player goes to
	// TriggerAlarm, walks to its AlarmPoint and fires that point's Event --
	// which is how the Nali "show you a secret" behaviour is built (see
	// ScriptedPawn's TriggerAlarm state). It is transient: the Nali returns to
	// Roaming afterwards, so the final state says nothing about whether it
	// happened. Recorded as it passes.
	UBOOL		EverAlarm;
	char		AlarmTag[32];	// mapper-assigned; "" = this one can never show a secret
	INT			Attitude;		// AttitudeToPlayer: <4 hostile, 4 ignore, >4 friendly
};
static TArray<FAIWatch> GAIWatch;
// Level time spanned by the audit. Frames are NOT seconds here -- the renderer
// runs at hundreds of frames a second, so a few hundred frames is a blink, far
// too short for a state machine built on Sleep() to show anything. Reported so
// a run that saw no AI activity can be told apart from one that was over
// before the AI had a chance.
static FLOAT GAIStartTime = -1.f, GAIEndTime = -1.f;
// The subject's own health. Creatures acquiring an enemy proves perception;
// the subject losing health proves the rest -- that they closed the distance,
// chose an attack and connected.
static INT GAISubjStart = -1, GAISubjEnd = -1;

// AlarmTag and AttitudeToPlayer live in UnrealI's ScriptedPawn script, not in
// any C++ class, so they are read through the property table the way the editor
// reads any script property. AlarmTag is the whole question for the "Nali shows
// you a secret" behaviour: only a creature the mapper gave one can lead anybody
// anywhere, so a run that faces a Nali without one proves nothing.
static FName GetNameProp( AActor* A, const char* PropName )
{
	guardSlow(GetNameProp);
	UNameProperty* P = FindField<UNameProperty>( A->GetClass(), PropName );
	return P ? *(FName*)((BYTE*)A + P->Offset) : FName(NAME_None);
	unguardSlow;
}
static INT GetByteProp( AActor* A, const char* PropName )
{
	guardSlow(GetByteProp);
	UByteProperty* P = FindField<UByteProperty>( A->GetClass(), PropName );
	return P ? (INT)*((BYTE*)A + P->Offset) : -1;
	unguardSlow;
}

// The two states a creature passes through while running an alarm. Compared by
// TEXT, not by constructing FNames: a name looked up with FNAME_Find that is
// not in the table comes back as NAME_None, which would then match every
// creature sitting in no state at all.
static UBOOL IsAlarmState( FName State )
{
	if( State==NAME_None )
		return 0;
	return appStricmp( *State, "TriggerAlarm" )==0
		|| appStricmp( *State, "AlarmPaused"  )==0;
}

// Movers that actually moved. An AlarmPoint's Event is nearly always a door,
// so a Nali reaching its alarm should be followed by a Mover going somewhere:
// that is the secret opening, and it is the difference between the Nali
// playing out the walk and the walk having any effect.
struct FMoverWatch { AActor* Mover; FVector Start; UBOOL Moved; };
static TArray<FMoverWatch> GMovers;

static UBOOL IsFiniteVec( const FVector& V )
{
	// Non-finite compares false against itself and against any bound.
	return (V.X==V.X) && (V.Y==V.Y) && (V.Z==V.Z)
		&& V.X>-1e9f && V.X<1e9f && V.Y>-1e9f && V.Y<1e9f && V.Z>-1e9f && V.Z<1e9f;
}

static void SampleAI( UEngine* Engine )
{
	guard(SampleAI);
	UGameEngine* Game = Cast<UGameEngine>( Engine );
	if( !Game || !Game->GLevel )
		return;
	ULevel* Level = Game->GLevel;
	if( Level->GetLevelInfo() )
	{
		GAIEndTime = Level->GetLevelInfo()->TimeSeconds;
		if( GAIStartTime < 0.f )
			GAIStartTime = GAIEndTime;
	}
	// The player as the AI sees it. Line-of-sight tests trace, so the
	// perception sampling is throttled rather than run every frame.
	static INT Tick = 0;
	UBOOL DeepSample = ((Tick++) % 20)==0;
	// Anything not seen in this pass has been destroyed; its stored facts stay
	// in the report but its pointer is never touched again.
	if( DeepSample )
		for( INT w=0; w<GAIWatch.Num(); w++ )
			GAIWatch(w).Alive = 0;
	APawn* Player = NULL;
	for( INT i=0; i<Level->Num(); i++ )
	{
		APawn* P = Cast<APawn>( Level->Element(i) );
		if( P && P->bIsPlayer )
			{ Player = P; break; }
	}
	if( Player )
	{
		GAISubjEnd = Player->Health;
		if( GAISubjStart < 0 )
			GAISubjStart = GAISubjEnd;
	}
	if( Player && GAIStartTime==GAIEndTime )
		debugf( NAME_Log, "AIPROBE player: %s '%s' at (%.0f,%.0f,%.0f) health=%i hidden=%i collide=%i zone=%s VISIBILITY=%i sightradius=%.0f",
			Player->GetClass()->GetName(), Player->GetName(),
			Player->Location.X, Player->Location.Y, Player->Location.Z,
			Player->Health, (INT)Player->bHidden, (INT)Player->bCollideActors,
			Player->Region.Zone ? Player->Region.Zone->GetName() : "None",
			(INT)Player->Visibility, Player->SightRadius );
	// Mover positions: recorded on the first pass, compared on later ones.
	if( DeepSample )
		for( INT i=0; i<Level->Num(); i++ )
		{
			AActor* A = Level->Element(i);
			if( !A || !A->IsA(AMover::StaticClass) )
				continue;
			INT m;
			for( m=0; m<GMovers.Num(); m++ )
				if( GMovers(m).Mover==A )
					break;
			if( m==GMovers.Num() )
			{
				FMoverWatch New;
				New.Mover = A;
				New.Start = A->Location;
				New.Moved = 0;
				GMovers.AddItem( New );
			}
			else if( (A->Location-GMovers(m).Start).SizeSquared() > 16.f )
				GMovers(m).Moved = 1;
		}
	for( INT i=0; i<Level->Num(); i++ )
	{
		APawn* P = Cast<APawn>( Level->Element(i) );
		if( !P || P->bIsPlayer )
			continue;
		FName State = P->GetMainFrame() && P->GetMainFrame()->StateNode
			? P->GetMainFrame()->StateNode->GetFName() : NAME_None;
		INT w;
		for( w=0; w<GAIWatch.Num(); w++ )
			if( GAIWatch(w).Actor==(AActor*)P )
				break;
		if( w==GAIWatch.Num() )
		{
			FAIWatch New;
			New.Actor       = P;
			appStrncpy( New.ClassName, P->GetClass()->GetName(), ARRAY_COUNT(New.ClassName) );
			appStrncpy( New.Name,      P->GetName(),             ARRAY_COUNT(New.Name)      );
			New.SightRadius      = P->SightRadius;
			New.PeripheralVision = P->PeripheralVision;
			New.Vis              = P->Visibility;
			New.Alive            = 1;
			New.FirstState  = New.LastState = State;
			New.StateChanges= 0;
			New.FirstLoc    = New.LastLoc = P->Location;
			New.Moved       = 0.f;
			New.StartHealth = New.MinHealth = P->Health;
			New.EverEnemy   = P->Enemy!=NULL;
			New.EverBadLoc  = !IsFiniteVec( P->Location );
			New.MinDist     = Player ? (P->Location-Player->Location).Size() : -1.f;
			New.EverProbing = 0;
			New.EverLOS     = 0;
			New.EverAlarm   = IsAlarmState( State );
			appStrncpy( New.AlarmTag, *GetNameProp( P, "AlarmTag" ), ARRAY_COUNT(New.AlarmTag) );
			if( appStricmp( New.AlarmTag, "None" )==0 )
				New.AlarmTag[0] = 0;
			New.Attitude    = GetByteProp( P, "AttitudeToPlayer" );
			GAIWatch.AddItem( New );
			continue;
		}
		FAIWatch& W = GAIWatch(w);
		W.Alive = 1;
		if( IsAlarmState( State ) )
			W.EverAlarm = 1;
		if( State!=W.LastState )
		{
			W.StateChanges++;
			W.LastState = State;
		}
		if( !IsFiniteVec( P->Location ) )
			W.EverBadLoc = 1;
		else
		{
			W.Moved += (P->Location - W.LastLoc).Size();
			W.LastLoc = P->Location;
		}
		if( P->Enemy )
			W.EverEnemy = 1;
		W.MinHealth = Min( W.MinHealth, (FLOAT)P->Health );
		if( Player && DeepSample )
		{
			FLOAT D = (P->Location-Player->Location).Size();
			W.MinDist = W.MinDist<0.f ? D : Min( W.MinDist, D );
			if( P->IsProbing( NAME_SeePlayer ) )
			{
				W.EverProbing = 1;
				if( P->LineOfSightTo( Player, 1 ) )
					W.EverLOS = 1;
			}
		}
	}
	unguard;
}

static void ReportAI( UEngine* Engine )
{
	guard(ReportAI);
	// The nearest creature's actual sight trace, spelled out. Every creature
	// traces to the same subject, so a subject standing inside rock fails all
	// of them identically and looks exactly like blind AI. Reporting what the
	// trace hit separates "the level is in the way" (a harness problem) from
	// "the perception code is broken" (an engine problem).
	{
		UGameEngine* Game = Cast<UGameEngine>( Engine );
		if( Game && Game->GLevel )
		{
			APawn* Player = NULL;
			for( INT i=0; i<Game->GLevel->Num(); i++ )
			{
				APawn* P = Cast<APawn>( Game->GLevel->Element(i) );
				if( P && P->bIsPlayer )
					{ Player = P; break; }
			}
			FAIWatch* Near = NULL;
			for( INT w=0; w<GAIWatch.Num(); w++ )
				if( GAIWatch(w).Alive && GAIWatch(w).MinDist>=0.f
				&& (!Near || GAIWatch(w).MinDist<Near->MinDist) )
					Near = &GAIWatch(w);
			if( Player && Near )
			{
				APawn* C = (APawn*)Near->Actor;
				FVector Eye = C->Location; Eye.Z += C->BaseEyeHeight;
				FVector Body = Player->Location; Body.Z += Player->CollisionHeight*0.8f;
				FCheckResult Hit(1.f);
				Game->GLevel->SingleLineCheck( Hit, C, Body, Eye, TRACE_VisBlocking );
				debugf( NAME_Log, "AIPROBE trace: %s at (%.0f,%.0f,%.0f) -> subject at (%.0f,%.0f,%.0f): time=%.3f blocked_by=%s",
					Near->ClassName, C->Location.X, C->Location.Y, C->Location.Z,
					Player->Location.X, Player->Location.Y, Player->Location.Z,
					Hit.Time, Hit.Time>=1.f ? "nothing (CLEAR)"
						: (Hit.Actor ? Hit.Actor->GetName() : "world geometry") );
				// And whether the subject is standing in solid space at all.
				FCheckResult Self(1.f);
				Game->GLevel->SingleLineCheck( Self, Player, Player->Location + FVector(0,0,4), Player->Location, TRACE_VisBlocking );
				debugf( NAME_Log, "AIPROBE subject: zone=%i in_solid=%i",
					(INT)Player->Region.ZoneNumber, (INT)(Self.Time<1.f) );
			}
		}
	}
	INT Moved=0, Woke=0, Hostile=0, Hurt=0, Bad=0, Alarms=0;
	for( INT w=0; w<GAIWatch.Num(); w++ )
	{
		FAIWatch& W = GAIWatch(w);
		if( W.Moved > 16.f )       Moved++;
		if( W.StateChanges > 0 )   Woke++;
		if( W.EverEnemy )          Hostile++;
		if( W.MinHealth < W.StartHealth ) Hurt++;
		if( W.EverBadLoc )         Bad++;
		if( W.EverAlarm )          Alarms++;
		debugf( NAME_Log, "AIPROBE %-20s %-16s state=%-16s changes=%-3i moved=%7.0f enemy=%i dist=%6.0f listens=%i sees=%i health=%.0f/%.0f sight=%.0f periph=%.2f vis=%i attitude=%i alarmtag=%-10s%s%s%s",
			W.ClassName, W.Name,
			*W.LastState, W.StateChanges, W.Moved, (INT)W.EverEnemy,
			W.MinDist, (INT)W.EverProbing, (INT)W.EverLOS,
			W.MinHealth, W.StartHealth,
			W.SightRadius, W.PeripheralVision, W.Vis,
			W.Attitude, W.AlarmTag[0] ? W.AlarmTag : "-",
			W.EverAlarm ? " ALARM" : "",
			W.Alive ? "" : " DIED",
			W.EverBadLoc ? "  *** NON-FINITE LOCATION ***" : "" );
	}
	INT MoversMoved = 0;
	for( INT m=0; m<GMovers.Num(); m++ )
		if( GMovers(m).Moved )
			MoversMoved++;
	// How many creatures in this level are even capable of showing a secret.
	INT WithAlarm = 0;
	for( INT w=0; w<GAIWatch.Num(); w++ )
		if( GAIWatch(w).AlarmTag[0] )
			WithAlarm++;
	debugf( NAME_Log, "AIPROBE: %i creature(s) carry an AlarmTag (can lead the player to a secret)", WithAlarm );
	debugf( NAME_Log, "AIPROBE: %i creature(s) over %.1fs: %i changed state, %i acquired an enemy, %i moved, %i took damage, %i ran an alarm, %i with bad locations; movers activated %i/%i; subject health %i -> %i",
		GAIWatch.Num(), GAIEndTime-GAIStartTime, Woke, Hostile, Moved, Hurt, Alarms, Bad,
		MoversMoved, GMovers.Num(), GAISubjStart, GAISubjEnd );
	unguard;
}

/*-----------------------------------------------------------------------------
	Texture probe (-probetex).
-----------------------------------------------------------------------------*/

// -probetex=<substr> dumps the facts that decide whether a sprite blends
// invisibly: its PolyFlags (bMasked?), and the palette entry its BORDER texels
// use. A translucent sprite is only invisible where its texels are BLACK, so a
// non-black border palette entry renders the whole quad as a visible rectangle
// -- the "square haze" signature. Corner + edge-midpoint texels are sampled
// because that is exactly the region that should vanish.
static void RunTexProbe()
{
	guard(RunTexProbe);
	char Filter[64]="";
	Parse( appCmdLine(), "PROBETEX=", Filter, ARRAY_COUNT(Filter) );
	INT Logged=0;
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		UTexture* T = *It;
		if( Filter[0] && !appStrfind( const_cast<char*>(T->GetPathName()), Filter ) )
			continue;
		if( !T->GetNumMips() )
			continue;
		FMipmap* M = T->GetMip(0);
		INT U = M->USize, V = M->VSize;
		if( U<=0 || V<=0 || M->DataArray.Num() < U*V )
		{
			debugf( NAME_Log, "TEXPROBE %-40s %ix%i NO DATA (%i bytes)", T->GetPathName(), U, V, M->DataArray.Num() );
			continue;
		}
		FColor* Pal = T->GetColors();
		BYTE* D = &M->DataArray(0);
		// Border sample points: 4 corners + 4 edge midpoints.
		INT SX[8] = { 0, U-1, 0,   U-1, U/2, U/2, 0,   U-1 };
		INT SY[8] = { 0, 0,   V-1, V-1, 0,   V-1, V/2, V/2 };
		INT MaxLum=0, WorstIdx=0;
		for( INT i=0; i<8; i++ )
		{
			BYTE Idx = D[ SY[i]*U + SX[i] ];
			FColor C = Pal ? Pal[Idx] : FColor(0,0,0,0);
			INT Lum = (INT)C.R + C.G + C.B;
			if( Lum > MaxLum ) { MaxLum = Lum; WorstIdx = Idx; }
		}
		FColor P0 = Pal ? Pal[0] : FColor(0,0,0,0);
		FColor PW = Pal ? Pal[WorstIdx] : FColor(0,0,0,0);
		debugf( NAME_Log, "TEXPROBE %-40s %3ix%-3i mips=%i flags=%08X masked=%i pal0=(%3i,%3i,%3i) brightestBorder=idx%-3i (%3i,%3i,%3i)",
			T->GetPathName(), U, V, T->GetNumMips(), T->PolyFlags,
			(INT)((T->PolyFlags & PF_Masked)!=0),
			P0.R, P0.G, P0.B, WorstIdx, PW.R, PW.G, PW.B );
		// -probetexdump additionally writes EVERY mip through the palette as
		// Texprobe_<name>_m<i>.bmp, so the actual texel fields can be inspected
		// -- border samples alone can't show whether a level's background is
		// clean black or a noise/averaging floor that will lift the whole quad
		// under additive blend (minified sprites sample the SMALL mips, where
		// box-filtering may have smeared the image over the entire tile).
		if( Pal && ParseParam( appCmdLine(), "PROBETEXDUMP" ) )
		{
			for( INT iMip=0; iMip<T->GetNumMips(); iMip++ )
			{
				FMipmap* MM = T->GetMip(iMip);
				INT MU = MM->USize, MV = MM->VSize;
				if( MU<=0 || MV<=0 || MM->DataArray.Num() < MU*MV )
					continue;
				BYTE* MD = &MM->DataArray(0);
				char BmpName[128];
				appSprintf( BmpName, "Texprobe_%s_m%i.bmp", T->GetName(), iMip );
				INT RowBytes = (MU*3+3)&~3, DataBytes = RowBytes*MV, FileBytes = 54+DataBytes;
				BYTE* Bmp = (BYTE*)appMalloc( FileBytes, "TexProbeBmp" );
				appMemset( Bmp, 0, FileBytes );
				Bmp[0]='B'; Bmp[1]='M';
				*(INT*)(Bmp+ 2) = FileBytes;
				*(INT*)(Bmp+10) = 54;
				*(INT*)(Bmp+14) = 40;
				*(INT*)(Bmp+18) = MU;
				*(INT*)(Bmp+22) = MV;
				*(WORD*)(Bmp+26) = 1;
				*(WORD*)(Bmp+28) = 24;
				for( INT y=0; y<MV; y++ )
					for( INT x=0; x<MU; x++ )
					{
						FColor C = Pal[ MD[y*MU+x] ];
						BYTE* P = Bmp + 54 + (MV-1-y)*RowBytes + x*3;
						P[0]=C.B; P[1]=C.G; P[2]=C.R;
					}
				FILE* F = appFopen( BmpName, "wb" );
				if( F )
				{
					appFwrite( Bmp, 1, FileBytes, F );
					appFclose( F );
				}
				appFree( Bmp );
			}
		}
		Logged++;
	}
	debugf( NAME_Log, "TEXPROBE: %i texture(s) logged (filter '%s')", Logged, Filter );
	unguard;
}

/*-----------------------------------------------------------------------------
	Surface probe (-probesurfs).
-----------------------------------------------------------------------------*/

// -probesurfs tallies the level's BSP surfaces by (texture, polyflags): which
// texture is actually ON a reported surface, its class (procedural fractal vs
// plain bitmap), and the surface flags that drive liquid looks (translucent,
// auto-pan, wavy). Non-interactive: log, then exit.
static void RunSurfProbe( UEngine* Engine )
{
	guard(RunSurfProbe);
	UGameEngine* Game = Cast<UGameEngine>( Engine );
	if( !Game || !Game->GLevel || !Game->GLevel->Model )
	{
		debugf( NAME_Log, "SURFPROBE: no level" );
		return;
	}
	UModel* Model = Game->GLevel->Model;
	enum {MAX_TALLY=512};
	UTexture* TalTex  [MAX_TALLY];
	DWORD     TalFlags[MAX_TALLY];
	INT       TalCount[MAX_TALLY];
	INT NumTally=0;
	for( INT i=0; i<Model->Surfs->Num(); i++ )
	{
		FBspSurf& Surf = Model->Surfs->Element(i);
		INT t;
		for( t=0; t<NumTally; t++ )
			if( TalTex[t]==Surf.Texture && TalFlags[t]==Surf.PolyFlags )
				break;
		if( t==NumTally )
		{
			if( NumTally==MAX_TALLY )
				continue;
			TalTex[t] = Surf.Texture; TalFlags[t] = Surf.PolyFlags; TalCount[t] = 0;
			NumTally++;
		}
		TalCount[t]++;
	}
	for( INT t=0; t<NumTally; t++ )
	{
		UTexture* T = TalTex[t];
		// Representative world point: centroid of the first BSP node drawn
		// with this (texture,flags) pair -- enough to aim a -probeview shot.
		FVector Centroid(0,0,0);
		UBOOL Found=0;
		for( INT n=0; n<Model->Nodes->Num() && !Found; n++ )
		{
			FBspNode& Node = Model->Nodes->Element(n);
			if( Node.NumVertices<3 || Node.iSurf>=Model->Surfs->Num() )
				continue;
			FBspSurf& S = Model->Surfs->Element(Node.iSurf);
			if( S.Texture!=TalTex[t] || S.PolyFlags!=TalFlags[t] )
				continue;
			for( INT v=0; v<Node.NumVertices; v++ )
				Centroid += Model->Points->Element( Model->Verts->Element(Node.iVertPool+v).pVertex );
			Centroid /= Node.NumVertices;
			Found=1;
		}
		debugf( NAME_Log, "SURFPROBE %-40s class=%-14s surfs=%-4i flags=%08X at(%7.0f,%8.0f,%7.0f)%s%s%s%s%s",
			T ? T->GetPathName() : "None",
			T ? T->GetClass()->GetName() : "None",
			TalCount[t], TalFlags[t],
			Centroid.X, Centroid.Y, Centroid.Z,
			(TalFlags[t] & PF_Translucent) ? " TRANSLUCENT" : "",
			(TalFlags[t] & PF_AutoUPan   ) ? " UPAN" : "",
			(TalFlags[t] & PF_AutoVPan   ) ? " VPAN" : "",
			(TalFlags[t] & PF_SmallWavy  ) ? " SMALLWAVY" : "",
			(TalFlags[t] & PF_BigWavy    ) ? " BIGWAVY" : "" );
	}
	debugf( NAME_Log, "SURFPROBE: %i unique (texture,flags) pairs over %i surfs", NumTally, Model->Surfs->Num() );

	// -probesurfsat=<texture substring> additionally dumps each matching
	// surface on its own: the whole unclipped world box (as Render's
	// GetSurfBounds computes it) plus its extents. The tally above says WHAT a
	// liquid is; this says what SHAPE the mapper built it as -- a single tall
	// sheet, or the four walls of a box -- which is what decides how the driver
	// should treat it.
	char At[64]="";
	if( Parse( appCmdLine(), "PROBESURFSAT=", At, ARRAY_COUNT(At) ) && At[0] )
	{
		TArray<FBox> Bounds;
		Bounds.Add( Model->Surfs->Num() );
		for( INT i=0; i<Bounds.Num(); i++ )
			Bounds(i) = FBox(0);
		// Texture-space extent too, in texels: how many TILES of its texture a
		// surface spans. A texture drawn once reads as the picture the artist
		// painted; the same texture wrapped several times puts its bottom row
		// against its top row at every seam, and if the art is not tileable that
		// seam is a hard luminance step -- which on a translucent sheet reads as
		// a straight bright edge cutting across it.
		TArray<FLOAT> UMin, UMax, VMin, VMax;
		UMin.Add( Model->Surfs->Num() ); UMax.Add( Model->Surfs->Num() );
		VMin.Add( Model->Surfs->Num() ); VMax.Add( Model->Surfs->Num() );
		for( INT i=0; i<Model->Surfs->Num(); i++ )
		{
			UMin(i) = VMin(i) =  1e30f;
			UMax(i) = VMax(i) = -1e30f;
		}
		for( INT n=0; n<Model->Nodes->Num(); n++ )
		{
			FBspNode& Node = Model->Nodes->Element(n);
			if( Node.NumVertices<3 || Node.iSurf<0 || Node.iSurf>=Bounds.Num() )
				continue;
			FBox& B = Bounds(Node.iSurf);
			FBspSurf& S = Model->Surfs->Element( Node.iSurf );
			FVector Base = Model->Points->Element( S.pBase );
			FVector TexU = Model->Vectors->Element( S.vTextureU );
			FVector TexV = Model->Vectors->Element( S.vTextureV );
			for( INT v=0; v<Node.NumVertices; v++ )
			{
				FVector P = Model->Points->Element( Model->Verts->Element(Node.iVertPool+v).pVertex );
				B += P;
				FLOAT U = TexU | (P-Base), V = TexV | (P-Base);
				UMin(Node.iSurf) = Min( UMin(Node.iSurf), U );
				UMax(Node.iSurf) = Max( UMax(Node.iSurf), U );
				VMin(Node.iSurf) = Min( VMin(Node.iSurf), V );
				VMax(Node.iSurf) = Max( VMax(Node.iSurf), V );
			}
		}
		INT Hits=0;
		for( INT i=0; i<Model->Surfs->Num(); i++ )
		{
			FBspSurf& Surf = Model->Surfs->Element(i);
			if( !Surf.Texture || !Bounds(i).IsValid )
				continue;
			// "*" lists every surface, so a spot can be interrogated by POSITION
			// rather than by guessing which texture is the one misbehaving --
			// filter the boxes afterwards. Hunting a decal by name first means
			// knowing the answer before you start.
			if( appStrcmp( At, "*" )!=0 && !appStrfind( const_cast<char*>(Surf.Texture->GetName()), At ) )
				continue;
			FVector Mn = Bounds(i).Min, Mx = Bounds(i).Max;
			FLOAT TilesU = Surf.Texture->USize>0 && UMax(i)>UMin(i) ? (UMax(i)-UMin(i))/Surf.Texture->USize : 0.f;
			FLOAT TilesV = Surf.Texture->VSize>0 && VMax(i)>VMin(i) ? (VMax(i)-VMin(i))/Surf.Texture->VSize : 0.f;
			debugf( NAME_Log, "SURFPROBE AT %-3i %-24s flags=%08X box=(%7.0f,%8.0f,%7.0f)..(%7.0f,%8.0f,%7.0f) HX=%4.0f HY=%4.0f HZ=%4.0f tex=%ix%i tiles=%.2fx%.2f",
				i, Surf.Texture->GetName(), Surf.PolyFlags,
				Mn.X, Mn.Y, Mn.Z, Mx.X, Mx.Y, Mx.Z,
				Mx.X-Mn.X, Mx.Y-Mn.Y, Mx.Z-Mn.Z,
				Surf.Texture->USize, Surf.Texture->VSize, TilesU, TilesV );
			Hits++;
		}
		debugf( NAME_Log, "SURFPROBE: %i surface(s) matching '%s'", Hits, At );
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Fountain audit (-probefountains).
-----------------------------------------------------------------------------*/

// -probefountains answers, for a whole map and without rendering a single
// frame, what OpenGLDrv's fountain path will make of every sheet in it: which
// become pouring columns, which flat splash caps get absorbed into one, and
// which candidates are left to draw as the authored pane. That last group is
// what a stray hard-edged rectangle at a fountain always turns out to be, so
// this is the sweep that says whether a fix holds across the game rather than
// in the one map it was found in.
//
// The classification MIRRORS DrawComplexSurface (see the StreamLook block
// there): a candidate is lit + translucent + not wavy + not auto-panning, a
// column is one narrow enough in both horizontal axes with height to pour, and
// a flat face joins a column it sits at. Two differences from the renderer,
// both deliberate: the whole map is examined rather than what a camera can
// see, and columns are registered before caps are matched, which is the steady
// state the renderer reaches on its second frame.
static void RunFountainProbe( UEngine* Engine )
{
	guard(RunFountainProbe);
	UGameEngine* Game = Cast<UGameEngine>( Engine );
	if( !Game || !Game->GLevel || !Game->GLevel->Model )
	{
		debugf( NAME_Log, "FOUNTAINS: no level" );
		return;
	}
	UModel* Model = Game->GLevel->Model;

	// World bounds per surface, whole and unclipped (mirrors Render's
	// GetSurfBounds, which fills FSurfaceFacet::Bounds).
	TArray<FBox> Bounds;
	Bounds.Add( Model->Surfs->Num() );
	for( INT i=0; i<Bounds.Num(); i++ )
		Bounds(i) = FBox(0);
	for( INT n=0; n<Model->Nodes->Num(); n++ )
	{
		FBspNode& Node = Model->Nodes->Element(n);
		if( Node.NumVertices<3 || Node.iSurf<0 || Node.iSurf>=Bounds.Num() )
			continue;
		FBox& B = Bounds(Node.iSurf);
		for( INT v=0; v<Node.NumVertices; v++ )
			B += Model->Points->Element( Model->Verts->Element(Node.iVertPool+v).pVertex );
	}

	// Pass 1: register columns from the faces that can pour.
	enum {MAX_COLS=64};
	FBox Cols[MAX_COLS];
	INT  NumCols=0, NumCaps=0, NumRejects=0, NumCandidates=0;
	for( INT Pass=0; Pass<2; Pass++ )
	{
		for( INT i=0; i<Model->Surfs->Num(); i++ )
		{
			FBspSurf& Surf = Model->Surfs->Element(i);
			if( !Bounds(i).IsValid || !Surf.Texture )
				continue;
			// Candidate test, as DrawComplexSurface sees it.
			UBOOL Translucent = (Surf.PolyFlags & PF_Translucent)!=0 && !(Surf.PolyFlags & PF_Modulated);
			UBOOL Lit         = !(Surf.PolyFlags & PF_Unlit);
			UBOOL Wavy        = (Surf.PolyFlags & (PF_SmallWavy|PF_BigWavy))!=0;
			// Render tags WaterTexture-class surfaces wavy before the driver
			// ever sees them, so they are pools, never pours.
			for( UClass* C=Surf.Texture->GetClass(); C && !Wavy; C=C->GetSuperClass() )
				if( appStricmp( C->GetName(), "WaterTexture" )==0 )
					Wavy = 1;
			UBOOL Flowing = Translucent && !Wavy && (Surf.PolyFlags & (PF_AutoUPan|PF_AutoVPan))!=0;
			if( !Translucent || !Lit || Wavy || Flowing )
				continue;

			FVector WMin = Bounds(i).Min, WMax = Bounds(i).Max;
			FLOAT HX = WMax.X-WMin.X, HY = WMax.Y-WMin.Y, HZ = WMax.Z-WMin.Z;
			UBOOL Narrow = Max( HX, HY ) <= 96.f;
			if( Pass==0 )
			{
				if( !Narrow || HZ<=0.f )
					continue;
				NumCandidates++;
				FVector FC = (WMin+WMax)*0.5f;
				INT c;
				for( c=0; c<NumCols; c++ )
					if( FC.X > Cols[c].Min.X-48.f && FC.X < Cols[c].Max.X+48.f
					 && FC.Y > Cols[c].Min.Y-48.f && FC.Y < Cols[c].Max.Y+48.f
					 && FC.Z > Cols[c].Min.Z-48.f && FC.Z < Cols[c].Max.Z+48.f )
						break;
				if( c==NumCols )
				{
					if( NumCols==MAX_COLS )
						continue;
					Cols[NumCols++] = Bounds(i);
				}
				else Cols[c] += Bounds(i);
				continue;
			}

			// Pass 2: everything that is not itself a pouring face.
			if( Narrow && HZ>0.f )
				continue;
			NumCandidates++;
			UBOOL Joined = 0;
			if( Narrow && HZ<=0.f )
			{
				FVector FC = (WMin+WMax)*0.5f;
				for( INT c=0; c<NumCols && !Joined; c++ )
					if( FC.X > Cols[c].Min.X-48.f && FC.X < Cols[c].Max.X+48.f
					 && FC.Y > Cols[c].Min.Y-48.f && FC.Y < Cols[c].Max.Y+48.f
					 && FC.Z > Cols[c].Min.Z-48.f && FC.Z < Cols[c].Max.Z+48.f )
						Joined = 1;
			}
			if( Joined )
				NumCaps++;
			else
			{
				NumRejects++;
				debugf( NAME_Log, "FOUNTAINS   PANE %-28s box=(%7.0f,%8.0f,%7.0f)..(%7.0f,%8.0f,%7.0f) HX=%4.0f HY=%4.0f HZ=%4.0f%s",
					Surf.Texture->GetPathName(),
					WMin.X, WMin.Y, WMin.Z, WMax.X, WMax.Y, WMax.Z, HX, HY, HZ,
					Narrow ? " (flat, no pour at it)" : " (too wide to be a stream)" );
			}
		}
	}
	for( INT c=0; c<NumCols; c++ )
		debugf( NAME_Log, "FOUNTAINS COLUMN box=(%7.0f,%8.0f,%7.0f)..(%7.0f,%8.0f,%7.0f)",
			Cols[c].Min.X, Cols[c].Min.Y, Cols[c].Min.Z, Cols[c].Max.X, Cols[c].Max.Y, Cols[c].Max.Z );
	debugf( NAME_Log, "FOUNTAINS: columns=%i caps absorbed=%i panes left=%i (candidates=%i over %i surfs)",
		NumCols, NumCaps, NumRejects, NumCandidates, Model->Surfs->Num() );
	unguard;
}

/*-----------------------------------------------------------------------------
	Animation probe (-probeanim).
-----------------------------------------------------------------------------*/

// -probeanim[=<substr>] answers "is this texture's animation actually
// running?" headlessly. Fractal textures (fire/water/wave/wet/ice) are ticked
// 70 steps at the 35Hz reference rate -- 2 seconds of animation -- and the
// number of mip-0 bytes that changed is logged: 0 means the procedural
// animation is dead, which on screen looks like a static image that only
// pans. Frame-animated textures log their AnimNext chain instead. Class,
// TF_Realtime and mip count are logged for all matches.
static void RunAnimProbe()
{
	guard(RunAnimProbe);
	char Filter[64]="";
	Parse( appCmdLine(), "PROBEANIM=", Filter, ARRAY_COUNT(Filter) );
	INT Logged=0;
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		UTexture* T = *It;
		if( Filter[0] && !appStrfind( const_cast<char*>(T->GetPathName()), Filter ) )
			continue;
		UBOOL IsFractal=0;
		for( UClass* C=T->GetClass(); C; C=C->GetSuperClass() )
			if( appStricmp( C->GetName(), "FractalTexture" )==0 )
				{ IsFractal=1; break; }
		if( !IsFractal && !Filter[0] )
			continue;
		INT Changed=-1, Total=0;
		UBOOL Dump = Filter[0] && ParseParam( appCmdLine(), "PROBEANIMDUMP" );
		if( IsFractal && T->GetNumMips() && T->GetMip(0)->DataArray.Num() )
		{
			FMipmap* M = T->GetMip(0);
			Total = M->DataArray.Num();
			BYTE* Before = (BYTE*)appMalloc( Total, "AnimProbe" );
			appMemcpy( Before, &M->DataArray(0), Total );
			DOUBLE Time = T->LastUpdateTime;
			for( INT i=0; i<=70; i++ )
			{
				// -probeanimdump (with a filter) writes the palette-resolved
				// output every 10 ticks, so the animation's actual look --
				// local ripples vs a uniformly sliding image -- can be
				// inspected offline.
				if( Dump && (i%10)==0 && T->GetColors() )
				{
					FColor* Pal = T->GetColors();
					INT MU = M->USize, MV = M->VSize;
					BYTE* MD = &M->DataArray(0);
					char BmpName[128];
					appSprintf( BmpName, "Animprobe_%s_t%02i.bmp", T->GetName(), i );
					INT RowBytes = (MU*3+3)&~3, DataBytes = RowBytes*MV, FileBytes = 54+DataBytes;
					BYTE* Bmp = (BYTE*)appMalloc( FileBytes, "AnimProbeBmp" );
					appMemset( Bmp, 0, FileBytes );
					Bmp[0]='B'; Bmp[1]='M';
					*(INT*)(Bmp+ 2) = FileBytes;
					*(INT*)(Bmp+10) = 54;
					*(INT*)(Bmp+14) = 40;
					*(INT*)(Bmp+18) = MU;
					*(INT*)(Bmp+22) = MV;
					*(WORD*)(Bmp+26) = 1;
					*(WORD*)(Bmp+28) = 24;
					for( INT y=0; y<MV; y++ )
						for( INT x=0; x<MU; x++ )
						{
							FColor C = Pal[ MD[y*MU+x] ];
							BYTE* P = Bmp + 54 + (MV-1-y)*RowBytes + x*3;
							P[0]=C.B; P[1]=C.G; P[2]=C.R;
						}
					FILE* F = appFopen( BmpName, "wb" );
					if( F )
					{
						appFwrite( Bmp, 1, FileBytes, F );
						appFclose( F );
					}
					appFree( Bmp );
				}
				if( i==70 )
					break;
				Time += 1.0/35.0;
				T->Update( Time );
			}
			Changed=0;
			BYTE* After = &M->DataArray(0);
			for( INT i=0; i<Total; i++ )
				if( Before[i]!=After[i] )
					Changed++;
			appFree( Before );
		}
		debugf( NAME_Log, "ANIMPROBE %-40s class=%-14s realtime=%i mips=%i animNext=%-20s ticked70: changed %i/%i bytes",
			T->GetPathName(), T->GetClass()->GetName(),
			(INT)((T->TextureFlags & TF_Realtime)!=0), T->GetNumMips(),
			T->AnimNext ? T->AnimNext->GetName() : "None",
			Changed, Total );
		Logged++;
	}
	debugf( NAME_Log, "ANIMPROBE: %i texture(s) logged (filter '%s')", Logged, Filter );
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
	UBOOL	ProbeWalk = 0, WalkPlaced = 0, ProbeStand = 0, ProbeIdle = 0;
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
		// -probestand=x:y:z:yaw is -probewalk without the walking: a real player
		// pawn, collision intact so pickups still work, but standing still.
		// Needed because a weapon's idle state branches on the owner's speed --
		// a walking player plays the walk animation and never reaches the idle
		// behaviour (the BioRifle's drip, a weapon's twiddle), so those can only
		// be observed by standing. -probeview cannot stand in for this: it pins
		// the actor with collision off, and a player who cannot touch anything
		// cannot pick a weapon up.
		else if( Parse( appCmdLine(), "PROBESTAND=", Spec, ARRAY_COUNT(Spec) ) )
		{
			FLOAT X,Y,Z;
			if( sscanf( Spec, "%f:%f:%f:%i", &X,&Y,&Z, &WalkYaw )==4 )
			{
				WalkStart = FVector(X,Y,Z);
				ProbeWalk = 1;
				ProbeStand = 1;
				Parse( appCmdLine(), "PROBEFRAMES=", ProbeFrames );
				debugf( NAME_Log, "PROBESTAND: standing at (%.0f,%.0f,%.0f) yaw=%i for %i frames",
					X,Y,Z, WalkYaw, ProbeFrames );
			}
			else debugf( NAME_Log, "PROBESTAND: need -probestand=x:y:z:yaw (got '%s')", Spec );
		}
		// -probeidle=x:y:z:yaw places a real player and then LEAVES IT ALONE:
		// no input, no velocity written, nothing forced per frame. The one way
		// to answer "does a player standing still stay still", which -probestand
		// cannot because it zeroes the velocity every frame.
		else if( Parse( appCmdLine(), "PROBEIDLE=", Spec, ARRAY_COUNT(Spec) ) )
		{
			FLOAT X,Y,Z;
			if( sscanf( Spec, "%f:%f:%f:%i", &X,&Y,&Z, &WalkYaw )==4 )
			{
				WalkStart = FVector(X,Y,Z);
				ProbeWalk = 1;
				ProbeIdle = 1;
				Parse( appCmdLine(), "PROBEFRAMES=", ProbeFrames );
				debugf( NAME_Log, "PROBEIDLE: idling at (%.0f,%.0f,%.0f) yaw=%i for %i frames",
					X,Y,Z, WalkYaw, ProbeFrames );
			}
			else debugf( NAME_Log, "PROBEIDLE: need -probeidle=x:y:z:yaw (got '%s')", Spec );
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

	// -probefx=x:y:z:ClassName spawns that class there -probefxlead frames
	// BEFORE the -probeview screenshot, so a short-lived animated effect
	// (explosions, smoke) is mid-animation when the shot is read back rather
	// than one frame stale. -probefxscale overrides DrawScale. Deterministic
	// repro for effect-rendering reports: no weapon, no aiming, no timing luck.
	FVector	ProbeFxLoc(0,0,0);
	char	ProbeFxName[64]="";
	FLOAT	ProbeFxScale = 0.f;
	INT		ProbeFxLead = 20;
	UBOOL	ProbeFx = 0, ProbeFxSpawned = 0, ProbeFxAim = 0, ProbeFxFire = 0;
	FRotator ProbeFxRot(0,0,0);
	{
		char Spec[256]="";
		if( Parse( appCmdLine(), "PROBEFX=", Spec, ARRAY_COUNT(Spec) ) )
		{
			FLOAT X,Y,Z;
			if( sscanf( Spec, "%f:%f:%f:%63s", &X,&Y,&Z, ProbeFxName )==4 )
			{
				ProbeFxLoc = FVector(X,Y,Z);
				ProbeFx = 1;
			}
			else debugf( NAME_Log, "PROBEFX: need -probefx=x:y:z:ClassName (got '%s')", Spec );
		}
		// -probefxaim=<ClassName> instead traces the player's OWN aim (eye +
		// ViewRotation) to the first thing a projectile would hit and spawns the
		// effect a rocket's-width off that surface. With a loaded save that is
		// literally "fire a rocket from where I was standing" -- no coordinates
		// to guess, and it reproduces the impact geometry, which is what makes
		// or breaks effect rendering.
		else if( Parse( appCmdLine(), "PROBEFXAIM=", ProbeFxName, ARRAY_COUNT(ProbeFxName) ) )
		{
			ProbeFx = ProbeFxAim = 1;
		}
		// -probefire=<ProjectileClass> launches the real thing from the muzzle
		// along the player's aim, rotation and all -- so it flies, lays its smoke
		// trail and explodes on its own. The only faithful repro of "I shot a
		// rocket here": a hand-placed explosion skips the trail entirely.
		else if( Parse( appCmdLine(), "PROBEFIRE=", ProbeFxName, ARRAY_COUNT(ProbeFxName) ) )
		{
			ProbeFx = ProbeFxAim = ProbeFxFire = 1;
		}
		if( ProbeFx )
		{
			Parse( appCmdLine(), "PROBEFXLEAD=", ProbeFxLead );
			Parse( appCmdLine(), "PROBEFXSCALE=", ProbeFxScale );
			debugf( NAME_Log, "PROBEFX: %s lead=%i frames aim=%i", ProbeFxName, ProbeFxLead, (INT)ProbeFxAim );
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
		// -probemusic: report what the level asks for and what the player was
		// handed, then kick the music state machine. A level's Song reaches the
		// player only through GameInfo.Login -> ClientSetMusic, so "no music on
		// this map" is either a Song the map never set, a handoff that did not
		// happen, or a module libxmp cannot decode -- and these three look
		// identical from inside the game.
		{
			static UBOOL MusicInfo = ParseParam( appCmdLine(), "PROBEMUSIC" );
			static INT MusicLogged = 0;
			UGameEngine* GM = Cast<UGameEngine>( Engine );
			if( MusicInfo && GM && GM->GLevel && Engine->Client && MusicLogged<3 )
			{
				ALevelInfo* LI = GM->GLevel->GetLevelInfo();
				for( INT v=0; v<Engine->Client->Viewports.Num(); v++ )
				{
					APlayerPawn* P = Engine->Client->Viewports(v)->Actor;
					if( !P )
						continue;
					MusicLogged++;
					debugf( NAME_Log, "MUSICPROBE level.Song=%s section=%i | player.Song=%s section=%i transition=%i",
						(LI && LI->Song) ? LI->Song->GetPathName() : "NONE", LI ? (INT)LI->SongSection : -1,
						P->Song ? P->Song->GetPathName() : "NONE", (INT)P->SongSection, (INT)P->Transition );
					// -probemusic=<Package.Object> plays a named module instead of
					// the level's. Maps that start SILENT (Vortex2, Dark,
					// SkyBase, SkyTown, Veloraend all have Level.Song NONE and
					// bring music in on a MusicEvent trigger) cannot be tested
					// any other way -- the module never loads unless the player
					// walks into the right trigger.
					UMusic* Want = LI ? LI->Song : NULL;
					char MusicName[128]="";
					if( Parse( appCmdLine(), "PROBEMUSIC=", MusicName, ARRAY_COUNT(MusicName) ) && MusicName[0] )
					{
						UMusic* M = LoadObject<UMusic>( NULL, MusicName, NULL, LOAD_NoWarn|LOAD_KeepImports, NULL );
						debugf( NAME_Log, "MUSICPROBE requested '%s' -> %s", MusicName, M ? M->GetPathName() : "LOAD FAILED" );
						if( M )
							Want = M;
					}
					// Force the handoff and a start, so a decode failure shows up
					// as Galaxy's own "failed to load music" warning.
					if( Want )
					{
						P->Song        = Want;
						P->SongSection = 0;
						P->Transition  = MTRAN_Instant;
					}
				}
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
					if( ProbeIdle )
					{
						// Aim it once, then never write to it again.
						P->Physics      = PHYS_Walking;
						P->Rotation.Yaw = P->ViewRotation.Yaw = WalkYaw;
						P->Velocity     = FVector(0,0,0);
						P->Acceleration = FVector(0,0,0);
					}
					debugf( NAME_Log, "PROBEWALK: placed at (%.0f,%.0f,%.0f)", P->Location.X, P->Location.Y, P->Location.Z );
				}
				FLOAT A = WalkYaw * (2.f*PI/65536.f);
				FVector Dir( appCos(A), appSin(A), 0.f );
				if( ProbeIdle )
				{
					// x64 port: IDLE is the one mode that must not touch the pawn
					// after placing it. -probestand rewrites Velocity and
					// Acceleration to zero every frame, which is precisely the
					// state a drift bug would be hiding in -- it cannot show
					// whether a player left alone STAYS put. Here the physics runs
					// untouched and the log reports what it did, to a resolution
					// that makes a slow creep obvious.
					if( ( FrameNum % 25 )==0 || FrameNum==ProbeFrames-1 )
						debugf( NAME_Log, "PROBEIDLE: frame %3i at (%.3f,%.3f,%.3f) vel=(%.4f,%.4f,%.4f) accel=(%.3f,%.3f,%.3f) phys=%i base=%s zone=%s friction=%.2f gravityZ=%.1f dt=%.5f",
							FrameNum, P->Location.X, P->Location.Y, P->Location.Z,
							P->Velocity.X, P->Velocity.Y, P->Velocity.Z,
							P->Acceleration.X, P->Acceleration.Y, P->Acceleration.Z,
							(INT)P->Physics, P->Base ? P->Base->GetName() : "none",
							P->Region.Zone ? P->Region.Zone->GetName() : "none",
							P->Region.Zone ? P->Region.Zone->ZoneGroundFriction : -1.f,
							P->Region.Zone ? P->Region.Zone->ZoneGravity.Z : 0.f,
							G->GLevel ? G->GLevel->GetLevelInfo()->TimeSeconds : 0.f );
				}
				else
				{
				P->Physics      = PHYS_Walking;
				P->Rotation.Yaw = P->ViewRotation.Yaw = WalkYaw;
				if( ProbeStand )
				{
					// Stand: keep the horizontal velocity at rest so weapon and
					// pawn idle behaviour runs, but leave gravity alone.
					P->Velocity     = FVector( 0, 0, P->Velocity.Z );
					P->Acceleration = FVector( 0, 0, 0 );
				}
				else
				{
					P->Velocity     = Dir * P->GroundSpeed + FVector(0,0,P->Velocity.Z);
					P->Acceleration = Dir * P->AccelRate;
				}
				if( ( FrameNum % 25 )==0 || FrameNum==ProbeFrames-1 )
						debugf( NAME_Log, "%s: frame %3i at (%.0f,%.0f,%.0f) class=%s bIsPlayer=%i intelligence=%i instigator=%s",
							ProbeStand ? "PROBESTAND" : "PROBEWALK", FrameNum, P->Location.X, P->Location.Y, P->Location.Z,
							P->GetClass()->GetName(), (INT)P->bIsPlayer, (INT)P->Intelligence,
							P->Instigator ? P->Instigator->GetName() : "None" );
					// Report any door that left its closed keyframe while walking:
					// the only way to tell a "the trigger never fired" door from a
					// "the trigger fired and the door moved" one is to walk the real
					// player path and watch the movers, not the triggers.
					if( ( FrameNum % 25 )==0 || FrameNum==ProbeFrames-1 )
						for( INT m=0; m<G->GLevel->Num(); m++ )
						{
							AMover* Mv = Cast<AMover>( G->GLevel->Element(m) );
							if( Mv && ( Mv->KeyNum!=0 || Mv->PrevKeyNum!=0
								|| (Mv->Location-P->Location).Size()<600.f ) )
								debugf( NAME_Log, "PROBEWALK   frame %3i door %s (%s) state=%s key=%i prev=%i savedTrigger=%s loc=(%.0f,%.0f,%.0f)",
									FrameNum, Mv->GetName(), *Mv->Tag,
									(Mv->GetMainFrame() && Mv->GetMainFrame()->StateNode) ? Mv->GetMainFrame()->StateNode->GetName() : "None",
									(INT)Mv->KeyNum, (INT)Mv->PrevKeyNum,
									Mv->SavedTrigger ? Mv->SavedTrigger->GetName() : "None",
									Mv->Location.X, Mv->Location.Y, Mv->Location.Z );
						}
					// And the triggers themselves: a door that never moves is either a
					// trigger that never saw the player, or one that saw him and
					// declined. Touching[] tells those two apart, and nothing else does.
					if( ( FrameNum % 25 )==0 || FrameNum==ProbeFrames-1 )
						for( INT t=0; t<G->GLevel->Num(); t++ )
						{
							AActor* Tr = G->GLevel->Element(t);
							if( !Tr || !Tr->IsA(ATrigger::StaticClass) )
								continue;
							FLOAT D = (Tr->Location-P->Location).Size();
							if( D > 300.f )
								continue;
							char Touch[128]="";
							for( INT k=0; k<ARRAY_COUNT(Tr->Touching); k++ )
								if( Tr->Touching[k] )
									appSprintf( Touch+appStrlen(Touch), "%s ", Tr->Touching[k]->GetName() );
							debugf( NAME_Log, "PROBEWALK   frame %3i trigger %s (tag=%s event=%s) dist=%.0f radius=%.0f height=%.0f collide=%i touching=[%s]",
								FrameNum, Tr->GetName(), *Tr->Tag, *Tr->Event, D,
								Tr->CollisionRadius, Tr->CollisionHeight, (INT)Tr->bCollideActors, Touch );
							// LIVE gate values: bInitiallyActive is toggled at runtime, so the
							// value the map was authored with says nothing about whether the
							// trigger will answer the player standing on it right now.
							DumpAddedProps( Tr, "     " );
						}
					// -probeforcetouch=<TriggerName> calls that trigger's Touch by hand at
					// frame 60. If the door then opens, the trigger's own logic is fine and
					// the engine never delivered the touch; if it stays shut, the script
					// declined the player. Nothing else separates those two.
					{
						static char ForceName[64]="";
						static UBOOL ForceParsed = 0, Forced = 0;
						if( !ForceParsed )
						{
							ForceParsed = 1;
							Parse( appCmdLine(), "PROBEFORCETOUCH=", ForceName, ARRAY_COUNT(ForceName) );
						}
						if( ForceName[0] && !Forced && FrameNum>=60 )
						{
							Forced = 1;
							for( INT t=0; t<G->GLevel->Num(); t++ )
							{
								AActor* Tr = G->GLevel->Element(t);
								if( Tr && !appStricmp( Tr->GetName(), ForceName ) )
								{
									debugf( NAME_Log, "PROBEWALK   forcing %s->Touch(%s)", Tr->GetName(), P->GetName() );
									Tr->eventTouch( P );
								}
							}
						}
					}
				}
			}
			// -probeshotevery=N applies here too, not just to the pinned view:
			// a walked sequence of consecutive frames is what shows temporal
			// behaviour -- mip shimmer, texture animation, anything that only
			// misbehaves while the camera moves. A still cannot show any of it.
			{
				INT WalkShotEvery = 0;
				Parse( appCmdLine(), "PROBESHOTEVERY=", WalkShotEvery );
				if( WalkShotEvery > 0 && FrameNum >= ProbeFrames/2 && (FrameNum % WalkShotEvery)==0 )
					for( INT v=0; v<Engine->Client->Viewports.Num(); v++ )
						Engine->Client->Viewports(v)->Exec( "SHOT", GSystem );
			}
			if( ++FrameNum >= ProbeFrames )
			{
				for( INT v=0; v<Engine->Client->Viewports.Num(); v++ )
					Engine->Client->Viewports(v)->Exec( "SHOT", GSystem );
				debugf( NAME_Log, "PROBEWALK: done, exiting" );
				GIsRequestingExit = 1;
			}
		}

		// -probeai: sample every creature's AI each frame while the level runs,
		// and report at the end (see SampleAI). Runs alongside -probewalk or
		// -probeview, so the creatures can be watched while standing still or
		// while walking into them; -probeframes sets how long.
		{
			static UBOOL ProbeAI = ParseParam( appCmdLine(), "PROBEAI" );
			static INT   AIFrames = 0, AILimit = 0;
			if( ProbeAI )
			{
				if( !AILimit )
				{
					AILimit = 300;
					Parse( appCmdLine(), "PROBEFRAMES=", AILimit );

					// -probeaiat=<class substring> drops the player in front of
					// the first creature of that kind. Watching a map's AI from
					// the player start only proves the creatures tick: nothing
					// can see the player, so nothing hunts, attacks or takes
					// damage. Confronting one exercises the whole chain --
					// perception, state change, pathing, combat -- and does it
					// the same way every run, whatever the map.
					char AtClass[64]="";
					if( Parse( appCmdLine(), "PROBEAIAT=", AtClass, ARRAY_COUNT(AtClass) ) )
					{
						UGameEngine* G = Cast<UGameEngine>( Engine );
						// The subject is any pawn the AI treats as a player --
						// found in the level, not through a viewport, so this
						// works on a dedicated server where the "player" is a
						// bot and there is no client at all.
						// Headless runs always use their OWN decoy. Some maps
						// carry a placed player-class actor for scripted scenes;
						// adopting one of those as the subject means auditing a
						// pawn parked in a corner of the map that no creature
						// will ever meet.
						APawn* Player = NULL;
						if( G && G->GLevel && Engine->Client )
							for( INT i=0; i<G->GLevel->Num(); i++ )
							{
								APawn* P = Cast<APawn>( G->GLevel->Element(i) );
								if( P && P->bIsPlayer && P->Health>0 )
									{ Player = P; break; }
							}
						// Headless: a dedicated server has no player at all, and
						// the deathmatch game types that do spawn bots set
						// bNoMonsters, which destroys every creature on sight --
						// so neither gives a level with creatures AND something
						// for them to notice. Spawn a decoy instead: an ordinary
						// player pawn, unpossessed. bIsPlayer is what the engine
						// keys perception off (UnLevTic.cpp calls ShowSelf on
						// player pawns), so creatures see, hunt and maul it
						// exactly as they would a person -- and its health is
						// then the proof that they did.
						UBOOL Decoy = 0;
						if( G && G->GLevel && !Player )
						{
							char DecoyClass[64]="MaleOne";
							Parse( appCmdLine(), "PROBEAIDECOY=", DecoyClass, ARRAY_COUNT(DecoyClass) );
							UClass* PC = FindObject<UClass>( ANY_PACKAGE, DecoyClass );
							// Spawn on a PlayerStart, not the world origin --
							// which is inside solid rock in most maps -- and
							// with bNoCollisionFail so a tight spot cannot
							// silently leave the audit with no subject at all.
							FVector Start(0,0,0);
							for( INT i=0; i<G->GLevel->Num(); i++ )
							{
								AActor* A = G->GLevel->Element(i);
								if( A && appStrfind( const_cast<char*>(A->GetClass()->GetName()), "PlayerStart" ) )
									{ Start = A->Location; break; }
							}
							if( PC )
							{
								Player = Cast<APawn>( G->GLevel->SpawnActor(
									PC, NAME_None, NULL, NULL, Start, FRotator(0,0,0), NULL, 1, 1 ) );
								Decoy = Player!=NULL;
							}
							debugf( NAME_Log, "AIPROBE: decoy player %s (class '%s')",
								Decoy ? "spawned" : "FAILED", DecoyClass );
						}
						(void)Decoy;
						if( G && G->GLevel && Player )
						{
							for( INT i=0; i<G->GLevel->Num(); i++ )
							{
								APawn* C = Cast<APawn>( G->GLevel->Element(i) );
								if( !C || C->bIsPlayer )
									continue;
								// "any" (or a bare -probeaiat=) takes the first
								// creature with a clear line, whatever it is --
								// what a sweep across every map wants, since
								// each map has a different cast. "alarm" takes
								// the first one the mapper gave an AlarmTag,
								// which is the only kind that can lead the
								// player to a secret.
								if( appStricmp( AtClass, "alarm" )==0 )
								{
									FName Tag = GetNameProp( C, "AlarmTag" );
									if( Tag==NAME_None )
										continue;
								}
								else if( AtClass[0] && appStricmp( AtClass, "any" )!=0
								&&  !appStrfind( const_cast<char*>(C->GetClass()->GetName()), AtClass ) )
									continue;
								// Stand 220 units away ALONG THE WAY IT IS
								// FACING: outside melee reach, well inside
								// sight range, and inside its view cone.
								// LineOfSightTo applies PeripheralVision to
								// the creature's own facing, so a subject
								// dropped at a fixed world offset lands behind
								// half the creatures in the game and is
								// correctly never seen -- which reads as
								// "the AI is blind" when it is nothing of the
								// sort.
								FVector At = C->Location + C->Rotation.Vector()*220.f + FVector(0,0,40);
								// And REQUIRE a clear line before settling on
								// this creature: doors and pillars block sight
								// legitimately, so a blocked candidate proves
								// nothing either way. Walk on to the next one.
								FCheckResult Hit(1.f);
								FVector Eye = C->Location; Eye.Z += C->BaseEyeHeight;
								G->GLevel->SingleLineCheck( Hit, C, At, Eye, TRACE_VisBlocking );
								if( Hit.Time < 1.f )
								{
									debugf( NAME_Log, "AIPROBE: skipping %s '%s' -- line blocked by %s",
										C->GetClass()->GetName(), C->GetName(),
										Hit.Actor ? Hit.Actor->GetName() : "world geometry" );
									continue;
								}
								G->GLevel->FarMoveActor( Player, At, 0, 1 );
								Player->ViewRotation = (C->Location-Player->Location).Rotation();
								Player->Rotation.Yaw = Player->ViewRotation.Yaw;
								debugf( NAME_Log, "AIPROBE: placed player at (%.0f,%.0f,%.0f), facing %s '%s' at (%.0f,%.0f,%.0f)",
									Player->Location.X, Player->Location.Y, Player->Location.Z,
									C->GetClass()->GetName(), C->GetName(),
									C->Location.X, C->Location.Y, C->Location.Z );
								break;
							}
						}
					}
				}
				SampleAI( Engine );
				if( ++AIFrames >= AILimit )
				{
					ReportAI( Engine );
					debugf( NAME_Log, "AIPROBE: done, exiting" );
					GIsRequestingExit = 1;
				}
			}
		}

		// -probeholdfire[=<Weapon class>] holds the trigger down. Weapon state
		// machines are where sound and animation bugs live -- a clip change, a
		// reload, an eject -- and none of them can be reached by spawning
		// actors or pinning a camera: something has to actually work the gun.
		// The optional class is given to the player first (summoned into the
		// world at their feet, which the pickup logic then hands over).
		{
			static UBOOL   HoldFire   = 0;
			static UBOOL   HoldInit   = 0;
			static char    HoldWeapon[64] = "";
			if( !HoldInit )
			{
				HoldInit = 1;
				// -probeholdfire[=<Weapon>] equips (optionally) and holds the
				// trigger; -probegive=<Weapon> only equips. Idle behaviour --
				// a weapon's twiddle, the BioRifle's drip -- only runs when the
				// trigger is NOT held, so the two have to be separable.
				HoldFire = ParseParam( appCmdLine(), "PROBEHOLDFIRE" );
				Parse( appCmdLine(), "PROBEHOLDFIRE=", HoldWeapon, ARRAY_COUNT(HoldWeapon) );
				Parse( appCmdLine(), "PROBEGIVE=", HoldWeapon, ARRAY_COUNT(HoldWeapon) );
				if( HoldWeapon[0] && Engine->Client && Engine->Client->Viewports.Num() )
				{
					// Spawn it ON the player, not via the summon console command:
					// summon drops the weapon some way in FRONT of the player, so
					// a stationary test never touches it and never picks it up.
					UGameEngine* WG = Cast<UGameEngine>( Engine );
					APlayerPawn* WP = Engine->Client->Viewports(0)->Actor;
					UClass* WC = FindObject<UClass>( ANY_PACKAGE, HoldWeapon );
					if( !WC )
						WC = GObj.LoadClass( AActor::StaticClass, NULL, HoldWeapon, NULL, LOAD_NoWarn|LOAD_KeepImports, NULL );
					AActor* W = (WC && WG && WG->GLevel && WP)
						? WG->GLevel->SpawnActor( WC, NAME_None, NULL, NULL, WP->Location, WP->Rotation, NULL, 0, 1 )
						: NULL;
					// And hand it over: touch detection fires on MOVEMENT, so a
					// pickup spawned already overlapping a stationary player is
					// never noticed. Calling Touch directly is what the engine
					// would have called had the player walked onto it.
					if( W && WP )
						W->eventTouch( WP );
					debugf( NAME_Log, "PROBEHOLDFIRE: %s %s at the player",
						W ? "spawned and handed over" : "FAILED to spawn", HoldWeapon );
				}
			}
			if( HoldFire && Engine->Client )
				for( INT v=0; v<Engine->Client->Viewports.Num(); v++ )
				{
					APlayerPawn* P = Engine->Client->Viewports(v)->Actor;
					if( P )
						P->bFire = 1;
				}
		}

		char ProbeExec[128]="";
		UBOOL HasExec = Parse( appCmdLine(), "PROBEEXEC=", ProbeExec, ARRAY_COUNT(ProbeExec) );
		if( !ProbeWalk && (PinView || HasExec || ProbeFx) && Engine->Client )
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
			++FrameNum;
			if( ProbeFx && !ProbeFxSpawned && FrameNum >= ProbeFrames-ProbeFxLead )
			{
				UGameEngine* G = Cast<UGameEngine>( Engine );
				if( G && G->GLevel )
				{
					ProbeFxSpawned = 1;
					if( ProbeFxAim )
					{
						APlayerPawn* P = Engine->Client->Viewports(0)->Actor;
						if( !P )
						{
							debugf( NAME_Log, "PROBEFX: aim failed -- no view actor" );
							continue;
						}
						FVector Eye = P->Location + FVector(0,0,P->BaseEyeHeight);
						FVector Dir = P->ViewRotation.Vector();
						FCheckResult Hit(1.f);
						G->GLevel->SingleLineCheck( Hit, P, Eye + Dir*10000.f, Eye, TRACE_VisBlocking );
						if( ProbeFxFire )
						{
							// Muzzle, aimed: the projectile does the rest itself.
							ProbeFxLoc = Eye + Dir*40.f;
							ProbeFxRot = P->ViewRotation;
						}
						else
						{
							ProbeFxLoc = ( Hit.Time < 1.f )
								? Hit.Location + Hit.Normal*16.f
								: Eye + Dir*512.f;
						}
						debugf( NAME_Log, "PROBEFX aim: eye (%.0f,%.0f,%.0f) view %i:%i -> spawn (%.0f,%.0f,%.0f) wall dist=%.0f fire=%i",
							Eye.X,Eye.Y,Eye.Z, (INT)P->ViewRotation.Pitch, (INT)P->ViewRotation.Yaw,
							ProbeFxLoc.X, ProbeFxLoc.Y, ProbeFxLoc.Z,
							Hit.Time<1.f ? (Hit.Location-Eye).Size() : -1.f, (INT)ProbeFxFire );
					}
					// Find it if it is already in memory, LOAD it if not: a class
					// is only resident once something in the map has referenced
					// it, so probing an effect in a level that happens not to
					// contain the weapon that spawns it would otherwise fail
					// with "not found". Qualified names (UnrealI.ShellCase) load
					// straight; bare ones are retried against the game package.
					UClass* C = FindObject<UClass>( ANY_PACKAGE, ProbeFxName );
					if( !C )
						C = GObj.LoadClass( AActor::StaticClass, NULL, ProbeFxName, NULL, LOAD_NoWarn|LOAD_KeepImports, NULL );
					if( !C && !appStrchr( ProbeFxName, '.' ) )
					{
						char Qualified[128];
						appSprintf( Qualified, "UnrealI.%s", ProbeFxName );
						C = GObj.LoadClass( AActor::StaticClass, NULL, Qualified, NULL, LOAD_NoWarn|LOAD_KeepImports, NULL );
					}
					APawn* Shooter = ProbeFxFire ? (APawn*)Engine->Client->Viewports(0)->Actor : NULL;
					AActor* A = C ? G->GLevel->SpawnActor( C, NAME_None, Shooter, Shooter, ProbeFxLoc, ProbeFxRot, NULL, 0, 1 ) : NULL;
					if( A )
					{
						if( ProbeFxScale > 0.f )
							A->DrawScale = ProbeFxScale;
						// Launch it by hand. Entering the projectile's auto state
						// (which is what normally sets Velocity) does not happen
						// for a C++-spawned actor here, so it would just hover at
						// the muzzle dribbling trail smoke.
						if( ProbeFxFire )
						{
							AProjectile* Proj = Cast<AProjectile>( A );
							FLOAT Speed = Proj ? Proj->speed : 900.f;
							A->Velocity = ProbeFxRot.Vector() * Speed;
							if( Proj )
								Proj->Acceleration = ProbeFxRot.Vector() * 50.f;
							debugf( NAME_Log, "PROBEFX: launched at %.0f u/s", Speed );
						}
						debugf( NAME_Log, "PROBEFX: spawned %s at (%.0f,%.0f,%.0f) scale=%.2f style=%i",
							A->GetName(), A->Location.X, A->Location.Y, A->Location.Z, A->DrawScale, (INT)A->Style );
					}
					else debugf( NAME_Log, "PROBEFX: spawn FAILED (class '%s' %s)", ProbeFxName, C ? "spawn refused" : "not found" );
				}
			}
			// -probeshotevery=N additionally screenshots every N frames from
			// half-way to ProbeFrames on -- a same-run frame series, so texture
			// animation can be compared shot-to-shot without cross-run timing
			// noise.
			INT ShotEvery = 0;
			Parse( appCmdLine(), "PROBESHOTEVERY=", ShotEvery );
			if( ShotEvery > 0 && FrameNum >= ProbeFrames/2 && FrameNum < ProbeFrames && ((ProbeFrames-FrameNum) % ShotEvery)==0 )
				for( INT v=0; v<Engine->Client->Viewports.Num(); v++ )
					Engine->Client->Viewports(v)->Exec( "SHOT", GSystem );
			// -probeexec runs -probeexeclead frames BEFORE the shot, so whatever
			// it does is on screen by the time the shot is taken. Running it in
			// the same frame captured the view as it was BEFORE the command --
			// a command that opens a menu, say, showed no menu at all.
			INT ExecLead = 8;
			Parse( appCmdLine(), "PROBEEXECLEAD=", ExecLead );
			if( HasExec && FrameNum == Max( 1, ProbeFrames-ExecLead ) )
			{
				// Through the VIEWPORT first: that is what reaches the player
				// pawn's own exec functions (ShowLoadMenu and its kin). Only
				// engine-level commands fall through to the engine itself.
				UBOOL Handled = 0;
				for( INT v=0; v<Engine->Client->Viewports.Num(); v++ )
					if( Engine->Client->Viewports(v)->Exec( ProbeExec, GSystem ) )
						Handled = 1;
				if( !Handled )
					Engine->Exec( ProbeExec, GSystem );
			}
			if( FrameNum == ProbeFrames )
			{
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
			GTouchTrace = ParseParam( appCmdLine(), "TOUCHTRACE" );
			if( !GIsRequestingExit && ParseParam( appCmdLine(), "PROBEDOOR" ) )
			{
				RunDoorProbe( Engine );
				GIsRequestingExit = 1;
			}
			if( !GIsRequestingExit && ParseParam( appCmdLine(), "PROBETEX" ) )
			{
				RunTexProbe();
				GIsRequestingExit = 1;
			}
			if( !GIsRequestingExit && ParseParam( appCmdLine(), "PROBESURFS" ) )
			{
				RunSurfProbe( Engine );
				GIsRequestingExit = 1;
			}
			if( !GIsRequestingExit && ParseParam( appCmdLine(), "PROBEMESHANIM" ) )
			{
				RunMeshAnimProbe();
				GIsRequestingExit = 1;
			}
			if( !GIsRequestingExit && ParseParam( appCmdLine(), "PROBEFOUNTAINS" ) )
			{
				RunFountainProbe( Engine );
				GIsRequestingExit = 1;
			}
			if( !GIsRequestingExit && ParseParam( appCmdLine(), "PROBEANIM" ) )
			{
				RunAnimProbe();
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
