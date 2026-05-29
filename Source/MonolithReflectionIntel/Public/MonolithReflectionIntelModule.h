// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 1).

#pragma once

#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"
#include "Modules/ModuleManager.h"
#include "Templates/UniquePtr.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithReflectionIntel, Log, All);

// UE 5.7: enum class declared without underlying type at UObject/UObjectGlobals.h:3216;
// forward-declare without an underlying type to match (see MonolithSourceSubsystem.h for precedent).
enum class EReloadCompleteReason;

// Forward-declare to keep SQLiteCore out of this header (the TUniquePtr member
// below only requires a forward decl; full definition is needed in the .cpp).
class FSQLiteDatabase;

/**
 * MonolithReflectionIntel — Phase 1 of Reflection Intelligence (v0.17.0).
 *
 * Hosts the deterministic markdown decision-record indexer (FDecisionRecordIndexer)
 * and registers the `decision_query` namespace (5 actions) into FMonolithToolRegistry.
 *
 * The indexer writes to the EngineSource.db SQLite file (owned by
 * UMonolithSourceSubsystem). New tables `decision_records` and
 * `decision_supersedes` use the `decision_` prefix.
 *
 * Loading phase: Default. Module type: Editor.
 *
 * SHARED-HANDLE POLICY (corrected 2026-05-29, plan §0): the read-only query side
 * BORROWS UMonolithSourceSubsystem's already-open FSQLiteDatabase handle (via
 * FMonolithSourceDatabase::GetRawHandle) instead of opening a second handle on
 * EngineSource.db. UE 5.7's custom `unreal-fs` SQLite VFS permits only ONE open
 * of a file per process, so the previous "own ReadOnly handle" approach was
 * rejected with SQLITE_IOERR ("disk I/O error") and broke all ~25 RI query
 * actions. The transient ReadWrite handle opened during the indexer pass
 * (Run*IndexerOnce) is unchanged — it is only ever opened while the subsystem's
 * handle is NOT (the subsystem closes its handle for the duration of a reindex,
 * and the indexer pass is brief), so it does not collide with the single-open VFS.
 */
class FMonolithReflectionIntelModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/**
	 * Run the decision-record indexer once on demand. Invoked lazily by the
	 * `decision_query` action handlers when the table is empty, and bound to
	 * FCoreUObjectDelegates::ReloadCompleteDelegate for hot-reload refresh.
	 * Cheap to call repeatedly — the indexer wipes and rewrites in one pass.
	 */
	static bool RunDecisionIndexerOnce(FString& OutStatus);

	/**
	 * Phase 2 (v0.17.0) — run the risk indexer suite (FGitCoChangeIndexer +
	 * FHotspotScorer + FConditionalGateIndexer) once on demand. Invoked
	 * lazily by `risk_query` action handlers when `risk_hotspot_scores` is
	 * absent, and bound to FCoreUObjectDelegates::ReloadCompleteDelegate for
	 * hot-reload refresh. Wipes + rewrites each Phase 2 table in one pass.
	 */
	static bool RunRiskIndexersOnce(FString& OutStatus);

	/**
	 * Phase 3a (v0.17.0) — run the cppreflect indexer pair on demand.
	 *   1. FUHTArtefactReader — sweeps Intermediate/Build/<...>/UHT/*.gen.cpp
	 *      (richer than .generated.h) and populates reflect_uclasses /
	 *      reflect_uproperties / reflect_ufunctions / reflect_uinterfaces /
	 *      reflect_uinterface_impls.
	 *   2. FAssetGraphJoiner — cross-joins those rows against IAssetRegistry
	 *      to produce cpp_asset_edges.
	 * Invoked lazily by `cppreflect_query` action handlers when
	 * `reflect_uclasses` is absent, and re-fired on hot-reload. Wipe-and-
	 * rewrite semantics in one transaction; tolerates missing UHT root.
	 */
	static bool RunCppReflectIndexersOnce(FString& OutStatus);

	/**
	 * Phase 4a (v0.17.0) — run the network-replication indexer on demand.
	 * FNetworkRepIndexer sweeps the same UHT artefact corpus Phase 3a touches
	 * but focuses on per-property `NewProp_<X>_MetaData[]` `ReplicatedUsing`
	 * pairs, producing reflect_replicated_properties.
	 *
	 * Invoked lazily by `network_query` action handlers when
	 * `reflect_replicated_properties` is absent; also re-fired on hot-reload.
	 * Wipe-and-rewrite semantics; tolerates missing UHT root.
	 */
	static bool RunNetworkIndexerOnce(FString& OutStatus);

	/**
	 * Return the SHARED EngineSource.db handle owned by UMonolithSourceSubsystem,
	 * borrowed for read-only queries. This module no longer opens its own handle
	 * (the UE 5.7 single-open `unreal-fs` VFS rejects a second open of the same
	 * file with SQLITE_IOERR — that was the "disk I/O error" bug).
	 *
	 * Returns nullptr when the subsystem's handle is NOT open: editor not yet up,
	 * EngineSource.db never indexed, or a reindex currently has it closed. Callers
	 * MUST null-check and surface "run source.trigger_reindex", never crash.
	 *
	 * THREAD SAFETY (game-thread-only contract): the returned pointer is NOT
	 * lock-protected. RI borrows the subsystem's open handle; ALL access must be on
	 * the GAME THREAD. The subsystem's handle CLOSE runs on the game thread (its sole
	 * caller is the source.trigger_reindex MCP handler, which is game-thread-dispatched;
	 * only the REOPEN is marshaled via AsyncTask), and its async indexer uses a SEPARATE
	 * handle on a worker — so game-thread-only RI reads are fully serialised against the
	 * subsystem's close without any per-read lock. Every borrowing adapter's GetRawDB()
	 * asserts ensure(IsInGameThread()) to enforce this; do NOT call read handlers off the
	 * game thread.
	 *
	 * NAME: kept as GetOrOpenCachedQueryDb for call-site compatibility with the
	 * four query adapters, even though it no longer "opens" anything itself.
	 */
	FSQLiteDatabase* GetOrOpenCachedQueryDb();

	/** Bootstrap-latch accessors — replaces the prior function-static `bAttemptedBootstrap`
	 *  so that a module reload re-arms the lazy bootstrap path. */
	bool HasAttemptedBootstrap() const { return bDecisionBootstrapAttempted; }
	void MarkBootstrapAttempted()       { bDecisionBootstrapAttempted = true; }

	/** Phase 2 risk-bootstrap-latch accessors. Mirror the Phase 1 decision
	 *  pattern so module reload re-arms the risk indexer too. */
	bool HasAttemptedRiskBootstrap() const { return bRiskBootstrapAttempted; }
	void MarkRiskBootstrapAttempted()       { bRiskBootstrapAttempted = true; }

	/** Phase 3a cppreflect-bootstrap-latch accessors. Same shape as decision
	 *  and risk latches — re-armed on module reload via StartupModule(). */
	bool HasAttemptedCppReflectBootstrap() const { return bCppReflectBootstrapAttempted; }
	void MarkCppReflectBootstrapAttempted()       { bCppReflectBootstrapAttempted = true; }

	/** Phase 4a network-bootstrap-latch accessors. Same shape as the prior
	 *  three latches — re-armed on module reload via StartupModule(). */
	bool HasAttemptedNetworkBootstrap() const { return bNetworkBootstrapAttempted; }
	void MarkNetworkBootstrapAttempted()       { bNetworkBootstrapAttempted = true; }

	/** No-op under the shared-handle policy (retained for call-site compatibility).
	 *  This module no longer owns a query handle, so there is nothing to close.
	 *  The legacy lazy-bootstrap path in FDecisionQueryAdapter::GetRawDB still
	 *  calls it; it must NOT touch the subsystem's handle (subsystem owns it). */
	void ResetCachedQueryDb();

private:
	void RegisterDecisionActions();
	void RegisterRiskActions();
	void RegisterSourceAuditActions();
	void RegisterCppReflectActions();
	// Phase 4a — three new register hooks. Network owns its own namespace
	// (`network`); audit and pipeline both register against existing namespaces
	// (material/niagara/blueprint/project for audits, `pipeline` is brand-new
	// for the composers).
	void RegisterNetworkActions();
	void RegisterAuditActions();
	void RegisterPipelineActions();
	void OnReloadComplete(EReloadCompleteReason Reason);

	FDelegateHandle ReloadCompleteHandle;

	// NOTE (2026-05-29): the former `TUniquePtr<FSQLiteDatabase> CachedQueryDb` +
	// `CachedQueryDbPath` members were removed. This module no longer opens its
	// own handle on EngineSource.db — it borrows UMonolithSourceSubsystem's
	// already-open handle (the UE 5.7 single-open `unreal-fs` VFS rejected the
	// second open with SQLITE_IOERR). See GetOrOpenCachedQueryDb().

	/** Replaces the prior function-static `bAttemptedBootstrap` in the adapter.
	 *  Re-arms automatically on module reload because the module instance is
	 *  reconstructed on hot-reload.
	 *
	 *  IMPORTANT: the adapter sets this to TRUE before calling the runner. The
	 *  runner CLEARS it on transient failure (file-missing, RW-open-failed,
	 *  indexer-failed) so subsequent adapter calls retry. The runner re-sets
	 *  it on success (no-op since the adapter already set it). Net effect:
	 *  the latch is only canonically true after at least one successful
	 *  bootstrap. */
	bool bDecisionBootstrapAttempted = false;

	/** Phase 2 risk-bootstrap latch. Re-armed on module reload like the
	 *  decision latch. The risk indexer is more expensive (spawns `git log`)
	 *  so we ALSO guard against second-call mid-session via this flag.
	 *  Cleared on failure inside RunRiskIndexersOnce — see decision-latch
	 *  comment above. */
	bool bRiskBootstrapAttempted = false;

	/** Phase 3a cppreflect-bootstrap latch. Re-armed on module reload. The
	 *  UHT-artefact sweep can scan thousands of files; this guard prevents
	 *  duplicate work when multiple cppreflect_query actions race the lazy
	 *  bootstrap path in the same session. Cleared on failure inside
	 *  RunCppReflectIndexersOnce. */
	bool bCppReflectBootstrapAttempted = false;

	/** Phase 4a network-bootstrap latch. Re-armed on module reload. The
	 *  network indexer sweeps the same UHT corpus Phase 3a does; the per-
	 *  action lazy bootstrap path in FNetworkQueryAdapter is fronted by this
	 *  latch to avoid duplicate work when multiple network_query actions
	 *  race the bootstrap. Cleared on failure inside RunNetworkIndexerOnce. */
	bool bNetworkBootstrapAttempted = false;

	/** Per-phase retry-throttle timestamps (FPlatformTime::Seconds() basis).
	 *  When a runner fails it records the failure time; subsequent calls
	 *  within RetryCooldownSeconds fast-return without re-opening SQLite
	 *  (prevents `disk I/O error` log spam when the DB is wedged). Reset to
	 *  0.0 on first success so the next failure-cycle starts a fresh cooldown. */
	double DecisionLastFailureTime    = 0.0;
	double RiskLastFailureTime        = 0.0;
	double CppReflectLastFailureTime  = 0.0;
	double NetworkLastFailureTime     = 0.0;

	/** Cooldown window for the retry-throttle. 5 seconds chosen so a burst of
	 *  back-to-back adapter calls (one per query action in a session ramp-up)
	 *  doesn't spam 28+ failed SQLite opens — but a follow-up call a few
	 *  seconds later (e.g., after source.trigger_reindex completes and the
	 *  user retries) is still served promptly. */
	static constexpr double RetryCooldownSeconds = 5.0;
};
