/*=============================================================================
	UnObjVer.h: Unreal object version.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.
=============================================================================*/

/*-----------------------------------------------------------------------------
	Unrealfile versions.
-----------------------------------------------------------------------------*/

// Package file information.
// Prevents incorrect files from being loaded.
#define PACKAGE_FILE_TAG 0x9E2A83C1

// The current Unrealfile version.
// x64 port: bumped from 61 so packages saved by this build are distinguishable
// from 1998 retail packages (also version 61), which store script bytecode with
// 4-byte object references and 32-bit frame sizes.
#define PACKAGE_FILE_VERSION 100

// x64 port: packages older than this carry 32-bit-era script bytecode and are
// loaded through the translating pass in UStruct::Serialize/SerializeExpr.
#define VER_SCRIPT_X64 100

// The earliest file version which we can load with complete
// backwards compatibility. Must be at least PACKAGE_FILE_VERSION.
#define PACKAGE_MIN_VERSION 34

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
