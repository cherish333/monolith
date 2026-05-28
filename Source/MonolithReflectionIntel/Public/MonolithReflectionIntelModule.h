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
 * DEVIATION (vs plan §0 "shared DB handle"): UMonolithSourceSubsystem does not
 * expose an accessor for its raw FSQLiteDatabase pointer, so this module owns
 * its own ReadOnly handle for queries (cached as a TUniquePtr member, torn
 * down explicitly in ShutdownModule) and opens a transient ReadWrite handle
 * for the indexer pass. See MonolithReflectionIntelModule.cpp for the policy.
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
	 * Lazily open (or return) the cached ReadOnly handle on EngineSource.db.
	 * Owned by this module instance (TUniquePtr); torn down in ShutdownModule
	 * so the SQLite handle + file lock release cleanly on editor exit / Live
	 * Coding module reload.
	 *
	 * Returns nullptr if EngineSource.db does not exist (caller should surface
	 * "run source.trigger_reindex"). Path-changes between calls (e.g. plugin
	 * re-mount) close the prior handle and open a fresh one.
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

	/** Close + drop the cached query DB handle. Called from the lazy-bootstrap
	 *  path in FDecisionQueryAdapter::GetRawDB before invoking the indexer
	 *  (which opens its own RW handle on the same file). */
	void ResetCachedQueryDb();

private:
	void RegisterDecisionActions();
	void RegisterRiskActions();
	void RegisterSourceAuditActions();
	void RegisterCppReflectActions();
	void OnReloadComplete(EReloadCompleteReason Reason);

	FDelegateHandle ReloadCompleteHandle;

	/** Cached ReadOnly handle on EngineSource.db. Owned by this module instance;
	 *  Reset() explicitly in ShutdownModule before module unload to release the
	 *  SQLite handle + file lock (TUniquePtr destruction order is not guaranteed
	 *  during module teardown). */
	TUniquePtr<FSQLiteDatabase> CachedQueryDb;
	FString CachedQueryDbPath;

	/** Replaces the prior function-static `bAttemptedBootstrap` in the adapter.
	 *  Re-arms automatically on module reload because the module instance is
	 *  reconstructed on hot-reload. */
	bool bDecisionBootstrapAttempted = false;

	/** Phase 2 risk-bootstrap latch. Re-armed on module reload like the
	 *  decision latch. The risk indexer is more expensive (spawns `git log`)
	 *  so we ALSO guard against second-call mid-session via this flag. */
	bool bRiskBootstrapAttempted = false;

	/** Phase 3a cppreflect-bootstrap latch. Re-armed on module reload. The
	 *  UHT-artefact sweep can scan thousands of files; this guard prevents
	 *  duplicate work when multiple cppreflect_query actions race the lazy
	 *  bootstrap path in the same session. */
	bool bCppReflectBootstrapAttempted = false;
};
