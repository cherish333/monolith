// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 1).
//
// DEVIATION (vs plan §6 row "Modify MonolithSourceSubsystem.cpp — register
// FDecisionRecordIndexer in the indexer roster"):
// UMonolithSourceSubsystem does NOT expose an indexer-roster API in the live
// codebase; it owns a single FMonolithSourceIndexer for C++ source mining
// only. To keep dependency direction correct (MonolithReflectionIntel ->
// MonolithSource is the legal direction; the reverse would be circular),
// the indexer self-bootstraps:
//   1. Once on demand from FDecisionQueryAdapter when a table miss occurs.
//   2. On FCoreUObjectDelegates::ReloadCompleteDelegate (Live Coding / hot
//      reload) so corpus changes since editor start are picked up.
// Net effect matches the plan's intent: decision_query results stay fresh,
// and the indexer runs in the same editor lifecycle as the source subsystem.

#include "MonolithReflectionIntelModule.h"
#include "Decision/FDecisionRecordIndexer.h"
#include "Decision/FDecisionQueryAdapter.h"
#include "Risk/FGitCoChangeIndexer.h"
#include "Risk/FHotspotScorer.h"
#include "Risk/FConditionalGateIndexer.h"
#include "Risk/FRiskQueryAdapter.h"
#include "SourceAudit/FModuleDepRealityAdapter.h"
#include "CppReflect/FUHTArtefactReader.h"
#include "CppReflect/FAssetGraphJoiner.h"
#include "CppReflect/FCppReflectQueryAdapter.h"
// Phase 4a (v0.17.0) headers — network namespace, audit-action extensions,
// pipeline composers.
#include "Network/FNetworkRepIndexer.h"
#include "Network/FNetworkQueryAdapter.h"
#include "Audit/FAuditAdapter.h"
#include "Pipeline/FPipelineAdapter.h"
// Maintenance (v0.17.0) — `reflect` namespace force-rebuild adapter.
#include "Maintenance/FReflectMaintenanceAdapter.h"
#include "MonolithReflectionIntelSettings.h"

#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MonolithToolRegistry.h"
#include "SQLiteDatabase.h"
#include "UObject/UObjectGlobals.h"

// Shared-handle borrow: route RI's read-only queries (and the bootstrap
// indexer writes) through UMonolithSourceSubsystem's already-open EngineSource.db
// handle, instead of opening a second handle that the UE 5.7 single-open
// `unreal-fs` SQLite VFS rejects with SQLITE_IOERR.
#include "MonolithSourceSubsystem.h"
#include "MonolithSourceDatabase.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY(LogMonolithReflectionIntel);

#define LOCTEXT_NAMESPACE "FMonolithReflectionIntelModule"

namespace
{
	// NOTE: the former GetEngineSourceDbPathStatic() helper was removed — RI no
	// longer resolves the DB path itself, since it borrows the subsystem's
	// already-open handle rather than opening EngineSource.db by path.

	/**
	 * Resolve the live FMonolithReflectionIntelModule instance from inside a
	 * static runner. Returns nullptr if the module is unloaded — caller MUST
	 * tolerate this (the runners log + fall through to their best-effort path).
	 */
	FMonolithReflectionIntelModule* GetReflectionIntelModulePtr()
	{
		return FModuleManager::GetModulePtr<FMonolithReflectionIntelModule>(
			TEXT("MonolithReflectionIntel"));
	}

	/**
	 * Resolve the source subsystem's FMonolithSourceDatabase wrapper (NOT the raw
	 * SQLite handle). Returns nullptr when the editor / subsystem / DB is not
	 * available — every caller tolerates this and surfaces a clean
	 * "run source.trigger_reindex" / "editor not running" state.
	 *
	 * Game-thread only: GEditor and editor subsystems must be touched on the game
	 * thread. All RI action handlers and the bootstrap runners execute on the game
	 * thread (FModuleDepRealityAdapter::GetRawDB ensure(IsInGameThread()) is the
	 * canonical witness), so this is safe.
	 */
	FMonolithSourceDatabase* GetSharedSourceDatabase()
	{
		if (!GEditor)
		{
			return nullptr;
		}
		UMonolithSourceSubsystem* SourceSS =
			GEditor->GetEditorSubsystem<UMonolithSourceSubsystem>();
		return SourceSS ? SourceSS->GetDatabase() : nullptr;
	}
}

void FMonolithReflectionIntelModule::StartupModule()
{
	// Explicit re-arm of the lazy-bootstrap latches on every module load. A
	// fresh module instance always starts with all four latches cleared so
	// that Live Coding reloads re-attempt bootstrap if a prior attempt failed.
	bDecisionBootstrapAttempted = false;
	bRiskBootstrapAttempted = false;
	bCppReflectBootstrapAttempted = false;
	bNetworkBootstrapAttempted = false;

	RegisterDecisionActions();
	// Phase 2 (v0.17.0) — risk_query namespace + source_query audit action.
	RegisterRiskActions();
	RegisterSourceAuditActions();
	// Phase 3a (v0.17.0) — cppreflect_query namespace (6 actions).
	RegisterCppReflectActions();
	// Phase 4a (v0.17.0) — network_query namespace (4 actions) + 4 audit
	// actions on existing namespaces + pipeline_query namespace (2 composers).
	RegisterNetworkActions();
	RegisterAuditActions();
	RegisterPipelineActions();
	// Maintenance (v0.17.0) — reflect_query namespace (1 force-rebuild action).
	RegisterMaintenanceActions();

	// Bind hot-reload hook so the decision corpus refreshes after Live Coding /
	// UBT rebuilds. The indexer pass now WRITES THROUGH the subsystem's shared
	// open handle (under its lock) rather than opening its own — so there is no
	// second-handle collision with the single-open `unreal-fs` VFS. If a reindex
	// is in progress the subsystem's handle is closed and the runners cleanly
	// fast-return "source DB not open" (retry on a later signal).
	ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddRaw(
		this, &FMonolithReflectionIntelModule::OnReloadComplete);

	// NO eager bootstrap on StartupModule — the source subsystem may not have
	// finished opening its EngineSource.db handle at module-load time. All
	// indexer bootstrap is LAZY and writes through the subsystem's shared handle:
	//   - Decision:    driven on first decision_query call.
	//   - Risk:        driven on first risk_query call.
	//   - CppReflect:  driven on first cppreflect_query call.
	//   - All:         driven on hot-reload via OnReloadComplete.

	UE_LOG(LogMonolithReflectionIntel, Log,
		TEXT("Monolith — ReflectionIntel module loaded (decision_query: 5 actions, "
		     "risk_query: 5 actions, source_query: +1 audit action, "
		     "cppreflect_query: 6 actions, network_query: 4 actions, "
		     "material/niagara/blueprint/project: +1 audit each, "
		     "pipeline_query: 2 composers, reflect_query: 1 maintenance action)"));
}

void FMonolithReflectionIntelModule::ShutdownModule()
{
	if (ReloadCompleteHandle.IsValid())
	{
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteHandle);
		ReloadCompleteHandle.Reset();
	}

	// Shared-handle policy: this module no longer owns a SQLite handle on
	// EngineSource.db (it borrows UMonolithSourceSubsystem's). Nothing to tear
	// down here — the subsystem closes its own handle in Deinitialize(). The
	// call is retained only because ResetCachedQueryDb() is part of the public
	// surface used by the adapters' legacy bootstrap path; it is now a no-op.
	ResetCachedQueryDb();

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("decision"));
	// Phase 2 — risk_query is fully owned by this module so unregister wholesale.
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("risk"));
	// Phase 3a — cppreflect_query is fully owned by this module.
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("cppreflect"));
	// Phase 4a — network_query is fully owned by this module.
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("network"));
	// Phase 4a — pipeline_query is fully owned by this module (no other
	// module registers there in Phase 4a).
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("pipeline"));
	// Maintenance — reflect_query is fully owned by this module.
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("reflect"));
	// NOTE: we do NOT unregister the `source` / `material` / `niagara` /
	// `blueprint` / `project` namespaces here — MonolithSource / MonolithMaterial
	// / MonolithNiagara / MonolithBlueprint / MonolithIndex own the lion's share
	// of those namespaces' actions. Our audit-action additions would also be
	// unregistered with a namespace-wide call. The risk is minimal: Phase 4a
	// ships with those modules loaded before us (the `MonolithCore` dep chain
	// enforces order) so shutdown-time leaks are bounded to one audit action
	// entry per host namespace per process exit.
}

FSQLiteDatabase* FMonolithReflectionIntelModule::GetOrOpenCachedQueryDb()
{
	// Borrow the subsystem's already-open handle. We open NOTHING here — the UE
	// 5.7 single-open `unreal-fs` SQLite VFS rejects a second open of the same
	// file with SQLITE_IOERR ("disk I/O error"), which is exactly the bug that
	// broke all ~25 RI query actions. Returns nullptr when the subsystem's handle
	// is not open (editor down, never indexed, or reindex has it closed) — the
	// caller surfaces "run source.trigger_reindex".
	FMonolithSourceDatabase* SharedDb = GetSharedSourceDatabase();
	if (!SharedDb)
	{
		return nullptr;
	}
	return SharedDb->GetRawHandle();
}

void FMonolithReflectionIntelModule::ResetCachedQueryDb()
{
	// No-op under the shared-handle policy — this module no longer owns a query
	// handle, so there is nothing to close. Retained for call-site compatibility
	// (the adapters' legacy lazy-bootstrap path still calls it; it must not touch
	// the subsystem's handle, which the subsystem alone owns).
}

void FMonolithReflectionIntelModule::RegisterDecisionActions()
{
	FDecisionQueryAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterRiskActions()
{
	FRiskQueryAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterSourceAuditActions()
{
	FModuleDepRealityAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterCppReflectActions()
{
	FCppReflectQueryAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterNetworkActions()
{
	FNetworkQueryAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterAuditActions()
{
	FAuditAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterPipelineActions()
{
	FPipelineAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::RegisterMaintenanceActions()
{
	FReflectMaintenanceAdapter::RegisterActions(FMonolithToolRegistry::Get());
}

void FMonolithReflectionIntelModule::OnReloadComplete(EReloadCompleteReason /*Reason*/)
{
	// Fire-and-log; never block the reload signal. Failure to refresh the
	// decision corpus is non-fatal — query handlers still return last-known data.
	FString DecisionStatus;
	RunDecisionIndexerOnce(DecisionStatus);

	// Phase 2 — also re-run risk indexers on hot-reload. The risk indexer is
	// more expensive (spawns `git log`), but a Live Coding rebuild typically
	// reflects code changes that warrant re-scoring complexity AND co-change
	// activity tends to spike around the commits the reload reflects.
	FString RiskStatus;
	RunRiskIndexersOnce(RiskStatus);

	// Phase 3a — UHT artefacts are exactly what a Live Coding rebuild
	// regenerates, so this is the single signal that means "reflection
	// edges may be stale". Refresh after every reload.
	FString CppReflectStatus;
	RunCppReflectIndexersOnce(CppReflectStatus);

	// Phase 4a — network rep metadata lives in the same UHT artefacts; rebuild
	// after reload so audit_unbalanced_onreps stays current. Audit actions
	// themselves are pure SQL/AR queries and don't need bootstrap.
	FString NetworkStatus;
	RunNetworkIndexerOnce(NetworkStatus);
}

bool FMonolithReflectionIntelModule::RunDecisionIndexerOnce(FString& OutStatus)
{
	// Latch policy (post-fix): the adapter sets bDecisionBootstrapAttempted = true
	// BEFORE calling this runner. On any TRANSIENT failure path below we CLEAR
	// the latch so the next adapter call retries — see header comment block on
	// bDecisionBootstrapAttempted. On full success we leave it set (true).
	// A retry-throttle (DecisionLastFailureTime + RetryCooldownSeconds) prevents
	// log spam when the DB is wedged across a burst of adapter calls.
	FMonolithReflectionIntelModule* Self = GetReflectionIntelModulePtr();

	// Throttle: if a prior call failed within the cooldown window, fast-return
	// without re-opening SQLite. We MUST clear the latch even during throttle
	// because the adapter sets it before calling us — leaving it set during
	// throttle would cause the NEXT adapter call (post-cooldown) to skip the
	// bootstrap entirely (HasAttempted=true → adapter returns DB without
	// retrying), re-creating the very latch-on-failure bug we are fixing.
	if (Self && Self->DecisionLastFailureTime > 0.0)
	{
		const double Now = FPlatformTime::Seconds();
		const double Elapsed = Now - Self->DecisionLastFailureTime;
		if (Elapsed < RetryCooldownSeconds)
		{
			OutStatus = FString::Printf(
				TEXT("RunDecisionIndexerOnce: throttled (last failure %.1fs ago, cooldown %.1fs)"),
				Elapsed, RetryCooldownSeconds);
			Self->bDecisionBootstrapAttempted = false;
			return false;
		}
		// Cooldown expired — fall through and try again. Reset the timer so a
		// further failure starts a fresh window.
		Self->DecisionLastFailureTime = 0.0;
	}

	const UMonolithReflectionIntelSettings* Settings = UMonolithReflectionIntelSettings::Get();
	if (!Settings || !Settings->bEnableDecisionMining)
	{
		OutStatus = TEXT("RunDecisionIndexerOnce: skipped (bEnableDecisionMining=false)");
		// Settings-disable is NOT a transient failure — it's a deliberate user
		// choice. Leave the latch set (the adapter set it) so we don't re-evaluate
		// the no-op every call. Do NOT mark LastFailureTime.
		return false;
	}

	// Borrow the subsystem's already-open ReadWrite handle for the indexer write
	// pass — do NOT open a second handle. The UE 5.7 single-open `unreal-fs` VFS
	// rejects a second open of EngineSource.db with SQLITE_IOERR, which is the
	// very bug we are fixing. The subsystem opens its handle ReadWrite, so the
	// indexer's CREATE TABLE / INSERT statements ride it fine.
	FMonolithSourceDatabase* SharedDb = GetSharedSourceDatabase();
	if (!SharedDb || !SharedDb->GetRawHandle())
	{
		OutStatus = TEXT("RunDecisionIndexerOnce: source DB not open (editor down, never indexed, or reindex in progress) — bootstrap with source.trigger_reindex");
		UE_LOG(LogMonolithReflectionIntel, Verbose, TEXT("%s"), *OutStatus);
		// Transient — the subsystem may open the handle later. Clear the latch
		// so the next adapter call retries.
		if (Self)
		{
			Self->bDecisionBootstrapAttempted = false;
			Self->DecisionLastFailureTime = FPlatformTime::Seconds();
		}
		return false;
	}

	TArray<FString> Roots = Settings->DecisionMarkdownRoots;
	if (Roots.Num() == 0)
	{
		Roots.Add(TEXT("Docs"));
		Roots.Add(TEXT("Plugins/Monolith/Docs"));
		Roots.Add(TEXT(".claude/rules"));
	}

	// Serialise the write pass against concurrent borrowed reads and the
	// subsystem's own locked methods by holding the shared DB lock for the whole
	// indexer Run. The journal mode is already DELETE (the subsystem flips it on
	// every open) so we do not re-flip it here.
	bool bOk = false;
	{
		FScopeLock Lock(&SharedDb->GetLock());
		FSQLiteDatabase* RawDb = SharedDb->GetRawHandle();
		if (!RawDb)
		{
			OutStatus = TEXT("RunDecisionIndexerOnce: source DB closed mid-bootstrap — will retry");
			if (Self)
			{
				Self->bDecisionBootstrapAttempted = false;
				Self->DecisionLastFailureTime = FPlatformTime::Seconds();
			}
			return false;
		}
		FDecisionRecordIndexer Indexer;
		bOk = Indexer.Run(*RawDb, Roots, OutStatus);
	}

	if (Self)
	{
		if (bOk)
		{
			// Success — latch stays set, clear any prior failure timestamp.
			Self->bDecisionBootstrapAttempted = true;
			Self->DecisionLastFailureTime = 0.0;
		}
		else
		{
			UE_LOG(LogMonolithReflectionIntel, Warning,
				TEXT("RunDecisionIndexerOnce: indexer reported failure; will retry after cooldown — %s"),
				*OutStatus);
			Self->bDecisionBootstrapAttempted = false;
			Self->DecisionLastFailureTime = FPlatformTime::Seconds();
		}
	}
	return bOk;
}

bool FMonolithReflectionIntelModule::RunRiskIndexersOnce(FString& OutStatus)
{
	// See RunDecisionIndexerOnce for latch-policy rationale. Same shape applied
	// across all four Phase-N runners.
	FMonolithReflectionIntelModule* Self = GetReflectionIntelModulePtr();

	if (Self && Self->RiskLastFailureTime > 0.0)
	{
		const double Now = FPlatformTime::Seconds();
		const double Elapsed = Now - Self->RiskLastFailureTime;
		if (Elapsed < RetryCooldownSeconds)
		{
			OutStatus = FString::Printf(
				TEXT("RunRiskIndexersOnce: throttled (last failure %.1fs ago, cooldown %.1fs)"),
				Elapsed, RetryCooldownSeconds);
			// Clear latch during throttle — see decision-runner comment for why.
			Self->bRiskBootstrapAttempted = false;
			return false;
		}
		Self->RiskLastFailureTime = 0.0;
	}

	const UMonolithReflectionIntelSettings* Settings = UMonolithReflectionIntelSettings::Get();
	if (!Settings || !Settings->bEnableGitCoChangeMining)
	{
		OutStatus = TEXT("RunRiskIndexersOnce: skipped (bEnableGitCoChangeMining=false)");
		// Settings-disable: not transient, leave latch set, no LastFailureTime.
		return false;
	}

	// Borrow the subsystem's open ReadWrite handle (see RunDecisionIndexerOnce
	// for the single-open VFS rationale). No second open, no journal re-flip.
	FMonolithSourceDatabase* SharedDb = GetSharedSourceDatabase();
	if (!SharedDb || !SharedDb->GetRawHandle())
	{
		OutStatus = TEXT("RunRiskIndexersOnce: source DB not open (editor down, never indexed, or reindex in progress) — bootstrap with source.trigger_reindex");
		UE_LOG(LogMonolithReflectionIntel, Verbose, TEXT("%s"), *OutStatus);
		if (Self)
		{
			Self->bRiskBootstrapAttempted = false;
			Self->RiskLastFailureTime = FPlatformTime::Seconds();
		}
		return false;
	}

	// Resolve git repo roots. Phase 2 mines NESTED git repos only — the
	// project's outer working tree is tracked by Diversion, not git, and lacks
	// a `.git` directory; FGitCoChangeIndexer silently skips it. Standard
	// nested repos under Leviathan today: Monolith plugin, Resonance plugin.
	// Future-siblings (`MonolithSteamBridge`, `MonolithISX`, etc.) live under
	// `Plugins/` so we add them by default — the indexer skips any without
	// `.git/`.
	TArray<FString> GitRoots;
	GitRoots.Add(TEXT("Plugins/Monolith"));
	GitRoots.Add(TEXT("Plugins/Resonance"));
	GitRoots.Add(TEXT("Plugins/MonolithSteamBridge"));
	GitRoots.Add(TEXT("Plugins/MonolithISX"));
	GitRoots.Add(TEXT("Plugins/MonolithSubstance"));
	GitRoots.Add(TEXT("Plugins/MonolithClaudeDesignBridge"));

	const int32 MaxWindow = Settings->MaxCoChangeWindowCommits > 0
		? Settings->MaxCoChangeWindowCommits : 200;
	const int32 MaxFiles = Settings->MaxCommitFileCount > 0
		? Settings->MaxCommitFileCount : 20;

	// Conditional gates — scan project Source/ + Plugins/<plugin>/Source/.
	// We do NOT scan the Plugins/Monolith folder root — only its Source/ —
	// because `.uplugin` / `.uproject` files are not C++.
	TArray<FString> GateRoots;
	GateRoots.Add(TEXT("Source"));
	GateRoots.Add(TEXT("Plugins"));

	// Serialise all three sub-indexers' writes against concurrent borrowed reads
	// and the subsystem's own locked methods by holding the shared DB lock for the
	// whole suite. FGitCoChangeIndexer spawns `git log` (slow) under the lock —
	// acceptable because RI reads are game-thread and the subsystem only touches
	// its handle on the game thread, so nothing else contends during the borrow.
	FString GitStatus, HotspotStatus, GateStatus;
	bool bGitOk = false, bHotspotOk = false, bGateOk = false;
	{
		FScopeLock Lock(&SharedDb->GetLock());
		FSQLiteDatabase* RawDb = SharedDb->GetRawHandle();
		if (!RawDb)
		{
			OutStatus = TEXT("RunRiskIndexersOnce: source DB closed mid-bootstrap — will retry");
			if (Self)
			{
				Self->bRiskBootstrapAttempted = false;
				Self->RiskLastFailureTime = FPlatformTime::Seconds();
			}
			return false;
		}
		FGitCoChangeIndexer GitIndexer;
		bGitOk = GitIndexer.Run(*RawDb, GitRoots, MaxWindow,
			Settings->GitMiningNoiseFilter, MaxFiles, GitStatus);

		FHotspotScorer HotspotScorer;
		bHotspotOk = HotspotScorer.Run(*RawDb, HotspotStatus);

		FConditionalGateIndexer GateIndexer;
		bGateOk = GateIndexer.Run(*RawDb, GateRoots, GateStatus);
	}

	OutStatus = FString::Printf(
		TEXT("RunRiskIndexersOnce: git=%s | hotspot=%s | gates=%s"),
		*GitStatus, *HotspotStatus, *GateStatus);
	UE_LOG(LogMonolithReflectionIntel, Log, TEXT("%s"), *OutStatus);

	const bool bAllOk = bGitOk && bHotspotOk && bGateOk;
	if (Self)
	{
		if (bAllOk)
		{
			Self->bRiskBootstrapAttempted = true;
			Self->RiskLastFailureTime = 0.0;
		}
		else
		{
			UE_LOG(LogMonolithReflectionIntel, Warning,
				TEXT("RunRiskIndexersOnce: at least one sub-indexer failed; will retry after cooldown"));
			Self->bRiskBootstrapAttempted = false;
			Self->RiskLastFailureTime = FPlatformTime::Seconds();
		}
	}
	return bAllOk;
}

bool FMonolithReflectionIntelModule::RunCppReflectIndexersOnce(FString& OutStatus)
{
	// No settings gate in Phase 3a — the UHT-artefact reader is cheap when no
	// artefacts exist on disk (graceful "0 rows" return). Phase 3b will wrap
	// the source-driven tree-sitter pass behind bIndexEnginePluginReflection.

	// See RunDecisionIndexerOnce for latch-policy rationale.
	FMonolithReflectionIntelModule* Self = GetReflectionIntelModulePtr();

	if (Self && Self->CppReflectLastFailureTime > 0.0)
	{
		const double Now = FPlatformTime::Seconds();
		const double Elapsed = Now - Self->CppReflectLastFailureTime;
		if (Elapsed < RetryCooldownSeconds)
		{
			OutStatus = FString::Printf(
				TEXT("RunCppReflectIndexersOnce: throttled (last failure %.1fs ago, cooldown %.1fs)"),
				Elapsed, RetryCooldownSeconds);
			// Clear latch during throttle — see decision-runner comment for why.
			Self->bCppReflectBootstrapAttempted = false;
			return false;
		}
		Self->CppReflectLastFailureTime = 0.0;
	}

	// Borrow the subsystem's open ReadWrite handle (single-open VFS rationale in
	// RunDecisionIndexerOnce). No second open, no journal re-flip.
	FMonolithSourceDatabase* SharedDb = GetSharedSourceDatabase();
	if (!SharedDb || !SharedDb->GetRawHandle())
	{
		OutStatus = TEXT("RunCppReflectIndexersOnce: source DB not open (editor down, never indexed, or reindex in progress) — bootstrap with source.trigger_reindex");
		UE_LOG(LogMonolithReflectionIntel, Verbose, TEXT("%s"), *OutStatus);
		if (Self)
		{
			Self->bCppReflectBootstrapAttempted = false;
			Self->CppReflectLastFailureTime = FPlatformTime::Seconds();
		}
		return false;
	}

	// Resolve UHT artefact roots from settings. Empty = auto-resolve to
	// ProjectIntermediateDir / Build inside FUHTArtefactReader::Run.
	const UMonolithReflectionIntelSettings* Settings = UMonolithReflectionIntelSettings::Get();
	TArray<FString> ArtefactRoots;
	bool bIncludeEnginePlugins = false;
	if (Settings)
	{
		bIncludeEnginePlugins = Settings->bIndexEnginePluginReflection;
		if (!Settings->UHTArtefactRoot.IsEmpty())
		{
			ArtefactRoots.Add(Settings->UHTArtefactRoot);
		}
	}

	// Run both indexers sequentially under the shared DB lock. Each opens its own
	// internal transaction so we do not need to wrap them in an outer BEGIN/COMMIT
	// — but ordering matters: FUHTArtefactReader populates reflect_uclasses before
	// FAssetGraphJoiner reads it.
	FString UhtStatus, JoinerStatus;
	bool bUhtOk = false, bJoinerOk = false;
	{
		FScopeLock Lock(&SharedDb->GetLock());
		FSQLiteDatabase* RawDb = SharedDb->GetRawHandle();
		if (!RawDb)
		{
			OutStatus = TEXT("RunCppReflectIndexersOnce: source DB closed mid-bootstrap — will retry");
			if (Self)
			{
				Self->bCppReflectBootstrapAttempted = false;
				Self->CppReflectLastFailureTime = FPlatformTime::Seconds();
			}
			return false;
		}
		FUHTArtefactReader UhtReader;
		bUhtOk = UhtReader.Run(*RawDb, ArtefactRoots, bIncludeEnginePlugins, UhtStatus);

		FAssetGraphJoiner Joiner;
		bJoinerOk = Joiner.Run(*RawDb, JoinerStatus);
	}

	OutStatus = FString::Printf(
		TEXT("RunCppReflectIndexersOnce: uht=%s | asset_graph=%s"),
		*UhtStatus, *JoinerStatus);
	UE_LOG(LogMonolithReflectionIntel, Log, TEXT("%s"), *OutStatus);

	const bool bAllOk = bUhtOk && bJoinerOk;
	if (Self)
	{
		if (bAllOk)
		{
			Self->bCppReflectBootstrapAttempted = true;
			Self->CppReflectLastFailureTime = 0.0;
		}
		else
		{
			UE_LOG(LogMonolithReflectionIntel, Warning,
				TEXT("RunCppReflectIndexersOnce: at least one sub-indexer failed; will retry after cooldown"));
			Self->bCppReflectBootstrapAttempted = false;
			Self->CppReflectLastFailureTime = FPlatformTime::Seconds();
		}
	}
	return bAllOk;
}

bool FMonolithReflectionIntelModule::RunNetworkIndexerOnce(FString& OutStatus)
{
	// No settings gate in Phase 4a — the network indexer's UHT-artefact reader
	// is cheap when no artefacts exist on disk (graceful "0 rows" return). A
	// settings toggle (bEnableNetworkReplicationAudit) controls behaviour at
	// the registration layer in a future ergonomics pass; Phase 4a unconditionally
	// runs the indexer here so the action surface stays consistent across
	// build configurations.

	// See RunDecisionIndexerOnce for latch-policy rationale.
	FMonolithReflectionIntelModule* Self = GetReflectionIntelModulePtr();

	if (Self && Self->NetworkLastFailureTime > 0.0)
	{
		const double Now = FPlatformTime::Seconds();
		const double Elapsed = Now - Self->NetworkLastFailureTime;
		if (Elapsed < RetryCooldownSeconds)
		{
			OutStatus = FString::Printf(
				TEXT("RunNetworkIndexerOnce: throttled (last failure %.1fs ago, cooldown %.1fs)"),
				Elapsed, RetryCooldownSeconds);
			// Clear latch during throttle — see decision-runner comment for why.
			Self->bNetworkBootstrapAttempted = false;
			return false;
		}
		Self->NetworkLastFailureTime = 0.0;
	}

	// Borrow the subsystem's open ReadWrite handle (single-open VFS rationale in
	// RunDecisionIndexerOnce). No second open, no journal re-flip.
	FMonolithSourceDatabase* SharedDb = GetSharedSourceDatabase();
	if (!SharedDb || !SharedDb->GetRawHandle())
	{
		OutStatus = TEXT("RunNetworkIndexerOnce: source DB not open (editor down, never indexed, or reindex in progress) — bootstrap with source.trigger_reindex");
		UE_LOG(LogMonolithReflectionIntel, Verbose, TEXT("%s"), *OutStatus);
		if (Self)
		{
			Self->bNetworkBootstrapAttempted = false;
			Self->NetworkLastFailureTime = FPlatformTime::Seconds();
		}
		return false;
	}

	// Resolve UHT artefact roots from settings — same shape as the Phase 3a
	// cppreflect runner. Empty = auto-resolve to ProjectIntermediateDir/Build
	// inside FNetworkRepIndexer::Run.
	const UMonolithReflectionIntelSettings* Settings = UMonolithReflectionIntelSettings::Get();
	TArray<FString> ArtefactRoots;
	bool bIncludeEnginePlugins = false;
	if (Settings)
	{
		bIncludeEnginePlugins = Settings->bIndexEnginePluginReflection;
		if (!Settings->UHTArtefactRoot.IsEmpty())
		{
			ArtefactRoots.Add(Settings->UHTArtefactRoot);
		}
	}

	bool bOk = false;
	{
		FScopeLock Lock(&SharedDb->GetLock());
		FSQLiteDatabase* RawDb = SharedDb->GetRawHandle();
		if (!RawDb)
		{
			OutStatus = TEXT("RunNetworkIndexerOnce: source DB closed mid-bootstrap — will retry");
			if (Self)
			{
				Self->bNetworkBootstrapAttempted = false;
				Self->NetworkLastFailureTime = FPlatformTime::Seconds();
			}
			return false;
		}
		FNetworkRepIndexer Indexer;
		bOk = Indexer.Run(*RawDb, ArtefactRoots, bIncludeEnginePlugins, OutStatus);
	}

	if (Self)
	{
		if (bOk)
		{
			Self->bNetworkBootstrapAttempted = true;
			Self->NetworkLastFailureTime = 0.0;
		}
		else
		{
			UE_LOG(LogMonolithReflectionIntel, Warning,
				TEXT("RunNetworkIndexerOnce: indexer reported failure; will retry after cooldown — %s"),
				*OutStatus);
			Self->bNetworkBootstrapAttempted = false;
			Self->NetworkLastFailureTime = FPlatformTime::Seconds();
		}
	}
	return bOk;
}

bool FMonolithReflectionIntelModule::ForceRebuildReflectionTables(
	FString& OutCppReflectStatus, FString& OutNetworkStatus)
{
	// Game-thread contract — the runners (and the indexers' Run() they invoke)
	// touch GEditor / the shared SQLite handle, which are game-thread-only. The
	// indexers assert ensure(IsInGameThread()) themselves; mirror it here so a
	// mis-dispatch is caught at the entry point rather than deep in the sweep.
	ensure(IsInGameThread());

	// FORCE the cppreflect set. The runner itself never checks the bootstrap
	// latch — it always calls FUHTArtefactReader::Run + FAssetGraphJoiner::Run,
	// and FUHTArtefactReader::Run WIPEs (DELETE FROM) reflect_uclasses /
	// reflect_uproperties / reflect_ufunctions / reflect_uinterfaces /
	// reflect_uinterface_impls before repopulating (FAssetGraphJoiner wipes +
	// rewrites cpp_asset_edges in the same lock window). So a single runner call
	// is already a genuine clear+rewrite. The ONLY thing that could skip it is
	// the failure-throttle cooldown — clear it (and the lazy latch) so a recent
	// transient failure cannot turn this explicit force into a throttled no-op.
	CppReflectLastFailureTime    = 0.0;
	bCppReflectBootstrapAttempted = false;
	const bool bCppOk = RunCppReflectIndexersOnce(OutCppReflectStatus);

	// FORCE the network set. Same reasoning: RunNetworkIndexerOnce always calls
	// FNetworkRepIndexer::Run, which WIPEs reflect_replicated_properties before
	// repopulating. Clear the cooldown + latch first so neither path skips.
	NetworkLastFailureTime    = 0.0;
	bNetworkBootstrapAttempted = false;
	const bool bNetOk = RunNetworkIndexerOnce(OutNetworkStatus);

	const bool bAllOk = bCppOk && bNetOk;
	UE_LOG(LogMonolithReflectionIntel, Log,
		TEXT("ForceRebuildReflectionTables: %s | cppreflect=%s | network=%s"),
		bAllOk ? TEXT("OK") : TEXT("PARTIAL/FAILED"),
		*OutCppReflectStatus, *OutNetworkStatus);
	return bAllOk;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithReflectionIntelModule, MonolithReflectionIntel)
