/*=============================================================================
	LaunchCrash.cpp: Crash reporter for the Unreal x64 port.

	Installs an unhandled-exception filter before WinMain runs and writes a
	symbolized stack trace to Crash.txt next to the executable, using the
	PDBs produced by the build. Port-debugging aid; harmless in release.
=============================================================================*/

#define STRICT
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI PortCrashFilter( EXCEPTION_POINTERS* Info )
{
	FILE* F = fopen( "Crash.txt", "w" );
	if( !F )
		return EXCEPTION_CONTINUE_SEARCH;

	fprintf( F, "Exception code %08lX at address %p\n\n",
		Info->ExceptionRecord->ExceptionCode,
		Info->ExceptionRecord->ExceptionAddress );

	HANDLE Process = GetCurrentProcess();
	SymSetOptions( SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS );
	SymInitialize( Process, NULL, TRUE );

	CONTEXT Ctx = *Info->ContextRecord;
	STACKFRAME64 Frame;
	memset( &Frame, 0, sizeof(Frame) );
	Frame.AddrPC.Offset    = Ctx.Rip; Frame.AddrPC.Mode    = AddrModeFlat;
	Frame.AddrFrame.Offset = Ctx.Rbp; Frame.AddrFrame.Mode = AddrModeFlat;
	Frame.AddrStack.Offset = Ctx.Rsp; Frame.AddrStack.Mode = AddrModeFlat;

	for( int i=0; i<64; i++ )
	{
		if( !StackWalk64( IMAGE_FILE_MACHINE_AMD64, Process, GetCurrentThread(),
				&Frame, &Ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL ) )
			break;
		if( !Frame.AddrPC.Offset )
			break;

		char SymBuf[ sizeof(SYMBOL_INFO) + 256 ];
		SYMBOL_INFO* Sym = (SYMBOL_INFO*)SymBuf;
		Sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		Sym->MaxNameLen   = 255;
		DWORD64 Disp = 0;

		char ModName[MAX_PATH] = "?";
		DWORD64 ModBase = SymGetModuleBase64( Process, Frame.AddrPC.Offset );
		if( ModBase )
			GetModuleFileNameA( (HMODULE)ModBase, ModName, MAX_PATH );

		if( SymFromAddr( Process, Frame.AddrPC.Offset, &Disp, Sym ) )
		{
			IMAGEHLP_LINE64 Line; DWORD LineDisp=0;
			Line.SizeOfStruct = sizeof(Line);
			if( SymGetLineFromAddr64( Process, Frame.AddrPC.Offset, &LineDisp, &Line ) )
				fprintf( F, "%2d %s!%s+0x%llx  [%s:%lu]\n", i, strrchr(ModName,'\\')?strrchr(ModName,'\\')+1:ModName, Sym->Name, Disp, Line.FileName, Line.LineNumber );
			else
				fprintf( F, "%2d %s!%s+0x%llx\n", i, strrchr(ModName,'\\')?strrchr(ModName,'\\')+1:ModName, Sym->Name, Disp );
		}
		else
			fprintf( F, "%2d %s!%p\n", i, strrchr(ModName,'\\')?strrchr(ModName,'\\')+1:ModName, (void*)Frame.AddrPC.Offset );
	}
	fclose( F );
	return EXCEPTION_CONTINUE_SEARCH;
}

struct FInstallCrashFilter
{
	FInstallCrashFilter()
	{
		SetUnhandledExceptionFilter( PortCrashFilter );
	}
};
static FInstallCrashFilter GInstallCrashFilter;
