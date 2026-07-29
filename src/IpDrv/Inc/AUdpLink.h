/*=============================================================================
	AUdpLink.h.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.
=============================================================================*/

	AUdpLink();
	void Destroy();
	// x64 port: the script property `Socket` is a 4-byte INT, but a Win64
	// SOCKET is 8 bytes — the old `return *(SOCKET*)&Socket` aliased an 8-byte
	// handle onto the 4-byte slot, so every assignment corrupted the adjacent
	// property. Store the handle 32-bit (Windows socket handles fit) and
	// convert, matching ATcpLink.
	SOCKET GetSocket() const { return (SOCKET)(PTRINT)Socket; }
	void   SetSocket( SOCKET s ) { Socket = (INT)(PTRINT)s; }
	UBOOL Tick( FLOAT DeltaTime, enum ELevelTick TickType );

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
