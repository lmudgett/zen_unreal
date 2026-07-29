/*=============================================================================
	UnClass.h: UClass definition.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney.
=============================================================================*/

/*-----------------------------------------------------------------------------
	Constants.
-----------------------------------------------------------------------------*/

// Boundary to align class properties on.
enum {PROPERTY_ALIGNMENT=4 };

/*-----------------------------------------------------------------------------
	FDependency.
-----------------------------------------------------------------------------*/

//
// One dependency record, for incremental compilation.
//
class CORE_API FDependency
{
public:
	// Variables.
	UClass*		Class;
	UBOOL		Deep;
	DWORD		ScriptTextCRC;

	// Functions.
	FDependency();
	FDependency( UClass* InClass, UBOOL InDeep );
	UBOOL IsUpToDate();
	CORE_API friend FArchive& operator<<( FArchive& Ar, FDependency& Dep );
};

/*-----------------------------------------------------------------------------
	FRepLink.
-----------------------------------------------------------------------------*/

//
// A tagged linked list of replicatable variables.
//
class FRepLink
{
public:
	UProperty*	Property;		// Replicated property.
	FRepLink*   Condition;		// Link with original condition.
	FRepLink*	Next;			// Next replicated link per class.
	UObject*	LastObject;		// Most recently evaluated actor.
	INT			LastStamp;		// Most recently evaluated actor timestamp.
	UBOOL		LastResult;		// Whether last result was replicated.
	FRepLink( UProperty* InProperty, FRepLink* InNext )
	:	Property	(InProperty)
	,	Next		(InNext)
	,	Condition	(NULL)
	,	LastObject	(NULL)
	,	LastStamp	(NULL)
	,	LastResult	(0)
	{}
};

/*-----------------------------------------------------------------------------
	FLabelEntry.
-----------------------------------------------------------------------------*/

//
// Entry in a state's label table.
//
struct CORE_API FLabelEntry
{
	// Variables.
	FName	Name;
	INT		iCode;

	// Functions.
	FLabelEntry( FName InName, INT iInCode );
	CORE_API friend FArchive& operator<<( FArchive& Ar, FLabelEntry &Label );
};

/*-----------------------------------------------------------------------------
	UField.
-----------------------------------------------------------------------------*/

//
// Base class of UnrealScript language objects.
//
class CORE_API UField : public UObject
{
	DECLARE_ABSTRACT_CLASS(UField,UObject,0)
	NO_DEFAULT_CONSTRUCTOR(UField)

	// Constants.
	enum {HASH_COUNT = 256};

	// Variables.
	UField*			SuperField;
	UField*			Next;
	UField*			HashNext;

	// Constructors.
	UField( EIntrinsicConstructor, UClass* InClass, FName InName, FName InPackageName );
	UField( UField* InSuperField );

	// UObject interface.
	void Serialize( FArchive& Ar );
	void PostLoad();
	
	// UField interface.
	virtual void Bind();
	virtual UClass* GetOwnerClass();
};

/*-----------------------------------------------------------------------------
	TFieldIterator.
-----------------------------------------------------------------------------*/

//
// For iterating through a linked list of fields.
//
template <class T> class TFieldIterator
{
public:
	TFieldIterator( UStruct* InStruct )
	: Struct( InStruct )
	, Field( InStruct ? InStruct->Children : NULL )
	{
		IterateToNext();
	}
	operator UBOOL()
	{
		return Field != NULL;
	}
	void operator++()
	{
		debug(Field);
		Field = Field->Next;
		IterateToNext();
	}
	T* operator*()
	{
		debug(Field);
		return (T*)Field;
	}
	T* operator->()
	{
		debug(Field);
		return (T*)Field;
	}
	UStruct* GetStruct()
	{
		return Struct;
	}
protected:
	void IterateToNext()
	{
		while( Struct )
		{
			while( Field )
			{
				if( Field->IsA(T::StaticClass) )
					return;
				Field = Field->Next;
			}
			Struct = Struct->GetInheritanceSuper();
			if( Struct )
				Field = Struct->Children;
		}
	}
	UStruct* Struct;
	UField* Field;
};

/*-----------------------------------------------------------------------------
	FScriptXlat.
-----------------------------------------------------------------------------*/

//
// x64 port: state for the translating loader that converts 32-bit-era script
// bytecode (packages older than VER_SCRIPT_X64) to the widened in-memory form.
// Object references in bytecode grow from 4 bytes to pointer size, so every
// code offset baked into the bytecode must be remapped from the old 32-bit
// offset space to the widened one. Owned by a UStruct while it loads; the
// offset map stays alive until PostLoad because UState::LabelTableOffset,
// replication offsets (UClass) and deferred value-size fixups need it.
//
struct CORE_API FScriptXlat
{
	// Result-type descriptor for a just-serialized expression; used to fix up
	// value-size operands (EX_Context zero-fill size, EX_Switch value size)
	// whose 32-bit sizes are stale for object/struct results on x64.
	enum EExprSizeKind
	{
		SIZE_Unknown	= 0,	// leave the old size byte alone
		SIZE_Fixed		= 1,	// Value is the size itself
		SIZE_Property	= 2,	// Value is a new-Script offset of a UProperty* operand
		SIZE_Function	= 3,	// Value is a new-Script offset of a UStruct* (UFunction) operand
	};
	struct FExprSize
	{
		BYTE Kind;
		INT  Value;
	};
	struct FSizeFix
	{
		INT       SizePos;	// new-Script offset of the BYTE to rewrite
		FExprSize Expr;
	};
	struct FRelWord
	{
		INT WordPos;		// new-Script offset of the relative skip _WORD
		INT OldAnchor;		// old-space offset the skip is relative to
		INT NewAnchor;		// new-space offset the skip is relative to
	};

	TArray<INT>      Map;		// old bytecode offset -> new bytecode offset, size OldScriptSize+1
	TArray<INT>      AbsWords;	// new-Script offsets of absolute code-offset _WORDs (MAXWORD = sentinel, skipped)
	TArray<FRelWord> RelWords;	// relative skip _WORDs (EX_Skip, EX_Context, EX_ClassContext)
	TArray<INT>      LabelInts;	// new-Script offsets of FLabelEntry::iCode INTs
	TArray<FSizeFix> SizeFixes;	// value-size BYTEs to recompute in PostLoad
	FExprSize        LastExpr;	// descriptor of the most recently serialized expression
	INT              OldICode;	// current offset in the old 32-bit code space
};

/*-----------------------------------------------------------------------------
	UStruct.
-----------------------------------------------------------------------------*/

//
// An UnrealScript structure definition.
//
class CORE_API UStruct : public UField
{
	DECLARE_CLASS(UStruct,UField,0)
	NO_DEFAULT_CONSTRUCTOR(UStruct)

	// Variables.
	UTextBuffer*		ScriptText;
	UField*				Children;
	INT					PropertiesSize;
	FName				FriendlyName;
	INT					TextPos;
	INT					Line;
	TArray<BYTE>		Script;
	UObjectProperty*	RefLink;
	UStructProperty*	StructLink;
	FScriptXlat*		Xlat = NULL;	// x64 port: transient, non-NULL from Serialize until PostLoad when translating old bytecode
	INT					CppLayoutSize = 0;	// x64 port: transient; registered C++ sizeof for intrinsic classes — the layout truth
											// LinkOffsets pins to. PropertiesSize can't serve as the pin source because the
											// script compiler zeroes/overwrites it mid-parse before relinking.

	// Constructors.
	UStruct( EIntrinsicConstructor, INT InSize, FName InName, FName InPackageName );
	UStruct( UStruct* InSuperStruct );

	// UObject interface.
	void Serialize( FArchive& Ar );
	void PostLoad();

	// UStruct interface.
	virtual UBOOL MergeBools() {return 1;}
	virtual UStruct* GetInheritanceSuper() {return GetSuperStruct();}
	virtual void LinkOffsets( FArchive& Ar );
	virtual void SerializeBin( FArchive& Ar, BYTE* Data );
	virtual void SerializeTaggedProperties( FArchive& Ar, BYTE* Data, UClass* DefaultsClass );
	virtual void CleanupDestroyed( BYTE* Data );
	virtual EExprToken SerializeExpr( INT& iCode, FArchive& Ar );
	// x64 port: translating-loader helpers (see FScriptXlat above).
	// Advances iCode by NewWidth; when translating, also records the old->new
	// offset map for the OldWidth bytes this transfer occupied in old space.
	void XlatStep( INT& iCode, INT OldWidth, INT NewWidth )
	{
		if( Xlat )
		{
			if( Xlat->OldICode+OldWidth > Xlat->Map.Num()-1 )
				appErrorf( "Script overrun translating %s: %i+%i/%i", GetFullName(), Xlat->OldICode, OldWidth, Xlat->Map.Num()-1 );
			for( INT k=0; k<OldWidth; k++ )
				Xlat->Map(Xlat->OldICode+k) = iCode + Min(k,NewWidth-1);
			Xlat->OldICode += OldWidth;
		}
		iCode += NewWidth;
	}
	INT XlatOffset( INT Old );
	_WORD XlatWordOffset( INT Old );
	INT ResolveExprSize( const FScriptXlat::FExprSize& Expr );
	INT GetPropertiesSize()
	{
		return PropertiesSize;
	}
	DWORD GetScriptTextCRC()
	{
		return ScriptText ? appMemCrc((BYTE*)*ScriptText->Text,ScriptText->Text.Length()) : 0;
	}
	void SetPropertiesSize( INT NewSize )
	{
		PropertiesSize = NewSize;
	}
	UBOOL IsChildOf( const UStruct* SomeBase ) const
	{
		guardSlow(UStruct::IsChildOf);
		for( const UStruct* Struct=this; Struct; Struct=Struct->GetSuperStruct() )
			if( Struct==SomeBase ) 
				return 1;
		return 0;
		unguardobjSlow;
	}
	virtual char* GetNameCPP()
	{
		static char Result[NAME_SIZE+1];
		appSprintf( Result, "F%s", GetName() );
		return Result;
	}
	UStruct* GetSuperStruct() const
	{
		guardSlow(UStruct::GetSuperStruct);
		debug(!SuperField||SuperField->IsA(UStruct::StaticClass));
		return (UStruct*)SuperField;
		unguardSlow;
	}
	UBOOL StructCompare( const void* A, const void* B );
};

/*-----------------------------------------------------------------------------
	UFunction.
-----------------------------------------------------------------------------*/

//
// An UnrealScript function.
//
class CORE_API UFunction : public UStruct
{
	DECLARE_CLASS(UFunction,UStruct,0)
	NO_DEFAULT_CONSTRUCTOR(UFunction)

	// Variables.
#if DO_SLOW_GUARD
	SQWORD Calls,Cycles;
#endif
	DWORD FunctionFlags;
	_WORD ParmsSize;
	_WORD iIntrinsic;
	_WORD RepOffset;
	_WORD ReturnValueOffset;
	BYTE  NumParms;
	BYTE  OperPrecedence;
	void (UObject::*Func)( FFrame& Stack, BYTE*& Result );

	// Constructors.
	UFunction( UFunction* InSuperFunction );

	// UObject interface.
	void Serialize( FArchive& Ar );
	void PostLoad();

	// UField interface.
	void Bind();

	// UStruct interface.
	UBOOL MergeBools() {return 0;}
	UStruct* GetInheritanceSuper() {return NULL;}

	// UFunction interface.
	UFunction* GetSuperFunction() const
	{
		guardSlow(UFunction::GetSuperFunction);
		debug(!SuperField||SuperField->IsA(UFunction::StaticClass));
		return (UFunction*)SuperField;
		unguardSlow;
	}
	UProperty* GetReturnProperty();
};

/*-----------------------------------------------------------------------------
	UState.
-----------------------------------------------------------------------------*/

//
// An UnrealScript state.
//
class CORE_API UState : public UStruct
{
	DECLARE_CLASS(UState,UStruct,0)
	NO_DEFAULT_CONSTRUCTOR(UState)

	// Variables.
	QWORD ProbeMask;
	QWORD IgnoreMask;
	UField** VfHash;
	DWORD StateFlags;
	_WORD LabelTableOffset;

	// Constructors.
	UState( EIntrinsicConstructor, INT InSize, FName InName, FName InPackageName );
	UState( UState* InSuperState );

	// UObject interface.
	void Serialize( FArchive& Ar );
	void Destroy();

	// UStruct interface.
	UBOOL MergeBools() {return 1;}
	UStruct* GetInheritanceSuper() {return GetSuperState();}

	// UState interface.
	UState* GetSuperState() const
	{
		guardSlow(UState::GetSuperState);
		debug(!SuperField||SuperField->IsA(UState::StaticClass));
		return (UState*)SuperField;
		unguardSlow;
	}
};

/*-----------------------------------------------------------------------------
	UEnum.
-----------------------------------------------------------------------------*/

//
// An enumeration, a list of names usable by UnrealScript.
//
class CORE_API UEnum : public UField
{
	DECLARE_CLASS(UEnum,UField,0)
	NO_DEFAULT_CONSTRUCTOR(UEnum)

	// Variables.
	TArray<FName> Names;

	// Constructors.
	UEnum( UEnum* InSuperEnum );

	// UObject interface.
	void Export( FOutputDevice& Out, const char* FileType, int Indent );
	void Serialize( FArchive& Ar );

	// UEnum interface.
	UEnum* GetSuperEnum() const
	{
		guardSlow(UEnum::GetSuperEnum);
		debug(!SuperField||SuperField->IsA(UEnum::StaticClass));
		return (UEnum*)SuperField;
		unguardSlow;
	}
};

/*-----------------------------------------------------------------------------
	UClass.
-----------------------------------------------------------------------------*/

//
// An object class.
//
class CORE_API UClass : public UState
{
	DECLARE_CLASS(UClass,UState,0)

	// Variables.
	DWORD				ClassFlags;
	INT					ClassRecordSize;
	INT					ClassUnique;
	FGuid				ClassGuid;
	TArray<FDependency> Dependencies;
	TArray<FName>		PackageImports;
	TArray<BYTE>		Defaults;
	UTextBuffer*		DefaultPropText;
	FRepLink*			Reps;
	void(*Constructor)(void*);
	void(*ClassInitializer)(UClass*);

	// Constructors.
	UClass() {};
	UClass( UClass* InSuperClass );
	UClass( DWORD InSize, DWORD InRecordSize, DWORD InClassFlags, UClass* InBaseClass, FGuid InGuid, const char* InNameStr, FName InPackageName, void(*Constructor)(void*), void(*ClassInitializer)(UClass*) );

	// UObject interface.
	void Serialize( FArchive& Ar );
	void Export( FOutputDevice& Out, const char* FileType, int Indent );
	void PostLoad();
	void Destroy();

	// UField interface.
	void Bind();

	// UStruct interface.
	UBOOL MergeBools() {return 1;}
	UStruct* GetInheritanceSuper() {return GetSuperClass();}
	char* GetNameCPP()
	{
		static char Result[NAME_SIZE+1];
		for( UClass* C=this; C; C=C->GetSuperClass() )
			if( appStricmp(C->GetName(),"Actor")==0 )
				break;
		appSprintf( Result, "%s%s", C ? "A" : "U", GetName() );
		return Result;
	}

	// UClass interface.
	void AddDependency( UClass* InClass, UBOOL InDeep )
	{
		guard(UClass::AddDependency);
		for( INT i=0; i<Dependencies.Num(); i++ )
			if( Dependencies(i).Class==InClass )
				Dependencies(i).Deep |= InDeep;
		if( i==Dependencies.Num() )
			new(Dependencies)FDependency( InClass, InDeep );
		unguard;
	}
	UClass* GetSuperClass() const
	{
		guardSlow(UClass::GetSuperClass);
		return (UClass *)SuperField;
		unguardSlow;
	}
	UObject* GetDefaultObject()
	{
		guardSlow(UClass::GetDefaultObject);
		check(Defaults.Num()==GetPropertiesSize());
		return (UObject*)&Defaults(0);
		unguardobjSlow;
	}
	class AActor* GetDefaultActor()
	{
		guardSlow(UClass::GetDefaultActor);
		check(Defaults.Num());
		return (AActor*)&Defaults(0);
		unguardobjSlow;
	}

private:
	// Hide IsA because calling IsA on a class almost always indicates
	// an error where the caller should use IsChildOf.
	UBOOL IsA( UClass* Parent ) const {return UObject::IsA(Parent);}
};

/*-----------------------------------------------------------------------------
	UConst.
-----------------------------------------------------------------------------*/

//
// An UnrealScript constant.
//
class CORE_API UConst : public UField
{
	DECLARE_CLASS(UConst,UField,0)
	NO_DEFAULT_CONSTRUCTOR(UConst)

	// Variables.
	FString Value;

	// Constructors.
	UConst( UConst* InSuperConst, const char* InValue );

	// UObject interface.
	void Serialize( FArchive& Ar );

	// UConst interface.
	UConst* GetSuperConst() const
	{
		guardSlow(UConst::GetSuperStruct);
		debug(!SuperField||SuperField->IsA(UConst::StaticClass));
		return (UConst*)SuperField;
		unguardSlow;
	}
};

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
