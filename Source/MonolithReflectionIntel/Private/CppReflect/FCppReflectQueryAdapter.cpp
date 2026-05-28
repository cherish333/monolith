// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 3a — v0.17.0).
//
// FCppReflectQueryAdapter — implementation. Five read-only handlers over the
// Phase 3a reflect_* + cpp_asset_edges tables. All run on the game thread.
// Cursor codec mirrored from the Phase 1 / Phase 2 adapters — consolidation
// into MonolithCore is a Phase 5+ item and out of scope.

#include "CppReflect/FCppReflectQueryAdapter.h"
#include "MonolithReflectionIntelModule.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Base64.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SQLiteDatabase.h"
#include "Templates/TypeHash.h"

namespace
{
	// ---------------------------------------------------------------------
	// Cursor codec — same base64(JSON{qh,p,tc}) shape as risk_query.
	// Phase 5+ consolidation into MonolithCore is out of scope.
	// ---------------------------------------------------------------------
	struct FCppReflectCursorState
	{
		uint32 QueryHash = 0;
		int32  Page = 0;
		int32  CachedTotalEstimate = -1;
	};

	FString EncodeCursor(const FCppReflectCursorState& S)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("qh"), static_cast<double>(S.QueryHash));
		O->SetNumberField(TEXT("p"),  S.Page);
		O->SetNumberField(TEXT("tc"), S.CachedTotalEstimate);
		FString Js;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Js);
		FJsonSerializer::Serialize(O.ToSharedRef(), W);
		return FBase64::Encode(Js);
	}

	bool DecodeCursor(const FString& Enc, FCppReflectCursorState& Out)
	{
		Out = FCppReflectCursorState();
		if (Enc.IsEmpty()) { return false; }
		FString Js;
		if (!FBase64::Decode(Enc, Js)) { return false; }
		TSharedPtr<FJsonObject> O;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Js);
		if (!FJsonSerializer::Deserialize(R, O) || !O.IsValid()) { return false; }
		double Qh = 0.0, P = 0.0, Tc = -1.0;
		if (!O->TryGetNumberField(TEXT("qh"), Qh)) { return false; }
		if (!O->TryGetNumberField(TEXT("p"),  P))  { return false; }
		if (!O->TryGetNumberField(TEXT("tc"), Tc)) { return false; }
		if (P < 0.0) { return false; }
		if (Qh < 0.0 || Qh > static_cast<double>(TNumericLimits<uint32>::Max())) { return false; }
		Out.QueryHash = static_cast<uint32>(Qh);
		Out.Page = static_cast<int32>(P);
		Out.CachedTotalEstimate = static_cast<int32>(Tc);
		return true;
	}

	FMonolithActionResult InvalidCursorError(const FString& Reason)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("error_code"), TEXT("INVALID_CURSOR"));
		return FMonolithActionResult::Error(Reason, FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(Data);
	}

	uint32 ComputeFilterHash(std::initializer_list<FString> Parts)
	{
		uint32 H = 0;
		for (const FString& P : Parts) { H = HashCombine(H, GetTypeHash(P)); }
		return H;
	}
}

// ============================================================================
// Registration
// ============================================================================

void FCppReflectQueryAdapter::RegisterActions(FMonolithToolRegistry& Registry)
{
	// ---- get_uclass ----
	Registry.RegisterAction(TEXT("cppreflect"), TEXT("get_uclass"),
		TEXT("Return one UCLASS row plus all UPROPERTYs, all UFUNCTIONs, and the "
		     "parent-class chain. `class_name` is the C++ symbol with prefix "
		     "(e.g. \"ALeviathanCharacterBase\"). Returns null when the class "
		     "is not in the reflection index — call UBT before re-querying."),
		FMonolithActionHandler::CreateStatic(&FCppReflectQueryAdapter::HandleGetUClass),
		FParamSchemaBuilder()
			.Required(TEXT("class_name"), TEXT("string"),
				TEXT("C++ class symbol (e.g. ALeviathanCharacterBase)"))
			.Optional(TEXT("module_name"), TEXT("string"),
				TEXT("Optional module filter when the class exists in multiple modules"))
			.Build());

	// ---- list_uproperties ----
	Registry.RegisterAction(TEXT("cppreflect"), TEXT("list_uproperties"),
		TEXT("List UPROPERTY rows. Filter by `class_name` (recommended — "
		     "engine-scope queries can hit thousands of rows). "
		     "`blueprint_visible_only=true` filters to properties with a "
		     "blueprint_visibility specifier set. Cursor pagination."),
		FMonolithActionHandler::CreateStatic(&FCppReflectQueryAdapter::HandleListUProperties),
		FParamSchemaBuilder()
			.Optional(TEXT("class_name"), TEXT("string"),
				TEXT("Filter by owning_class (exact match)"))
			.Optional(TEXT("blueprint_visible_only"), TEXT("boolean"),
				TEXT("Restrict to properties with a non-empty blueprint_visibility"), TEXT("false"))
			.Optional(TEXT("limit"), TEXT("integer"),
				TEXT("Max rows per page (default 50, hard cap 200)"), TEXT("50"))
			.Optional(TEXT("cursor"), TEXT("string"),
				TEXT("Opaque pagination cursor"))
			.Build());

	// ---- list_ufunctions ----
	Registry.RegisterAction(TEXT("cppreflect"), TEXT("list_ufunctions"),
		TEXT("List UFUNCTION rows. Filter by `class_name` or restrict to "
		     "BlueprintCallable-eligible functions via `blueprint_callable_only`. "
		     "Cursor pagination."),
		FMonolithActionHandler::CreateStatic(&FCppReflectQueryAdapter::HandleListUFunctions),
		FParamSchemaBuilder()
			.Optional(TEXT("class_name"), TEXT("string"),
				TEXT("Filter by owning_class (exact match)"))
			.Optional(TEXT("blueprint_callable_only"), TEXT("boolean"),
				TEXT("Restrict to functions exposed to the BP VM"), TEXT("false"))
			.Optional(TEXT("limit"), TEXT("integer"),
				TEXT("Max rows per page (default 50, hard cap 200)"), TEXT("50"))
			.Optional(TEXT("cursor"), TEXT("string"),
				TEXT("Opaque pagination cursor"))
			.Build());

	// ---- find_interface_impls ----
	Registry.RegisterAction(TEXT("cppreflect"), TEXT("find_interface_impls"),
		TEXT("List UCLASSes implementing `interface_name`. `interface_name` is "
		     "the U-prefixed companion class symbol (e.g. \"UISXWeaponFireBridgeInterface\"). "
		     "Returns the implementing_class + module + source_path tuple."),
		FMonolithActionHandler::CreateStatic(&FCppReflectQueryAdapter::HandleFindInterfaceImpls),
		FParamSchemaBuilder()
			.Required(TEXT("interface_name"), TEXT("string"),
				TEXT("U-prefixed interface class symbol"))
			.Build());

	// ---- find_class_specifier ----
	Registry.RegisterAction(TEXT("cppreflect"), TEXT("find_class_specifier"),
		TEXT("Find UCLASS rows whose `flags` colon-list contains the given "
		     "specifier (e.g. \"BlueprintType\", \"Abstract\", \"MinimalAPI\"). "
		     "Case-sensitive substring match on flags. Cursor pagination."),
		FMonolithActionHandler::CreateStatic(&FCppReflectQueryAdapter::HandleFindClassSpecifier),
		FParamSchemaBuilder()
			.Required(TEXT("specifier_name"), TEXT("string"),
				TEXT("UCLASS specifier token to search the flags column for"))
			.Optional(TEXT("limit"), TEXT("integer"),
				TEXT("Max rows per page (default 50, hard cap 200)"), TEXT("50"))
			.Optional(TEXT("cursor"), TEXT("string"),
				TEXT("Opaque pagination cursor"))
			.Build());

	// Dispatcher annotation — all five handlers are pure SELECT against the
	// Phase 3a reflect_* tables.
	FMonolithDispatcherAnnotations Anno;
	Anno.bReadOnlyHint    = true;
	Anno.bDestructiveHint = false;
	Anno.bIdempotentHint  = true;
	Anno.Title = TEXT("C++ reflection-edge query");
	Registry.SetDispatcherAnnotations(TEXT("cppreflect"), Anno);
}

// ============================================================================
// DB accessor — lazy bootstrap of the cppreflect tables on first call.
// ============================================================================

FSQLiteDatabase* FCppReflectQueryAdapter::GetRawDB()
{
	// Phase 1+2 lesson #5: any DB accessor — read or write — runs on the game
	// thread. Writers (FUHTArtefactReader::Run, FAssetGraphJoiner::Run) already
	// enforce this; mirror the assertion on the read side.
	ensure(IsInGameThread());

	FMonolithReflectionIntelModule* Module =
		FModuleManager::GetModulePtr<FMonolithReflectionIntelModule>(
			TEXT("MonolithReflectionIntel"));
	if (!Module) { return nullptr; }

	FSQLiteDatabase* DB = Module->GetOrOpenCachedQueryDb();
	if (!DB) { return nullptr; }

	if (!Module->HasAttemptedCppReflectBootstrap())
	{
		Module->MarkCppReflectBootstrapAttempted();
		FSQLitePreparedStatement TableCheck;
		const bool bPrepared = TableCheck.Create(*DB,
			TEXT("SELECT name FROM sqlite_master WHERE type='table' AND name='reflect_uclasses';"));
		const bool bTableExists = bPrepared
			&& TableCheck.Step() == ESQLitePreparedStatementStepResult::Row;
		TableCheck.Destroy();
		if (!bTableExists)
		{
			Module->ResetCachedQueryDb();
			FString IndexerStatus;
			FMonolithReflectionIntelModule::RunCppReflectIndexersOnce(IndexerStatus);
			DB = Module->GetOrOpenCachedQueryDb();
			if (!DB) { return nullptr; }
		}
	}
	return DB;
}

// ============================================================================
// Handlers
// ============================================================================

FMonolithActionResult FCppReflectQueryAdapter::HandleGetUClass(const TSharedPtr<FJsonObject>& Params)
{
	FSQLiteDatabase* DB = GetRawDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(
			TEXT("EngineSource.db not available. Run source.trigger_reindex to bootstrap, "
			     "or build the project at least once so UHT artefacts exist."));
	}

	const FString ClassName = Params->GetStringField(TEXT("class_name"));
	if (ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("`class_name` is required."),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	const FString ModuleFilter = Params->HasField(TEXT("module_name"))
		? Params->GetStringField(TEXT("module_name")) : FString();

	// --- UClass row(s) ---
	FString ClassSql = TEXT(
		"SELECT class_name, module_name, parent_class, source_path, source_line, flags "
		"FROM reflect_uclasses WHERE class_name = ?");
	if (!ModuleFilter.IsEmpty()) { ClassSql += TEXT(" AND module_name = ?"); }
	ClassSql += TEXT(" LIMIT 1;");

	FSQLitePreparedStatement ClassStmt;
	if (!ClassStmt.Create(*DB, *ClassSql))
	{
		return FMonolithActionResult::Error(TEXT("SELECT prepare failed (reflect_uclasses absent?)."));
	}
	int32 BindIdx = 1;
	ClassStmt.SetBindingValueByIndex(BindIdx++, ClassName);
	if (!ModuleFilter.IsEmpty()) { ClassStmt.SetBindingValueByIndex(BindIdx++, ModuleFilter); }

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	if (ClassStmt.Step() != ESQLitePreparedStatementStepResult::Row)
	{
		Out->SetField(TEXT("uclass"), MakeShared<FJsonValueNull>());
		return FMonolithActionResult::Success(Out);
	}

	FString CName, MName, Parent, SrcPath, Flags;
	int32 SrcLine = 0;
	ClassStmt.GetColumnValueByIndex(0, CName);
	ClassStmt.GetColumnValueByIndex(1, MName);
	ClassStmt.GetColumnValueByIndex(2, Parent);
	ClassStmt.GetColumnValueByIndex(3, SrcPath);
	ClassStmt.GetColumnValueByIndex(4, SrcLine);
	ClassStmt.GetColumnValueByIndex(5, Flags);

	TSharedPtr<FJsonObject> UClassObj = MakeShared<FJsonObject>();
	UClassObj->SetStringField(TEXT("class_name"), CName);
	UClassObj->SetStringField(TEXT("module_name"), MName);
	UClassObj->SetStringField(TEXT("parent_class"), Parent);
	UClassObj->SetStringField(TEXT("source_path"), SrcPath);
	UClassObj->SetNumberField(TEXT("source_line"), SrcLine);
	UClassObj->SetStringField(TEXT("flags"), Flags);

	// --- Parent-class chain via repeated lookup. Cap at 16 to avoid loops. ---
	{
		TArray<TSharedPtr<FJsonValue>> Chain;
		FString Cur = Parent;
		int32 Walks = 0;
		while (!Cur.IsEmpty() && Walks++ < 16)
		{
			FSQLitePreparedStatement ParentStmt;
			if (!ParentStmt.Create(*DB, TEXT(
				"SELECT parent_class FROM reflect_uclasses WHERE class_name = ? LIMIT 1;")))
			{
				break;
			}
			ParentStmt.SetBindingValueByIndex(1, Cur);
			if (ParentStmt.Step() != ESQLitePreparedStatementStepResult::Row)
			{
				// Unknown — engine class outside the index (UObject, AActor, etc).
				Chain.Add(MakeShared<FJsonValueString>(Cur));
				break;
			}
			Chain.Add(MakeShared<FJsonValueString>(Cur));
			FString Next;
			ParentStmt.GetColumnValueByIndex(0, Next);
			Cur = Next;
			if (Cur == Chain.Last()->AsString()) { break; } // defensive cycle guard
		}
		UClassObj->SetArrayField(TEXT("parent_chain"), Chain);
	}

	// --- All UPROPERTYs on this class ---
	{
		FSQLitePreparedStatement PropStmt;
		FString PropSql = TEXT(
			"SELECT property_name, property_type, cpp_module, blueprint_visibility, specifiers "
			"FROM reflect_uproperties WHERE owning_class = ?");
		if (!ModuleFilter.IsEmpty()) { PropSql += TEXT(" AND cpp_module = ?"); }
		PropSql += TEXT(" ORDER BY property_name;");
		if (PropStmt.Create(*DB, *PropSql))
		{
			int32 PBind = 1;
			PropStmt.SetBindingValueByIndex(PBind++, CName);
			if (!ModuleFilter.IsEmpty()) { PropStmt.SetBindingValueByIndex(PBind++, ModuleFilter); }

			TArray<TSharedPtr<FJsonValue>> Props;
			while (PropStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString PName, PType, PModule, BpVis, Specs;
				PropStmt.GetColumnValueByIndex(0, PName);
				PropStmt.GetColumnValueByIndex(1, PType);
				PropStmt.GetColumnValueByIndex(2, PModule);
				PropStmt.GetColumnValueByIndex(3, BpVis);
				PropStmt.GetColumnValueByIndex(4, Specs);

				TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("property_name"), PName);
				P->SetStringField(TEXT("property_type"), PType);
				P->SetStringField(TEXT("cpp_module"), PModule);
				P->SetStringField(TEXT("blueprint_visibility"), BpVis);
				P->SetStringField(TEXT("specifiers"), Specs);
				Props.Add(MakeShared<FJsonValueObject>(P));
			}
			UClassObj->SetArrayField(TEXT("uproperties"), Props);
		}
	}

	// --- All UFUNCTIONs on this class ---
	{
		FSQLitePreparedStatement FuncStmt;
		FString FuncSql = TEXT(
			"SELECT function_name, return_type, blueprint_callable, cpp_module, specifiers "
			"FROM reflect_ufunctions WHERE owning_class = ?");
		if (!ModuleFilter.IsEmpty()) { FuncSql += TEXT(" AND cpp_module = ?"); }
		FuncSql += TEXT(" ORDER BY function_name;");
		if (FuncStmt.Create(*DB, *FuncSql))
		{
			int32 FBind = 1;
			FuncStmt.SetBindingValueByIndex(FBind++, CName);
			if (!ModuleFilter.IsEmpty()) { FuncStmt.SetBindingValueByIndex(FBind++, ModuleFilter); }

			TArray<TSharedPtr<FJsonValue>> Funcs;
			while (FuncStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString FName, RetType, FModule, Specs;
				int32 BpCallable = 0;
				FuncStmt.GetColumnValueByIndex(0, FName);
				FuncStmt.GetColumnValueByIndex(1, RetType);
				FuncStmt.GetColumnValueByIndex(2, BpCallable);
				FuncStmt.GetColumnValueByIndex(3, FModule);
				FuncStmt.GetColumnValueByIndex(4, Specs);

				TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("function_name"), FName);
				F->SetStringField(TEXT("return_type"), RetType);
				F->SetBoolField(TEXT("blueprint_callable"), BpCallable != 0);
				F->SetStringField(TEXT("cpp_module"), FModule);
				F->SetStringField(TEXT("specifiers"), Specs);
				Funcs.Add(MakeShared<FJsonValueObject>(F));
			}
			UClassObj->SetArrayField(TEXT("ufunctions"), Funcs);
		}
	}

	Out->SetObjectField(TEXT("uclass"), UClassObj);
	return FMonolithActionResult::Success(Out);
}

FMonolithActionResult FCppReflectQueryAdapter::HandleListUProperties(const TSharedPtr<FJsonObject>& Params)
{
	FSQLiteDatabase* DB = GetRawDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(
			TEXT("EngineSource.db not available. Run source.trigger_reindex to bootstrap."));
	}

	const FString ClassName = Params->HasField(TEXT("class_name"))
		? Params->GetStringField(TEXT("class_name")) : FString();
	const bool bBpOnly = Params->HasField(TEXT("blueprint_visible_only"))
		? Params->GetBoolField(TEXT("blueprint_visible_only")) : false;
	const int32 ReqLimit = Params->HasField(TEXT("limit"))
		? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;
	const FString CursorIn = Params->HasField(TEXT("cursor"))
		? Params->GetStringField(TEXT("cursor")) : FString();

	constexpr int32 HARD_CAP = 200;
	const int32 Limit = FMath::Clamp(ReqLimit, 1, HARD_CAP);
	const uint32 FilterHash = ComputeFilterHash({ ClassName, bBpOnly ? TEXT("1") : TEXT("0") });

	int32 Page = 0;
	int32 CachedTotal = -1;
	const bool bHasCursor = !CursorIn.IsEmpty();
	if (bHasCursor)
	{
		FCppReflectCursorState State;
		if (!DecodeCursor(CursorIn, State))
		{
			return InvalidCursorError(TEXT("Cursor decode failed; restart pagination without `cursor`."));
		}
		if (State.QueryHash != FilterHash)
		{
			return InvalidCursorError(TEXT("Cursor filter mismatch; restart pagination without `cursor`."));
		}
		Page = State.Page;
		CachedTotal = State.CachedTotalEstimate;
	}

	FString WhereSql = TEXT("WHERE 1=1");
	if (!ClassName.IsEmpty()) { WhereSql += TEXT(" AND owning_class = ?"); }
	if (bBpOnly)                { WhereSql += TEXT(" AND blueprint_visibility IS NOT NULL AND blueprint_visibility <> ''"); }

	const FString Sql = FString::Printf(
		TEXT("SELECT owning_class, property_name, property_type, cpp_module, blueprint_visibility, specifiers "
			 "FROM reflect_uproperties %s "
			 "ORDER BY owning_class, property_name "
			 "LIMIT ? OFFSET ?;"),
		*WhereSql);

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*DB, *Sql))
	{
		return FMonolithActionResult::Error(TEXT("SELECT prepare failed (reflect_uproperties absent?)."));
	}
	int32 BindIdx = 1;
	if (!ClassName.IsEmpty()) { Stmt.SetBindingValueByIndex(BindIdx++, ClassName); }
	Stmt.SetBindingValueByIndex(BindIdx++, Limit);
	Stmt.SetBindingValueByIndex(BindIdx++, Page * Limit);

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString OClass, PName, PType, PModule, BpVis, Specs;
		Stmt.GetColumnValueByIndex(0, OClass);
		Stmt.GetColumnValueByIndex(1, PName);
		Stmt.GetColumnValueByIndex(2, PType);
		Stmt.GetColumnValueByIndex(3, PModule);
		Stmt.GetColumnValueByIndex(4, BpVis);
		Stmt.GetColumnValueByIndex(5, Specs);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("owning_class"), OClass);
		R->SetStringField(TEXT("property_name"), PName);
		R->SetStringField(TEXT("property_type"), PType);
		R->SetStringField(TEXT("cpp_module"), PModule);
		R->SetStringField(TEXT("blueprint_visibility"), BpVis);
		R->SetStringField(TEXT("specifiers"), Specs);
		Rows.Add(MakeShared<FJsonValueObject>(R));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("uproperties"), Rows);

	if (!bHasCursor)
	{
		FString CountSql = TEXT("SELECT COUNT(*) FROM reflect_uproperties ") + WhereSql + TEXT(";");
		FSQLitePreparedStatement CountStmt;
		if (CountStmt.Create(*DB, *CountSql))
		{
			int32 CBind = 1;
			if (!ClassName.IsEmpty()) { CountStmt.SetBindingValueByIndex(CBind++, ClassName); }
			if (CountStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int32 Total = 0;
				CountStmt.GetColumnValueByIndex(0, Total);
				CachedTotal = Total;
				Out->SetNumberField(TEXT("total_estimate"), CachedTotal);
			}
		}
	}

	if (Rows.Num() == Limit)
	{
		FCppReflectCursorState OutCursor;
		OutCursor.QueryHash = FilterHash;
		OutCursor.Page = Page + 1;
		OutCursor.CachedTotalEstimate = CachedTotal;
		Out->SetStringField(TEXT("next_cursor"), EncodeCursor(OutCursor));
	}
	return FMonolithActionResult::Success(Out);
}

FMonolithActionResult FCppReflectQueryAdapter::HandleListUFunctions(const TSharedPtr<FJsonObject>& Params)
{
	FSQLiteDatabase* DB = GetRawDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(
			TEXT("EngineSource.db not available. Run source.trigger_reindex to bootstrap."));
	}

	const FString ClassName = Params->HasField(TEXT("class_name"))
		? Params->GetStringField(TEXT("class_name")) : FString();
	const bool bBpOnly = Params->HasField(TEXT("blueprint_callable_only"))
		? Params->GetBoolField(TEXT("blueprint_callable_only")) : false;
	const int32 ReqLimit = Params->HasField(TEXT("limit"))
		? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;
	const FString CursorIn = Params->HasField(TEXT("cursor"))
		? Params->GetStringField(TEXT("cursor")) : FString();

	constexpr int32 HARD_CAP = 200;
	const int32 Limit = FMath::Clamp(ReqLimit, 1, HARD_CAP);
	const uint32 FilterHash = ComputeFilterHash({ ClassName, bBpOnly ? TEXT("1") : TEXT("0") });

	int32 Page = 0;
	int32 CachedTotal = -1;
	const bool bHasCursor = !CursorIn.IsEmpty();
	if (bHasCursor)
	{
		FCppReflectCursorState State;
		if (!DecodeCursor(CursorIn, State))
		{
			return InvalidCursorError(TEXT("Cursor decode failed; restart pagination without `cursor`."));
		}
		if (State.QueryHash != FilterHash)
		{
			return InvalidCursorError(TEXT("Cursor filter mismatch; restart pagination without `cursor`."));
		}
		Page = State.Page;
		CachedTotal = State.CachedTotalEstimate;
	}

	FString WhereSql = TEXT("WHERE 1=1");
	if (!ClassName.IsEmpty()) { WhereSql += TEXT(" AND owning_class = ?"); }
	if (bBpOnly)                { WhereSql += TEXT(" AND blueprint_callable = 1"); }

	const FString Sql = FString::Printf(
		TEXT("SELECT owning_class, function_name, return_type, blueprint_callable, cpp_module, specifiers "
			 "FROM reflect_ufunctions %s "
			 "ORDER BY owning_class, function_name "
			 "LIMIT ? OFFSET ?;"),
		*WhereSql);

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*DB, *Sql))
	{
		return FMonolithActionResult::Error(TEXT("SELECT prepare failed (reflect_ufunctions absent?)."));
	}
	int32 BindIdx = 1;
	if (!ClassName.IsEmpty()) { Stmt.SetBindingValueByIndex(BindIdx++, ClassName); }
	Stmt.SetBindingValueByIndex(BindIdx++, Limit);
	Stmt.SetBindingValueByIndex(BindIdx++, Page * Limit);

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString OClass, FName, RetType, FModule, Specs;
		int32 BpCallable = 0;
		Stmt.GetColumnValueByIndex(0, OClass);
		Stmt.GetColumnValueByIndex(1, FName);
		Stmt.GetColumnValueByIndex(2, RetType);
		Stmt.GetColumnValueByIndex(3, BpCallable);
		Stmt.GetColumnValueByIndex(4, FModule);
		Stmt.GetColumnValueByIndex(5, Specs);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("owning_class"), OClass);
		R->SetStringField(TEXT("function_name"), FName);
		R->SetStringField(TEXT("return_type"), RetType);
		R->SetBoolField(TEXT("blueprint_callable"), BpCallable != 0);
		R->SetStringField(TEXT("cpp_module"), FModule);
		R->SetStringField(TEXT("specifiers"), Specs);
		Rows.Add(MakeShared<FJsonValueObject>(R));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("ufunctions"), Rows);

	if (!bHasCursor)
	{
		FString CountSql = TEXT("SELECT COUNT(*) FROM reflect_ufunctions ") + WhereSql + TEXT(";");
		FSQLitePreparedStatement CountStmt;
		if (CountStmt.Create(*DB, *CountSql))
		{
			int32 CBind = 1;
			if (!ClassName.IsEmpty()) { CountStmt.SetBindingValueByIndex(CBind++, ClassName); }
			if (CountStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int32 Total = 0;
				CountStmt.GetColumnValueByIndex(0, Total);
				CachedTotal = Total;
				Out->SetNumberField(TEXT("total_estimate"), CachedTotal);
			}
		}
	}

	if (Rows.Num() == Limit)
	{
		FCppReflectCursorState OutCursor;
		OutCursor.QueryHash = FilterHash;
		OutCursor.Page = Page + 1;
		OutCursor.CachedTotalEstimate = CachedTotal;
		Out->SetStringField(TEXT("next_cursor"), EncodeCursor(OutCursor));
	}
	return FMonolithActionResult::Success(Out);
}

FMonolithActionResult FCppReflectQueryAdapter::HandleFindInterfaceImpls(const TSharedPtr<FJsonObject>& Params)
{
	FSQLiteDatabase* DB = GetRawDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(
			TEXT("EngineSource.db not available. Run source.trigger_reindex to bootstrap."));
	}

	const FString InterfaceName = Params->GetStringField(TEXT("interface_name"));
	if (InterfaceName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("`interface_name` is required."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	// Join the implementer rows with their reflect_uclasses entry so we can
	// surface module + source_path for each implementer.
	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*DB, TEXT(
		"SELECT impl.implementing_class, impl.cpp_module, cls.source_path "
		"FROM reflect_uinterface_impls impl "
		"LEFT JOIN reflect_uclasses cls "
		"  ON cls.class_name = impl.implementing_class "
		" AND cls.module_name = impl.cpp_module "
		"WHERE impl.interface_name = ? "
		"ORDER BY impl.cpp_module, impl.implementing_class;")))
	{
		return FMonolithActionResult::Error(TEXT("SELECT prepare failed."));
	}
	Stmt.SetBindingValueByIndex(1, InterfaceName);

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString CName, MName, SPath;
		Stmt.GetColumnValueByIndex(0, CName);
		Stmt.GetColumnValueByIndex(1, MName);
		Stmt.GetColumnValueByIndex(2, SPath);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("implementing_class"), CName);
		R->SetStringField(TEXT("cpp_module"), MName);
		R->SetStringField(TEXT("source_path"), SPath);
		Rows.Add(MakeShared<FJsonValueObject>(R));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("interface_name"), InterfaceName);
	Out->SetArrayField(TEXT("implementers"), Rows);
	return FMonolithActionResult::Success(Out);
}

FMonolithActionResult FCppReflectQueryAdapter::HandleFindClassSpecifier(const TSharedPtr<FJsonObject>& Params)
{
	FSQLiteDatabase* DB = GetRawDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(
			TEXT("EngineSource.db not available. Run source.trigger_reindex to bootstrap."));
	}

	const FString SpecifierName = Params->GetStringField(TEXT("specifier_name"));
	if (SpecifierName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("`specifier_name` is required."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	const int32 ReqLimit = Params->HasField(TEXT("limit"))
		? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;
	const FString CursorIn = Params->HasField(TEXT("cursor"))
		? Params->GetStringField(TEXT("cursor")) : FString();

	constexpr int32 HARD_CAP = 200;
	const int32 Limit = FMath::Clamp(ReqLimit, 1, HARD_CAP);
	const uint32 FilterHash = ComputeFilterHash({ SpecifierName });

	int32 Page = 0;
	const bool bHasCursor = !CursorIn.IsEmpty();
	if (bHasCursor)
	{
		FCppReflectCursorState State;
		if (!DecodeCursor(CursorIn, State))
		{
			return InvalidCursorError(TEXT("Cursor decode failed; restart pagination without `cursor`."));
		}
		if (State.QueryHash != FilterHash)
		{
			return InvalidCursorError(TEXT("Cursor filter mismatch; restart pagination without `cursor`."));
		}
		Page = State.Page;
	}

	// Match against the colon-delimited `flags` column. SQLite LIKE pattern:
	//   `flags = ?`                exact-only (whole specifier list = match)
	//   `flags LIKE ?:%`           starts with this specifier
	//   `flags LIKE %:?:%`         contains this specifier in the middle
	//   `flags LIKE %:?`           ends with this specifier
	// We OR all four shapes to catch any position. Simpler than a regex.
	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*DB, TEXT(
		"SELECT class_name, module_name, parent_class, source_path, flags "
		"FROM reflect_uclasses "
		"WHERE flags = ?1 OR flags LIKE ?2 OR flags LIKE ?3 OR flags LIKE ?4 "
		"ORDER BY module_name, class_name "
		"LIMIT ? OFFSET ?;")))
	{
		return FMonolithActionResult::Error(TEXT("SELECT prepare failed."));
	}
	const FString ExactMatch = SpecifierName;
	const FString PrefixMatch = SpecifierName + TEXT(":%");
	const FString MidMatch    = TEXT("%:") + SpecifierName + TEXT(":%");
	const FString SuffixMatch = TEXT("%:") + SpecifierName;
	Stmt.SetBindingValueByIndex(1, ExactMatch);
	Stmt.SetBindingValueByIndex(2, PrefixMatch);
	Stmt.SetBindingValueByIndex(3, MidMatch);
	Stmt.SetBindingValueByIndex(4, SuffixMatch);
	Stmt.SetBindingValueByIndex(5, Limit);
	Stmt.SetBindingValueByIndex(6, Page * Limit);

	TArray<TSharedPtr<FJsonValue>> Rows;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString CName, MName, Parent, SPath, Flags;
		Stmt.GetColumnValueByIndex(0, CName);
		Stmt.GetColumnValueByIndex(1, MName);
		Stmt.GetColumnValueByIndex(2, Parent);
		Stmt.GetColumnValueByIndex(3, SPath);
		Stmt.GetColumnValueByIndex(4, Flags);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("class_name"), CName);
		R->SetStringField(TEXT("module_name"), MName);
		R->SetStringField(TEXT("parent_class"), Parent);
		R->SetStringField(TEXT("source_path"), SPath);
		R->SetStringField(TEXT("flags"), Flags);
		Rows.Add(MakeShared<FJsonValueObject>(R));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("specifier_name"), SpecifierName);
	Out->SetArrayField(TEXT("uclasses"), Rows);

	if (Rows.Num() == Limit)
	{
		FCppReflectCursorState OutCursor;
		OutCursor.QueryHash = FilterHash;
		OutCursor.Page = Page + 1;
		OutCursor.CachedTotalEstimate = -1;
		Out->SetStringField(TEXT("next_cursor"), EncodeCursor(OutCursor));
	}
	return FMonolithActionResult::Success(Out);
}
