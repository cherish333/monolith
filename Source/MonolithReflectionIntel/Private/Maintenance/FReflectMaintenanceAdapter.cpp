// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Maintenance — v0.17.0).
//
// FReflectMaintenanceAdapter — implementation. A single write/maintenance
// handler that force-rebuilds the Reflection Intelligence reflect_* tables from
// PROJECT UHT artefacts only. See the header for the namespace-placement
// rationale and FMonolithReflectionIntelModule::ForceRebuildReflectionTables for
// the force-semantics + project-only contract.

#include "Maintenance/FReflectMaintenanceAdapter.h"
#include "MonolithReflectionIntelModule.h"

#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"
#include "MonolithParamSchema.h"

// ============================================================================
// Registration
// ============================================================================

void FReflectMaintenanceAdapter::RegisterActions(FMonolithToolRegistry& Registry)
{
	// ---- rebuild_reflection_index ----
	Registry.RegisterAction(TEXT("reflect"), TEXT("rebuild_reflection_index"),
		TEXT("MAINTENANCE (write): force-repopulate the Reflection Intelligence "
		     "reflect_* tables from PROJECT UHT artefacts only (engine excluded). "
		     "Rebuilds the cppreflect set (reflect_uclasses / reflect_uproperties / "
		     "reflect_ufunctions / reflect_uinterfaces / reflect_uinterface_impls "
		     "+ cpp_asset_edges) via FUHTArtefactReader + FAssetGraphJoiner, AND the "
		     "network set (reflect_replicated_properties) via FNetworkRepIndexer. "
		     "Each indexer's Run() WIPEs (DELETE FROM) its tables before "
		     "repopulating, so this is a genuine clear+rewrite — NOT an append. "
		     "Use this after the project is rebuilt with UBT (or after the indexer "
		     "parsing code changes) to refresh tables that would otherwise stay "
		     "stale: the lazy bootstrap only fires when a table is ABSENT, and "
		     "source.trigger_(project_)reindex rebuilds the source-symbol tables, "
		     "NOT these reflect_* tables. Project-scoped: roots resolve to "
		     "Intermediate/Build and bIncludeEnginePlugins follows the "
		     "MonolithReflectionIntel setting (default false) — it never scans the "
		     "engine or rebuilds the engine source-symbol index. Requires the "
		     "project to have been built with UBT at least once (Live Coding "
		     "patches do not emit gen.cpp). Returns a per-indexer status object. "
		     "Game-thread; no parameters."),
		FMonolithActionHandler::CreateStatic(&FReflectMaintenanceAdapter::HandleRebuildReflectionIndex),
		FParamSchemaBuilder().Build());

	// Dispatcher annotation — this dispatcher hosts a WRITE/maintenance action,
	// so it is NOT read-only. It is non-destructive (idempotent regeneration: the
	// tables are wiped + rebuilt from on-disk artefacts, never partially mutated
	// from caller input) and idempotent (re-running yields identical tables for
	// unchanged artefacts). readOnlyHint is deliberately left false to signal the
	// index mutation to clients.
	FMonolithDispatcherAnnotations Anno;
	Anno.bReadOnlyHint    = false;
	Anno.bDestructiveHint = false;
	Anno.bIdempotentHint  = true;
	Anno.Title = TEXT("Reflection-index maintenance");
	Registry.SetDispatcherAnnotations(TEXT("reflect"), Anno);
}

// ============================================================================
// Handler
// ============================================================================

FMonolithActionResult FReflectMaintenanceAdapter::HandleRebuildReflectionIndex(
	const TSharedPtr<FJsonObject>& /*Params*/)
{
	// Game-thread only — ForceRebuildReflectionTables touches GEditor + the shared
	// SQLite handle. Assert here at the dispatch boundary (the module method and
	// the indexers' Run() also assert, but failing fast at entry is clearer).
	ensure(IsInGameThread());

	FMonolithReflectionIntelModule* Module =
		FModuleManager::GetModulePtr<FMonolithReflectionIntelModule>(
			TEXT("MonolithReflectionIntel"));
	if (!Module)
	{
		return FMonolithActionResult::Error(
			TEXT("MonolithReflectionIntel module not loaded."));
	}

	FString CppReflectStatus;
	FString NetworkStatus;
	const bool bAllOk = Module->ForceRebuildReflectionTables(CppReflectStatus, NetworkStatus);

	// Per-indexer status object. `rebuilt` is the AND of both runner results so a
	// caller can branch on overall success while still reading each indexer's
	// detailed status string (which carries row counts, "no UHT artefacts found"
	// guidance, or the "source DB not open — bootstrap" message on failure).
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("rebuilt"), bAllOk);
	Out->SetStringField(TEXT("cppreflect"), CppReflectStatus);
	Out->SetStringField(TEXT("network"), NetworkStatus);
	Out->SetBoolField(TEXT("project_only"), true);
	return FMonolithActionResult::Success(Out);
}
