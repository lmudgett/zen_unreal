/*=============================================================================
	EdGui.cpp: Dear ImGui editor frame (cross-platform port, Phase 5).

	The portable UnrealEd shell replacing the retired original frontend.
	Runs in-process on the SDL window: SDLDrv's GSDLEventHook feeds ImGui
	first (UI panels take input focus ahead of the engine), and OpenGLDrv's
	GGLPostRenderHook draws the UI over the finished frame before the swap.
	The editor engine is driven exactly the way the old shell drove it — by
	UEditorEngine::Exec strings (CAMERA/MAP/MODE/BRUSH/TRANSACTION/...); the
	exec/topic/EdCallback contract is unchanged, only the transport (Win32
	window messages) is gone.

	Milestone scope: single Standard3V perspective viewport, main menu
	(map load/save, undo/redo, edit modes, rebuild), log panel (tees off
	Core's GLogHook), and an exec console. Ortho quad views, browsers, and
	property panels come next.

	Author: Len Mudgett
=============================================================================*/

#include "Engine.h"
#include "EdGui.h"
#include "../../SDLDrv/Inc/SDLDrvHooks.h"
#include "../../OpenGLDrv/Inc/OpenGLDrvHooks.h"
// Editor.h: declarations only (topic Get/Set are virtuals reached through the
// engine object's vtable, so no Editor import-library dependency).
#ifndef EDITOR_API
	#define EDITOR_API DLL_IMPORT
#endif
#include "../../Editor/Src/Editor.h"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_internal.h"	// ImGuiItemFlags_MixedValue (tri-state checkboxes)
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"

#include <vector>
#include <string>
#include <stdlib.h>	// strtoul (topic-value parsing)

/*-----------------------------------------------------------------------------
	State.
-----------------------------------------------------------------------------*/

static UEngine*	GEdEngine		= NULL;
static UBOOL	GInstalled		= 0;
static UBOOL	GImGuiReady		= 0;
static SDL_Window* GImGuiWindow	= NULL;	// the primary (3D) window ImGui lives on
static FOutputDevice* GPrevLogHook = NULL;

// Panel visibility.
static bool		GShowToolbar	= true;
static bool		GShowLog		= true;
static bool		GShowConsole	= true;
static bool		GShowProps		= true;
static bool		GShowClasses	= true;
static bool		GShowDemo		= false;

// Current actor class (mirrors the engine's SETCURRENTCLASS state).
static char		GCurrentClass[NAME_SIZE] = "Light";

// Texture browser state.
static bool		GShowTextures	= true;
static char		GCurTexture[128] = "None";
static bool		GCurTexDirty	= true;
static std::vector<std::string> GTexPackages;
static bool		GTexPkgsDirty	= true;

// Surface (BSP poly) properties panel state. The flag masks are re-read from
// the "Polys" topic whenever the surface selection changes (EDC_SelPolyChange)
// or the panel issues a POLY command.
static bool		GShowSurface	= true;
static bool		GSurfDirty		= true;
static int		GSurfNum		= 0;	// number of selected surfaces
static DWORD	GSurfSetFlags	= 0;	// flags set on ALL selected surfaces
static DWORD	GSurfClearFlags	= 0;	// flags clear on ALL selected surfaces
static int		GSurfPanStep	= 16;	// texel step for the pan nudge buttons

// Pending right-click context menu (EDC_RtClick* callback code, 0 = none).
static DWORD	GPendingPopup	= 0;

// Selection state, refreshed when the editor fires EDC_SelChange /
// EDC_MapChange through GEdCallbackHook (the in-process replacement for the
// legacy PostMessage transport).
static bool						GSelDirty = true;
static std::vector<AActor*>		GSelected;

// Console.
static char		GConsoleInput[512] = "";
static bool		GConsoleFocus	= false;

// Map load/save popups.
static char		GMapPath[256]	= "..\\content\\Maps\\";
static int		GMapDialog		= 0;	// 0=none 1=load 2=save

// Edit mode, tracked shell-side like the VB frontend did (the engine has no
// topic that reports Mode back).
static int		GModeIndex		= 0;
struct FEdGuiMode { const char* Label; const char* Token; };
static const FEdGuiMode GModes[] =
{
	{ "Camera Move",	"CAMERAMOVE"	},
	{ "Camera Zoom",	"CAMERAZOOM"	},
	{ "Brush Rotate",	"BRUSHROTATE"	},
	{ "Brush Sheer",	"BRUSHSHEER"	},
	{ "Brush Scale",	"BRUSHSCALE"	},
	{ "Brush Stretch",	"BRUSHSTRETCH"	},
	{ "Brush Snap",		"BRUSHSNAP"		},
	{ "Texture Pan",	"TEXTUREPAN"	},
	{ "Texture Rotate",	"TEXTUREROTATE"	},
	{ "Texture Scale",	"TEXTURESCALE"	},
};

/*-----------------------------------------------------------------------------
	Log capture.
-----------------------------------------------------------------------------*/

// Chained onto Core's GLogHook: every engine log line lands in the panel and
// still flows to the previous hook (the launcher's stdout tee).
class FEdGuiLog : public FOutputDevice
{
public:
	std::vector<std::string> Lines;
	bool ScrollToBottom;
	FEdGuiLog() : ScrollToBottom(false) {}
	void WriteBinary( const void* Data, INT Length, EName Event )
	{
		char Line[4096];
		appSprintf( Line, "%s: %s", *FName(Event), (const char*)Data );
		if( Lines.size() > 5000 )
			Lines.erase( Lines.begin(), Lines.begin()+1000 );
		Lines.push_back( Line );
		ScrollToBottom = true;
		if( GPrevLogHook )
			GPrevLogHook->WriteBinary( Data, Length, Event );
	}
};
static FEdGuiLog GEdLog;

static UBOOL EdGuiCommand( const char* Cmd );

// Run an editor exec, echoing it and its output into the log panel.
// Commands starting with "EDGUI." are handled by the shell itself (brush
// builders etc.) so they are usable from the console and EdGuiBoot.txt.
static void EdExec( const char* Cmd )
{
	GEdLog.Logf( "> %s", Cmd );
	if( appStrnicmp( Cmd, "EDGUI.", 6 )==0 )
	{
		if( !EdGuiCommand( Cmd+6 ) )
			GEdLog.Logf( "EdGui: unknown command '%s'", Cmd );
		return;
	}
	if( GEdEngine )
		GEdEngine->Exec( Cmd, &GEdLog );
}

/*-----------------------------------------------------------------------------
	Brush builders.
-----------------------------------------------------------------------------*/

// Emit one polygon of a BRUSH SET command (normals/texture axes are computed
// by FPoly::Finalize from the winding: cross of successive edges = outward).
static void EmitPoly( std::string& S, const FVector* V, INT N )
{
	S += "Begin Polygon\r\n";
	for( INT i=0; i<N; i++ )
	{
		char L[128];
		appSprintf( L, "Vertex %f,%f,%f\r\n", V[i].X, V[i].Y, V[i].Z );
		S += L;
	}
	S += "End Polygon\r\n";
}

static void BuildBrushCube( FLOAT SX, FLOAT SY, FLOAT SZ )
{
	FLOAT HX=SX*0.5f, HY=SY*0.5f, HZ=SZ*0.5f;
	static const FLOAT F[6][9] =
	{	// Nx,Ny,Nz,  Ux,Uy,Uz,  Vx,Vy,Vz  with U x V = N (outward)
		{ -1,0,0,  0,1,0,  0,0,-1 },
		{ +1,0,0,  0,1,0,  0,0,+1 },
		{ 0,-1,0,  1,0,0,  0,0,+1 },
		{ 0,+1,0,  1,0,0,  0,0,-1 },
		{ 0,0,-1,  1,0,0,  0,-1,0 },
		{ 0,0,+1,  1,0,0,  0,+1,0 },
	};
	std::string S = "BRUSH SET\r\nBegin PolyList\r\n";
	for( INT f=0; f<6; f++ )
	{
		FVector N(F[f][0],F[f][1],F[f][2]), U(F[f][3],F[f][4],F[f][5]), Vv(F[f][6],F[f][7],F[f][8]);
		FVector C( N.X*HX, N.Y*HY, N.Z*HZ );
		FLOAT HU = Abs(U.X)*HX + Abs(U.Y)*HY + Abs(U.Z)*HZ;
		FLOAT HV = Abs(Vv.X)*HX + Abs(Vv.Y)*HY + Abs(Vv.Z)*HZ;
		FVector P[4] = { C-U*HU-Vv*HV, C+U*HU-Vv*HV, C+U*HU+Vv*HV, C-U*HU+Vv*HV };
		EmitPoly( S, P, 4 );
	}
	S += "End PolyList\r\n";
	EdExec( S.c_str() );
}

static void BuildBrushSheet( FLOAT W, FLOAT H )
{
	FVector P[4] =
	{
		FVector(-W/2,0,-H/2), FVector(+W/2,0,-H/2),
		FVector(+W/2,0,+H/2), FVector(-W/2,0,+H/2)
	};
	std::string S = "BRUSH SET\r\nBegin PolyList\r\n";
	EmitPoly( S, P, 4 );
	S += "End PolyList\r\n";
	EdExec( S.c_str() );
}

static void BuildBrushCylinder( FLOAT R, FLOAT H, INT Sides )
{
	Sides = Clamp( Sides, 3, 32 );
	FLOAT HZ = H*0.5f;
	std::string S = "BRUSH SET\r\nBegin PolyList\r\n";
	// Side quads (outward normals with increasing angle winding).
	for( INT i=0; i<Sides; i++ )
	{
		FLOAT A0 = 2.f*PI*i/Sides, A1 = 2.f*PI*(i+1)/Sides;
		FVector P[4] =
		{
			FVector(R*appCos(A0),R*appSin(A0),-HZ), FVector(R*appCos(A1),R*appSin(A1),-HZ),
			FVector(R*appCos(A1),R*appSin(A1),+HZ), FVector(R*appCos(A0),R*appSin(A0),+HZ)
		};
		EmitPoly( S, P, 4 );
	}
	// Caps.
	{
		FVector Top[32], Bottom[32];
		for( INT i=0; i<Sides; i++ )
		{
			FLOAT A = 2.f*PI*i/Sides;
			Top[i]            = FVector(R*appCos(A),R*appSin(A),+HZ);
			Bottom[Sides-1-i] = FVector(R*appCos(A),R*appSin(A),-HZ);
		}
		EmitPoly( S, Top, Sides );
		EmitPoly( S, Bottom, Sides );
	}
	S += "End PolyList\r\n";
	EdExec( S.c_str() );
}

// EDGUI.* console commands (also the automated-test surface).
static UBOOL EdGuiCommand( const char* Cmd )
{
	const char* Str = Cmd;
	if( ParseCommand( &Str, "CUBE" ) )
	{
		FLOAT S=256.f; Parse( Str, "SIZE=", S );
		FLOAT X=S,Y=S,Z=S; Parse(Str,"X=",X); Parse(Str,"Y=",Y); Parse(Str,"Z=",Z);
		BuildBrushCube( X, Y, Z );
		return 1;
	}
	else if( ParseCommand( &Str, "SHEET" ) )
	{
		FLOAT W=256.f, H=256.f; Parse(Str,"W=",W); Parse(Str,"H=",H);
		BuildBrushSheet( W, H );
		return 1;
	}
	else if( ParseCommand( &Str, "CYLINDER" ) )
	{
		FLOAT R=128.f, H=256.f; INT N=8;
		Parse(Str,"R=",R); Parse(Str,"H=",H); Parse(Str,"SIDES=",N);
		BuildBrushCylinder( R, H, N );
		return 1;
	}
	else if( ParseCommand( &Str, "MOVERINFO" ) )
	{
		// Report the collision database of every mover brush in the loaded
		// level. A mover renders from Polys but blocks from Nodes/LeafHulls
		// (UModel::PointCheck -> FBoxCheckInfo::SetupHulls), so a mover whose
		// saved model has nodes but no leaf hulls -- or a stale/empty
		// BoundingBox, which is what FCollisionHash hashes it by -- is visible
		// and walk-through. NAME= limits the report to one mover.
		char Only[NAME_SIZE]="";
		Parse( Str, "NAME=", Only, ARRAY_COUNT(Only) );
		UEditorEngine* Ed = (UEditorEngine*)GEdEngine;
		if( !Ed || !Ed->Level )
		{
			GEdLog.Logf( "MOVERINFO: no level loaded" );
			return 1;
		}
		INT Count=0;
		for( INT i=0; i<Ed->Level->Num(); i++ )
		{
			AMover* M = Cast<AMover>( Ed->Level->Element(i) );
			if( !M )
				continue;
			if( Only[0] && appStricmp( M->GetName(), Only )!=0 )
				continue;
			UModel* B = M->Brush;
			Count++;
			if( !B )
			{
				GEdLog.Logf( "MOVERINFO %-9s tag=%-10s NO BRUSH", M->GetName(), *M->Tag );
				continue;
			}
			FBox Bb = B->BoundingBox;
			GEdLog.Logf
			(
				"MOVERINFO %-9s tag=%-10s polys=%-4i nodes=%-4i hulls=%-5i leaves=%-4i bounds=%-4i "
				"col=%i blkA=%i blkP=%i box=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) valid=%i",
				M->GetName(), *M->Tag,
				B->Polys ? B->Polys->Num() : -1,
				B->Nodes ? B->Nodes->Num() : -1,
				B->LeafHulls.Num(), B->Leaves.Num(), B->Bounds.Num(),
				M->bCollideActors, M->bBlockActors, M->bBlockPlayers,
				Bb.Min.X, Bb.Min.Y, Bb.Min.Z, Bb.Max.X, Bb.Max.Y, Bb.Max.Z,
				Bb.IsValid
			);
			GEdLog.Logf
			(
				"          %-9s rootOutside=%i linked=%i moverLink=%u",
				M->GetName(), B->RootOutside, B->Linked, B->MoverLink
			);
		}
		GEdLog.Logf( "MOVERINFO: %i mover(s)", Count );
		return 1;
	}
	else if( ParseCommand( &Str, "MOVERPROBE" ) )
	{
		// Ask a mover's brush the same question the running game asks it:
		// UModel::PointCheck with a player-sized box, sampled over the volume
		// the mover occupies in the world. PointCheck returns "outside", so a
		// solid mover must report 0 (blocked) for interior samples -- a mover
		// that reports 1 everywhere renders normally and is walked through.
		char Want[NAME_SIZE]="";
		Parse( Str, "NAME=", Want, ARRAY_COUNT(Want) );
		FLOAT ER=17.f, EH=39.f;	// default human pawn extent
		Parse( Str, "R=", ER ); Parse( Str, "H=", EH );
		UEditorEngine* Ed = (UEditorEngine*)GEdEngine;
		if( !Ed || !Ed->Level )
		{
			GEdLog.Logf( "MOVERPROBE: no level loaded" );
			return 1;
		}
		for( INT i=0; i<Ed->Level->Num(); i++ )
		{
			AMover* M = Cast<AMover>( Ed->Level->Element(i) );
			if( !M || !M->Brush )
				continue;
			if( Want[0] && appStricmp( M->GetName(), Want )!=0 )
				continue;
			FBox W = M->Brush->GetCollisionBoundingBox( M );
			FVector Ext( ER, ER, EH );
			INT Box=0, Pt=0, N=0;
			const INT S=5;
			for( INT a=0; a<S; a++ )
			for( INT b=0; b<S; b++ )
			for( INT c=0; c<S; c++ )
			{
				FVector P
				(
					W.Min.X + (W.Max.X-W.Min.X)*(a+0.5f)/S,
					W.Min.Y + (W.Max.Y-W.Min.Y)*(b+0.5f)/S,
					W.Min.Z + (W.Max.Z-W.Min.Z)*(c+0.5f)/S
				);
				FCheckResult H1(1.f), H2(1.f);
				if( !M->Brush->PointCheck( H1, M, P, Ext, 0 ) )
					Box++;
				if( !M->Brush->PointCheck( H2, M, P, FVector(0,0,0), 0 ) )
					Pt++;
				N++;
			}
			GEdLog.Logf
			(
				"MOVERPROBE %-9s tag=%-10s world=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) "
				"blocked box=%i/%i point=%i/%i%s",
				M->GetName(), *M->Tag,
				W.Min.X, W.Min.Y, W.Min.Z, W.Max.X, W.Max.Y, W.Max.Z,
				Box, N, Pt, N, Box ? "" : "   <== NEVER BLOCKS"
			);
		}
		return 1;
	}
	return 0;
}

/*-----------------------------------------------------------------------------
	Selection and properties.
-----------------------------------------------------------------------------*/

// EDC_* codes the panel reacts to (values are fixed ABI, Editor.h).
enum
{
	EDGUI_EDC_SelPolyChange=20, EDGUI_EDC_SelChange=21,
	EDGUI_EDC_RtClickTexture=23, EDGUI_EDC_RtClickPoly=24, EDGUI_EDC_RtClickActor=25,
	EDGUI_EDC_RtClickWindow=26, EDGUI_EDC_RtClickWindowCanAdd=27,
	EDGUI_EDC_MapChange=42
};

enum { EDGUI_EDC_CurTexChange=10 };

static void EdGuiEdCallback( DWORD Code, UBOOL Send )
{
	if( Code==EDGUI_EDC_SelChange || Code==EDGUI_EDC_MapChange || Code==EDGUI_EDC_SelPolyChange )
		GSelDirty = true;
	if( Code==EDGUI_EDC_SelPolyChange || Code==EDGUI_EDC_MapChange )
		GSurfDirty = true;
	if( Code==EDGUI_EDC_CurTexChange )
		GCurTexDirty = true;
	if( Code>=EDGUI_EDC_RtClickTexture && Code<=EDGUI_EDC_RtClickWindowCanAdd )
		GPendingPopup = Code;
}

static void RefreshSelection()
{
	GSelected.clear();
	for( TObjectIterator<AActor> It; It; ++It )
		if( It->bSelected && !It->bDeleteMe )
			GSelected.push_back( *It );
	GSelDirty = false;
}

// Commit a property-value string to every selected actor.
static void CommitProperty( UProperty* Prop, INT Elem, const char* Value )
{
	for( size_t i=0; i<GSelected.size(); i++ )
	{
		AActor* A = GSelected[i];
		if( !Prop->ImportText( Value, (BYTE*)A + Prop->Offset + Elem*Prop->GetElementSize(), 1 ) )
		{
			GEdLog.Logf( "EdGui: could not parse '%s' for %s", Value, Prop->GetName() );
			return;
		}
		A->PostEditChange();
	}
	GEdLog.Logf( "EdGui: set %s = %s on %i actor(s)", Prop->GetName(), Value, (INT)GSelected.size() );
}

// The actor properties panel: reflection-driven, grouped by UnrealScript
// category, edits applied to the whole selection (classic UnrealEd multi-
// edit). TODO: transaction (undo) bracketing around edits.
static void BuildPropertiesPanel()
{
	ImGui::SetNextWindowSize( ImVec2(360,480), ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowPos( ImVec2(ImGui::GetIO().DisplaySize.x-368, 32), ImGuiCond_FirstUseEver );
	if( !ImGui::Begin("Properties", &GShowProps) )
	{
		ImGui::End();
		return;
	}
	if( GSelected.empty() )
	{
		ImGui::TextDisabled( "Nothing selected" );
		ImGui::End();
		return;
	}
	AActor* Actor = GSelected[0];
	ImGui::Text( "%i selected  (%s%s)", (INT)GSelected.size(), Actor->GetClass()->GetName(),
		GSelected.size()>1 ? ", editing all" : "" );
	ImGui::Separator();

	// One InputText is "live" at a time; it keeps its own buffer so the
	// per-frame reexport below doesn't clobber typing.
	static const void*	ActiveRow = NULL;
	static char			ActiveBuf[1024];

	// Walk categories in first-encounter order.
	std::vector<FName>	Cats;
	for( TFieldIterator<UProperty> It( Actor->GetClass() ); It; ++It )
	{
		if( !(It->PropertyFlags & CPF_Edit) )
			continue;
		FName Cat = It->Category;
		bool Seen = false;
		for( size_t i=0; i<Cats.size(); i++ )
			if( Cats[i]==Cat ) { Seen = true; break; }
		if( !Seen )
			Cats.push_back( Cat );
	}
	for( size_t c=0; c<Cats.size(); c++ )
	{
		const char* CatName = Cats[c]!=NAME_None ? *Cats[c] : "Misc";
		if( !ImGui::CollapsingHeader( CatName, ImGuiTreeNodeFlags_DefaultOpen ) )
			continue;
		for( TFieldIterator<UProperty> It( Actor->GetClass() ); It; ++It )
		{
			UProperty* Prop = *It;
			if( !(Prop->PropertyFlags & CPF_Edit) || Prop->Category!=Cats[c] )
				continue;
			UBOOL Locked = (Prop->PropertyFlags & CPF_EditConst)!=0;
			for( INT Elem=0; Elem<Prop->ArrayDim; Elem++ )
			{
				char Label[128];
				if( Prop->ArrayDim>1 )
					appSprintf( Label, "%s[%i]", Prop->GetName(), Elem );
				else
					appSprintf( Label, "%s", Prop->GetName() );

				char Value[1024]="";
				Prop->ExportText( Elem, Value, (BYTE*)Actor, (BYTE*)Actor, 1 );

				const void* RowId = (const BYTE*)Prop + Elem;	// unique per row
				ImGui::PushID( (const void*)RowId );
				ImGui::TextUnformatted( Label );
				ImGui::SameLine( 170 );
				ImGui::SetNextItemWidth( -8 );
				if( Locked )
					ImGui::TextDisabled( "%s", Value );
				else if( Prop->IsA(UBoolProperty::StaticClass) && Prop->ArrayDim==1 )
				{
					bool bVal = appStricmp( Value, "True" )==0;
					if( ImGui::Checkbox( "##v", &bVal ) )
						CommitProperty( Prop, Elem, bVal ? "True" : "False" );
				}
				else
				{
					char* Buf   = (RowId==ActiveRow) ? ActiveBuf : Value;
					INT   Size  = (RowId==ActiveRow) ? (INT)sizeof(ActiveBuf) : (INT)sizeof(Value);
					bool Commit = ImGui::InputText( "##v", Buf, Size, ImGuiInputTextFlags_EnterReturnsTrue );
					if( ImGui::IsItemActivated() )
					{
						ActiveRow = RowId;
						appStrcpy( ActiveBuf, Value );
					}
					if( RowId==ActiveRow )
					{
						Commit |= ImGui::IsItemDeactivatedAfterEdit();
						if( Commit )
							CommitProperty( Prop, Elem, ActiveBuf );
						if( !ImGui::IsItemActive() )
							ActiveRow = NULL;
					}
				}
				ImGui::PopID();
			}
		}
	}
	ImGui::End();
}

/*-----------------------------------------------------------------------------
	Texture browser.
-----------------------------------------------------------------------------*/

// Tiny output device that captures topic Get results.
class FEdGuiStrOut : public FOutputDevice
{
public:
	char Str[256];
	FEdGuiStrOut() { Str[0]=0; }
	void WriteBinary( const void* Data, INT Length, EName Event )
	{
		appStrncpy( Str, (const char*)Data, ARRAY_COUNT(Str) );
	}
};

static void RefreshCurTexture()
{
	FEdGuiStrOut Out;
	((UEditorEngine*)GEdEngine)->Get( "Ed", "CURTEX", Out );
	appStrncpy( GCurTexture, Out.Str[0] ? Out.Str : "None", ARRAY_COUNT(GCurTexture) );
	GCurTexDirty = false;
}

static void RefreshTexPackages()
{
	GTexPackages.clear();
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		UObject* Top = *It;
		while( Top->GetParent() )
			Top = Top->GetParent();
		bool Seen = false;
		for( size_t i=0; i<GTexPackages.size(); i++ )
			if( GTexPackages[i]==Top->GetName() ) { Seen = true; break; }
		if( !Seen )
			GTexPackages.push_back( Top->GetName() );
	}
	GTexPkgsDirty = false;
}

static void BuildTexturePanel()
{
	ImGui::SetNextWindowSize( ImVec2(300,360), ImGuiCond_FirstUseEver );
	if( ImGui::Begin("Textures", &GShowTextures) )
	{
		if( GCurTexDirty )
			RefreshCurTexture();
		if( GTexPkgsDirty )
			RefreshTexPackages();
		ImGui::Text( "Current: %s", GCurTexture );
		ImGui::Separator();

		ImGui::TextDisabled( "Browse package (opens browser window):" );
		ImGui::BeginChild( "pkgs", ImVec2(0,140), ImGuiChildFlags_Borders );
		for( size_t i=0; i<GTexPackages.size(); i++ )
			if( ImGui::Selectable( GTexPackages[i].c_str() ) )
			{
				char Cmd[320];
				appSprintf( Cmd,
					"CAMERA OPEN NAME=TexBrowser X=1460 Y=60 XR=420 YR=680 FLAGS=%i REN=%i MISC1=64 MISC2=0 PACKAGE=%s",
					SHOW_RealTime|SHOW_NoButtons, REN_TexBrowser, GTexPackages[i].c_str() );
				EdExec( Cmd );
			}
		ImGui::EndChild();

		if( ImGui::CollapsingHeader( "Load texture package (.utx)" ) )
		{
			static std::vector<std::string> UtxFiles;
			static bool UtxScanned = false;
			if( !UtxScanned )
			{
				UtxFiles.clear();
				TArray<FString> Found = appFindFiles( "..\\content\\Textures\\*.utx" );
				for( INT i=0; i<Found.Num(); i++ )
					UtxFiles.push_back( *Found(i) );
				UtxScanned = true;
			}
			ImGui::BeginChild( "utx", ImVec2(0,140), ImGuiChildFlags_Borders );
			for( size_t i=0; i<UtxFiles.size(); i++ )
				if( ImGui::Selectable( UtxFiles[i].c_str() ) )
				{
					char Cmd[512];
					appSprintf( Cmd, "OBJ LOAD FILE=..\\content\\Textures\\%s", UtxFiles[i].c_str() );
					EdExec( Cmd );
					GTexPkgsDirty = true;
				}
			ImGui::EndChild();
		}
	}
	ImGui::End();
}

/*-----------------------------------------------------------------------------
	Surface (BSP poly) properties.

	The faithful port of UnrealEd's Surface Properties dialog. All edits go
	through UEditorEngine::Exec POLY commands (each self-brackets a
	transaction, so undo/redo works); current flag state is read back through
	the "Polys" topic (SelectedSetFlags = flags set on every selected surface,
	SelectedClearFlags = flags clear on every selected surface — the two give
	a tri-state per flag).
-----------------------------------------------------------------------------*/

// Read an unsigned decimal value from an editor topic item.
static DWORD GetTopicUInt( const char* Topic, const char* Item )
{
	FEdGuiStrOut Out;
	((UEditorEngine*)GEdEngine)->Get( Topic, Item, Out );
	return Out.Str[0] ? (DWORD)strtoul( Out.Str, NULL, 10 ) : 0;
}

static void RefreshSurface()
{
	GSurfNum        = (INT)GetTopicUInt( "Polys", "NumSelected" );
	GSurfSetFlags   = GetTopicUInt( "Polys", "SelectedSetFlags" );
	GSurfClearFlags = GetTopicUInt( "Polys", "SelectedClearFlags" );
	GSurfDirty      = false;
}

// Issue a POLY command and force a flag re-read next frame. GET_VARARGS must
// expand va_start in this function (x64 varargs travel in registers).
static void SurfCmd( const char* Fmt, ... )
{
	char Buf[256];
	GET_VARARGS( Buf, Fmt );
	EdExec( Buf );
	GSurfDirty = true;
}

// User-editable surface flags (PF_NoEdit / internal bits excluded).
struct FSurfFlagDef { const char* Label; DWORD Flag; };
static const FSurfFlagDef GSurfFlagDefs[] =
{
	{ "Invisible",		PF_Invisible	},
	{ "Masked",			PF_Masked		},
	{ "Translucent",	PF_Translucent	},
	{ "Modulated",		PF_Modulated	},
	{ "Two Sided",		PF_TwoSided		},
	{ "Unlit",			PF_Unlit		},
	{ "Environment",	PF_Environment	},
	{ "Fake Backdrop",	PF_FakeBackdrop	},
	{ "No Smooth",		PF_NoSmooth		},
	{ "Special Lit",	PF_SpecialLit	},
	{ "Small Wavy",		PF_SmallWavy	},
	{ "Auto U Pan",		PF_AutoUPan		},
	{ "Auto V Pan",		PF_AutoVPan		},
	{ "Not Solid",		PF_NotSolid		},
	{ "Semisolid",		PF_Semisolid	},
	{ "Portal",			PF_Portal		},
	{ "Mirror",			PF_Mirrored		},
};

static void BuildSurfacePanel()
{
	ImGui::SetNextWindowSize( ImVec2(320,520), ImGuiCond_FirstUseEver );
	if( !ImGui::Begin("Surface", &GShowSurface) )
	{
		ImGui::End();
		return;
	}
	if( GSurfDirty )
		RefreshSurface();

	if( GSurfNum==0 )
	{
		ImGui::TextDisabled( "No surface selected" );
		ImGui::TextDisabled( "(click a BSP surface in a viewport)" );
		ImGui::End();
		return;
	}
	ImGui::Text( "%i surface%s selected", GSurfNum, GSurfNum==1?"":"s" );
	ImGui::Text( "Texture: %s", GCurTexture );
	ImGui::SameLine();
	if( ImGui::SmallButton("Apply Current") )
		SurfCmd( "POLY SETTEXTURE" );

	// Numeric readout of the first selected surface. No topic exposes pan or
	// scale, so read them straight off the model: scale is encoded as the
	// length of the texture axis vectors (see polyTexScale).
	UEditorEngine* Ed = (UEditorEngine*)GEdEngine;
	if( Ed->Level && Ed->Level->Model )
	{
		UModel* M = Ed->Level->Model;
		for( INT i=0; i<M->Surfs->Num(); i++ )
		{
			FBspSurf& Sf = M->Surfs->Element(i);
			if( Sf.PolyFlags & PF_Selected )
			{
				FLOAT US = M->Vectors->Element(Sf.vTextureU).Size();
				FLOAT VS = M->Vectors->Element(Sf.vTextureV).Size();
				ImGui::TextDisabled( "Pan %i,%i   Scale %.3g,%.3g", Sf.PanU, Sf.PanV, US, VS );
				break;
			}
		}
	}
	ImGui::Separator();

	// --- Pan ------------------------------------------------------------
	if( ImGui::CollapsingHeader( "Pan", ImGuiTreeNodeFlags_DefaultOpen ) )
	{
		ImGui::SetNextItemWidth( 90 );
		ImGui::InputInt( "Step (texels)", &GSurfPanStep );
		GSurfPanStep = Clamp( GSurfPanStep, 1, 1024 );
		INT S = GSurfPanStep;
		if( ImGui::Button("U-") )   SurfCmd( "POLY TEXPAN U=%i", -S );
		ImGui::SameLine(); if( ImGui::Button("U+") ) SurfCmd( "POLY TEXPAN U=%i", S );
		ImGui::SameLine(); if( ImGui::Button("V-") ) SurfCmd( "POLY TEXPAN V=%i", -S );
		ImGui::SameLine(); if( ImGui::Button("V+") ) SurfCmd( "POLY TEXPAN V=%i", S );
		ImGui::SameLine(); if( ImGui::Button("Reset##pan") ) SurfCmd( "POLY TEXPAN RESET" );
	}

	// --- Scale ----------------------------------------------------------
	if( ImGui::CollapsingHeader( "Scale", ImGuiTreeNodeFlags_DefaultOpen ) )
	{
		static const FLOAT Scales[] = { 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f };
		static int Sel = 2;
		ImGui::TextDisabled( "Absolute (higher = more tiling)" );
		ImGui::SetNextItemWidth( 90 );
		const char* Preview = Sel==0?"0.25":Sel==1?"0.5":Sel==2?"1":Sel==3?"2":Sel==4?"4":"8";
		if( ImGui::BeginCombo( "##scale", Preview ) )
		{
			const char* Names[] = { "0.25","0.5","1","2","4","8" };
			for( int i=0; i<(int)ARRAY_COUNT(Scales); i++ )
				if( ImGui::Selectable( Names[i], Sel==i ) )
					Sel = i;
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if( ImGui::Button("Apply##scale") )
			SurfCmd( "POLY TEXSCALE UU=%f VV=%f", Scales[Sel], Scales[Sel] );
		if( ImGui::Button("Tile x2") )   SurfCmd( "POLY TEXMULT UU=2 VV=2" );
		ImGui::SameLine(); if( ImGui::Button("Tile /2") ) SurfCmd( "POLY TEXMULT UU=0.5 VV=0.5" );
	}

	// --- Rotate ---------------------------------------------------------
	if( ImGui::CollapsingHeader( "Rotate", ImGuiTreeNodeFlags_DefaultOpen ) )
	{
		// Relative texture-matrix rotation via TEXMULT (cos/-sin/sin/cos).
		if( ImGui::Button("45\xC2\xB0 CW") )
			SurfCmd( "POLY TEXMULT UU=%f UV=%f VU=%f VV=%f", 0.70711f, 0.70711f, -0.70711f, 0.70711f );
		ImGui::SameLine(); if( ImGui::Button("90\xC2\xB0 CW") )
			SurfCmd( "POLY TEXMULT UU=0 UV=1 VU=-1 VV=0" );
		ImGui::SameLine(); if( ImGui::Button("45\xC2\xB0 CCW") )
			SurfCmd( "POLY TEXMULT UU=%f UV=%f VU=%f VV=%f", 0.70711f, -0.70711f, 0.70711f, 0.70711f );
	}

	// --- Alignment ------------------------------------------------------
	if( ImGui::CollapsingHeader( "Alignment", ImGuiTreeNodeFlags_DefaultOpen ) )
	{
		if( ImGui::Button("Default") )    SurfCmd( "POLY TEXALIGN DEFAULT" );
		ImGui::SameLine(); if( ImGui::Button("Floor/Ceil") ) SurfCmd( "POLY TEXALIGN FLOOR" );
		if( ImGui::Button("Wall Dir") )   SurfCmd( "POLY TEXALIGN WALLDIR" );
		ImGui::SameLine(); if( ImGui::Button("Wall Pan") )   SurfCmd( "POLY TEXALIGN WALLPAN" );
		ImGui::SameLine(); if( ImGui::Button("One Tile") )   SurfCmd( "POLY TEXALIGN ONETILE" );
	}

	// --- Flags (tri-state across the selection) -------------------------
	if( ImGui::CollapsingHeader( "Flags", ImGuiTreeNodeFlags_DefaultOpen ) )
	{
		if( ImGui::BeginTable( "surfflags", 2, ImGuiTableFlags_SizingStretchSame ) )
		{
			for( int i=0; i<(int)ARRAY_COUNT(GSurfFlagDefs); i++ )
			{
				const FSurfFlagDef& F = GSurfFlagDefs[i];
				bool All  = (GSurfSetFlags   & F.Flag)!=0;
				bool None = (GSurfClearFlags & F.Flag)!=0;
				bool Mixed = !All && !None;
				ImGui::TableNextColumn();
				if( Mixed )
					ImGui::PushItemFlag( ImGuiItemFlags_MixedValue, true );
				bool V = All;
				if( ImGui::Checkbox( F.Label, &V ) )
					SurfCmd( V ? "POLY SET SETFLAGS=%u" : "POLY SET CLEARFLAGS=%u", F.Flag );
				if( Mixed )
					ImGui::PopItemFlag();
			}
			ImGui::EndTable();
		}
	}

	// --- Selection helpers ---------------------------------------------
	if( ImGui::CollapsingHeader( "Select" ) )
	{
		if( ImGui::Button("Matching Texture") ) SurfCmd( "POLY SELECT MATCHING TEXTURE" );
		ImGui::SameLine(); if( ImGui::Button("Matching Groups") ) SurfCmd( "POLY SELECT MATCHING GROUPS" );
		if( ImGui::Button("Adjacent Coplanar") ) SurfCmd( "POLY SELECT ADJACENT COPLANARS" );
		ImGui::SameLine(); if( ImGui::Button("Adjacent All") ) SurfCmd( "POLY SELECT ADJACENT ALL" );
		if( ImGui::Button("Adjacent Walls") ) SurfCmd( "POLY SELECT ADJACENT WALLS" );
		ImGui::SameLine(); if( ImGui::Button("Adjacent Floors") ) SurfCmd( "POLY SELECT ADJACENT FLOORS" );
		if( ImGui::Button("Reverse") ) SurfCmd( "POLY SELECT REVERSE" );
		ImGui::SameLine(); if( ImGui::Button("All") ) SurfCmd( "POLY SELECT ALL" );
		ImGui::SameLine(); if( ImGui::Button("None") ) SurfCmd( "POLY SELECT NONE" );
		ImGui::Separator();
		if( ImGui::Button("Memorize") ) SurfCmd( "POLY SELECT MEMORY SET" );
		ImGui::SameLine(); if( ImGui::Button("Recall") ) SurfCmd( "POLY SELECT MEMORY RECALL" );
	}

	ImGui::End();
}

/*-----------------------------------------------------------------------------
	Class browser.
-----------------------------------------------------------------------------*/

static void SetCurrentClass( const char* Name )
{
	appStrcpy( GCurrentClass, Name );
	char Cmd[160];
	appSprintf( Cmd, "SETCURRENTCLASS CLASS=%s", Name );
	EdExec( Cmd );
}

// Recursive class-tree node: a class and all its loaded subclasses.
static void ClassTreeNode( UClass* Class )
{
	// Collect children (loaded classes whose immediate super is this one).
	std::vector<UClass*> Kids;
	for( TObjectIterator<UClass> It; It; ++It )
		if( It->GetSuperClass()==Class && *It!=Class )
			Kids.push_back( *It );

	ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if( Kids.empty() )
		Flags |= ImGuiTreeNodeFlags_Leaf;
	if( appStricmp( Class->GetName(), GCurrentClass )==0 )
		Flags |= ImGuiTreeNodeFlags_Selected;
	if( Class->ClassFlags & CLASS_Abstract )
		ImGui::PushStyleColor( ImGuiCol_Text, ImVec4(0.6f,0.6f,0.6f,1.f) );
	bool Open = ImGui::TreeNodeEx( Class->GetName(), Flags );
	if( Class->ClassFlags & CLASS_Abstract )
		ImGui::PopStyleColor();
	if( ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && !(Class->ClassFlags & CLASS_Abstract) )
		SetCurrentClass( Class->GetName() );
	if( Open )
	{
		for( size_t i=0; i<Kids.size(); i++ )
			ClassTreeNode( Kids[i] );
		ImGui::TreePop();
	}
}

static void BuildClassBrowser()
{
	ImGui::SetNextWindowSize( ImVec2(300,420), ImGuiCond_FirstUseEver );
	if( ImGui::Begin("Classes", &GShowClasses) )
	{
		ImGui::Text( "Current: %s", GCurrentClass );
		ImGui::Separator();
		ImGui::BeginChild( "tree" );
		// Root the tree at Actor: that's what can be placed in a level.
		ClassTreeNode( AActor::StaticClass );
		ImGui::EndChild();
	}
	ImGui::End();
}

/*-----------------------------------------------------------------------------
	Right-click context menus (EDC_RtClick* transport).
-----------------------------------------------------------------------------*/

static void BuildContextMenu()
{
	static DWORD PopupKind = 0;
	if( GPendingPopup )
	{
		PopupKind = GPendingPopup;
		GPendingPopup = 0;
		ImGui::OpenPopup( "EdCtx" );
	}
	if( !ImGui::BeginPopup( "EdCtx" ) )
		return;
	char Cmd[256];
	switch( PopupKind )
	{
		case EDGUI_EDC_RtClickWindow:
		case EDGUI_EDC_RtClickWindowCanAdd:
		{
			appSprintf( Cmd, "Add %s Here", GCurrentClass );
			if( ImGui::MenuItem( Cmd, NULL, false, GCurrentClass[0]!=0 ) )
			{
				appSprintf( Cmd, "ACTOR ADD CLASS=%s", GCurrentClass );
				EdExec( Cmd );
			}
			if( ImGui::MenuItem( "Add Light Here" ) )
				EdExec( "ACTOR ADD CLASS=Light" );
			if( ImGui::MenuItem( "Add PlayerStart Here" ) )
				EdExec( "ACTOR ADD CLASS=PlayerStart" );
			ImGui::Separator();
			if( ImGui::MenuItem( "Paste" ) )
				EdExec( "EDIT PASTE" );
			break;
		}
		case EDGUI_EDC_RtClickActor:
		{
			ImGui::TextDisabled( "%i actor(s) selected", (INT)GSelected.size() );
			ImGui::Separator();
			if( ImGui::MenuItem( "Properties" ) )
				GShowProps = true;
			if( ImGui::MenuItem( "Duplicate" ) )
				EdExec( "DUPLICATE" );
			if( ImGui::MenuItem( "Delete" ) )
				EdExec( "DELETE" );
			ImGui::Separator();
			if( !GSelected.empty() )
			{
				appSprintf( Cmd, "Select All %s", GSelected[0]->GetClass()->GetName() );
				if( ImGui::MenuItem( Cmd ) )
				{
					appSprintf( Cmd, "ACTOR SELECT OFCLASS CLASS=%s", GSelected[0]->GetClass()->GetName() );
					EdExec( Cmd );
				}
			}
			if( ImGui::MenuItem( "Select None" ) )
				EdExec( "SELECT NONE" );
			break;
		}
		case EDGUI_EDC_RtClickPoly:
		{
			ImGui::TextDisabled( "Surface" );
			ImGui::Separator();
			if( ImGui::MenuItem( "Surface Properties..." ) )
				GShowSurface = true;
			if( ImGui::MenuItem( "Apply Current Texture" ) )
				EdExec( "POLY SETTEXTURE" );
			if( ImGui::MenuItem( "Select Matching Texture" ) )
				EdExec( "POLY SELECT MATCHING TEXTURE" );
			if( ImGui::MenuItem( "Select Adjacent All" ) )
				EdExec( "POLY SELECT ADJACENT ALL" );
			if( ImGui::MenuItem( "Select None" ) )
				EdExec( "POLY SELECT NONE" );
			break;
		}
		default:
			ImGui::TextDisabled( "(no actions)" );
			break;
	}
	ImGui::EndPopup();
}

/*-----------------------------------------------------------------------------
	UI.
-----------------------------------------------------------------------------*/

/*-----------------------------------------------------------------------------
	Toolbar.

	The 1998 shell's camera-speed / grid / snap toolbar. Every control READS
	the live UEditorEngine members and WRITES through Exec, so the bar stays
	truthful when the same settings are changed elsewhere -- the console, or
	UnEdCam's 1/2/3 camera-speed hotkeys. That is the opposite of GModeIndex,
	which has to be shadowed shell-side because the engine exposes no topic
	reporting Mode back.
-----------------------------------------------------------------------------*/

// Camera speeds, matching the '1'/'2'/'3' hotkeys in UnEdCam.cpp.
static const FLOAT	GCamSpeeds[]		= { 1.f, 4.f, 16.f };
static const char*	GCamSpeedNames[]	= { "Slow", "Normal", "Fast" };

static const INT	GGridSizes[]		= { 1, 2, 4, 8, 16, 32, 64, 128, 256 };

// Rotation grid. RotGridSize is in Unreal angle units (65536 == 360 degrees),
// but "MAP ROTGRID PITCH=" is parsed with GetFROTATOR's ScaleFactor of 256, so
// the command takes units/256 -- 22.5 degrees is 4096 units, sent as 16.
static const INT	GRotSizes[]			= { 1024, 2048, 4096, 8192, 16384 };
static const char*	GRotNames[]			= { "5.625", "11.25", "22.5", "45", "90" };

static void BuildToolbar()
{
	UEditorEngine* Ed = (UEditorEngine*)GEdEngine;
	char Cmd[128];

	// Mirrors BeginMainMenuBar: a viewport side bar insets the work area, so
	// this docks directly under the menu bar and panels lay out below it.
	ImGuiWindowFlags Flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
	if( ImGui::BeginViewportSideBar( "##EdToolbar", ImGui::GetMainViewport(), ImGuiDir_Up, ImGui::GetFrameHeight(), Flags ) )
	{
		// A menu bar lays items out horizontally, so no SameLine calls here,
		// and Separator draws vertically.
		if( ImGui::BeginMenuBar() )
		{
			// --- Camera movement speed ------------------------------------
			ImGui::TextUnformatted( "Speed" );
			for( INT i=0; i<(INT)ARRAY_COUNT(GCamSpeeds); i++ )
				if( ImGui::RadioButton( GCamSpeedNames[i], Ed->MovementSpeed==GCamSpeeds[i] ) )
				{
					appSprintf( Cmd, "MODE SPEED=%f", GCamSpeeds[i] );
					EdExec( Cmd );
				}

			ImGui::Separator();

			// --- Movement grid --------------------------------------------
			bool GridOn = Ed->Constraints.GridEnabled ? true : false;
			if( ImGui::Checkbox( "Grid", &GridOn ) )
			{
				appSprintf( Cmd, "MODE GRID=%i", GridOn ? 1 : 0 );
				EdExec( Cmd );
			}
			INT CurGrid = (INT)Ed->Constraints.GridSize.X;
			char GridLabel[16];
			appSprintf( GridLabel, "%i", CurGrid );
			ImGui::SetNextItemWidth( 72 );
			if( ImGui::BeginCombo( "##gridsize", GridLabel ) )
			{
				for( INT i=0; i<(INT)ARRAY_COUNT(GGridSizes); i++ )
				{
					char Item[16];
					appSprintf( Item, "%i", GGridSizes[i] );
					if( ImGui::Selectable( Item, CurGrid==GGridSizes[i] ) )
					{
						// Uniform on all three axes, as the 1998 drag grid was.
						appSprintf( Cmd, "MAP GRID X=%i Y=%i Z=%i", GGridSizes[i], GGridSizes[i], GGridSizes[i] );
						EdExec( Cmd );
					}
				}
				ImGui::EndCombo();
			}

			ImGui::Separator();

			// --- Rotation grid --------------------------------------------
			bool RotOn = Ed->Constraints.RotGridEnabled ? true : false;
			if( ImGui::Checkbox( "Rot", &RotOn ) )
			{
				appSprintf( Cmd, "MODE ROTGRID=%i", RotOn ? 1 : 0 );
				EdExec( Cmd );
			}
			INT CurRot = Ed->Constraints.RotGridSize.Yaw;
			const char* RotLabel = "custom";
			for( INT i=0; i<(INT)ARRAY_COUNT(GRotSizes); i++ )
				if( CurRot==GRotSizes[i] )
					RotLabel = GRotNames[i];
			ImGui::SetNextItemWidth( 80 );
			if( ImGui::BeginCombo( "##rotsize", RotLabel ) )
			{
				for( INT i=0; i<(INT)ARRAY_COUNT(GRotSizes); i++ )
					if( ImGui::Selectable( GRotNames[i], CurRot==GRotSizes[i] ) )
					{
						INT N = GRotSizes[i] / 256;
						appSprintf( Cmd, "MAP ROTGRID PITCH=%i YAW=%i ROLL=%i", N, N, N );
						EdExec( Cmd );
					}
				ImGui::EndCombo();
			}

			ImGui::Separator();

			// --- Snapping and grid display --------------------------------
			bool SnapVert = Ed->Constraints.SnapVertices ? true : false;
			if( ImGui::Checkbox( "Snap Vtx", &SnapVert ) )
			{
				appSprintf( Cmd, "MODE SNAPVERTEX=%i", SnapVert ? 1 : 0 );
				EdExec( Cmd );
			}
			// MAP GRID always runs GetFVECTOR on its argument string, and with no
			// X=/Y=/Z= present that falls through to the comma format and leaves
			// GridSize.X clobbered to appAtof("SHOW2D=ON") == 0. So the display
			// toggles have to resend the current grid size alongside the flag.
			const INT GX = (INT)Ed->Constraints.GridSize.X;
			const INT GY = (INT)Ed->Constraints.GridSize.Y;
			const INT GZ = (INT)Ed->Constraints.GridSize.Z;
			bool Show2D = Ed->Show2DGrid ? true : false;
			if( ImGui::Checkbox( "2D Grid", &Show2D ) )
			{
				appSprintf( Cmd, "MAP GRID SHOW2D=%s X=%i Y=%i Z=%i", Show2D ? "ON" : "OFF", GX, GY, GZ );
				EdExec( Cmd );
			}
			bool Show3D = Ed->Show3DGrid ? true : false;
			if( ImGui::Checkbox( "3D Grid", &Show3D ) )
			{
				appSprintf( Cmd, "MAP GRID SHOW3D=%s X=%i Y=%i Z=%i", Show3D ? "ON" : "OFF", GX, GY, GZ );
				EdExec( Cmd );
			}

			ImGui::EndMenuBar();
		}
	}
	// Begin/End must always pair, open or not.
	ImGui::End();
}

static void BuildUI( UViewport* Viewport )
{
	if( GSelDirty )
		RefreshSelection();

	// Main menu bar.
	if( ImGui::BeginMainMenuBar() )
	{
		if( ImGui::BeginMenu("File") )
		{
			if( ImGui::MenuItem("New Map") )
				EdExec( "MAP NEW" );
			if( ImGui::MenuItem("Open Map...") )
				GMapDialog = 1;
			if( ImGui::MenuItem("Save Map...") )
				GMapDialog = 2;
			ImGui::Separator();
			if( ImGui::MenuItem("Exit") )
				GIsRequestingExit = 1;
			ImGui::EndMenu();
		}
		if( ImGui::BeginMenu("Edit") )
		{
			if( ImGui::MenuItem("Undo", "Ctrl+Z") )
				EdExec( "TRANSACTION UNDO" );
			if( ImGui::MenuItem("Redo", "Ctrl+Y") )
				EdExec( "TRANSACTION REDO" );
			ImGui::Separator();
			if( ImGui::MenuItem("Select None") )
				EdExec( "SELECT NONE" );
			if( ImGui::MenuItem("Duplicate") )
				EdExec( "DUPLICATE" );
			if( ImGui::MenuItem("Delete") )
				EdExec( "DELETE" );
			ImGui::EndMenu();
		}
		if( ImGui::BeginMenu("Mode") )
		{
			for( int i=0; i<(int)ARRAY_COUNT(GModes); i++ )
			{
				if( ImGui::MenuItem( GModes[i].Label, NULL, GModeIndex==i ) )
				{
					GModeIndex = i;
					char Cmd[128];
					appSprintf( Cmd, "MODE %s", GModes[i].Token );
					EdExec( Cmd );
				}
			}
			ImGui::EndMenu();
		}
		if( ImGui::BeginMenu("Brush") )
		{
			static float CubeS[3] = { 256, 256, 256 };
			static float SheetS[2] = { 256, 256 };
			static float CylR = 128, CylH = 256;
			static int   CylN = 8;
			if( ImGui::BeginMenu("Cube") )
			{
				ImGui::SetNextItemWidth(180); ImGui::InputFloat3( "Size", CubeS );
				if( ImGui::Button("Build Cube") )
				{
					BuildBrushCube( CubeS[0], CubeS[1], CubeS[2] );
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndMenu();
			}
			if( ImGui::BeginMenu("Sheet") )
			{
				ImGui::SetNextItemWidth(140); ImGui::InputFloat2( "W/H", SheetS );
				if( ImGui::Button("Build Sheet") )
				{
					BuildBrushSheet( SheetS[0], SheetS[1] );
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndMenu();
			}
			if( ImGui::BeginMenu("Cylinder") )
			{
				ImGui::SetNextItemWidth(80); ImGui::InputFloat( "Radius", &CylR );
				ImGui::SetNextItemWidth(80); ImGui::InputFloat( "Height", &CylH );
				ImGui::SetNextItemWidth(80); ImGui::InputInt( "Sides", &CylN );
				if( ImGui::Button("Build Cylinder") )
				{
					BuildBrushCylinder( CylR, CylH, CylN );
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndMenu();
			}
			if( ImGui::MenuItem("Reset Transform") )
				EdExec( "BRUSH RESET" );
			ImGui::Separator();
			if( ImGui::MenuItem("Add (solid)") )
				EdExec( "BRUSH ADD" );
			if( ImGui::MenuItem("Subtract") )
				EdExec( "BRUSH SUBTRACT" );
			if( ImGui::MenuItem("Intersect") )
				EdExec( "BRUSH FROM INTERSECTION" );
			if( ImGui::MenuItem("Deintersect") )
				EdExec( "BRUSH FROM DEINTERSECTION" );
			ImGui::EndMenu();
		}
		if( ImGui::BeginMenu("Build") )
		{
			if( ImGui::MenuItem("Rebuild Geometry") )
				EdExec( "MAP REBUILD" );
			if( ImGui::MenuItem("Build Paths") )
				EdExec( "PATHS DEFINE" );
			if( ImGui::MenuItem("Redraw Level") )
				EdExec( "LEVEL REDRAW" );
			ImGui::EndMenu();
		}
		if( ImGui::BeginMenu("View") )
		{
			ImGui::MenuItem( "Toolbar",    NULL, &GShowToolbar );
			ImGui::MenuItem( "Log",        NULL, &GShowLog );
			ImGui::MenuItem( "Console",    NULL, &GShowConsole );
			ImGui::MenuItem( "Properties", NULL, &GShowProps );
			ImGui::MenuItem( "Surface",    NULL, &GShowSurface );
			ImGui::MenuItem( "Classes",    NULL, &GShowClasses );
			ImGui::MenuItem( "Textures",   NULL, &GShowTextures );
			ImGui::Separator();
			ImGui::MenuItem( "ImGui Demo", NULL, &GShowDemo );
			ImGui::EndMenu();
		}
		// Right-aligned status: current class, selection count, mode.
		char Status[192];
		appSprintf( Status, "Class: %s   %i sel   Mode: %s", GCurrentClass, (INT)GSelected.size(), GModes[GModeIndex].Label );
		ImGui::SameLine( ImGui::GetWindowWidth() - ImGui::CalcTextSize(Status).x - 16 );
		ImGui::TextDisabled( "%s", Status );
		ImGui::EndMainMenuBar();
	}

	if( GShowToolbar )
		BuildToolbar();

	// Map load/save dialog: content\Maps listing + editable path.
	if( GMapDialog )
	{
		static std::vector<std::string> MapFiles;
		static bool Scanned = false;
		if( !Scanned )
		{
			MapFiles.clear();
			TArray<FString> Found = appFindFiles( "..\\content\\Maps\\*.unr" );
			for( INT i=0; i<Found.Num(); i++ )
				MapFiles.push_back( *Found(i) );
			Scanned = true;
		}
		ImGui::OpenPopup( GMapDialog==1 ? "Open Map" : "Save Map" );
		if( ImGui::BeginPopupModal( GMapDialog==1 ? "Open Map" : "Save Map", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
		{
			ImGui::BeginChild( "files", ImVec2(420,260), ImGuiChildFlags_Borders );
			for( size_t i=0; i<MapFiles.size(); i++ )
				if( ImGui::Selectable( MapFiles[i].c_str(), false, ImGuiSelectableFlags_AllowDoubleClick ) )
				{
					appSprintf( GMapPath, "..\\content\\Maps\\%s", MapFiles[i].c_str() );
					if( ImGui::IsMouseDoubleClicked(0) && GMapDialog==1 )
					{
						char Cmd[512];
						appSprintf( Cmd, "MAP LOAD FILE=%s", GMapPath );
						EdExec( Cmd );
						GMapDialog = 0;
						Scanned = false;
						ImGui::CloseCurrentPopup();
					}
				}
			ImGui::EndChild();
			ImGui::SetNextItemWidth( 420 );
			ImGui::InputText( "File", GMapPath, sizeof(GMapPath) );
			if( ImGui::Button("OK", ImVec2(120,0)) )
			{
				char Cmd[512];
				appSprintf( Cmd, "MAP %s FILE=%s", GMapDialog==1 ? "LOAD" : "SAVE", GMapPath );
				EdExec( Cmd );
				GMapDialog = 0;
				Scanned = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if( ImGui::Button("Cancel", ImVec2(120,0)) )
			{
				GMapDialog = 0;
				Scanned = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	// Log panel.
	if( GShowLog )
	{
		ImGui::SetNextWindowSize( ImVec2(560,240), ImGuiCond_FirstUseEver );
		ImGui::SetNextWindowPos( ImVec2(8, ImGui::GetIO().DisplaySize.y-248), ImGuiCond_FirstUseEver );
		if( ImGui::Begin("Log", &GShowLog) )
		{
			ImGui::BeginChild( "scroll", ImVec2(0,0), 0, ImGuiWindowFlags_HorizontalScrollbar );
			ImGuiListClipper Clip;
			Clip.Begin( (int)GEdLog.Lines.size() );
			while( Clip.Step() )
				for( int i=Clip.DisplayStart; i<Clip.DisplayEnd; i++ )
					ImGui::TextUnformatted( GEdLog.Lines[i].c_str() );
			if( GEdLog.ScrollToBottom && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()-40 )
				ImGui::SetScrollHereY( 1.0f );
			GEdLog.ScrollToBottom = false;
			ImGui::EndChild();
		}
		ImGui::End();
	}

	// Exec console.
	if( GShowConsole )
	{
		ImGui::SetNextWindowSize( ImVec2(560,64), ImGuiCond_FirstUseEver );
		ImGui::SetNextWindowPos( ImVec2(8, ImGui::GetIO().DisplaySize.y-320), ImGuiCond_FirstUseEver );
		if( ImGui::Begin("Console", &GShowConsole, ImGuiWindowFlags_NoScrollbar) )
		{
			ImGui::SetNextItemWidth( -70 );
			bool Run = ImGui::InputText( "##cmd", GConsoleInput, sizeof(GConsoleInput), ImGuiInputTextFlags_EnterReturnsTrue );
			ImGui::SameLine();
			Run |= ImGui::Button( "Exec" );
			if( Run && GConsoleInput[0] )
			{
				EdExec( GConsoleInput );
				GConsoleInput[0] = 0;
				GConsoleFocus = true;
			}
			if( GConsoleFocus )
			{
				ImGui::SetKeyboardFocusHere( -1 );
				GConsoleFocus = false;
			}
		}
		ImGui::End();
	}

	if( GShowProps )
		BuildPropertiesPanel();

	if( GShowSurface )
		BuildSurfacePanel();

	if( GShowClasses )
		BuildClassBrowser();

	if( GShowTextures )
		BuildTexturePanel();

	BuildContextMenu();

	if( GShowDemo )
		ImGui::ShowDemoWindow( &GShowDemo );
}

/*-----------------------------------------------------------------------------
	Hooks.
-----------------------------------------------------------------------------*/

static UBOOL EdGuiEventHook( const union SDL_Event* InEv )
{
	if( !GImGuiReady )
		return 0;
	const SDL_Event* Ev = (const SDL_Event*)InEv;

	// Multi-viewport: ImGui lives on the primary window only; events for the
	// ortho windows go straight to the engine.
	Uint32 UiWinId = SDL_GetWindowID( GImGuiWindow );
	switch( Ev->type )
	{
		case SDL_EVENT_MOUSE_MOTION:		if( Ev->motion.windowID!=UiWinId ) return 0; break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:		if( Ev->button.windowID!=UiWinId ) return 0; break;
		case SDL_EVENT_MOUSE_WHEEL:			if( Ev->wheel.windowID!=UiWinId  ) return 0; break;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:				if( Ev->key.windowID!=UiWinId    ) return 0; break;
		case SDL_EVENT_TEXT_INPUT:			if( Ev->text.windowID!=UiWinId   ) return 0; break;
	}

	ImGui_ImplSDL3_ProcessEvent( Ev );
	ImGuiIO& io = ImGui::GetIO();
	switch( Ev->type )
	{
		case SDL_EVENT_MOUSE_MOTION:
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		case SDL_EVENT_MOUSE_WHEEL:
			return io.WantCaptureMouse ? 1 : 0;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
		case SDL_EVENT_TEXT_INPUT:
			return io.WantCaptureKeyboard ? 1 : 0;
	}
	return 0;
}

static void EdGuiPostRender( UViewport* Viewport )
{
	if( !GInstalled || !Viewport || !Viewport->GetWindow() )
		return;
	if( !GImGuiReady )
	{
		// First swap of the primary (Standard3V) window: the GL context is
		// current here, so init lazily. Other viewports' swaps are ignored.
		if( appStricmp( Viewport->GetName(), "Standard3V" )!=0 )
			return;
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
		io.IniFilename = "EdGui.ini";	// persists panel layout next to Unreal.ini
		ImGui::StyleColorsDark();
		GImGuiWindow = (SDL_Window*)Viewport->GetWindow();
		ImGui_ImplSDL3_InitForOpenGL( GImGuiWindow, SDL_GL_GetCurrentContext() );
		ImGui_ImplOpenGL3_Init( "#version 130" );
		GImGuiReady = 1;
		debugf( NAME_Init, "EdGui: ImGui editor frame initialized" );
	}
	// Multi-viewport: the UI draws only on its own window's swaps.
	if( (SDL_Window*)Viewport->GetWindow()!=GImGuiWindow )
		return;
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	BuildUI( Viewport );
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
}

/*-----------------------------------------------------------------------------
	Install / shutdown.
-----------------------------------------------------------------------------*/

void EdGuiInstall( UEngine* InEngine )
{
	if( GInstalled )
		return;
	GEdEngine = InEngine;

	// Tee the log into the panel (keeps the previous hook, i.e. stdout).
	GPrevLogHook = GLogHook;
	GLogHook = &GEdLog;

	// Take the input, overlay, and editor-notification hooks.
	GSDLEventHook     = EdGuiEventHook;
	GGLPostRenderHook = EdGuiPostRender;
	GEdCallbackHook   = EdGuiEdCallback;
	GInstalled = 1;

	// Open the classic quad views as four SDL windows — the perspective
	// Standard3V first (it hosts the ImGui frame; index 0 = the window whose
	// close quits), then top/front/side orthos. All realtime so the client
	// repaints them every frame; SHOW_NoButtons is required: DrawButtons
	// draws the 1998 viewport toolbar from textures only the old shell loaded
	// (NULL deref); ImGui is the toolbar now.
	char Cmd[256];
	DWORD PerspFlags = SHOW_Frame|SHOW_Backdrop|SHOW_Actors|SHOW_Brush|SHOW_MovingBrushes|SHOW_StandardView|SHOW_RealTime|SHOW_NoButtons;
	DWORD OrthoFlags = SHOW_Frame|SHOW_Actors|SHOW_Brush|SHOW_MovingBrushes|SHOW_StandardView|SHOW_RealTime|SHOW_NoButtons;
	appSprintf( Cmd, "CAMERA OPEN NAME=Standard3V X=40 Y=40 XR=900 YR=560 FLAGS=%i REN=%i", PerspFlags, REN_DynLight );
	EdExec( Cmd );
	appSprintf( Cmd, "CAMERA OPEN NAME=OrthXY X=950 Y=40 XR=560 YR=560 FLAGS=%i REN=%i", OrthoFlags, REN_OrthXY );
	EdExec( Cmd );
	appSprintf( Cmd, "CAMERA OPEN NAME=OrthXZ X=40 Y=640 XR=560 YR=360 FLAGS=%i REN=%i", OrthoFlags, REN_OrthXZ );
	EdExec( Cmd );
	appSprintf( Cmd, "CAMERA OPEN NAME=OrthYZ X=950 Y=640 XR=560 YR=360 FLAGS=%i REN=%i", OrthoFlags, REN_OrthYZ );
	EdExec( Cmd );

	// Keep the engine's current-class in sync with the UI default.
	SetCurrentClass( GCurrentClass );

	// Optional startup macro: System\EdGuiBoot.txt, one exec per line (also
	// the hook used by the automated editor tests).
	{
		FILE* F = fopen( "EdGuiBoot.txt", "rt" );
		if( F )
		{
			char Line[512];
			while( fgets( Line, sizeof(Line), F ) )
			{
				char* End = Line + strlen(Line);
				while( End>Line && (End[-1]=='\n' || End[-1]=='\r') )
					*--End = 0;
				if( Line[0] && Line[0]!=';' )
					EdExec( Line );
			}
			fclose( F );
			debugf( NAME_Init, "EdGui: executed EdGuiBoot.txt" );
		}
	}

	debugf( NAME_Init, "EdGui: installed (engine=%s)", GEdEngine ? GEdEngine->GetName() : "none" );
}

void EdGuiShutdown()
{
	if( !GInstalled )
		return;
	GSDLEventHook     = NULL;
	GGLPostRenderHook = NULL;
	GEdCallbackHook   = NULL;
	GLogHook          = GPrevLogHook;
	if( GImGuiReady )
	{
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();
		GImGuiReady = 0;
	}
	GInstalled = 0;
	GEdEngine  = NULL;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
