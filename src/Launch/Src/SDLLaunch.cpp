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
		// Zone facts too: an actor whose zone has a SkyZone is OUTDOORS, which is
		// how to find an open-sky camera spot without guessing yaws (fog matters
		// for effect rendering as well, so report it alongside).
		AZoneInfo* Zone = Act->Region.Zone;
		debugf( NAME_Log, "ACTORPROBE %-22s %-26s (%7.0f,%8.0f,%7.0f) rot=%i:%i corona=%i skin=%s mesh=%s collide=%i zone=%s sky=%i fog=%i",
			Act->GetClass()->GetName(), Act->GetName(),
			Act->Location.X, Act->Location.Y, Act->Location.Z,
			(INT)Act->Rotation.Pitch, (INT)Act->Rotation.Yaw,
			(INT)Act->bCorona,
			Act->Skin ? Act->Skin->GetName() : "None",
			Act->Mesh ? Act->Mesh->GetName() : "None",
			(INT)Act->bCollideActors,
			Zone ? Zone->GetName() : "None",
			(INT)( Zone && Zone->SkyZone ),
			(INT)( Zone && Zone->bFogZone ) );
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
					UClass* C = FindObject<UClass>( ANY_PACKAGE, ProbeFxName );
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
			if( FrameNum == ProbeFrames )
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
