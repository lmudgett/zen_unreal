/*=============================================================================
	UnClass.cpp: Object class implementation.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

#include "CorePrivate.h"

/*-----------------------------------------------------------------------------
	FPropertyTag.
-----------------------------------------------------------------------------*/

//
// A tag describing a class property, to aid in serialization.
//
struct FPropertyTag
{
	// Archive for counting property sizes.
	class FArchiveCountSize : public FArchive
	{
	public:
		FArchiveCountSize( FArchive& InSaveAr )
		: Size(0), SaveAr(InSaveAr)
		{
			ArIsSaving = 1;
		}
		INT Size;
	private:
		FArchive& SaveAr;
		FArchive& operator<<( UObject*& Obj )
		{
			INT Index = SaveAr.MapObject(Obj);
			return *this << AR_INDEX(Index);
		}
		FArchive& operator<<( FName& Name )
		{
			INT Index = SaveAr.MapName(&Name);
			return *this << AR_INDEX(Index);
		}
		FArchive& Serialize( void* V, int Length )
		{
			Size += Length;
			return *this;
		}
	};

	// Variables.
	BYTE	Type;		// Type of property, 0=end.
	BYTE	Info;		// Packed info byte.
	FName	Name;		// Name of property.
	FName	ItemName;	// Struct name if UStructProperty.
	INT		Size;       // Property size.
	INT		ArrayIndex;	// Index if an array; else 0.

	// Constructors.
	FPropertyTag()
	{}
	FPropertyTag( FArchive& InSaveAr, UProperty* Property, INT InIndex, BYTE* Value )
	:	Type		( Property->GetID() )
	,	Name		( Property->GetFName() )
	,	ItemName	( NAME_None     )
	,	Size		( 0             )
	,	ArrayIndex	( InIndex       )
	,	Info		( Property->GetID() )
	{
		// Handle structs.
		UStructProperty* StructProperty = Cast<UStructProperty>( Property );
		if( StructProperty )
			ItemName = StructProperty->Struct->GetFName();

		// Set size.
		FArchiveCountSize ArCount( InSaveAr );
		SerializeTaggedProperty( ArCount, Property, Value );
		Size = ArCount.Size;

		// Update info bits.
		Info |=
		(	Size==1		? 0x00
		:	Size==2     ? 0x10
		:	Size==4     ? 0x20
		:	Size==12	? 0x30
		:	Size==16	? 0x40
		:	Size<=255	? 0x50
		:	Size<=65536 ? 0x60
		:			      0x70);
		UBoolProperty* Bool = Cast<UBoolProperty>( Property );
		if( ArrayIndex || (Bool && (*(DWORD*)Value & Bool->BitMask)) )
			Info |= 0x80;
	}

	// Serializer.
	friend FArchive& operator<<( FArchive& Ar, FPropertyTag& Tag )
	{
		static char PrevTag[NAME_SIZE]="";
		guard(FPropertyTag<<);
		BYTE SizeByte;
		_WORD SizeWord;
		INT SizeInt;

		// Name.
		guard(TagName);
		Ar << Tag.Name;
		if( Tag.Name == NAME_None )
			return Ar;
		appStrcpy( PrevTag, *Tag.Name );
		unguard;

		// Packed info byte:
		// Bit 0..3 = raw type.
		// Bit 4..6 = serialized size: [1 2 4 12 16 byte word int].
		// Bit 7    = array flag.
		Ar << Tag.Info;
		Tag.Type = Tag.Info & 0x0f;
		if( Tag.Type == NAME_StructProperty )
			Ar << Tag.ItemName;
		switch( Tag.Info & 0x70 )
		{
			case 0x00:
				Tag.Size = 1;
				break;
			case 0x10:
				Tag.Size = 2;
				break;
			case 0x20:
				Tag.Size = 4;
				break;
			case 0x30:
				Tag.Size = 12;
				break;
			case 0x40:
				Tag.Size = 16;
				break;
			case 0x50:
				SizeByte =  Tag.Size;
				Ar       << SizeByte;
				Tag.Size =  SizeByte;
				break;
			case 0x60:
				SizeWord =  Tag.Size;
				Ar       << SizeWord;
				Tag.Size =  SizeWord;
				break;
			case 0x70:
				SizeInt		=  Tag.Size;
				Ar          << SizeInt;
				Tag.Size    =  SizeInt;
				break;
		}
		if( (Tag.Info&0x80) && Tag.Type!=NAME_BoolProperty )
		{
			BYTE B
			=	(Tag.ArrayIndex<=127  ) ? (Tag.ArrayIndex    )
			:	(Tag.ArrayIndex<=16383) ? (Tag.ArrayIndex>>8 )+0x80
			:	                          (Tag.ArrayIndex>>24)+0xC0;
			Ar << B;
			if( (B & 0x80)==0 )
			{
				Tag.ArrayIndex = B;
			}
			else if( (B & 0xC0)==0x80 )
			{
				BYTE C = Tag.ArrayIndex & 255;
				Ar << C;
				Tag.ArrayIndex = ((INT)(B&0x7F)<<8) + ((INT)C);
			}
			else
			{
				BYTE C = Tag.ArrayIndex>>16;
				BYTE D = Tag.ArrayIndex>>8;
				BYTE E = Tag.ArrayIndex;
				Ar << C << D << E;
				Tag.ArrayIndex = ((INT)(B&0x3F)<<24) + ((INT)C<<16) + ((INT)D<<8) + ((INT)E);
			}
		}
		else Tag.ArrayIndex = 0;
		return Ar;
		unguardf(( "(After %s)", PrevTag ));
	}

	// Property serializer.
	void SerializeTaggedProperty( FArchive& Ar, UProperty* Property, BYTE* Value )
	{
		guard(FPropertyTag::SerializeTaggedProperty);
		if( Property->GetClass()==UBoolProperty::StaticClass )
		{
			UBoolProperty* Bool = CastChecked<UBoolProperty>( Property );
			check(Bool->BitMask!=0);
			if( Ar.IsLoading() )				
			{
				if( Info&0x80)	*(DWORD *)Value |=  Bool->BitMask;
				else			*(DWORD *)Value &= ~Bool->BitMask;
			}
		}
		else
		{
			Property->SerializeItem( Ar, Value );
		}
		unguard;
	}
};

/*-----------------------------------------------------------------------------
	UField implementation.
-----------------------------------------------------------------------------*/

UField::UField( EIntrinsicConstructor, UClass* InClass, FName InName, FName InPackageName )
: UObject( EC_IntrinsicConstructor, InClass, InName, InPackageName )
, SuperField( NULL )
, Next( NULL )
, HashNext( NULL )
{}
UField::UField( UField* InSuperField )
:	SuperField( InSuperField )
{}
UClass* UField::GetOwnerClass()
{
	guardSlow(UField::GetOwnerClass);
	for( UObject* Obj=this; Obj->GetClass()!=UClass::StaticClass; Obj=Obj->GetParent() );
	return (UClass*)Obj;
	unguardSlow;
}
void UField::Bind()
{
	guard(UField::Bind);
	unguard;
}
void UField::PostLoad()
{
	guard(UField::PostLoad);
	UObject::PostLoad();
	Bind();
	unguard;
}
void UField::Serialize( FArchive& Ar )
{
	guard(UField::Serialize);
	UObject::Serialize( Ar );

	Ar << SuperField << Next;
	if( Ar.IsLoading() )
		HashNext = NULL;

	unguardobj;
}
IMPLEMENT_CLASS(UField)

/*-----------------------------------------------------------------------------
	Hash building.
-----------------------------------------------------------------------------*/

// Virtual function hash builder.
static void BuildVfHashes( UStruct* Struct )
{
	guard(BuildVfHashes);

	// Link the references.
	UObjectProperty** RefLink = &Struct->RefLink;
	for( TFieldIterator<UObjectProperty> ItR(Struct); ItR; ++ItR,RefLink=&(*RefLink)->NextReference )
		*RefLink = *ItR;
	*RefLink = NULL;

	// Link the structs.
	UStructProperty** StructLink = &Struct->StructLink;
	for( TFieldIterator<UStructProperty> ItS(Struct); ItS; ++ItS,StructLink=&(*StructLink)->NextStruct )
		*StructLink = *ItS;
	*StructLink = NULL;

	// Handle state stuff.
	UState* State = Cast<UState>( Struct );
	if( State )
	{
		// Allocate hash.
		if( !State->VfHash )
			State->VfHash = new("vfhash")UField*[UField::HASH_COUNT];

		// Initialize hash.
		UField** PrevLink[UField::HASH_COUNT];
		for( INT i=0; i<UField::HASH_COUNT; i++ )
		{
			State->VfHash[i] = NULL;
			PrevLink[i] = &State->VfHash[i];
		}

		// Add all stuff at this node to the hash.
		for( TFieldIterator<UStruct> It(State); It; ++It )
		{
			INT iHash          = It->GetFName().GetIndex() & (UField::HASH_COUNT-1);
			*PrevLink[iHash]   = *It;
			PrevLink[iHash]    = &It->HashNext;
			It->HashNext       = NULL;
		}
	}

	// Build hash for child states.
	for( TFieldIterator<UStruct> It(Struct); It && It.GetStruct()==Struct; ++It )
		BuildVfHashes( *It );

	unguard;
}

/*-----------------------------------------------------------------------------
	UStruct implementation.
-----------------------------------------------------------------------------*/

//
// Constructors.
//
UStruct::UStruct( EIntrinsicConstructor, INT InSize, FName InName, FName InPackageName )
:	UField( EC_IntrinsicConstructor, UClass::StaticClass, InName, InPackageName )
,	ScriptText( NULL )
,	Children( NULL )
,	PropertiesSize( InSize )
,	FriendlyName( InName )
,	TextPos( 0 )
,	Line( 0 )
,	CppLayoutSize( InSize ) // x64 port: remember the compiled C++ size as the pin source for LinkOffsets
{}
UStruct::UStruct( UStruct* InSuperStruct )
:	UField( InSuperStruct )
,	PropertiesSize( InSuperStruct ? InSuperStruct->GetPropertiesSize() : 0 )
,	FriendlyName( GetFName() )
{}

//
// Link offsets.
//
void UStruct::LinkOffsets( FArchive& Ar )
{
	guard(UStruct::LinkOffsets);
	// x64 port: for registered intrinsic classes the compiled C++ layout is the
	// truth; the recomputed script layout is checked against it and pinned
	// below. CppLayoutSize is captured at intrinsic registration — the script
	// compiler zeroes PropertiesSize before relinking, so the entry value can't
	// be trusted here. (Intrinsic classes in our load set are always registered
	// from their DLL before their package loads; an unregistered one would fail
	// UClass::Bind later anyway.)
	INT CppSize = ( GetClass()==UClass::StaticClass && (GetFlags()&RF_Intrinsic) ) ? (CppLayoutSize ? CppLayoutSize : PropertiesSize) : 0;
	PropertiesSize = 0;
	if( GetInheritanceSuper() )
	{
		Ar.Preload( GetInheritanceSuper() );
		PropertiesSize = Align(GetInheritanceSuper()->GetPropertiesSize(),4);
	}
	// x64 port: UConsole and UPlayer carry a second C++ base (FOutputDevice)
	// whose vfptr grew from 4 to 8 bytes; the script mirror only reserves an
	// int (vtblOut/vfOut) for it, so bias the layout by the difference.
	if( CppSize && (appStricmp(GetName(),"Console")==0 || appStricmp(GetName(),"Player")==0) )
		PropertiesSize += 4;
	UProperty* Prev = NULL;
	for( UField* Field=Children; Field; Field=Field->Next )
	{
		Ar.Preload( Field );
		UProperty* Property = Cast<UProperty>( Field );
		if( Property && Field->GetParent()==this )
		{
			Property->Link( Ar, Prev );
			PropertiesSize = Property->Offset + Property->GetSize();
			Prev = Property;
		}
	}
	// x64 port: #pragma pack(4) in the class headers caps the whole class
	// alignment at 4 (including the vfptr/base contribution), so C++ sizeof
	// rounds to 4 — same as script structs and function frames. Verified
	// against MSVC-compiled AActor (sizeof 556).
	PropertiesSize = Align( PropertiesSize, 4 );

	// x64 port: pin intrinsic classes to the compiled C++ size. A mismatch
	// means the Link rules diverge from the generated headers for this class —
	// property offsets may be wrong, so surface it.
	if( CppSize )
	{
		// Remaining known mismatches are benign: trailing native-only mirrors
		// (TArray/FString script stand-ins sized for 32 bits) that scripts
		// never access; the pinned C++ size keeps instances correctly sized.
		// Logged on the DevLoad channel (not as a warning) so the ~10 expected
		// mismatches don't spam a clean boot while still being visible when
		// diagnosing an unexpected layout divergence.
		if( PropertiesSize != CppSize )
			debugf( NAME_DevLoad, "Class %s script layout size %i != C++ size %i; using C++ size", GetFullName(), PropertiesSize, CppSize );
		PropertiesSize = CppSize;

		// Class Object's script declaration mirrors the hand-written UObject
		// through a padding array sized for 32-bit internals; pin the real C++
		// offsets of the mirrored members. (ObjectInternal is private padding,
		// never accessed by script, and keeps its computed offset.)
		if( !GetInheritanceSuper() && appStricmp(GetName(),"Object")==0 )
		{
			for( TFieldIterator<UProperty> It(this); It; ++It )
			{
				// (v200 Object.uc names the outer "Parent"; later engines call
				// it "Outer" — pin both spellings. Unpinned, the walk read the
				// middle of MainFrame/Linker and CleanupDestroyed asserted on
				// any map with 128+ deleted actors, e.g. Dig.unr.)
				if     ( appStricmp(It->GetName(),"Parent"     )==0 ) It->Offset = STRUCT_OFFSET(UObject,Parent);
				else if( appStricmp(It->GetName(),"Outer"      )==0 ) It->Offset = STRUCT_OFFSET(UObject,Parent);
				else if( appStricmp(It->GetName(),"ObjectFlags")==0 ) It->Offset = STRUCT_OFFSET(UObject,ObjectFlags);
				else if( appStricmp(It->GetName(),"Name"       )==0 ) It->Offset = STRUCT_OFFSET(UObject,Name);
				else if( appStricmp(It->GetName(),"Class"      )==0 ) It->Offset = STRUCT_OFFSET(UObject,Class);
			}
		}
	}
	unguard;
}

//
// Serialize all of the class's data that belongs in a particular
// bin and resides in Data.
//
void UStruct::SerializeBin( FArchive& Ar, BYTE* Data )
{
	FName PropertyName(NAME_None);
	INT Index=0;
	guard(UStruct::SerializeBin);
	for( TFieldIterator<UProperty> It(this); It; ++It )
	{
		PropertyName = It->GetFName();
		if( !(It->PropertyFlags & (CPF_Transient|CPF_Intrinsic)) )
			for( Index=0; Index<It->ArrayDim; Index++ )
				It->SerializeItem( Ar, Data + It->Offset + Index*It->GetElementSize() );
	}
	unguardf(( "(%s %s[%i])", GetFullName(), *PropertyName, Index ));
}
void UStruct::SerializeTaggedProperties( FArchive& Ar, BYTE* Data, UClass* DefaultsClass )
{
	FName PropertyName(NAME_None);
	INT Index=-1;
	guard(UStruct::SerializeTaggedProperties);
	check(Ar.IsLoading() || Ar.IsSaving());

	// Find defaults.
	BYTE* Defaults      = NULL;
	INT   DefaultsCount = 0;
	if( DefaultsClass && DefaultsClass->Defaults.Num() )
	{
		Defaults      = &DefaultsClass->Defaults(0);
		DefaultsCount =  DefaultsClass->Defaults.Num();
	}

	// Load/save.
	if( Ar.IsLoading() )
	{
		// Load all stored properties.
		INT Count=0;
		guard(LoadStream);
		while( 1 )
		{
			FPropertyTag Tag;
			Ar << Tag;
			if( Tag.Name == NAME_None )
				break;
			PropertyName = Tag.Name;
			if( Tag.Type==NAME_StructProperty && appStricmp(*Tag.ItemName,"Rotation")==0 )//oldver
				Tag.ItemName = "Rotator";
			if( Tag.Type==NAME_StructProperty && appStricmp(*Tag.ItemName,"Region")==0 )//oldver
				Tag.ItemName = "PointRegion";
			for( TFieldIterator<UProperty> It(this); It; ++It )
				if( It->GetFName()==Tag.Name )
					break;
			if( !It )
			{
				debugf( NAME_Warning, "Property %s of %s not found", *Tag.Name, GetName() );
			}
			else if( Tag.Type!=It->GetID() )
			{
				debugf( NAME_Warning, "Type mismatch in %s of %s: file %i, class %i", *Tag.Name, GetName(), Tag.Type, It->GetID() );
			}
			else if( Tag.ArrayIndex>=It->ArrayDim )
			{
				debugf( NAME_Warning, "Array bounds in %s of %s: %i/%i", *Tag.Name, GetName(), Tag.ArrayIndex, It->ArrayDim );
			}
			else if( Tag.Type==NAME_StructProperty && Tag.ItemName!=CastChecked<UStructProperty>(*It)->Struct->GetFName() )
			{
				debugf( NAME_Warning, "Property %s of %s struct type mismatch %s/%s", *Tag.Name, GetName(), *Tag.ItemName, CastChecked<UStructProperty>(*It)->Struct->GetName() );
			}
			else if( It->PropertyFlags & (CPF_Transient | CPF_Intrinsic) )
			{
				debugf( NAME_Warning, "Property %s of %s is not serialiable", *Tag.Name, GetName() );
			}
			else
			{
				// This property is ok.
				Tag.SerializeTaggedProperty( Ar, *It, Data + It->Offset + Tag.ArrayIndex*It->GetElementSize() );
				continue;
			}

			// Skip unknown or bad property.
			if( Ar.Ver()<36 && Tag.Type==NAME_ObjectProperty )//oldver
			{
				UObject* Tmp;
				Ar << Tmp;
			}
			else
			{
				BYTE B;
				for( int i=0; i<Tag.Size; i++ )
					Ar << B;
			}
		}
		unguardf(( "(Count %i)", Count ));
	}
	else
	{
		// Save tagged properties.
		guard(SaveStream);
		for( TFieldIterator<UProperty> It(this); It; ++It )
		{
			if( !(It->PropertyFlags & (CPF_Transient | CPF_Intrinsic)) )
			{
				PropertyName = It->GetFName();
				for( Index=0; Index<It->ArrayDim; Index++ )
				{
					INT Offset = It->Offset + Index*It->GetElementSize();
					if( !It->Matches( Data, (Offset+It->GetElementSize()<=DefaultsCount) ? Defaults : NULL, Index) )
					{
 						FPropertyTag Tag( Ar, *It, Index, Data + Offset );
						Ar << Tag;
						Tag.SerializeTaggedProperty( Ar, *It, Data + Offset );
					}
				}
			}
		}
		FName Temp(NAME_None);
		Ar << Temp;
		unguard;
	}
	unguardf(( "(%s[%i])", *PropertyName, Index ));
}
void UStruct::Serialize( FArchive& Ar )
{
	guard(UStruct::Serialize);
	UField::Serialize(Ar);

	// Serialize stuff.
	Ar << ScriptText << Children;
	Ar << FriendlyName << Line << TextPos;
	check(FriendlyName!=NAME_None);

	// Link the properties.
	if( Ar.IsLoading() )
		LinkOffsets( Ar );

	// Script code.
	INT ScriptSize = Script.Num();
	Ar << ScriptSize;
	// x64 port / security: ScriptSize is file-controlled. It sizes two
	// allocations below — Map(ScriptSize+1) and Script(2*ScriptSize+8) — and
	// bounds the translation loop. Without a cap, a crafted value overflows
	// the 32-bit `2*ScriptSize+8` (wrapping to a tiny buffer while the loop
	// runs ~2 billion iterations → heap overflow) or forces a huge up-front
	// allocation. Real per-struct bytecode is at most tens of KB; reject
	// anything absurd (16 MB cap keeps 2*ScriptSize+8 well within INT).
	if( Ar.IsLoading() && (ScriptSize<0 || ScriptSize>0x1000000) )
		appErrorf( "UStruct::Serialize: bad ScriptSize %i in %s", ScriptSize, GetFullName() );
	// x64 port: packages older than VER_SCRIPT_X64 store bytecode with 4-byte
	// object references and code offsets in that 32-bit space. Load through a
	// translating pass: references widen to pointer size while an old->new
	// offset map is recorded, then every code offset baked into the bytecode
	// is remapped through the map.
	check(Xlat==NULL);
	if( Ar.IsLoading() && Ar.Ver()<VER_SCRIPT_X64 && ScriptSize>0 )
	{
		Xlat = new FScriptXlat;
		Xlat->Map.SetNum( ScriptSize+1 );
		Xlat->OldICode = 0;
		Xlat->LastExpr.Kind = FScriptXlat::SIZE_Unknown;
		Xlat->LastExpr.Value = 0;
		Script.SetNum( 2*ScriptSize+8 ); // widening at most doubles the size
		INT iCode = 0;
		while( Xlat->OldICode < ScriptSize )
			SerializeExpr( iCode, Ar );
		if( Xlat->OldICode != ScriptSize )
			appErrorf( "Script serialization mismatch: Got %i, expected %i", Xlat->OldICode, ScriptSize );
		Xlat->Map(ScriptSize) = iCode;
		Script.SetNum( iCode );

		// Remap absolute code-offset words (EX_Jump, EX_JumpIfNot, EX_Case, EX_Iterator).
		INT i;
		for( i=0; i<Xlat->AbsWords.Num(); i++ )
		{
			_WORD& W = *(_WORD*)&Script(Xlat->AbsWords(i));
			if( W != MAXWORD )
				W = XlatWordOffset(W);
		}

		// Recompute relative skip words (EX_Skip, EX_Context, EX_ClassContext):
		// the skipped region contains widened references, so the distance grows.
		for( i=0; i<Xlat->RelWords.Num(); i++ )
		{
			FScriptXlat::FRelWord& R = Xlat->RelWords(i);
			_WORD& W = *(_WORD*)&Script(R.WordPos);
			INT NewSkip = XlatOffset(R.OldAnchor+W) - R.NewAnchor;
			if( NewSkip<0 || NewSkip>=MAXWORD )
				appErrorf( "Bad translated skip %i in %s", NewSkip, GetFullName() );
			W = NewSkip;
		}

		// Remap label table code offsets.
		for( i=0; i<Xlat->LabelInts.Num(); i++ )
		{
			INT& C = *(INT*)&Script(Xlat->LabelInts(i));
			C = XlatOffset(C);
		}
	}
	else
	{
		Script.SetNum( ScriptSize );
		INT iCode = 0;
		while( iCode < ScriptSize )
			SerializeExpr( iCode, Ar );
		if( iCode != ScriptSize )
			appErrorf( "Script serialization mismatch: Got %i, expected %i", iCode, ScriptSize );
	}

	unguardobj;
}

//
// x64 port: translating-loader helpers.
//
INT UStruct::XlatOffset( INT Old )
{
	guard(UStruct::XlatOffset);
	if( Old<0 || Old>=Xlat->Map.Num() )
		appErrorf( "Bad script offset %i/%i translating %s", Old, Xlat->Map.Num()-1, GetFullName() );
	return Xlat->Map(Old);
	unguard;
}
_WORD UStruct::XlatWordOffset( INT Old )
{
	guard(UStruct::XlatWordOffset);
	INT New = XlatOffset( Old );
	if( New >= MAXWORD )
		appErrorf( "Translated script offset %i overflows word in %s", New, GetFullName() );
	return New;
	unguard;
}
INT UStruct::ResolveExprSize( const FScriptXlat::FExprSize& Expr )
{
	guard(UStruct::ResolveExprSize);
	switch( Expr.Kind )
	{
		case FScriptXlat::SIZE_Fixed:
		{
			return Expr.Value;
		}
		case FScriptXlat::SIZE_Property:
		{
			// x64 port / security: the operand is an object pointer resolved
			// from a file-controlled index; a crafted package can point it at
			// a non-UProperty object. Cast<> yields NULL on a type mismatch
			// instead of a type-confused virtual call (matches SIZE_Function).
			UProperty* Property = Cast<UProperty>( *(UObject**)&Script(Expr.Value) );
			return Property ? Property->GetElementSize() : 0;
		}
		case FScriptXlat::SIZE_Function:
		{
			UFunction* Function = Cast<UFunction>( *(UStruct**)&Script(Expr.Value) );
			if( Function )
				for( TFieldIterator<UProperty> It(Function); It && (It->PropertyFlags&CPF_Parm); ++It )
					if( It->PropertyFlags & CPF_ReturnParm )
						return It->GetSize();
			return 0;
		}
		default:
		{
			return 0;
		}
	}
	unguardobj;
}

//
// Actor reference cleanup.
//
void UStruct::CleanupDestroyed( BYTE* Data )
{
	guard(UStruct::CleanupDestroyed);
	if( GIsEditor )
	{
		// Slow cleanup.
		for( TFieldIterator<UProperty> It(this); It; ++It )
		{
			UProperty* Property = *It;
			if( Property->IsA(UObjectProperty::StaticClass) )
			{
				// Cleanup object reference.
				UObject** LinkedObjects = (UObject**)(Data + Property->Offset);
				for( INT k=0; k<Property->ArrayDim; k++ )
				{
					if( LinkedObjects[k] )
					{
						check(LinkedObjects[k]->IsValid());
						if( LinkedObjects[k]->IsPendingKill() )
						{
							// Remove this reference.
							LinkedObjects[k]->Modify();
							LinkedObjects[k] = NULL;
						}
					}
				}
			}
			else if( Property->GetClass()==UStructProperty::StaticClass )
			{
				// Cleanup substructure.
				for( INT k=0; k<Property->ArrayDim; k++ )
					((UStructProperty*)Property)->Struct->CleanupDestroyed( Data + Property->Offset + k*Property->GetElementSize() );
			}
		}
	}
	else
	{
		// Optimal cleanup.
		for( UObjectProperty* Ref=RefLink; Ref; Ref=Ref->NextReference )
		{
			UObject** LinkedObjects = (UObject**)(Data+Ref->Offset);
			for( INT k=0; k<Ref->ArrayDim; k++ )
			{
				if( LinkedObjects[k] )
				{
					// x64 port: identify the culprit before the assert below
					// fires — a garbage pointer here is the signature of a
					// 4-byte write into an 8-byte reference slot (seen once
					// during travel autosave; not yet reproduced).
					if( !LinkedObjects[k]->IsValid() )
					{
						UObject* Walked = (UObject*)Data;
						debugf( "CleanupDestroyed: bad ref in %s property %s[%i] off=%i value=%p", GetFullName(), Ref->GetName(), k, Ref->Offset, LinkedObjects[k] );
						debugf( "CleanupDestroyed: walking Data=%p %s", Data, Walked->IsValid() ? Walked->GetFullName() : "(invalid)" );
					}
					check(LinkedObjects[k]->IsValid());
					if( LinkedObjects[k]->IsPendingKill() )
						LinkedObjects[k] = NULL;
				}
			}
		}
		for( UStructProperty* St=StructLink; St; St=St->NextStruct )
		{
			for( INT k=0; k<St->ArrayDim; k++ )
				St->Struct->CleanupDestroyed( Data + St->Offset + k*St->GetElementSize() );
		}
	}
	unguardobj;
}

IMPLEMENT_CLASS(UStruct);

/*-----------------------------------------------------------------------------
	UClass implementation.
-----------------------------------------------------------------------------*/

//
// Find the class's intrinsic constructor.
//
void UClass::Bind()
{
	guard(UClass::Bind);
	UStruct::Bind();
	if( !Constructor && (GetFlags() & RF_Intrinsic) )
	{
		// Find the intrinsic implementation.
		char ProcName[256];
		appSprintf( ProcName, "autoclass%s", GetNameCPP() );

		// Find export from the DLL.
		UPackage* ClassPackage = CastChecked<UPackage>(GetParent());
		UClass* ClassPtr = (UClass*)ClassPackage->GetDllExport( ProcName, 0 );
		if( ClassPtr )
			Constructor = ClassPtr->Constructor;
		else if( !GIsEditor )
			appErrorf( "Can't bind to intrinsic class %s", GetPathName() );
	}
	if( !Constructor && GetSuperClass() )
	{
		// Chase down constructor in parent class.
		GetSuperClass()->Bind();
		Constructor = GetSuperClass()->Constructor;
	}
	unguardobj;
}

/*-----------------------------------------------------------------------------
	UClass UObject implementation.
-----------------------------------------------------------------------------*/

static void RecursiveTagNames( UClass* Class )
{
	guard(RecursiveTagNames);
	if( (Class->GetFlags() & RF_TagExp) && (Class->GetFlags() & RF_Intrinsic) )
		for( TFieldIterator<UFunction> Function(Class); Function && Function.GetStruct()==Class; ++Function )
			if
			(	(Function->FunctionFlags & FUNC_Event)
			&&	!Function->GetSuperFunction() )
				Function->GetFName().SetFlags( RF_TagExp );
	for( TObjectIterator<UClass> It; It; ++It )
		if( It->GetSuperClass()==Class )
			RecursiveTagNames( *It );
	unguard;
}

void UClass::Export( FOutputDevice& Out, const char* FileType, int Indent )
{
	guard(UClass::Export);
	static int RecursionDepth=0, DidTop, i;
	if( appStricmp(FileType,"H")==0 )
	{
		char API[256];
		appStrcpy( API, GetParent()->GetName() );
		appStrupr( API );

		// Export as C++ header.
		if( RecursionDepth==0 )
		{
			DidTop = 0;
			RecursiveTagNames( this );
		}

		// Export this.
		if( (GetFlags() & RF_TagExp) && (GetFlags() & RF_Intrinsic) )
		{
			// Top of file.
			if( !DidTop )
			{
				DidTop=1;
				Out.Logf
				(
					"/*===========================================================================\r\n"
					"	C++ class definitions exported from UnrealScript.\r\n"
					"\r\n"
					"   This is automatically generated using 'Unreal.exe -make -h'\r\n"
					"   DO NOT modify this manually! Edit the corresponding .uc files instead!\r\n"
					"===========================================================================*/\r\n"
					"#pragma pack (push,%i)\r\n"
					"\r\n"
					"#ifndef %s_API\r\n"
					"#define %s_API DLL_IMPORT\r\n"
					"#endif\r\n"
					"\r\n"
					"#ifndef NAMES_ONLY\r\n"
					"#define DECLARE_NAME(name) extern %s_API FName %s_##name;\r\n"
					"#endif\r\n"
					"\r\n",
					PROPERTY_ALIGNMENT,
					API,
					API,
					API,
					API
				);
				for( INT i=0; i<FName::GetMaxNames(); i++ )
					if( FName::GetEntry(i) && (FName::GetEntry(i)->Flags & RF_TagExp) )
						Out.Logf( "DECLARE_NAME(%s)\r\n", *FName((EName)(i)) );
				for( i=0; i<FName::GetMaxNames(); i++ )
					if( FName::GetEntry(i) )
						FName::GetEntry(i)->Flags &= ~RF_TagExp;
				Out.Logf( "\r\n#ifndef NAMES_ONLY\r\n\r\n" );
			}

			// Enum definitions.
			for( TFieldIterator<UEnum> ItE(this); ItE && ItE.GetStruct()==this; ++ItE )
			{
				// Export enum.
				if( ItE->GetParent()==this )
					ItE->Export( Out, FileType, Indent );
				else
					Out.Logf( "enum %s;\r\n", ItE->GetName() );
			}

			// Struct definitions.
			for( TFieldIterator<UStruct> ItS(this); ItS && ItS.GetStruct()==this; ++ItS )
			{
				if( ItS->GetFlags() & RF_Intrinsic )
				{
					// Export struct.
					Out.Logf( "struct %s_API %s", API, ItS->GetNameCPP() );
					if( ItS->SuperField )
						Out.Logf(" : public %s\r\n", ItS->GetSuperStruct()->GetNameCPP() );
					Out.Logf("\r\n{\r\n" );
					for( TFieldIterator<UProperty> It2(*ItS); It2; ++It2 )
					{
						if( It2.GetStruct()==*ItS )
						{
							Out.Logf( appSpc(Indent+4) );
							It2->ExportCPP( Out, 0, 0 );
							Out.Logf( ";\r\n" );
						}
					}
					Out.Logf("};\r\n\r\n" );
				}
			}

			// Constants.
			INT Consts=0;
			for( TFieldIterator<UConst> ItC(this); ItC && ItC.GetStruct()==this; ++ItC )
				Out.Logf( "#define UCONST_%s %s\r\n", ItC->GetName(), *ItC->Value ),Consts++;
			if( Consts )
				Out.Logf( "\r\n" );

			// Class definition.
			Out.Logf( "class %s_API %s", API, GetNameCPP() );
			if( GetSuperClass() )
				Out.Logf(" : public %s\r\n", GetSuperClass()->GetNameCPP() );
			Out.Logf("{\r\npublic:\r\n" );

			// All per-object properties defined in this class.
			for( TFieldIterator<UProperty> It(this); It; ++It )
			{
				if( It.GetStruct()==this && It->GetElementSize() )
				{
					Out.Logf( appSpc(Indent+4) );
					It->ExportCPP( Out, 0, 0 );
					Out.Logf( ";\r\n" );
				}
			}

			// C++ -> UnrealScript stubs.
			for( TFieldIterator<UFunction> Function(this); Function && Function.GetStruct()==this; ++Function )
			{
				if( Function->FunctionFlags & FUNC_Intrinsic )
				{
					Out.Logf( "    void exec%s( FFrame& Stack, BYTE*& Result )", Function->GetName() );
					Out.Logf( ";\r\n", Function->GetName() );
				}
			}

			// UnrealScript -> C++ proxies.
			for( Function = TFieldIterator<UFunction>(this); Function && Function.GetStruct()==this; ++Function )
			{
				if
				(	(Function->FunctionFlags & FUNC_Event)
				&&	(!Function->GetSuperFunction()) )
				{
					// Return type.
					UProperty* Return = Function->GetReturnProperty();
					Out.Log( "    " );
					if( !Return )
						Out.Log( "void" );
					else
						Return->ExportCPPItem( Out );

					// Function name and parms.
					INT ParmCount=0;
					Out.Logf( " event%s(", Function->GetName() );
					for( TFieldIterator<UProperty> It(*Function); It && (It->PropertyFlags&(CPF_Parm|CPF_ReturnParm))==CPF_Parm; ++It )
					{
						if( ParmCount++ )
							Out.Log(", ");
						It->ExportCPP( Out, 0, 1 );
					}
					Out.Log( ")\r\n" );

					// Function call.
					Out.Log( "    {\r\n" );
					if( ParmCount )
					{
						// Parms struct definition.
						Out.Log( "        struct {" );
						for( It=TFieldIterator<UProperty>(*Function); It && (It->PropertyFlags&CPF_Parm); ++It )
						{
							It->ExportCPP( Out, 1, 0 );
							Out.Log( "; " );
						}
						Out.Log( "} Parms;\r\n");

						// Parms struct initialization.
						for( It=TFieldIterator<UProperty>(*Function); It && (It->PropertyFlags&(CPF_Parm|CPF_ReturnParm))==CPF_Parm; ++It )
						{
							UStringProperty* StringProp = Cast<UStringProperty>( *It );
							if( StringProp )
								Out.Logf( "        appStrncpy(Parms.%s,%s,%i);\r\n", It->GetName(), It->GetName(), StringProp->StringSize );
							else
								Out.Logf( "        Parms.%s=%s;\r\n", It->GetName(), It->GetName() );
						}
						if( Return )
							Out.Logf( "        Parms.%s=0;\r\n", Return->GetName() );
						Out.Logf( "        ProcessEvent(FindFunctionChecked(%s_%s),&Parms);\r\n", API, Function->GetName() );
					}
					else Out.Logf( "        ProcessEvent(FindFunctionChecked(%s_%s),NULL);\r\n", API, Function->GetName() );

					// Out parm copying.
					for( It=TFieldIterator<UProperty>(*Function); It && (It->PropertyFlags&(CPF_Parm|CPF_ReturnParm))==CPF_Parm; ++It )
					{
						if( It->PropertyFlags & CPF_OutParm )
						{
							if( It->IsA(UStringProperty::StaticClass) )
								Out.Logf( "        appStrcpy(%s,Parms.%s);\r\n", It->GetName(), It->GetName() );
							else
								Out.Logf( "        %s=Parms.%s;\r\n", It->GetName(), It->GetName() );
						}
					}

					// Return value.
					if( Return )
						Out.Logf( "        return Parms.%s;\r\n", Return->GetName() );
					Out.Log( "    }\r\n" );
				}
			}

			// Code.
			if( 1 )
			{
				Out.Logf( "    DECLARE_CLASS(%s,", GetNameCPP() ); //warning: GetNameCPP uses static storage.
				Out.Logf( "%s,0", GetSuperClass()->GetNameCPP() );
				if( ClassFlags & CLASS_Transient      )
					Out.Log("|CLASS_Transient"      );
				if( ClassFlags & CLASS_Config     )
					Out.Log("|CLASS_Config"     );
				Out.Logf( ")\r\n" );
				char Filename[256];
				appSprintf( Filename, "..\\%s\\Inc\\%s.h", GetParent()->GetName(), GetNameCPP() );
				if( appFSize(Filename) > 0 )
					Out.Logf( "    #include \"%s.h\"\r\n", GetNameCPP() );
				else
					Out.Logf( "    NO_DEFAULT_CONSTRUCTOR(%s)\r\n", GetNameCPP() );
			}
			Out.Logf( "};\r\n\r\n" );
		}

		// Export all child classes that are tagged for export.
		RecursionDepth++;
		for( TObjectIterator<UClass> It; It; ++It )
			if( It->GetSuperClass()==this )
				It->Export( Out, FileType, Indent );
		RecursionDepth--;

		// Finish C++ header.
		if( RecursionDepth==0 )
		{
			Out.Logf( "#undef DECLARE_NAME\r\n" );
			Out.Logf( "#endif\r\n" );
			Out.Logf( "#pragma pack (pop)\r\n" );
		}
	}
	else if( appStricmp(FileType,"UC")==0 )
	{
		// Export script text.
		check(Defaults.Num());
		check(ScriptText!=NULL);
		ScriptText->Export( Out, FileType, Indent );

		// Export default properties that differ from parent's.
		Out.Log( "\r\n\r\ndefaultproperties\r\n{\r\n" );
		GObj.ExportProperties
		(
			this,
			&Defaults(0),
			&Out,
			Indent+4,
			GetSuperClass(),
			GetSuperClass() ? &GetSuperClass()->Defaults(0) : NULL
		);
		Out.Log( "}\r\n" );
	}
	unguardobj;
}
void UClass::Destroy()
{
	guard(UClass::Destroy);

	// Free replication links.
	FRepLink* Next;
	for( FRepLink* Link=Reps; Link; Link=Next )
	{
		Next = Link->Next;
		delete Link;
	}

	Super::Destroy();
	unguard;
}
void UClass::PostLoad()
{
	guard(UClass::PostLoad);
	Super::PostLoad();

	// Bind with C++.
	Bind();

	// Update default bin.
	if( Defaults.Num() )
	{
		*((void**)&Defaults(0)) = *((void**)GetParent());
		((UObject*)&Defaults(0))->SetClass( this );
	}

	// Build linked list of replicated properties.
	if( !Reps )
	{
		for( TFieldIterator<UProperty> It(this); It && It->GetOwnerClass()==this; ++It )
		{
			if( It->PropertyFlags & CPF_Net )
			{
				Reps = new FRepLink( *It, Reps );
				for( FRepLink* Other=Reps; Other; Other=Other->Next )
					if( Reps->Property->RepOffset==Other->Property->RepOffset )
						Reps->Condition = Other;
			}
		}
	}
	unguardobj;
}
void UClass::Serialize( FArchive& Ar )
{
	guard(UClass::Serialize);
	// x64 port: loading a package serializes over already-registered intrinsic
	// classes and recomputes their property layout from script declarations. If
	// that layout no longer matches the compiled C++ layout, every instance
	// would corrupt memory — fail loudly instead.
	DWORD WasIntrinsic = GetFlags() & RF_Intrinsic;
	INT   CppSize      = PropertiesSize;

	UState::Serialize( Ar );

	if( Ar.IsLoading() && WasIntrinsic && CppSize && PropertiesSize!=CppSize )
		appErrorf( "Class %s script layout size %i != C++ layout size %i", GetPathName(), PropertiesSize, CppSize );

	// x64 port: replication condition offsets of this class's properties and
	// functions point into the class bytecode, which was just translated.
	if( Ar.IsLoading() && Ar.Ver()<VER_SCRIPT_X64 && Xlat )
	{
		for( UField* Field=Children; Field; Field=Field->Next )
		{
			UProperty* Property = Cast<UProperty>( Field );
			if( Property && (Property->PropertyFlags&CPF_Net) && Property->RepOffset!=MAXWORD )
				Property->RepOffset = XlatWordOffset( Property->RepOffset );
			UFunction* Function = Cast<UFunction>( Field );
			if( Function && (Function->FunctionFlags&FUNC_Net) && Function->RepOffset!=MAXWORD )
				Function->RepOffset = XlatWordOffset( Function->RepOffset );
		}
	}

	// Variables.
	Ar << ClassRecordSize << ClassFlags << ClassGuid;
	Ar << Dependencies << PackageImports;

	// Defaults.
	if( Ar.IsLoading() )
	{
		Defaults.SetNum(GetPropertiesSize());
		check(Defaults.Num()>=sizeof(UObject));
		if( GetSuperClass() )
			Ar.Preload( GetSuperClass() );
		appMemset( &Defaults(0), 0, sizeof(UObject) );
		GObj.InitProperties( this, Defaults.Num() ? &Defaults(0) : NULL, Defaults.Num(), GetSuperClass(), NULL, 0 );
		DefaultPropText = NULL;
		ClassUnique = 0;
		SerializeTaggedProperties( Ar, &Defaults(0), GetSuperClass() );
		GetDefaultObject()->SetClass(this);
		GetDefaultObject()->LoadConfig(NAME_Config);
		GetDefaultObject()->LoadConfig(NAME_Localized);
	}
	else if( Ar.IsSaving() )
	{
		check(Defaults.Num()==0 || Defaults.Num()==GetPropertiesSize());
		if( Defaults.Num() )
			SerializeTaggedProperties( Ar, &Defaults(0), GetSuperClass() );
	}
	else
	{
		Ar << DefaultPropText;
		if( Defaults.Num() )
		{
			Defaults.SetNum(GetPropertiesSize());
			SerializeBin( Ar, &Defaults(0) );
		}
	}
	if( Ar.Ver() < 57 )//oldver
	{
		FName X, Y;
		Ar << X << Y;
	}
	unguardobj;
}

/*-----------------------------------------------------------------------------
	UClass constructors.
-----------------------------------------------------------------------------*/

//
// Create a new UClass given its parent.
//
UClass::UClass( UClass* InBaseClass )
:	UState( InBaseClass )
{
	guard(UClass::UClass);

	// Copy defaults from superclass.
	if( GetSuperClass() )
		Defaults = GetSuperClass()->Defaults;

	// Find C++ class, if any.
	if( GetSuperClass() )
	{
		Bind();		
		ClassRecordSize = GetSuperClass()->ClassRecordSize;
	}

	// Done creating class.
	unguardobj;
}

//
// UClass autoregistry constructor.
//warning: Called at DLL init time.
//
UClass::UClass
(
	DWORD		InSize,
	DWORD		InRecordSize,
	DWORD		InClassFlags,
	UClass*		InSuperClass,
	FGuid		InGuid,
	const char*	InNameStr,
	FName		InPackageName,
	void		(*InConstructor)(void*),
	void		(*InClassInitializer)(UClass*)
)
:	UState( EC_IntrinsicConstructor, InSize, FName(InNameStr+1), InPackageName )
,	Reps  ( NULL )
{
	guard(UClass::UClass);

	// Set object flags.
	SetFlags( RF_Public | RF_Standalone | RF_Transient | RF_Intrinsic );

	// Set info.
	Constructor			= InConstructor;
	ClassInitializer    = InClassInitializer;
	ClassRecordSize		= InRecordSize;
	ClassFlags			= InClassFlags | CLASS_Parsed | CLASS_Compiled;
	SuperField			= InSuperClass!=this ? InSuperClass : NULL;
	ClassGuid			= InGuid;

	// Init defaults.
	Defaults.SetNum( InSize );
	appMemset( &Defaults(0), 0, InSize );
	((UObject*)&Defaults(0))->SetClass( this );

	// Call the initializer.
	if( GObj.GetInitialized() )
	{
		ClassInitializer( this );
		GetDefaultObject()->LoadConfig( NAME_Config );
		GetDefaultObject()->LoadConfig( NAME_Localized );
	}

	unguard;
}

IMPLEMENT_CLASS(UClass);

/*-----------------------------------------------------------------------------
	FDependency.
-----------------------------------------------------------------------------*/

//
// FDepdendency inlines.
//
FDependency::FDependency()
{}
FDependency::FDependency( UClass* InClass, UBOOL InDeep )
:	Class( InClass )
,	Deep( InDeep )
,	ScriptTextCRC( Class ? Class->GetScriptTextCRC() : 0 )
{}
UBOOL FDependency::IsUpToDate()
{
	guard(FDependency::IsUpToDate);
	check(Class!=NULL);
	return Class->GetScriptTextCRC()==ScriptTextCRC;
	unguard;
}
CORE_API FArchive& operator<<( FArchive& Ar, FDependency& Dep )
{
	return Ar << Dep.Class << Dep.Deep << Dep.ScriptTextCRC;
}

/*-----------------------------------------------------------------------------
	FLabelEntry.
-----------------------------------------------------------------------------*/

FLabelEntry::FLabelEntry( FName InName, INT iInCode )
:	Name	(InName)
,	iCode	(iInCode)
{}
CORE_API FArchive& operator<<( FArchive& Ar, FLabelEntry &Label )
{
	Ar << Label.Name;
	Ar << Label.iCode;
	return Ar;
}

/*-----------------------------------------------------------------------------
	UStruct implementation.
-----------------------------------------------------------------------------*/

//
// Serialize an expression to an archive.
// Returns expression token.
//
EExprToken UStruct::SerializeExpr( INT& iCode, FArchive& Ar )
{
	EExprToken Expr=(EExprToken)0;
	guard(SerializeExpr);
	// x64 port: XFER routes through XlatStep so the translating loader can track
	// the old 32-bit code offset alongside the widened one. XFERPTR is for
	// object references embedded in bytecode: 4 bytes in old files, pointer
	// size in memory.
	// x64 port / security: XFERBOUND guards the write against the end of the
	// Script buffer. A single expression can advance iCode arbitrarily (the
	// outer loop only re-tests between top-level exprs), so a truncated/crafted
	// final expression would otherwise write attacker bytes past the buffer.
	// This protects the non-translating path (Ver>=VER_SCRIPT_X64, where
	// Script is sized exactly) as well as the translating one.
	#define XFERBOUND(T) { if( iCode+(INT)sizeof(T) > Script.Num() ) appErrorf( "Script overrun in %s", GetFullName() ); }
	#define XFER(T) {XFERBOUND(T); Ar << *(T*)&Script(iCode); XlatStep( iCode, sizeof(T), sizeof(T) ); }
	#define XFERPTR(T) {XFERBOUND(T); Ar << *(T*)&Script(iCode); XlatStep( iCode, 4, sizeof(T) ); }

	// x64 port: result-type descriptor of this expression, reported to the
	// parent expression through Xlat->LastExpr for deferred value-size fixups.
	FScriptXlat::FExprSize This;
	This.Kind  = FScriptXlat::SIZE_Unknown;
	This.Value = 0;

	// Get expr token.
	XFER(BYTE);
	Expr = (EExprToken)Script(iCode-1);
	if( Expr >= EX_MinConversion && Expr < EX_MaxConversion )
	{
		// A type conversion.
		SerializeExpr( iCode, Ar );
	}
	else if( Expr >= EX_FirstIntrinsic )
	{
		// Intrinsic final function with id 1-127.
		while( SerializeExpr( iCode, Ar ) != EX_EndFunctionParms );
	}
	else if( Expr >= EX_ExtendedIntrinsic )
	{
		// Intrinsic final function with id 128-16383.
		XFER(BYTE);
		while( SerializeExpr( iCode, Ar ) != EX_EndFunctionParms );
	}
	else switch( Expr )
	{
		case EX_LocalVariable:
		case EX_InstanceVariable:
		case EX_DefaultVariable:
		{
			XFERPTR(UProperty*);
			if( Xlat )
			{
				This.Kind  = FScriptXlat::SIZE_Property;
				This.Value = iCode - sizeof(UProperty*);
			}
			break;
		}
		case EX_BoolVariable:
		case EX_ValidateObject:
		case EX_Nothing:
		case EX_EndFunctionParms:
		case EX_IntZero:
		case EX_IntOne:
		case EX_True:
		case EX_False:
		case EX_NoObject:
		case EX_Self:
		case EX_IteratorPop:
		case EX_EndCode:
		case EX_Stop:
		case EX_Return:
		case EX_IteratorNext:
		{
			break;
		}
		case EX_IntrinsicParm:
		{
			XFERPTR(UProperty*);
			break;
		}
		case EX_ClassContext:
		case EX_Context:
		{
			SerializeExpr( iCode, Ar ); // Object expression.
			XFER(_WORD); // Code offset for NULL expressions.
			INT WordPos = iCode - sizeof(_WORD);
			XFER(BYTE); // Zero-fill size if skipped.
			INT SizePos = iCode - 1;
			// x64 port: the skip word is relative to the position after the size
			// byte (execContext: Code += wSkip), and the zero-fill size depends
			// on the member expression's result type; record both for fixup.
			if( Xlat )
			{
				FScriptXlat::FRelWord R;
				R.WordPos   = WordPos;
				R.OldAnchor = Xlat->OldICode;
				R.NewAnchor = iCode;
				Xlat->RelWords.AddItem( R );
			}
			SerializeExpr( iCode, Ar ); // Context expression.
			if( Xlat )
			{
				FScriptXlat::FSizeFix F;
				F.SizePos = SizePos;
				F.Expr    = Xlat->LastExpr;
				Xlat->SizeFixes.AddItem( F );
				This = Xlat->LastExpr; // context result is the member's result
			}
			break;
		}
		case EX_ArrayElement:
		{
			SerializeExpr( iCode, Ar ); // Index expression.
			SerializeExpr( iCode, Ar ); // Base expression.
			if( Xlat )
				This = Xlat->LastExpr; // element of the base variable's type
			break;
		}
		case EX_VirtualFunction:
		case EX_GlobalFunction:
		{
			XFER(FName); // Virtual function name.
			while( SerializeExpr( iCode, Ar ) != EX_EndFunctionParms ); // Parms.
			break;
		}
		case EX_FinalFunction:
		{
			XFERPTR(UStruct*); // Stack node.
			INT FuncPos = iCode - sizeof(UStruct*);
			while( SerializeExpr( iCode, Ar ) != EX_EndFunctionParms ); // Parms.
			if( Xlat )
			{
				This.Kind  = FScriptXlat::SIZE_Function;
				This.Value = FuncPos;
			}
			break;
		}
		case EX_IntConst:
		{
			XFER(INT);
			break;
		}
		case EX_FloatConst:
		{
			XFER(FLOAT);
			break;
		}
		case EX_StringConst:
		{
			do XFER(BYTE) while( Script(iCode-1) );
			break;
		}
		case EX_ObjectConst:
		{
			XFERPTR(UObject*);
			This.Kind  = FScriptXlat::SIZE_Fixed;
			This.Value = sizeof(UObject*);
			break;
		}
		case EX_NameConst:
		{
			XFER(FName);
			break;
		}
		case EX_RotationConst:
		{
			XFER(INT); XFER(INT); XFER(INT);
			break;
		}
		case EX_VectorConst:
		{
			XFER(FLOAT); XFER(FLOAT); XFER(FLOAT);
			break;
		}
		case EX_ByteConst:
		case EX_IntConstByte:
		{
			XFER(BYTE);
			break;
		}
		case EX_ResizeString:
		{
			XFER(BYTE);
			SerializeExpr( iCode, Ar );
			break;
		}
		case EX_MetaCast:
		{
			XFERPTR(UClass*);
			SerializeExpr( iCode, Ar );
			This.Kind  = FScriptXlat::SIZE_Fixed;
			This.Value = sizeof(UObject*);
			break;
		}
		case EX_DynamicCast:
		{
			XFERPTR(UClass*);
			SerializeExpr( iCode, Ar );
			This.Kind  = FScriptXlat::SIZE_Fixed;
			This.Value = sizeof(UObject*);
			break;
		}
		case EX_JumpIfNot:
		{
			XFER(_WORD); // Code offset.
			if( Xlat )
				Xlat->AbsWords.AddItem( iCode-(INT)sizeof(_WORD) );
			SerializeExpr( iCode, Ar ); // Boolean expr.
			break;
		}
		case EX_Iterator:
		{
			SerializeExpr( iCode, Ar ); // Iterator expr.
			XFER(_WORD); // Code offset.
			if( Xlat )
				Xlat->AbsWords.AddItem( iCode-(INT)sizeof(_WORD) );
			break;
		}
		case EX_Switch:
		{
			XFER(BYTE); // Value size.
			INT SizePos = iCode - 1;
			SerializeExpr( iCode, Ar ); // Switch expr.
			// x64 port: the value size is used to memcmp case values; object
			// results widened, so record for fixup (0 = string switch, kept).
			if( Xlat )
			{
				FScriptXlat::FSizeFix F;
				F.SizePos = SizePos;
				F.Expr    = Xlat->LastExpr;
				Xlat->SizeFixes.AddItem( F );
			}
			break;
		}
		case EX_Jump:
		{
			XFER(_WORD); // Code offset.
			if( Xlat )
				Xlat->AbsWords.AddItem( iCode-(INT)sizeof(_WORD) );
			break;
		}
		case EX_Assert:
		{
			XFER(_WORD); // Line number.
			SerializeExpr( iCode, Ar ); // Assert expr.
			break;
		}
		case EX_Case:
		{
			_WORD *W=(_WORD*)&Script(iCode);
			XFER(_WORD);; // Code offset.
			if( Xlat )
				Xlat->AbsWords.AddItem( iCode-(INT)sizeof(_WORD) );
			if( *W != MAXWORD )
				SerializeExpr( iCode, Ar ); // Boolean expr.
			break;
		}
		case EX_LabelTable:
		{
			// x64 port: widened code positions lose the 4-byte alignment the
			// compiler arranged in 32-bit space; unaligned access is fine on
			// x64, so only check alignment in the untranslated old space.
			check(Xlat ? (Xlat->OldICode&3)==0 : (iCode&3)==0);
			for( ; ; )
			{
				FLabelEntry* E = (FLabelEntry*)&Script(iCode);
				XFER(FLabelEntry);
				if( Xlat && E->Name != NAME_None )
					Xlat->LabelInts.AddItem( iCode-(INT)sizeof(INT) );
				if( E->Name == NAME_None )
					break;
			}
			break;
		}
		case EX_GotoLabel:
		{
			SerializeExpr( iCode, Ar ); // Label name expr.
			break;
		}
		case EX_Let:
		{
			SerializeExpr( iCode, Ar ); // Variable expr.
			SerializeExpr( iCode, Ar ); // Assignment expr.
			break;
		}
		case EX_Skip:
		{
			XFER(_WORD); // Skip size.
			// x64 port: relative to the position right after this word
			// (P_GET_SKIP_OFFSET then Code += W); the skipped region widens.
			if( Xlat )
			{
				FScriptXlat::FRelWord R;
				R.WordPos   = iCode - sizeof(_WORD);
				R.OldAnchor = Xlat->OldICode;
				R.NewAnchor = iCode;
				Xlat->RelWords.AddItem( R );
			}
			SerializeExpr( iCode, Ar ); // Expression to possibly skip.
			break;
		}
		case EX_BeginFunction:
		{
			for( ; ; )
			{
				XFER(BYTE); // Parm size.
				if( Script(iCode-1) == 0 )
					break;
				XFER(BYTE); // OutParm flag.
			}
			break;
		}
		case EX_StructCmpEq:
		case EX_StructCmpNe:
		{
			XFERPTR(UStruct*); // Struct.
			SerializeExpr( iCode, Ar ); // Left expr.
			SerializeExpr( iCode, Ar ); // Right expr.
			break;
		}
		case EX_StructMember:
		{
			XFERPTR(UProperty*); // Property.
			INT PropPos = iCode - sizeof(UProperty*);
			SerializeExpr( iCode, Ar ); // Inner expr.
			if( Xlat )
			{
				This.Kind  = FScriptXlat::SIZE_Property;
				This.Value = PropPos;
			}
			break;
		}
		default:
		{
			// This should never occur.
			appErrorf( "Bad expr token %02x", Expr );
			break;
		}
	}
	// x64 port: report this expression's result-type descriptor to the parent.
	if( Xlat )
		Xlat->LastExpr = This;
	return Expr;
	#undef XFER
	#undef XFERPTR
	#undef XFERBOUND
	unguardf(( "(%02X)", Expr ));
}

void UStruct::PostLoad()
{
	guard(UStruct::PostLoad);
	UField::PostLoad();
	// x64 port: apply deferred value-size fixups now that every referenced
	// property and function has been fully loaded and linked, then drop the
	// translation state.
	if( Xlat )
	{
		for( INT i=0; i<Xlat->SizeFixes.Num(); i++ )
		{
			FScriptXlat::FSizeFix& F = Xlat->SizeFixes(i);
			INT NewSize = ResolveExprSize( F.Expr );
			BYTE& B = Script(F.SizePos);
			if( NewSize>0 && B!=0 && B!=NewSize )
			{
				if( NewSize>255 )
					appErrorf( "Translated value size %i too large in %s", NewSize, GetFullName() );
				B = NewSize;
			}
		}
		delete Xlat;
		Xlat = NULL;
	}
	if( !GIsEditor )
		BuildVfHashes( this );
	unguard;
}

/*-----------------------------------------------------------------------------
	UFunction.
-----------------------------------------------------------------------------*/

UFunction::UFunction( UFunction* InSuperFunction )
: UStruct( InSuperFunction )
{}
void UFunction::Serialize( FArchive& Ar )
{
	guard(UFunction::Serialize);
	UStruct::Serialize( Ar );

	// Function info.
	Ar << ParmsSize << iIntrinsic;
	Ar << NumParms << OperPrecedence;
	Ar << ReturnValueOffset << FunctionFlags;

	// Replication info.
	if( FunctionFlags & FUNC_Net )
		Ar << RepOffset;

	// x64 port: 32-bit-era files store frame sizes computed with 4-byte object
	// references. LinkOffsets already recomputed the property offsets natively,
	// so rebuild ParmsSize/ReturnValueOffset and the EX_BeginFunction parm-size
	// bytes (which CallFunction uses to advance through the parms frame) from
	// the freshly linked parameter properties.
	if( Ar.IsLoading() && Ar.Ver()<VER_SCRIPT_X64 )
	{
		ParmsSize         = 0;
		ReturnValueOffset = MAXWORD;
		for( TFieldIterator<UProperty> It(this); It && (It->PropertyFlags&CPF_Parm); ++It )
		{
			if( It->Offset+It->GetSize() > MAXWORD )
				appErrorf( "Parms of %s too large after translation", GetFullName() );
			ParmsSize = It->Offset + It->GetSize();
			if( It->PropertyFlags & CPF_ReturnParm )
				ReturnValueOffset = It->Offset;
		}
		if( Script.Num() && Script(0)==EX_BeginFunction )
		{
			INT Pos = 1;
			for( TFieldIterator<UProperty> It(this); It && (It->PropertyFlags&(CPF_Parm|CPF_ReturnParm))==CPF_Parm; ++It )
			{
				// Mirror the compiler: parm size plus padding up to the next parm.
				UProperty* Property = *It;
				INT Size = Property->GetSize();
				TFieldIterator<UProperty> Next(It);
				++Next;
				if( Next && (Next->PropertyFlags&(CPF_Parm|CPF_ReturnParm))==CPF_Parm )
					Size = Next->Offset - Property->Offset;
				if( Size<1 || Size>255 )
					appErrorf( "Parameter %s of %s has bad translated size %i", Property->GetName(), GetFullName(), Size );
				if( Pos+1>=Script.Num() || Script(Pos)==0 )
					appErrorf( "Parm info mismatch translating %s", GetFullName() );
				Script(Pos) = Size;
				Pos += 2;
			}
			if( Pos>=Script.Num() || Script(Pos)!=0 )
				appErrorf( "Parm info terminator mismatch translating %s", GetFullName() );
		}
	}

	unguard;
}
void UFunction::PostLoad()
{
	guard(UFunction::PostLoad);
	UStruct::PostLoad();
	unguard;
}
UProperty* UFunction::GetReturnProperty()
{
	guard(UFunction::GetReturnProperty);
	for( TFieldIterator<UProperty> It(this); It && (It->PropertyFlags & CPF_Parm); ++It )
		if( It->PropertyFlags & CPF_ReturnParm )
			return *It;
	return NULL;
	unguard;
}
void UFunction::Bind()
{
	guard(UFunction::Bind);
	if( FunctionFlags & FUNC_Intrinsic )
	{
		if( iIntrinsic != 0 )
		{
			// Find hardcoded intrinsic.
			check(iIntrinsic<EX_Max);
			check(GIntrinsics[iIntrinsic]!=NULL);
			Func = GIntrinsics[iIntrinsic];
		}
		else
		{
			// Find dynamic intrinsic.
			char Proc[256];
			appSprintf( Proc, "int%sexec%s", GetOwnerClass()->GetNameCPP(), GetName() );
			UPackage* ClassPackage = CastChecked<UPackage>( GetOwnerClass()->GetParent() );
			void** Ptr = (void**)ClassPackage->GetDllExport( Proc, 1 );
			if( Ptr )
				*(void**)&Func = *Ptr;
		}
	}
	else check(iIntrinsic==0);
	unguard;
}
IMPLEMENT_CLASS(UFunction);

/*-----------------------------------------------------------------------------
	UState.
-----------------------------------------------------------------------------*/

UState::UState( UState* InSuperState )
: UStruct( InSuperState )
{}
UState::UState( EIntrinsicConstructor, INT InSize, FName InName, FName InPackageName )
:	UStruct( EC_IntrinsicConstructor, InSize, InName, InPackageName )
,	ProbeMask( NULL )
,	IgnoreMask( NULL )
,	VfHash( NULL )
,	StateFlags( NULL )
,	LabelTableOffset( NULL )
{}
void UState::Destroy()
{
	guard(UState::Destroy);
	if( VfHash )
		delete VfHash;
	UStruct::Destroy();
	unguard;
}
void UState::Serialize( FArchive& Ar )
{
	guard(UState::Serialize);
	UStruct::Serialize( Ar );

	// Class/State-specific union info.
	Ar << ProbeMask << IgnoreMask;
	Ar << LabelTableOffset << StateFlags;
	if( Ar.IsLoading() )
		VfHash = NULL;

	// x64 port: the label table offset was written in 32-bit bytecode space.
	if( Ar.IsLoading() && Ar.Ver()<VER_SCRIPT_X64 && Xlat && LabelTableOffset!=MAXWORD )
		LabelTableOffset = XlatWordOffset( LabelTableOffset );

	unguard;
}
IMPLEMENT_CLASS(UState);

/*-----------------------------------------------------------------------------
	UConst.
-----------------------------------------------------------------------------*/

UConst::UConst( UConst* InSuperConst, const char* InValue )
:	UField( InSuperConst )
,	Value( InValue )
{}
void UConst::Serialize( FArchive& Ar )
{
	guard(UConst::Serialize);
	UField::Serialize( Ar );
	Ar << Value;
	unguard;
}
IMPLEMENT_CLASS(UConst);

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
