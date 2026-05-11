// SPDX-License-Identifier: MIT
// FMonolithReflectionWalker implementation. Phase 0 framework primitive.

#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithJsonUtils.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/SoftObjectPtr.h"
#include "Misc/StringOutputDevice.h"
#include "Algo/Count.h"

// ---------------------------------------------------------------------------
// Lookup helper. Mirrors MonolithBlueprintCDOActions.cpp:385-396 — exact match
// first, then case-insensitive iteration. Keeps walker behaviour aligned with
// set_cdo_property so adapters don't surprise callers with name-resolution drift.
// ---------------------------------------------------------------------------
FProperty* FMonolithReflectionWalker::FindPropertyForwarding(UStruct* Struct, const FString& Name)
{
	if (!Struct)
	{
		return nullptr;
	}
	FProperty* P = Struct->FindPropertyByName(FName(*Name));
	if (P)
	{
		return P;
	}
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Inner switch — routes a single JSON value to its FProperty subtype handler.
// Order matters: most-derived subclasses tested first (FEnumProperty before the
// generic numeric path), object refs distinguished from soft refs by CastField.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::DispatchByPropertyType(
	FProperty* Prop,
	void* ValuePtr,
	const TSharedPtr<FJsonValue>& JsonVal,
	UObject* Owner,
	const FBulkFillSpec& Spec,
	FDryRunReport& OutReport,
	const FString& PathPrefix,
	FBulkFillFieldWrite& OutWrite)
{
	if (!Prop || !JsonVal.IsValid())
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("null property or json value");
		return;
	}

	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		WriteEnum(EnumProp, ValuePtr, JsonVal, OutWrite);
		return;
	}
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		WriteArray(ArrayProp, ValuePtr, JsonVal, Owner, Spec, OutReport, PathPrefix, OutWrite);
		return;
	}
	if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		WriteMap(MapProp, ValuePtr, JsonVal, Owner, Spec, OutReport, PathPrefix, OutWrite);
		return;
	}
	if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		WriteSet(SetProp, ValuePtr, JsonVal, Owner, Spec, OutReport, PathPrefix, OutWrite);
		return;
	}
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		WriteStruct(StructProp, ValuePtr, JsonVal, Owner, Spec, OutReport, PathPrefix, OutWrite);
		return;
	}
	if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
	{
		WriteSoftObjectRef(SoftProp, ValuePtr, JsonVal, OutWrite);
		return;
	}
	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		WriteObjectRef(ObjProp, ValuePtr, JsonVal, Owner, OutWrite);
		return;
	}
	// Fall-through: scalar (int, float, bool, FName, FString, FText, byte, enum-as-byte).
	WriteScalar(Prop, ValuePtr, JsonVal, Owner, OutWrite);
}

// ---------------------------------------------------------------------------
// Scalar write — stringify JSON then ImportText_Direct (matches existing
// MonolithBlueprintCDOActions.cpp:451-475 path so behaviour matches set_cdo_property).
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteScalar(FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, FBulkFillFieldWrite& OutWrite)
{
	FString ValStr;
	if (JsonVal->Type == EJson::Number)
	{
		ValStr = FString::SanitizeFloat(JsonVal->AsNumber());
	}
	else if (JsonVal->Type == EJson::Boolean)
	{
		ValStr = JsonVal->AsBool() ? TEXT("true") : TEXT("false");
	}
	else if (JsonVal->Type == EJson::Null)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("scalar field cannot be null");
		return;
	}
	else
	{
		ValStr = JsonVal->AsString();
	}

	// Snapshot current value for the report.
	Prop->ExportText_Direct(OutWrite.CurrentValue, ValuePtr, ValuePtr, Owner, PPF_None);
	OutWrite.ProposedValue = ValStr;

	// Per UE 5.7 source_query result: ImportText_Direct(Buffer, Data, OwnerObject, PortFlags, ErrorText)
	// returns nullptr on failure. Capture errors via FStringOutputDevice.
	FStringOutputDevice ErrText;
	const TCHAR* Result = Prop->ImportText_Direct(*ValStr, ValuePtr, Owner, PPF_None, &ErrText);
	if (!Result)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = FString::Printf(TEXT("ImportText rejected '%s'%s%s"),
			*ValStr,
			ErrText.IsEmpty() ? TEXT("") : TEXT(": "),
			*ErrText);
	}
	else
	{
		OutWrite.bOk = true;
	}
}

// ---------------------------------------------------------------------------
// Enum write — per design quirk: enum keys serialise as value-name strings.
// UEnum::GetValueByNameString returns INDEX_NONE on miss (verified Enum.cpp:1046).
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteEnum(FEnumProperty* EnumProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, FBulkFillFieldWrite& OutWrite)
{
	const FString NameStr = JsonVal->AsString();
	OutWrite.ProposedValue = NameStr;

	UEnum* Enum = EnumProp->GetEnum();
	if (!Enum)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("enum property has no UEnum metadata");
		return;
	}

	const int64 Value = Enum->GetValueByNameString(NameStr);
	if (Value == INDEX_NONE)
	{
		OutWrite.bOk = false;
		// Build a "did you mean" hint — first 3 enum entries.
		FString Hint;
		const int32 N = FMath::Min(3, Enum->NumEnums() - 1); // -1 for _MAX
		for (int32 i = 0; i < N; ++i)
		{
			Hint += (i == 0 ? TEXT("") : TEXT(", "));
			Hint += Enum->GetNameStringByIndex(i);
		}
		OutWrite.Reason = FString::Printf(TEXT("enum value '%s' not found; valid entries include: %s"),
			*NameStr, *Hint);
		return;
	}

	// Snapshot pre-write value as a name string.
	FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
	if (Underlying)
	{
		const int64 OldValue = Underlying->GetSignedIntPropertyValue(ValuePtr);
		OutWrite.CurrentValue = Enum->GetNameStringByValue(OldValue);
		Underlying->SetIntPropertyValue(ValuePtr, Value);
	}
	OutWrite.bOk = true;
}

// ---------------------------------------------------------------------------
// Soft-object ref — FSoftObjectProperty inherits ImportText from FProperty
// (verified search_source: PropertySoftObjectPtr.cpp:208 ConvertFromType).
// The TSoftObjectPtr value is set via the path string going through the normal
// ImportText_Direct path.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteSoftObjectRef(FSoftObjectProperty* SoftProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, FBulkFillFieldWrite& OutWrite)
{
	const FString PathStr = JsonVal->AsString();
	OutWrite.ProposedValue = PathStr;

	SoftProp->ExportText_Direct(OutWrite.CurrentValue, ValuePtr, ValuePtr, nullptr, PPF_None);

	FStringOutputDevice ErrText;
	const TCHAR* Result = SoftProp->ImportText_Direct(*PathStr, ValuePtr, nullptr, PPF_None, &ErrText);
	OutWrite.bOk = (Result != nullptr);
	if (!OutWrite.bOk)
	{
		OutWrite.Reason = FString::Printf(TEXT("Soft-ref '%s' failed: %s"), *PathStr, *ErrText);
	}
}

// ---------------------------------------------------------------------------
// Hard object ref — StaticLoadObject path. Use the raw form
// SetObjectPropertyValue (verified PropertyBaseObject.cpp:671 via plan §4 H4)
// because we hold ValuePtr directly, not a container offset.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteObjectRef(FObjectProperty* ObjProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* /*Container*/, FBulkFillFieldWrite& OutWrite)
{
	const FString PathStr = JsonVal->AsString();
	OutWrite.ProposedValue = PathStr;

	// Snapshot current pointed-at object's path (or empty if null).
	UObject* OldRef = ObjProp->GetObjectPropertyValue(ValuePtr);
	OutWrite.CurrentValue = OldRef ? OldRef->GetPathName() : FString();

	if (PathStr.IsEmpty())
	{
		ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
		OutWrite.bOk = true;
		return;
	}

	UObject* Resolved = StaticLoadObject(ObjProp->PropertyClass, nullptr, *PathStr);
	if (!Resolved)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = FString::Printf(TEXT("hard ref '%s' did not resolve to a %s"),
			*PathStr, *ObjProp->PropertyClass->GetName());
		return;
	}
	ObjProp->SetObjectPropertyValue(ValuePtr, Resolved);
	OutWrite.bOk = true;
}

// ---------------------------------------------------------------------------
// Array write — FScriptArrayHelper. Per UE 5.7 search_source (plan §4):
// ctor at UnrealType.h:4455, AddUninitializedValues at 4340, AddValue at 4331.
// Per-element dispatch through DispatchByPropertyType so nested types work.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteArray(FArrayProperty* ArrayProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
	if (!JsonVal->TryGetArray(JsonArray) || !JsonArray)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("expected JSON array");
		return;
	}

	FScriptArrayHelper Helper(ArrayProp, ValuePtr);
	Helper.EmptyValues(JsonArray->Num());
	if (JsonArray->Num() > 0)
	{
		Helper.AddUninitializedValues(JsonArray->Num());
	}

	int32 LocalErrors = 0;
	for (int32 i = 0; i < JsonArray->Num(); ++i)
	{
		uint8* ElemPtr = Helper.GetRawPtr(i);
		// Init each element so ImportText has a stable starting point.
		ArrayProp->Inner->InitializeValue(ElemPtr);

		FBulkFillFieldWrite W;
		W.Path = FString::Printf(TEXT("%s[%d]"), *PathPrefix, i);
		DispatchByPropertyType(ArrayProp->Inner, ElemPtr, (*JsonArray)[i], Owner, Spec, OutReport, W.Path, W);
		if (!W.bOk) { ++LocalErrors; }
		OutReport.FieldWrites.Add(W);
	}

	OutWrite.bOk = (LocalErrors == 0);
	OutWrite.ProposedValue = FString::Printf(TEXT("[%d elements]"), JsonArray->Num());
	if (!OutWrite.bOk)
	{
		OutWrite.Reason = FString::Printf(TEXT("%d element write(s) failed"), LocalErrors);
	}
}

// ---------------------------------------------------------------------------
// Map write — FScriptMapHelper. AddPair clones key+value (verified UnrealType.h:5320).
// JSON shape: object whose string keys are stringified property-keys (FName/FString/int).
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteMap(FMapProperty* MapProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite)
{
	const TSharedPtr<FJsonObject>* JsonObj = nullptr;
	if (!JsonVal->TryGetObject(JsonObj) || !JsonObj || !(*JsonObj).IsValid())
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("expected JSON object for map");
		return;
	}

	FScriptMapHelper Helper(MapProp, ValuePtr);
	Helper.EmptyValues((*JsonObj)->Values.Num());

	int32 LocalErrors = 0;
	int32 Index = 0;
	for (const auto& Pair : (*JsonObj)->Values)
	{
		// Allocate scratch key + value buffers, init them, ImportText key from the
		// JSON object's STRING key, then dispatch the value through the inner walker.
		void* KeyTemp = FMemory::Malloc(MapProp->KeyProp->GetSize(), MapProp->KeyProp->GetMinAlignment());
		void* ValTemp = FMemory::Malloc(MapProp->ValueProp->GetSize(), MapProp->ValueProp->GetMinAlignment());
		MapProp->KeyProp->InitializeValue(KeyTemp);
		MapProp->ValueProp->InitializeValue(ValTemp);

		FBulkFillFieldWrite KeyWrite;
		KeyWrite.Path = FString::Printf(TEXT("%s{key#%d}"), *PathPrefix, Index);
		{
			FStringOutputDevice ErrText;
			const TCHAR* Result = MapProp->KeyProp->ImportText_Direct(*Pair.Key, KeyTemp, Owner, PPF_None, &ErrText);
			KeyWrite.ProposedValue = Pair.Key;
			KeyWrite.bOk = (Result != nullptr);
			if (!KeyWrite.bOk)
			{
				KeyWrite.Reason = FString::Printf(TEXT("map key '%s' rejected: %s"), *Pair.Key, *ErrText);
				++LocalErrors;
			}
		}

		FBulkFillFieldWrite ValWrite;
		ValWrite.Path = FString::Printf(TEXT("%s{val#%d}"), *PathPrefix, Index);
		DispatchByPropertyType(MapProp->ValueProp, ValTemp, Pair.Value, Owner, Spec, OutReport, ValWrite.Path, ValWrite);
		if (!ValWrite.bOk) { ++LocalErrors; }

		if (KeyWrite.bOk && ValWrite.bOk)
		{
			Helper.AddPair(KeyTemp, ValTemp);
		}

		OutReport.FieldWrites.Add(KeyWrite);
		OutReport.FieldWrites.Add(ValWrite);

		// Release scratch (AddPair clones).
		MapProp->KeyProp->DestroyValue(KeyTemp);
		MapProp->ValueProp->DestroyValue(ValTemp);
		FMemory::Free(KeyTemp);
		FMemory::Free(ValTemp);
		++Index;
	}

	Helper.Rehash();
	OutWrite.bOk = (LocalErrors == 0);
	OutWrite.ProposedValue = FString::Printf(TEXT("{%d entries}"), (*JsonObj)->Values.Num());
	if (!OutWrite.bOk)
	{
		OutWrite.Reason = FString::Printf(TEXT("%d map write(s) failed"), LocalErrors);
	}
}

// ---------------------------------------------------------------------------
// Set write — FScriptSetHelper. Per UE 5.7 source_query: ctor at UnrealType.h:5710,
// Rehash() at PropertySet.cpp:1032 (mandatory post-population).
// JSON shape: array of element values.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteSet(FSetProperty* SetProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
	if (!JsonVal->TryGetArray(JsonArray) || !JsonArray)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("expected JSON array for set");
		return;
	}

	FScriptSetHelper Helper(SetProp, ValuePtr);
	Helper.EmptyElements(JsonArray->Num());

	int32 LocalErrors = 0;
	for (int32 i = 0; i < JsonArray->Num(); ++i)
	{
		void* ElemTemp = FMemory::Malloc(SetProp->ElementProp->GetSize(), SetProp->ElementProp->GetMinAlignment());
		SetProp->ElementProp->InitializeValue(ElemTemp);

		FBulkFillFieldWrite W;
		W.Path = FString::Printf(TEXT("%s{#%d}"), *PathPrefix, i);
		DispatchByPropertyType(SetProp->ElementProp, ElemTemp, (*JsonArray)[i], Owner, Spec, OutReport, W.Path, W);
		if (!W.bOk) { ++LocalErrors; }
		OutReport.FieldWrites.Add(W);

		if (W.bOk)
		{
			Helper.AddElement(ElemTemp);
		}

		SetProp->ElementProp->DestroyValue(ElemTemp);
		FMemory::Free(ElemTemp);
	}

	Helper.Rehash();
	OutWrite.bOk = (LocalErrors == 0);
	OutWrite.ProposedValue = FString::Printf(TEXT("{%d unique elements}"), JsonArray->Num());
	if (!OutWrite.bOk)
	{
		OutWrite.Reason = FString::Printf(TEXT("%d set write(s) failed"), LocalErrors);
	}
}

// ---------------------------------------------------------------------------
// Struct write — recurse via WriteTree on nested JSON object, or fall through
// to ImportText for "(X=1,Y=2,Z=3)" literal forms.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteStruct(FStructProperty* StructProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite)
{
	// Object form -> recursive walk into the nested struct's properties.
	if (JsonVal->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>* NestedObj = nullptr;
		if (!JsonVal->TryGetObject(NestedObj) || !NestedObj || !(*NestedObj).IsValid())
		{
			OutWrite.bOk = false;
			OutWrite.Reason = TEXT("nested struct expected JSON object");
			return;
		}

		// Per UE57Gotchas.md §JSON: empty-not-null guard.
		if ((*NestedObj)->Values.Num() == 0)
		{
			OutWrite.bOk = true; // empty object = no-op write, not an error
			OutWrite.ProposedValue = TEXT("{}");
			return;
		}

		int32 LocalErrors = 0;
		for (const auto& Pair : (*NestedObj)->Values)
		{
			FBulkFillFieldWrite W;
			W.Path = FString::Printf(TEXT("%s.%s"), *PathPrefix, *Pair.Key);
			FProperty* InnerProp = FindPropertyForwarding(StructProp->Struct, Pair.Key);
			if (!InnerProp)
			{
				W.bOk = false;
				W.Reason = FString::Printf(TEXT("unknown field '%s' on %s"), *Pair.Key, *StructProp->Struct->GetName());
				OutReport.FieldWrites.Add(W);
				++LocalErrors;
				continue;
			}
			void* InnerPtr = InnerProp->ContainerPtrToValuePtr<void>(ValuePtr);
			DispatchByPropertyType(InnerProp, InnerPtr, Pair.Value, Owner, Spec, OutReport, W.Path, W);
			if (!W.bOk) { ++LocalErrors; }
			OutReport.FieldWrites.Add(W);
		}
		OutWrite.bOk = (LocalErrors == 0);
		OutWrite.ProposedValue = FString::Printf(TEXT("{%d fields}"), (*NestedObj)->Values.Num());
		if (!OutWrite.bOk)
		{
			OutWrite.Reason = FString::Printf(TEXT("%d nested write(s) failed"), LocalErrors);
		}
		return;
	}

	// String form -> ImportText literal grammar ("(X=1,Y=2,Z=3)").
	WriteScalar(StructProp, ValuePtr, JsonVal, Owner, OutWrite);
}

// ---------------------------------------------------------------------------
// WriteTree — top-level entry. Iterates the JSON object's keys and dispatches
// each into the matching FProperty on Container.
// ---------------------------------------------------------------------------
FDryRunReport FMonolithReflectionWalker::WriteTree(
	const TSharedPtr<FJsonObject>& Tree,
	UStruct* TopStruct,
	void* Container,
	UObject* OwnerForCradle,
	const FBulkFillSpec& Spec)
{
	check(IsInGameThread());

	FDryRunReport Report;
	Report.bWouldApply = true; // optimistic; cleared at the end on strict + errors.

	// Per UE57Gotchas.md §JSON: empty-not-null guard.
	if (!Tree.IsValid() || Tree->Values.Num() == 0)
	{
		Report.bWouldApply = false;
		return Report;
	}
	if (!TopStruct || !Container)
	{
		Report.bWouldApply = false;
		return Report;
	}

	for (const auto& Pair : Tree->Values)
	{
		FBulkFillFieldWrite W;
		W.Path = Pair.Key;
		FProperty* Prop = FindPropertyForwarding(TopStruct, Pair.Key);
		if (!Prop)
		{
			W.bOk = false;
			W.Reason = FString::Printf(TEXT("unknown field '%s' on %s"), *Pair.Key, *TopStruct->GetName());
			Report.FieldWrites.Add(W);
			continue;
		}
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
		DispatchByPropertyType(Prop, ValuePtr, Pair.Value, OwnerForCradle, Spec, Report, Pair.Key, W);
		Report.FieldWrites.Add(W);
	}

	// Strict-mode handling per Decision Q6.
	Report.Errors = Algo::CountIf(Report.FieldWrites, [](const FBulkFillFieldWrite& Fw){ return !Fw.bOk; });
	if (Spec.bStrict && Report.Errors > 0)
	{
		Report.bWouldApply = false;
	}
	return Report;
}

// ---------------------------------------------------------------------------
// InspectTree — dry-run. Same shape as WriteTree, but ALL writes route through
// a per-field scratch buffer allocated by FProperty::InitializeValue and
// freed by DestroyValue. Container is never mutated.
// Guarantees: test Leviathan.Monolith.Reflection.DryRunNoSideEffects passes.
// ---------------------------------------------------------------------------
FDryRunReport FMonolithReflectionWalker::InspectTree(
	const TSharedPtr<FJsonObject>& Tree,
	UStruct* TopStruct,
	const void* /*Container*/,
	const FBulkFillSpec& Spec)
{
	check(IsInGameThread());

	FDryRunReport Report;
	Report.bWouldApply = false; // dry-run NEVER applies

	if (!Tree.IsValid() || Tree->Values.Num() == 0 || !TopStruct)
	{
		return Report;
	}

	for (const auto& Pair : Tree->Values)
	{
		FBulkFillFieldWrite W;
		W.Path = Pair.Key;
		FProperty* Prop = FindPropertyForwarding(TopStruct, Pair.Key);
		if (!Prop)
		{
			W.bOk = false;
			W.Reason = FString::Printf(TEXT("unknown field '%s' on %s"), *Pair.Key, *TopStruct->GetName());
			Report.FieldWrites.Add(W);
			continue;
		}

		// Allocate a scratch buffer the size of the property; init it; dispatch
		// against the scratch; destroy. The real container is never touched.
		void* Scratch = FMemory::Malloc(Prop->GetSize(), Prop->GetMinAlignment());
		Prop->InitializeValue(Scratch);
		DispatchByPropertyType(Prop, Scratch, Pair.Value, nullptr, Spec, Report, Pair.Key, W);
		Prop->DestroyValue(Scratch);
		FMemory::Free(Scratch);

		Report.FieldWrites.Add(W);
	}

	Report.Errors = Algo::CountIf(Report.FieldWrites, [](const FBulkFillFieldWrite& Fw){ return !Fw.bOk; });
	return Report;
}

// ---------------------------------------------------------------------------
// Populate clamp meta from UIMin/UIMax/ClampMin/ClampMax property metadata.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::PopulateClampMeta(FProperty* Prop, FSchemaDescriptor& OutDesc)
{
#if WITH_EDITORONLY_DATA
	if (!Prop) return;
	const FString ClampMin = Prop->GetMetaData(TEXT("ClampMin"));
	const FString ClampMax = Prop->GetMetaData(TEXT("ClampMax"));
	const FString UIMin = Prop->GetMetaData(TEXT("UIMin"));
	const FString UIMax = Prop->GetMetaData(TEXT("UIMax"));
	const FString& MinStr = !ClampMin.IsEmpty() ? ClampMin : UIMin;
	const FString& MaxStr = !ClampMax.IsEmpty() ? ClampMax : UIMax;
	if (!MinStr.IsEmpty()) { OutDesc.RangeMin = FCString::Atof(*MinStr); }
	if (!MaxStr.IsEmpty()) { OutDesc.RangeMax = FCString::Atof(*MaxStr); }
#endif
}

// ---------------------------------------------------------------------------
// DescribeStruct — recursive FSchemaDescriptor builder.
// Per design Decision Q3: rich custom tree, NOT JSON Schema standard.
// ---------------------------------------------------------------------------
FSchemaDescriptor FMonolithReflectionWalker::DescribeStruct(UStruct* TopStruct, int32 MaxDepth)
{
	FSchemaDescriptor Root;
	if (!TopStruct || MaxDepth <= 0)
	{
		return Root;
	}
	Root.FieldPath = TopStruct->GetName();
	Root.TypeName = TopStruct->GetName();

	for (TFieldIterator<FProperty> It(TopStruct); It; ++It)
	{
		FProperty* Prop = *It;
		FSchemaDescriptor Child;
		Child.FieldPath = Prop->GetName();
		Child.TypeName = Prop->GetCPPType();
		PopulateClampMeta(Prop, Child);

		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				const int32 N = Enum->NumEnums() - 1; // -1 for _MAX
				for (int32 i = 0; i < N; ++i)
				{
					Child.EnumValues.Add(Enum->GetNameStringByIndex(i));
				}
			}
			Child.ImportTextForm = (Child.EnumValues.Num() > 0) ? Child.EnumValues[0] : FString();
		}
		else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("(Field1=...,Field2=...)");
			if (MaxDepth > 1 && StructProp->Struct)
			{
				Child.Children.Add(DescribeStruct(StructProp->Struct, MaxDepth - 1));
			}
		}
		else if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("((Key=K1,Value=V1),(Key=K2,Value=V2))");
			// Optional: descend into value type if it is a struct.
			if (MaxDepth > 1 && MapProp->ValueProp)
			{
				if (FStructProperty* ValueStruct = CastField<FStructProperty>(MapProp->ValueProp))
				{
					if (ValueStruct->Struct)
					{
						Child.Children.Add(DescribeStruct(ValueStruct->Struct, MaxDepth - 1));
					}
				}
			}
		}
		else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("(Element1,Element2,Element3)");
			if (MaxDepth > 1 && ArrayProp->Inner)
			{
				if (FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner))
				{
					if (InnerStruct->Struct)
					{
						Child.Children.Add(DescribeStruct(InnerStruct->Struct, MaxDepth - 1));
					}
				}
			}
		}
		else if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("(Element1,Element2)");
			if (MaxDepth > 1 && SetProp->ElementProp)
			{
				if (FStructProperty* ElemStruct = CastField<FStructProperty>(SetProp->ElementProp))
				{
					if (ElemStruct->Struct)
					{
						Child.Children.Add(DescribeStruct(ElemStruct->Struct, MaxDepth - 1));
					}
				}
			}
		}
		else if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("/Game/Path/To/Asset.Asset");
			Child.TypeName = FString::Printf(TEXT("TSoftObjectPtr<%s>"), SoftProp->PropertyClass ? *SoftProp->PropertyClass->GetName() : TEXT("UObject"));
		}
		else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("/Game/Path/To/Asset.Asset");
			Child.TypeName = FString::Printf(TEXT("%s*"), ObjProp->PropertyClass ? *ObjProp->PropertyClass->GetName() : TEXT("UObject"));
		}
		else
		{
			// Scalar — leave ImportTextForm empty; CPPType already communicates the shape.
		}

		Root.Children.Add(Child);
	}
	return Root;
}
