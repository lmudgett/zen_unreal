/*=============================================================================
	SelfTest.cpp: x64 port subsystem self-tests (Unreal.exe -selftest).

	Exercises the port's subsystems in isolation from gameplay: no window,
	renderer, audio device, level tick, or input — just the object system
	plus the on-disk retail content as test vectors. Each check logs
	"SelfTest: PASS/FAIL <name>"; RunSelfTests() returns the failure count
	(the process exit code), so scripts/selftest.ps1 can gate on it.

	Covers: Core math/containers/names, compiled-vs-script layout
	invariants (the class-Object mirror pins that caused the
	CleanupDestroyed bug), the translating loader across EVERY retail
	package (script, texture, sound, music, and all maps), the hardened
	WAV parser against every retail sound, and script/bytecode presence
	for the modernized bot classes.
=============================================================================*/

#include "LaunchPrivate.h" // Engine.h (via) already includes UnAudio.h

static INT GPassed = 0, GFailed = 0;

static void TestResult( const char* Name, UBOOL Ok, const char* Detail = "" )
{
	if( Ok )
	{
		GPassed++;
		debugf( "SelfTest: PASS %s %s", Name, Detail );
	}
	else
	{
		GFailed++;
		debugf( "SelfTest: FAIL %s %s", Name, Detail );
	}
}
#define TEST(expr) TestResult( #expr, (expr) )

/*-----------------------------------------------------------------------------
	Core: math, names, containers, varargs.
-----------------------------------------------------------------------------*/

static void TestCore()
{
	debugf( "SelfTest: --- Core ---" );

	// Vector math, including the zero-vector guard the physics fixes rely on.
	TEST( FVector(0,0,0).SafeNormal()==FVector(0,0,0) );
	TEST( Abs(FVector(3,4,0).Size()-5.f) < 0.001f );
	TEST( Abs(FVector(1,0,0) | FVector(0,1,0)) < 0.001f );
	TEST( ((FVector(1,0,0) ^ FVector(0,1,0)) - FVector(0,0,1)).Size() < 0.001f );

	// FName: add, then case-insensitive find resolves to the same entry.
	FName A( "SelfTestXyzzy", FNAME_Add );
	FName B( "SELFTESTXYZZY", FNAME_Find );
	TEST( A==B && A!=NAME_None );

	// TArray growth and removal.
	{
		TArray<INT> Arr;
		for( INT i=0; i<100; i++ )
			Arr.AddItem( i*i );
		TEST( Arr.Num()==100 && Arr(99)==9801 );
		Arr.Remove( 0, 50 );
		TEST( Arr.Num()==50 && Arr(0)==2500 );
	}

	// appSprintf %s with a plain char* (the x64 varargs discipline).
	{
		char Buf[64];
		appSprintf( Buf, "%s-%i", "x", 42 );
		TEST( appStrcmp( Buf, "x-42" )==0 );
	}
	TEST( appAtoi("200")==200 );
	TEST( Abs(appAtof("0.5")-0.5f) < 0.0001f );
}

/*-----------------------------------------------------------------------------
	Layout invariants: the compiled C++ truth the script layouts pin to.
-----------------------------------------------------------------------------*/

static void TestLayout()
{
	debugf( "SelfTest: --- Layout ---" );
	TestResult( "sizeof(FVector)==12",  sizeof(FVector)==12 );
	TestResult( "sizeof(FRotator)==12", sizeof(FRotator)==12 );
	TestResult( "sizeof(UObject)==72",  sizeof(UObject)==72 );
	TestResult( "sizeof(AActor)==556",  sizeof(AActor)==556 );
}

/*-----------------------------------------------------------------------------
	Translating loader: every retail package must load.
-----------------------------------------------------------------------------*/

static INT LoadOne( const char* Path )
{
	try
	{
		if( GObj.LoadPackage( NULL, Path, LOAD_KeepImports ) )
			return 1;
	}
	catch( const char* Err )
	{
		debugf( "SelfTest: load of %s threw: %s", Path, Err );
		GIsCriticalError = 0;
	}
	catch( ... )
	{
		debugf( "SelfTest: load of %s threw", Path );
		GIsCriticalError = 0;
	}
	return 0;
}

static void TestLoader()
{
	debugf( "SelfTest: --- Loader (all retail content) ---" );
	static const struct { const char* Label; const char* Dir; const char* Spec; } Sets[] =
	{
		{ "script packages",  "",                       "*.u"   },
		{ "texture packages", "..\\content\\Textures\\", "*.utx" },
		{ "sound packages",   "..\\content\\Sounds\\",   "*.uax" },
		{ "music packages",   "..\\content\\Music\\",    "*.umx" },
		{ "maps",             "..\\content\\Maps\\",     "*.unr" },
	};
	for( INT s=0; s<ARRAY_COUNT(Sets); s++ )
	{
		char Spec[256], Path[256], Detail[256];
		appSprintf( Spec, "%s%s", Sets[s].Dir, Sets[s].Spec );
		TArray<FString> Files = appFindFiles( Spec );
		INT Ok = 0;
		for( INT i=0; i<Files.Num(); i++ )
		{
			appSprintf( Path, "%s%s", Sets[s].Dir, *Files(i) );
			Ok += LoadOne( Path );
		}
		appSprintf( Detail, "(%s: %i/%i)", Sets[s].Label, Ok, Files.Num() );
		TestResult( "load package set", Files.Num()>0 && Ok==Files.Num(), Detail );
	}
}

/*-----------------------------------------------------------------------------
	Class-Object mirror pins (the CleanupDestroyed root cause, kept honest).
-----------------------------------------------------------------------------*/

static void TestObjectPins()
{
	debugf( "SelfTest: --- Script layout pins ---" );
	UClass* Obj = FindObject<UClass>( ANY_PACKAGE, "Object" );
	TestResult( "class Object loaded", Obj!=NULL );
	if( !Obj )
		return;
	TestResult( "class Object PropertiesSize==sizeof(UObject)",
		Obj->GetPropertiesSize()==sizeof(UObject) );

	INT ParentOff=-1, FlagsOff=-1, NameOff=-1, ClassOff=-1;
	for( TFieldIterator<UProperty> It(Obj); It; ++It )
	{
		if     ( appStricmp(It->GetName(),"Parent"     )==0 ) ParentOff = It->Offset;
		else if( appStricmp(It->GetName(),"ObjectFlags")==0 ) FlagsOff  = It->Offset;
		else if( appStricmp(It->GetName(),"Name"       )==0 ) NameOff   = It->Offset;
		else if( appStricmp(It->GetName(),"Class"      )==0 ) ClassOff  = It->Offset;
	}
	// The compiled x64 UObject layout (pack(4), sizeof 72) puts the mirrored
	// members at Parent=48, ObjectFlags=56, Name=60, Class=64 — the values
	// UStruct::LinkOffsets pins them to via STRUCT_OFFSET. If these move,
	// the pins moved (or broke): this test forces that conversation. The
	// unpinned computed offset for Parent was 24 — the CleanupDestroyed bug.
	char Detail[128];
	appSprintf( Detail, "(Parent=%i Flags=%i Name=%i Class=%i)", ParentOff, FlagsOff, NameOff, ClassOff );
	TestResult( "Object.Parent pinned",      ParentOff==48, Detail );
	TestResult( "Object.ObjectFlags pinned", FlagsOff==56 );
	TestResult( "Object.Name pinned",        NameOff==60 );
	TestResult( "Object.Class pinned",       ClassOff==64 );
}

/*-----------------------------------------------------------------------------
	Script/bytecode presence for the game and modernized bot classes.
-----------------------------------------------------------------------------*/

// Class-level Script is empty by design — bytecode lives in the function and
// state children. Sum it across the class's fields.
static INT ClassCodeBytes( UClass* Class )
{
	INT Bytes = Class->Script.Num();
	for( TFieldIterator<UFunction> It(Class); It; ++It )
		Bytes += It->Script.Num();
	return Bytes;
}

static void TestScripts()
{
	debugf( "SelfTest: --- Script classes ---" );
	UClass* SP = FindObject<UClass>( ANY_PACKAGE, "ScriptedPawn" );
	TestResult( "ScriptedPawn loaded with bytecode", SP && ClassCodeBytes(SP)>0 );

	UClass* Bots = FindObject<UClass>( ANY_PACKAGE, "Bots" );
	TestResult( "class Bots loaded", Bots!=NULL );
	if( Bots )
	{
		UBOOL HasModern=0, HasBelief=0, HasAxes=0, HasKeyItems=0;
		for( TFieldIterator<UProperty> It(Bots); It; ++It )
		{
			if( appStricmp(It->GetName(),"bModernAI" )==0 ) HasModern=1;
			if( appStricmp(It->GetName(),"BeliefPos" )==0 ) HasBelief=1;
			if( appStricmp(It->GetName(),"AimSkill"  )==0 ) HasAxes=1;
			if( appStricmp(It->GetName(),"KeyItem"   )==0 ) HasKeyItems=1;
		}
		TestResult( "Bots modern-AI Phase 1 (belief)",    HasModern && HasBelief );
		TestResult( "Bots modern-AI Phase 3 (axes)",      HasAxes );
		TestResult( "Bots modern-AI Phase 4 (key items)", HasKeyItems );
	}
	UClass* DM = FindObject<UClass>( ANY_PACKAGE, "DeathMatchGame" );
	TestResult( "DeathMatchGame loaded with bytecode", DM && ClassCodeBytes(DM)>0 );
}

/*-----------------------------------------------------------------------------
	WAV parser: every retail sound must parse under the hardened reader.
-----------------------------------------------------------------------------*/

static void TestWavs()
{
	debugf( "SelfTest: --- WAV parser (all retail sounds) ---" );
	INT Total=0, Ok=0;
	for( TObjectIterator<USound> It; It; ++It )
	{
		USound* S = *It;
		if( S->Data.Num()==0 )
			continue;
		Total++;
		FWaveModInfo Info;
		if( Info.ReadWaveInfo( S->Data ) )
			Ok++;
		else
			debugf( "SelfTest: WAV parse failed: %s", S->GetFullName() );
	}
	char Detail[64];
	appSprintf( Detail, "(%i/%i)", Ok, Total );
	TestResult( "parse all retail sounds", Total>0 && Ok==Total, Detail );
}

/*-----------------------------------------------------------------------------
	Driver.
-----------------------------------------------------------------------------*/

INT RunSelfTests()
{
	guard(RunSelfTests);
	debugf( "SelfTest: ============ x64 port subsystem self-tests ============" );

	TestCore();
	TestLayout();
	TestLoader();
	TestObjectPins();
	TestScripts();
	TestWavs();

	debugf( "SelfTest: ============ %s: %i passed, %i failed ============",
		GFailed ? "FAILED" : "ALL PASSED", GPassed, GFailed );
	return GFailed;
	unguard;
}
