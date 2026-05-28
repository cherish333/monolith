#include "MonolithNiagaraTimingActions.h"
#include "MonolithNiagaraActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "NiagaraSystem.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithNiagaraTiming, Log, All);

// ============================================================================
//  Local file-static helpers (mirror MonolithNiagaraActions.cpp:686-705 pattern)
// ============================================================================

namespace MonolithNiagaraTimingLocal
{
	static FMonolithActionResult SuccessStr(const FString& Msg)
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("result"), Msg);
		return FMonolithActionResult::Success(R);
	}

	static FMonolithActionResult SuccessObj(const TSharedRef<FJsonObject>& Obj)
	{
		return FMonolithActionResult::Success(Obj);
	}

	static FString GetAssetPath(const TSharedPtr<FJsonObject>& Params)
	{
		FString Path = Params->GetStringField(TEXT("asset_path"));
		if (Path.IsEmpty()) Path = Params->GetStringField(TEXT("system_path"));
		return Path;
	}

	static UNiagaraSystem* LoadSystem(const FString& SystemPath)
	{
		UNiagaraSystem* System = FMonolithAssetUtils::LoadAssetByPath<UNiagaraSystem>(SystemPath);
		if (!System)
		{
			UE_LOG(LogMonolithNiagaraTiming, Error, TEXT("Failed to load Niagara system: %s"), *SystemPath);
		}
		return System;
	}
}

using namespace MonolithNiagaraTimingLocal;

// ============================================================================
//  Registration
// ============================================================================

void FMonolithNiagaraTimingActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_system_timing"),
		TEXT("**Phase 0 stub.** Read system-level timing fields (warmup, fixed tick delta, require current frame data). Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleGetSystemTiming),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_warmup_profile"),
		TEXT("**Phase 0 stub.** Composite write of warmup_time + warmup_tick_delta on a Niagara system. Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleSetWarmupProfile),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("warmup_time"), TEXT("number"), TEXT("Target warmup time in seconds (snaps to nearest tick_count x tick_delta multiple)"))
			.Optional(TEXT("warmup_tick_delta"), TEXT("number"), TEXT("Tick delta in seconds (default: existing system value, typically 1/15s)"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_fixed_tick_delta"),
		TEXT("**Phase 0 stub.** Set bFixedTickDelta + FixedTickDeltaTime on a Niagara system. Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleSetFixedTickDelta),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("enabled"), TEXT("bool"), TEXT("Enable fixed-tick substepping"))
			.Optional(TEXT("fixed_delta_time"), TEXT("number"), TEXT("Fixed delta time in seconds (only meaningful when enabled=true; default 1/60s if unset)"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_require_current_frame_data"),
		TEXT("**Phase 0 stub.** Toggle bRequireCurrentFrameData on a Niagara system. Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleSetRequireCurrentFrameData),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("require"), TEXT("bool"), TEXT("If true, tick-group dependencies use strict current-frame data; if false, looser previous-frame data permitted"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_emitter_loop_profile"),
		TEXT("**Phase 0 stub.** Composite write of EmitterState loop inputs (LoopBehavior, LoopDuration, LoopDelay, LoopCount, bLoopDelayEnabled). Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleSetEmitterLoopProfile),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("get_emitter_timing_summary"),
		TEXT("**Phase 0 stub.** Aggregated read of emitter timing (loop config, sim stages, particle lifetime bounds). Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleGetEmitterTimingSummary),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_sim_stage_iteration_count"),
		TEXT("Alias for set_simulation_stage_property(property=\"NumIterations\"). Sets NumIterations on a UNiagaraSimulationStageGeneric (FNiagaraParameterBindingWithValue — written via reflection ImportText_Direct). Provide exactly one of stage_index / stage_name."),
		FMonolithActionHandler::CreateStatic(&HandleSetSimStageIterationCount),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter handle id/name (same convention as set_simulation_stage_property)"))
			.Optional(TEXT("stage_index"), TEXT("integer"), TEXT("0-based index into the emitter's simulation stages (use this OR stage_name)"))
			.Optional(TEXT("stage_name"), TEXT("string"), TEXT("Simulation stage name (use this OR stage_index)"))
			.Required(TEXT("iterations"), TEXT("integer"), TEXT("Iteration count to write to NumIterations"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_sim_stage_execute_behavior"),
		TEXT("Alias for set_simulation_stage_property(property=\"ExecuteBehavior\"). Sets ExecuteBehavior on a UNiagaraSimulationStageGeneric. Provide exactly one of stage_index / stage_name."),
		FMonolithActionHandler::CreateStatic(&HandleSetSimStageExecuteBehavior),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter handle id/name (same convention as set_simulation_stage_property)"))
			.Optional(TEXT("stage_index"), TEXT("integer"), TEXT("0-based index into the emitter's simulation stages (use this OR stage_name)"))
			.Optional(TEXT("stage_name"), TEXT("string"), TEXT("Simulation stage name (use this OR stage_index)"))
			.Required(TEXT("behavior"), TEXT("string"), TEXT("ENiagaraSimStageExecuteBehavior name: 'Always' | 'OnSimulationReset' | 'NotOnSimulationReset' (snake_case also accepted)"))
			.Build());

	Registry.RegisterAction(TEXT("niagara"), TEXT("set_particle_lifetime"),
		TEXT("**Phase 0 stub.** Set Lifetime min/max on the Initialize Particle module. Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleSetParticleLifetime),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset"))
			.Build());
}

// ============================================================================
//  Phase 1 — System-level handlers (real implementations)
// ============================================================================

FMonolithActionResult FMonolithNiagaraTimingActions::HandleGetSystemTiming(const TSharedPtr<FJsonObject>& Params)
{
	const FString SystemPath = GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), SystemPath);
	R->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	R->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
	R->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
	R->SetBoolField(TEXT("fixed_tick_delta_enabled"), System->HasFixedTickDelta());
	R->SetNumberField(TEXT("fixed_tick_delta_time"), System->GetFixedTickDeltaTime());

	// bRequireCurrentFrameData is a uint8:1 bitfield with no inline getter — reflect it.
	bool bRequireCurrent = false;
	if (FBoolProperty* BP = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bRequireCurrentFrameData")))
	{
		bRequireCurrent = BP->GetPropertyValue_InContainer(System);
	}
	R->SetBoolField(TEXT("require_current_frame_data"), bRequireCurrent);

	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetWarmupProfile(const TSharedPtr<FJsonObject>& Params)
{
	const FString SystemPath = GetAssetPath(Params);

	// warmup_time required (per plan § Phase 1 spec)
	TSharedPtr<FJsonValue> WarmupTimeJV = Params->TryGetField(TEXT("warmup_time"));
	if (!WarmupTimeJV.IsValid() || WarmupTimeJV->Type != EJson::Number)
		return FMonolithActionResult::Error(TEXT("Missing required field: warmup_time (number)"));
	const float WarmupTimeIn = static_cast<float>(WarmupTimeJV->AsNumber());

	// warmup_tick_delta optional
	bool bHasTickDelta = false;
	float TickDeltaIn = 0.0f;
	TSharedPtr<FJsonValue> TickDeltaJV = Params->TryGetField(TEXT("warmup_tick_delta"));
	if (TickDeltaJV.IsValid() && TickDeltaJV->Type == EJson::Number)
	{
		TickDeltaIn = static_cast<float>(TickDeltaJV->AsNumber());
		bHasTickDelta = true;
	}

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetWarmupProfile", "Set Niagara Warmup Profile"));
	System->Modify();

	// SetWarmupTickDelta first so SetWarmupTime resolves count against the new delta
	// (both setters internally call ResolveWarmupTickCount).
	if (bHasTickDelta)
	{
		System->SetWarmupTickDelta(TickDeltaIn);
	}
	System->SetWarmupTime(WarmupTimeIn);

	// Fire PostEditChangeProperty for each touched UPROPERTY so the editor/graph dirties correctly.
	if (FProperty* TimeProp = FindFProperty<FProperty>(UNiagaraSystem::StaticClass(), TEXT("WarmupTime")))
	{
		FPropertyChangedEvent PCE(TimeProp);
		System->PostEditChangeProperty(PCE);
	}
	if (bHasTickDelta)
	{
		if (FProperty* DeltaProp = FindFProperty<FProperty>(UNiagaraSystem::StaticClass(), TEXT("WarmupTickDelta")))
		{
			FPropertyChangedEvent PCE(DeltaProp);
			System->PostEditChangeProperty(PCE);
		}
	}

	System->RequestCompile(false);
	GEditor->EndTransaction();

	// Response: resolved triple so callers observe the snap (WarmupTime rounds to TickCount * TickDelta).
	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), SystemPath);
	R->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	R->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
	R->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetFixedTickDelta(const TSharedPtr<FJsonObject>& Params)
{
	const FString SystemPath = GetAssetPath(Params);

	// enabled required (bool)
	TSharedPtr<FJsonValue> EnabledJV = Params->TryGetField(TEXT("enabled"));
	if (!EnabledJV.IsValid() || EnabledJV->Type != EJson::Boolean)
		return FMonolithActionResult::Error(TEXT("Missing required field: enabled (bool)"));
	const bool bEnabled = EnabledJV->AsBool();

	// fixed_delta_time optional (number)
	bool bHasDeltaTime = false;
	float FixedDeltaIn = 0.0f;
	TSharedPtr<FJsonValue> DeltaJV = Params->TryGetField(TEXT("fixed_delta_time"));
	if (DeltaJV.IsValid() && DeltaJV->Type == EJson::Number)
	{
		FixedDeltaIn = static_cast<float>(DeltaJV->AsNumber());
		bHasDeltaTime = true;
	}

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FBoolProperty* EnabledProp = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bFixedTickDelta"));
	if (!EnabledProp)
		return FMonolithActionResult::Error(TEXT("UPROPERTY 'bFixedTickDelta' not found on UNiagaraSystem"));
	FFloatProperty* DeltaProp = nullptr;
	if (bHasDeltaTime)
	{
		DeltaProp = FindFProperty<FFloatProperty>(UNiagaraSystem::StaticClass(), TEXT("FixedTickDeltaTime"));
		if (!DeltaProp)
			return FMonolithActionResult::Error(TEXT("UPROPERTY 'FixedTickDeltaTime' not found on UNiagaraSystem"));
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetFixedTickDelta", "Set Niagara Fixed Tick Delta"));
	System->Modify();

	EnabledProp->SetPropertyValue_InContainer(System, bEnabled);
	if (DeltaProp)
	{
		DeltaProp->SetPropertyValue_InContainer(System, FixedDeltaIn);
	}

	{
		FPropertyChangedEvent PCE(EnabledProp);
		System->PostEditChangeProperty(PCE);
	}
	if (DeltaProp)
	{
		FPropertyChangedEvent PCE(DeltaProp);
		System->PostEditChangeProperty(PCE);
	}

	System->RequestCompile(false);
	GEditor->EndTransaction();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), SystemPath);
	R->SetBoolField(TEXT("fixed_tick_delta_enabled"), System->HasFixedTickDelta());
	R->SetNumberField(TEXT("fixed_tick_delta_time"), System->GetFixedTickDeltaTime());
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetRequireCurrentFrameData(const TSharedPtr<FJsonObject>& Params)
{
	const FString SystemPath = GetAssetPath(Params);

	TSharedPtr<FJsonValue> RequireJV = Params->TryGetField(TEXT("require"));
	if (!RequireJV.IsValid() || RequireJV->Type != EJson::Boolean)
		return FMonolithActionResult::Error(TEXT("Missing required field: require (bool)"));
	const bool bRequire = RequireJV->AsBool();

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FBoolProperty* Prop = FindFProperty<FBoolProperty>(UNiagaraSystem::StaticClass(), TEXT("bRequireCurrentFrameData"));
	if (!Prop)
		return FMonolithActionResult::Error(TEXT("UPROPERTY 'bRequireCurrentFrameData' not found on UNiagaraSystem"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetRequireCurrentFrameData", "Set Niagara Require Current Frame Data"));
	System->Modify();

	Prop->SetPropertyValue_InContainer(System, bRequire);

	{
		FPropertyChangedEvent PCE(Prop);
		System->PostEditChangeProperty(PCE);
	}

	System->RequestCompile(false);
	GEditor->EndTransaction();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), SystemPath);
	R->SetBoolField(TEXT("require_current_frame_data"), Prop->GetPropertyValue_InContainer(System));
	return SuccessObj(R);
}

// ============================================================================
//  Phase 2-4 — stubs (untouched by Phase 1)
// ============================================================================

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetEmitterLoopProfile(const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithActionResult::Error(TEXT("not implemented"));
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleGetEmitterTimingSummary(const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithActionResult::Error(TEXT("not implemented"));
}

// ----------------------------------------------------------------------------
//  Phase 2 — Sim-stage alias dispatch helper
// ----------------------------------------------------------------------------
//
// Both Phase-2 actions are thin aliases over niagara::set_simulation_stage_property
// (MonolithNiagaraActions.cpp:11495-11652). We dispatch through the public
// registry rather than calling FMonolithNiagaraActions::HandleSetSimulationStageProperty
// directly because the canonical handler is a private static in another TU
// (same private-static cross-TU issue as Phase 1's LoadSystem). Registry
// dispatch is schema-validated, single source of truth, zero duplication.
//
namespace MonolithNiagaraTimingLocal
{
	// Build the JSON envelope set_simulation_stage_property expects:
	//   { asset_path, emitter, stage_index?, stage_name?, property, value }
	// then dispatch via FMonolithToolRegistry::Get().ExecuteAction.
	static FMonolithActionResult DispatchSimStageAlias(
		const TSharedPtr<FJsonObject>& Params,
		const FString& Property,
		const TSharedPtr<FJsonValue>& Value)
	{
		// Forward asset_path / system_path (canonical handler reads asset_path; the
		// upstream GetAssetPath also accepts system_path so either works).
		const FString SystemPath = GetAssetPath(Params);
		if (SystemPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

		// Forward emitter (required by canonical handler).
		FString Emitter;
		if (!Params->TryGetStringField(TEXT("emitter"), Emitter) || Emitter.IsEmpty())
			return FMonolithActionResult::Error(TEXT("Missing required field: emitter (string)"));

		// Forward exactly one of stage_index / stage_name (canonical handler
		// validates that at least one is supplied; we mirror that here to give a
		// clearer error attributed to the alias).
		const bool bHasIndex = Params->HasTypedField<EJson::Number>(TEXT("stage_index"));
		const bool bHasName = Params->HasTypedField<EJson::String>(TEXT("stage_name"));
		if (!bHasIndex && !bHasName)
			return FMonolithActionResult::Error(TEXT("Must provide stage_index or stage_name"));

		if (!Value.IsValid())
			return FMonolithActionResult::Error(TEXT("Internal: alias passed null value"));

		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("asset_path"), SystemPath);
		Envelope->SetStringField(TEXT("emitter"), Emitter);
		if (bHasIndex)
		{
			Envelope->SetNumberField(TEXT("stage_index"), Params->GetNumberField(TEXT("stage_index")));
		}
		if (bHasName)
		{
			Envelope->SetStringField(TEXT("stage_name"), Params->GetStringField(TEXT("stage_name")));
		}
		Envelope->SetStringField(TEXT("property"), Property);
		Envelope->SetField(TEXT("value"), Value);

		return FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("niagara"), TEXT("set_simulation_stage_property"), Envelope);
	}
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetSimStageIterationCount(const TSharedPtr<FJsonObject>& Params)
{
	// iterations: integer (required) — NumIterations on UNiagaraSimulationStageGeneric is
	// FNiagaraParameterBindingWithValue, written via ImportText_Direct fallback at
	// MonolithNiagaraActions.cpp:11600-11635. The fallback stringifies a JSON Number
	// using "%g" before ImportText_Direct, so we forward the integer as a JSON Number.
	TSharedPtr<FJsonValue> IterJV = Params->TryGetField(TEXT("iterations"));
	if (!IterJV.IsValid() || IterJV->Type != EJson::Number)
		return FMonolithActionResult::Error(TEXT("Missing required field: iterations (integer)"));

	const int32 Iterations = static_cast<int32>(IterJV->AsNumber());
	if (Iterations < 0)
		return FMonolithActionResult::Error(TEXT("iterations must be >= 0"));

	TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(FString::Printf(TEXT("(Value=%d)"), Iterations));
	return MonolithNiagaraTimingLocal::DispatchSimStageAlias(Params, TEXT("NumIterations"), Value);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetSimStageExecuteBehavior(const TSharedPtr<FJsonObject>& Params)
{
	// behavior: string (required) — one of Always / OnSimulationReset / NotOnSimulationReset.
	// The canonical handler at MonolithNiagaraActions.cpp:11572-11578 accepts both
	// PascalCase and snake_case via a .ToLower() comparison, so we forward verbatim
	// and let it do the enum mapping. Validate non-empty here for a clearer error.
	FString Behavior;
	if (!Params->TryGetStringField(TEXT("behavior"), Behavior) || Behavior.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required field: behavior (string: 'Always' | 'OnSimulationReset' | 'NotOnSimulationReset')"));

	TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueString>(Behavior);
	return MonolithNiagaraTimingLocal::DispatchSimStageAlias(Params, TEXT("ExecuteBehavior"), Value);
}

FMonolithActionResult FMonolithNiagaraTimingActions::HandleSetParticleLifetime(const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithActionResult::Error(TEXT("not implemented"));
}
