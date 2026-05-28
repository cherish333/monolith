# Monolith — MonolithReflectionIntel Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.17.0 — 11 actions shipped across Phases 1–2 (5 decision + 5 risk + 1 source-namespace module-dep audit)

---

## 1. Purpose

`MonolithReflectionIntel` is a deterministic, $0-LLM intelligence layer that mines high-signal facts out of the project's own artefacts (markdown, git history, C++, AssetRegistry) and exposes them as MCP query actions. It exists to give AI agents structured answers to questions the project itself already knows the answer to — without spending tokens re-deriving them from raw source.

Phases 1 and 2 fold into the same v0.17.0 release. Phase 1 ships the **Decision Intelligence** slice — architectural decision records mined from the project's markdown corpora (specs, plans, CHANGELOG, `.claude/rules/`) and served through the `decision_query` namespace (5 actions). Phase 2 ships the **Risk Intelligence** slice — git-log mining + conditional-gate inventory served through a new `risk_query` namespace (5 actions), plus a **Module-Dep Reality Audit** that registers a single audit action onto the existing `source_query` namespace. The remaining two phases are planned but not yet implemented.

### Roadmap

| Phase | Status | Surface | Substrate |
|-------|--------|---------|-----------|
| 1 — Decision Intelligence | **shipped v0.17.0** | `decision_query` (5 actions) | Markdown heuristic harvest |
| 2 — Risk Intelligence | **shipped v0.17.0** | `risk_query` (5 actions) + `source_query("audit_module_dep_reality")` (1 audit action) | Git log subprocess + LOC sweep + regex over `#if WITH_*` / `bHas*` + Build.cs parsing against `EngineSource.db` symbol resolution |
| 3 — CppReflect Intelligence | `(WISHLIST)` | `cppreflect_query` + cpp↔asset edges | tree-sitter-unreal-cpp + UHT artefacts + `IAssetRegistry` |
| 4 — Network Intelligence | `(WISHLIST)` | `network_query` + audit actions + `pipeline_query("pr_review")` composer | Composes Phases 1+2+3 |

The phases are independent (Phase 2 does not depend on Phase 1; Phase 4 depends on Phase 3 reflection-edge tables for `network_query` and the audit actions). Each phase ships as its own point release, except Phases 1 + 2 which co-shipped in v0.17.0.

---

## 2. Module Architecture

**Type:** `Editor`
**Loading phase:** `Default`
**Public namespaces owned by this module:** `decision` (5 actions, Phase 1) + `risk` (5 actions, Phase 2). Phase 2 additionally registers one audit action onto the **existing** `source` namespace owned by `MonolithSource` (`source_query("audit_module_dep_reality")`); the audit handler lives in `MonolithReflectionIntel` but is registered against the source dispatcher for caller ergonomics — agents already discover `source_query` first.

`MonolithReflectionIntel` is a self-contained editor module. Phase 1 owns one indexer worker (`FDecisionRecordIndexer`), one query adapter (`FDecisionQueryAdapter`), one settings UCLASS (`UMonolithReflectionIntelSettings`), and a SQLite schema fragment (`MonolithDecisionSchema` namespace). Phase 2 adds three indexer workers (`FGitChurnIndexer`, `FGitCoChangeIndexer`, `FConditionalGateIndexer`), two query adapters (`FRiskQueryAdapter`, `FModuleDepRealityAdapter`), and a second SQLite schema fragment (`MonolithRiskSchema` namespace) sharing `EngineSource.db`.

### Lazy bootstrap

The module does NOT eagerly run the indexer on `StartupModule`. Two reasons:

1. The `UMonolithSourceSubsystem` may hold a ReadWrite handle on `EngineSource.db` at startup time; opening a second writer would race.
2. The decision corpus is small (~50–500 records at Leviathan scale) and tolerates lazy first-call cost.

Bootstrap fires on two events:

- **First `decision_query` call** — `FDecisionQueryAdapter::GetRawDB` checks for the `decision_records` table; if missing, it closes its ReadOnly handle, calls `FMonolithReflectionIntelModule::RunDecisionIndexerOnce` (which opens a brief ReadWrite handle, ensures schema, writes rows, closes), and reopens ReadOnly. Subsequent calls hit the cached ReadOnly handle.
- **`FCoreUObjectDelegates::ReloadCompleteDelegate`** — bound at `StartupModule`. After Live Coding or UBT-driven hot-reload, the corpus refreshes automatically so agents see decisions added to spec files in the current session without manually triggering a re-index.

The `RunDecisionIndexerOnce` entry point is idempotent — calling it repeatedly is cheap (one wipe-and-rewrite per call) and safe.

### Shutdown

`ShutdownModule` unbinds the reload delegate and calls `FMonolithToolRegistry::UnregisterNamespace("decision")` so dispatcher state stays clean on editor exit.

---

## 3. Decision Intelligence (Phase 1 — SHIPPED v0.17.0)

### 3.1 Markdown corpus harvest scope

The indexer walks `*.md` files recursively under each configured markdown root via `IFileManager::IterateDirectoryRecursively` (visitor pattern — sidesteps the `FindFilesRecursive` 6th-param `bClearFileNames=true` trap documented in `.claude/rules/scoped/cpp-code.md`).

Default roots (used when `UMonolithReflectionIntelSettings::DecisionMarkdownRoots` is empty):

- `Docs/` — project-level specs and plans
- `Plugins/Monolith/Docs/` — Monolith specs, plans, CHANGELOG, guides
- `.claude/rules/` — agent rules

Each root is resolved relative to `FPaths::ProjectDir()` unless absolute. Non-existent roots are skipped with a `Verbose` log line — never an error.

Files are read in full via `FFileHelper::LoadFileToString` and tokenised into lines via `FString::ParseIntoArrayLines` for header walk.

### 3.2 Heuristic detection

The indexer emits at most one row per markdown header (or one per file in the frontmatter-decision path). Three detection tiers with distinct confidence floors:

| Tier | Trigger | Confidence | Status default |
|------|---------|------------|----------------|
| **YAML frontmatter** | Leading `---` block with `decision: true` OR any `status:` key | `0.90` | from `status:` value (lowercased), else `accepted` |
| **ADR-style header** | Line matches `(?i)^#+\s*(?:ADR[-\s]?\d+|Architectural\s+Decision)\b` | `0.85` | `open` |
| **Header + rationale marker** | Markdown header (H2–H6 only — H1 skipped unless ADR-style) followed within 8 lines by a paragraph containing `because` / `rationale` / `evidence` / `decision:` | `0.65` | `open` |

Files matching neither tier contribute zero rows. Headers without rationale markers and without ADR shape are skipped — the indexer is conservative by design.

The `UMonolithReflectionIntelSettings::DecisionMinConfidence` floor (default `0.6`) is applied at **query time** by `decision_query("list_decisions")`, not at extraction time — every detected record is stored, then filtered on read so callers can override the floor per call.

### 3.3 SQLite schema

Tables live inside the shared `EngineSource.db` file under the `decision_` prefix so they coexist with the source-indexer's tables without name collision.

```sql
CREATE TABLE IF NOT EXISTS decision_records (
    decision_id     TEXT PRIMARY KEY,
    title           TEXT NOT NULL,
    status          TEXT NOT NULL DEFAULT 'open',
    source_path     TEXT NOT NULL,
    source_line     INTEGER NOT NULL DEFAULT 0,
    confidence      REAL NOT NULL DEFAULT 0.0,
    rationale       TEXT,
    source_mtime    INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS decision_supersedes (
    from_decision_id TEXT NOT NULL,
    to_decision_id   TEXT NOT NULL,
    PRIMARY KEY (from_decision_id, to_decision_id)
);

CREATE INDEX IF NOT EXISTS idx_decision_records_status
    ON decision_records(status);
CREATE INDEX IF NOT EXISTS idx_decision_records_source_path
    ON decision_records(source_path);
CREATE INDEX IF NOT EXISTS idx_decision_supersedes_to
    ON decision_supersedes(to_decision_id);
```

All schema statements use `CREATE ... IF NOT EXISTS` so first-run bootstrap and subsequent re-runs are both safe. Index creation failure is non-fatal (logged at `Warning`); the base tables MUST succeed or the indexer aborts.

**`decision_id` shape:** `<forward-slashed-project-relative-path>#<header-anchor>`, where the anchor is a slug derived from header text (lowercased, alphanumeric + hyphens, trailing hyphens trimmed). Frontmatter-decision rows use `#frontmatter` as the anchor. The ID is stable across reindex runs as long as the path and header text are stable.

**Wipe-and-rewrite semantics:** every `Run()` call wipes both tables and rewrites from scratch. The corpus is small enough that incremental delta-detection isn't justified; full rewrite makes "decision removed from markdown" reflect immediately. Writes occur inside a single `BEGIN TRANSACTION ... COMMIT` block with a reused prepared statement per `MeshCatalogIndexer.cpp` pattern.

### 3.4 Action surface

5 actions register under `decision` from `FDecisionQueryAdapter::RegisterActions`. All five carry `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true` on the dispatcher annotations (v0.17.0 `tools/list` surface). All five participate in v0.17.0 universal response shaping (`_fields` / `_omit` / `_compact_json`) for free.

#### `decision_query("list_decisions", params)`

List architectural decisions filtered by source-path substring and minimum heuristic confidence. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `path_filter` | string | `DiskPath` | no | `""` | Substring match against `source_path` (project-relative). `\` → `/` rewritten by dispatcher with surfaced warning. |
| `min_confidence` | number | `Other` | no | `0.6` | Floor in `[0, 1]`. Settings default is also `0.6`; per-call override wins. |
| `status` | string | `Other` | no | `""` | Exact match — typical values: `open`, `accepted`, `superseded`, `deprecated`, `draft`. |
| `limit` | integer | `Other` | no | `50` | Page size. Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor from a prior `next_cursor`. Restart pagination by omitting. |

**Response:**

```json
{
  "decisions": [
    {
      "decision_id": "Plugins/Monolith/Docs/SPEC_CORE.md#some-anchor",
      "title": "Some Architectural Decision",
      "status": "open",
      "source_path": "Plugins/Monolith/Docs/SPEC_CORE.md",
      "source_line": 142,
      "confidence": 0.85,
      "rationale": "Rationale paragraph if one was mined.",
      "source_mtime": 1717094400
    }
  ],
  "total_estimate": 47,
  "next_cursor": "<opaque>"
}
```

`total_estimate` is emitted on page 0 only (one `COUNT(*)` per filter set). Subsequent pages carry the cached count inside the cursor. `next_cursor` is omitted on the last page.

#### `decision_query("get_decision", params)`

Fetch one record by stable id.

| Param | Type | Required |
|-------|------|----------|
| `decision_id` | string | yes |

**Response:** `{ "decision": <row-or-null> }` — `decision` is `null` when the id is unknown.

#### `decision_query("list_stale", params)`

List decisions whose source markdown has not been modified within `max_age_days` days. Useful for spec-drift detection.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `max_age_days` | integer | `Other` | yes | — | Positive only. Compared against source-file mtime in UTC. |
| `path_filter` | string | `DiskPath` | no | `""` | Substring match. |
| `limit` | integer | `Other` | no | `50` | Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Response:**

```json
{
  "stale_decisions": [ /* row objects */ ],
  "cutoff_unix": 1714502400,
  "next_cursor": "<opaque>"
}
```

Rows are ordered by `source_mtime ASC` (oldest first). Records with `source_mtime = 0` (mtime unavailable) are excluded — they cannot be honestly classified.

#### `decision_query("find_supersession_chain", params)`

Walk supersedes edges outward from a starting decision. Returns the ordered chain of decisions the start id transitively supersedes.

| Param | Type | Required | Default | Notes |
|-------|------|----------|---------|-------|
| `decision_id` | string | yes | — | Start of the walk. |
| `depth` | integer | no | `10` | Maximum traversal depth. Hard cap `50`. |

**Response:**

```json
{
  "start": "<decision_id>",
  "chain": [
    { "from": "<id>", "to": "<id>", "depth": 1 },
    { "from": "<id>", "to": "<id>", "depth": 2 }
  ],
  "truncated": false
}
```

`truncated: true` indicates the walk hit `depth` while frontier nodes remained — call again with higher `depth` if needed. Cycle protection via a visited set.

#### `decision_query("find_referent_decisions", params)`

Inverse of `find_supersession_chain` — list decisions that explicitly supersede the given id (the records that replaced it).

| Param | Type | Required |
|-------|------|----------|
| `decision_id` | string | yes |

**Response:**

```json
{
  "decision_id": "<id>",
  "referent_decisions": [ /* full row objects */ ]
}
```

Rows ordered by `source_path, source_line`.

### 3.5 ReadOnly fallback to `EngineSource.db`

`FDecisionQueryAdapter::GetRawDB` does NOT share `UMonolithSourceSubsystem`'s in-memory `FSQLiteDatabase` handle. The subsystem does not expose a `GetRawDatabase()` accessor on `FMonolithSourceDatabase` at the time of Phase 1 implementation, so the adapter opens its own ReadOnly handle on the same `EngineSource.db` file.

SQLite tolerates concurrent readers fine when the database is opened with `journal_mode=DELETE` (Monolith's default — see the WAL silent-fail trap in `Docs/references/UE57Gotchas.md`). The subsystem's ReadWrite handle and the adapter's ReadOnly handle coexist safely:

- ReadOnly takes no write lock; queries never block writes.
- The indexer (`RunDecisionIndexerOnce`) opens a brief ReadWrite handle only when the table is missing on first call. The subsystem closes its handle during full reindex, so the brief overlap is non-contentious.

**Migration note:** if a `GetRawDatabase()` accessor is added to `FMonolithSourceDatabase` in a later release, the adapter SHOULD migrate to the shared handle and `MonolithReflectionIntel.Build.cs` SHOULD re-add `MonolithSource` as a `PrivateDependencyModuleNames` entry. As of Phase 1, the build deps deliberately exclude `MonolithSource` to keep the dependency direction one-way.

The handle is cached statically in the adapter file scope. The cache rebuilds on path change or `IsValid()` failure.

### 3.6 Staleness detection

`source_mtime` is captured via `IFileManager::Get().GetTimeStamp(path)` and stored as a UTC Unix timestamp in the `decision_records` table. `FDateTime::MinValue()` returns sentinel `0` — those rows are excluded from `list_stale` so a filesystem error doesn't masquerade as fresh data.

`list_stale` computes the cutoff at query time: `cutoff = utc_now - max_age_days * 86400`. The SQL is `WHERE source_mtime > 0 AND source_mtime < ?`.

### 3.7 Test coverage

Four automation tests under `Monolith.ReflectionIntel.Decision.*` (`EditorContext | EngineFilter` flags):

| Test | Asserts |
|------|---------|
| `SchemaBootstrap` | Empty-corpus `Run()` succeeds; `decision_records` and `decision_supersedes` tables exist after the call. |
| `HeuristicAccuracy` | ≥4 rows ingested from the 5-file fixture corpus; `03_non_decision.md` contributes zero rows. |
| `SupersessionChain` | ≥1 edge in `decision_supersedes` after indexing the fixture corpus (file `02` carries two `Supersedes:` lines). |
| `StalenessFlag` | A fixture file copied to `FPaths::AutomationTransientDir()` and aged by 60 days via `IFileManager::SetTimeStamp` shows up in a 30-day cutoff query. |

Fixture corpus under `Source/MonolithReflectionIntel/Private/Tests/Fixtures/DecisionCorpus/` (5 markdown files: `01_decision_with_rationale.md`, `02_decision_with_supersedes.md`, `03_non_decision.md`, `04_yaml_frontmatter.md`, `05_adr_style.md`).

Disposable test DBs are created at `FPaths::AutomationTransientDir() / "decision-test-<guid>.db"` via `ESQLiteDatabaseOpenMode::ReadWriteCreate` and deleted at test teardown — the real `EngineSource.db` is never touched.

Run via `editor_query("run_automation_tests", "Monolith.ReflectionIntel.Decision")`.

---

## 4. Risk Intelligence (Phase 2 — SHIPPED v0.17.0)

### 4.1 Substrate scope

The risk slice is deterministic — no LLM calls, no embeddings, no scoring heuristics that aren't traceable to a single line of git log or a single LOC count. Three substrates feed four indexers and five `risk_query` actions:

| Substrate | Mining method | Output |
|-----------|---------------|--------|
| Git log (per-repo) | `FPlatformProcess::CreateProc` invoking `git log --name-only --pretty=format:%H|%at|%an` against each tracked repo's `.git/` | Per-file churn (commit count, line delta) + co-change pairs (files appearing in the same commit window) |
| Source-file LOC | `IFileManager::IterateDirectoryRecursively` walk of `.cpp` / `.h` under each repo's source root + line counting via `FFileHelper::LoadFileToStringArray` | LOC count per file as a coarse complexity proxy. **No** AST parsing, **no** McCabe-style cyclomatic measure — those land in Phase 3. |
| Build.cs + `.cpp` / `.h` conditional gates | Regex sweep for `#if WITH_*` blocks, `bHas*` 3-location probe blocks in `.Build.cs`, `MONOLITH_RELEASE_BUILD` bypasses | Conditional-gate inventory keyed by module |

The mining is read-only against the working tree and the `.git/` directory — no `git checkout`, no `git reset`, no index touches.

### 4.2 Git repo enumeration

Phase 2 ships with a hardcoded six-repo list (defined in `MonolithReflectionIntelModule.cpp:291-297`):

- `Plugins/Monolith/` (Monolith itself — primary tracked repo, dv-ignored)
- `Plugins/Resonance/` (audio plugin, separate GitHub repo)
- `Plugins/MonolithSteamBridge/` (sibling)
- `Plugins/MonolithSteamBridgeLeaderboard/` (sibling)
- `Plugins/MonolithSubstance/` (sibling)
- `Plugins/MonolithClaudeDesignBridge/` (sibling)

Each path is probed for `<path>/.git/`; missing `.git/` directories are skipped silently (`Verbose` log only — not an error). The hardcoded list is a known limitation; **Phase 3 follow-up** is to make the probe directory-walk-based so future sibling plugins are picked up without a code change.

The Leviathan project itself (`D:\Unreal Projects\Leviathan\`) uses Diversion, not git. The risk indexer correctly skips it because `.git/` is absent.

### 4.3 Co-change pair detection algorithm

A co-change pair `(A, B)` means files `A` and `B` appeared in the **same git commit** within a configurable commit window. The default window is the entire history of the repo at index time; `UMonolithReflectionIntelSettings::MaxCoChangeWindowCommits` (default `50`, range `[10, 500]`) caps the window so very long histories don't dominate.

For each commit observed:

1. Parse the commit's changed-file list from `git log --name-only` output.
2. Apply the `GitMiningNoiseFilter` blacklist (file-pattern globs — `Saved/`, `Intermediate/`, `Binaries/*.dll`, etc.).
3. Apply `MaxCommitFileCount` cap (default `50`) — commits touching more files than the cap (typical for tree-wide refactors or initial imports) contribute zero pairs; they would otherwise dominate co-change scores. The cap suppresses the "monster commit" noise floor.
4. Emit every unordered pair `(A, B)` with `A < B` lexicographically — symmetric storage avoided.
5. Aggregate `(A, B)` → commit count across the window.

The pair scoring is **count-based, not normalised**. Two files appearing together in 12 commits beat two files appearing together in 3 commits. Phase 3 may layer in tf-idf-style normalisation; v0.17.0 ships the raw count.

### 4.4 Hotspot score formula

The hotspot score for a file is a deterministic blend of normalised churn and normalised complexity proxy:

```
hotspot_score(file) = 0.6 * normalised_churn(file) + 0.4 * normalised_loc(file)

normalised_churn(file) = file.commit_count / max(repo.commit_count_across_files)
normalised_loc(file)   = file.loc / max(repo.loc_across_files)
```

Both normalisers are per-repo (a busy Monolith file isn't penalised against a quiet Resonance file). Score range `[0.0, 1.0]`. Score `> 0.7` is the documented "hotspot" threshold for `get_release_window_hotspots` filtering.

The weight split (0.6 churn / 0.4 LOC) is hardcoded in v0.17.0. Configurable weighting is a Phase 3 enhancement once the LOC proxy is replaced by a real complexity measure.

### 4.5 Conditional gate sweep

The conditional-gate inventory is built by regex sweep against three patterns:

| Pattern | Where | What it captures |
|---------|-------|------------------|
| `#if\s+WITH_(\w+)` | `.cpp` / `.h` under each module's source root | Compile-time feature gates the module honours (e.g. `#if WITH_GBA`, `#if WITH_COMBOGRAPH`) |
| `bool\s+bHas(\w+)\s*=` | `.Build.cs` | 3-location detection probe variables (e.g. `bHasGameplayAbilities`, `bHasCommonUI`) |
| `MONOLITH_RELEASE_BUILD` | `.Build.cs` | Release-build bypass branches |

For each match the indexer records the module, the gate name, the file path, the source line, and (for `bHas*` probes) the surrounding probe block's classification (3-location, 4-location, or release-bypass). The output table `reflect_conditional_gates` is the substrate for `risk_query("list_conditional_gates")` and is also intended to feed Phase 4's `network_query` audits.

Regex-based detection is intentionally cheap. Phase 3 may swap to tree-sitter for higher fidelity (catching commented-out `#if WITH_*` blocks, multi-line conditions, etc.); v0.17.0 accepts the false-positive rate for the indexer-runtime budget.

### 4.6 SQLite schema

Four new tables live in the shared `EngineSource.db` file under the `git_*`, `risk_*`, and `reflect_*` prefixes so they coexist with the source-indexer's tables and the Phase 1 `decision_*` tables.

```sql
CREATE TABLE IF NOT EXISTS git_file_churn (
    file_path       TEXT NOT NULL,
    repo_path       TEXT NOT NULL,
    commit_count    INTEGER NOT NULL DEFAULT 0,
    lines_added     INTEGER NOT NULL DEFAULT 0,
    lines_deleted   INTEGER NOT NULL DEFAULT 0,
    first_commit_ts INTEGER NOT NULL DEFAULT 0,
    last_commit_ts  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (repo_path, file_path)
);

CREATE TABLE IF NOT EXISTS git_cochange_pairs (
    file_a       TEXT NOT NULL,
    file_b       TEXT NOT NULL,
    repo_path    TEXT NOT NULL,
    commit_count INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (repo_path, file_a, file_b),
    CHECK (file_a < file_b)
);

CREATE TABLE IF NOT EXISTS risk_hotspot_scores (
    file_path       TEXT NOT NULL,
    repo_path       TEXT NOT NULL,
    score           REAL NOT NULL DEFAULT 0.0,
    normalised_churn REAL NOT NULL DEFAULT 0.0,
    normalised_loc   REAL NOT NULL DEFAULT 0.0,
    loc              INTEGER NOT NULL DEFAULT 0,
    indexed_at_ts    INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (repo_path, file_path)
);

CREATE TABLE IF NOT EXISTS reflect_conditional_gates (
    module_name   TEXT NOT NULL,
    gate_name     TEXT NOT NULL,
    gate_kind     TEXT NOT NULL,      -- 'with_macro' | 'bhas_probe' | 'release_bypass'
    source_path   TEXT NOT NULL,
    source_line   INTEGER NOT NULL DEFAULT 0,
    probe_arity   INTEGER NOT NULL DEFAULT 0,  -- 3 or 4 for bHas* probes; 0 otherwise
    PRIMARY KEY (module_name, gate_name, source_path, source_line)
);

CREATE INDEX IF NOT EXISTS idx_git_file_churn_count
    ON git_file_churn(commit_count DESC);
CREATE INDEX IF NOT EXISTS idx_git_cochange_count
    ON git_cochange_pairs(commit_count DESC);
CREATE INDEX IF NOT EXISTS idx_risk_hotspot_score
    ON risk_hotspot_scores(score DESC);
CREATE INDEX IF NOT EXISTS idx_reflect_gates_module
    ON reflect_conditional_gates(module_name);
```

All four tables follow the wipe-and-rewrite semantics from Phase 1 — `Run()` truncates and rewrites in a single `BEGIN TRANSACTION ... COMMIT` block per indexer.

### 4.7 Action surface

Five actions register under `risk` from `FRiskQueryAdapter::RegisterActions`. All five carry `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true` on the dispatcher annotations. All five participate in v0.17.0 universal response shaping (`_fields` / `_omit` / `_compact_json`) for free.

#### `risk_query("get_hotspot_score", params)`

Fetch the hotspot score for a single file path.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `file_path` | string | `DiskPath` | yes | — | Project-relative or repo-relative path. `\` → `/` rewritten by dispatcher with surfaced warning. |
| `repo_path` | string | `DiskPath` | no | `""` | When omitted, searches across all indexed repos and returns the first match. |

**Response:** `{ "score": <number-or-null>, "normalised_churn": <number>, "normalised_loc": <number>, "loc": <int>, "repo_path": <string> }` — `score` is `null` when the file is not in the index.

#### `risk_query("get_cochange_pairs", params)`

List files that frequently change in the same commits as the given file. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `file_path` | string | `DiskPath` | yes | — | Anchor file. |
| `repo_path` | string | `DiskPath` | no | `""` | Optional repo scope. |
| `min_commits` | integer | `Other` | no | `2` | Lower bound on `commit_count` per pair. Pairs with `commit_count == 1` are filtered to suppress one-off co-touches. |
| `limit` | integer | `Other` | no | `50` | Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor. |

**Response:**

```json
{
  "anchor": "path/to/file.cpp",
  "pairs": [
    { "partner": "path/to/other.cpp", "commit_count": 12 }
  ],
  "total_estimate": 47,
  "next_cursor": "<opaque>"
}
```

#### `risk_query("get_file_churn", params)`

Per-file churn record — commit count and line-delta totals.

| Param | Type | EMonolithParamKind | Required |
|-------|------|---------------------|----------|
| `file_path` | string | `DiskPath` | yes |
| `repo_path` | string | `DiskPath` | no |

**Response:** `{ "churn": <row-or-null> }` — row includes `commit_count`, `lines_added`, `lines_deleted`, `first_commit_ts`, `last_commit_ts`.

#### `risk_query("get_release_window_hotspots", params)`

List files whose hotspot score exceeds a threshold, ordered descending. Designed for release-readiness queries — "which files are most likely to bite us before tagging?" Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `threshold` | number | `Other` | no | `0.7` | Floor in `[0, 1]`. |
| `repo_path` | string | `DiskPath` | no | `""` | Optional repo scope. |
| `limit` | integer | `Other` | no | `50` | Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Response:** `{ "hotspots": [ { "file_path": ..., "score": ..., "normalised_churn": ..., "normalised_loc": ..., "loc": ..., "repo_path": ... } ], "total_estimate": 12, "next_cursor": "<opaque>" }`.

#### `risk_query("list_conditional_gates", params)`

List `#if WITH_*` macros, `bHas*` 3-location probe variables, and `MONOLITH_RELEASE_BUILD` bypass branches across the project. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `module_filter` | string | `Other` | no | `""` | Substring match against module name. |
| `gate_kind` | string | `Other` | no | `""` | Exact match — `with_macro`, `bhas_probe`, `release_bypass`. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Response:** `{ "gates": [ { "module_name": ..., "gate_name": ..., "gate_kind": ..., "source_path": ..., "source_line": ..., "probe_arity": ... } ], "total_estimate": 87, "next_cursor": "<opaque>" }`.

### 4.8 Test coverage

Automation tests under `Monolith.ReflectionIntel.Risk.*` and `Monolith.ReflectionIntel.ModuleDepReality.*` (`EditorContext | EngineFilter` flags). Disposable test DBs at `FPaths::AutomationTransientDir()`; the real `EngineSource.db` is never touched.

| Test | Asserts |
|------|---------|
| `RiskSchemaBootstrap` | Empty-corpus `Run()` succeeds; all 4 Phase 2 tables exist after the call. |
| `ChurnAggregation` | Fixture mini-repo with 5 known commits produces correct per-file `commit_count` rows. |
| `CoChangePairSymmetry` | Pair `(A, B)` is stored with `A < B`; reverse lookup returns the same row. |
| `HotspotScoreFormula` | Hand-computed expected score for a fixture file matches `0.6 * churn + 0.4 * loc` blend within `1e-6`. |
| `ConditionalGateSweep` | Fixture `.cpp` / `.Build.cs` corpus produces expected `with_macro` + `bhas_probe` + `release_bypass` rows. |
| `MonsterCommitSuppression` | A fixture commit with `MaxCommitFileCount+1` files contributes zero co-change pairs. |

Fixture corpus under `Source/MonolithReflectionIntel/Private/Tests/Fixtures/RiskCorpus/` — synthetic mini-repos and `.cpp` / `.Build.cs` snippets covering each detection path.

Run via `editor_query("run_automation_tests", "Monolith.ReflectionIntel.Risk")` and `editor_query("run_automation_tests", "Monolith.ReflectionIntel.ModuleDepReality")`.

---

## 4b. Module-Dep Reality Audit (Phase 2 — SHIPPED v0.17.0)

### 4b.1 Purpose

The audit catches a specific bug class: a UPROPERTY (or other reflection-touching declaration) references a symbol from a module that the owning module's `Build.cs` does not list in `PrivateDependencyModuleNames` (or `PublicDependencyModuleNames`). UHT generates `Z_Construct_*_NoRegister` reflection code that calls into the foreign module's API macro at link time — if the dep is missing, the failure surfaces as a downstream LNK2019 with a confusing-looking unresolved external referring to a symbol the developer didn't expect to be a link-time dep.

The bug class is documented in `.claude/agent-memory/<agent>/feedback_softptr_uproperty_needs_module_dep.md` — the canonical example is `TSoftObjectPtr<UWidgetBase>` UPROPERTY in a non-UMG module, where `UMG` is missing from `Build.cs`. Same class also covers FGameplayTag UPROPERTY without `GameplayTags`, FNiagaraSystemAsset without `Niagara`, etc.

### 4b.2 Algorithm

The audit is a four-pass scan against the project's source tree:

1. **Parse every `*.Build.cs` under `Source/`** — regex-extract `PublicDependencyModuleNames.AddRange({...})` and `PrivateDependencyModuleNames.AddRange({...})` array contents. Build a `module → declared_deps` map.
2. **Parse every `*.h` / `*.cpp` under each module's source root** — regex-extract type-bearing reflection declarations (`UPROPERTY(...)\s+\w+`, `UFUNCTION(...)\s+\w+`, function signatures). Extract the type names used (including template arguments — `TSoftObjectPtr<UMyClass>` extracts `UMyClass`).
3. **Resolve each extracted type name against `EngineSource.db`** — locate the symbol's owning module via the existing source-indexer tables. Unknown / external-to-Unreal types are skipped silently.
4. **Emit a violation** for each `(declaring_module, used_type, used_type_owning_module)` triple where `used_type_owning_module` is NOT in `declaring_module`'s declared deps AND NOT in the implicit-deps whitelist (see §4b.3).

The audit is heuristic — it catches the common case (direct UCLASS / USTRUCT references in UPROPERTY) but does not currently chase typedef aliases or template-argument metaclasses with full UHT fidelity. That fidelity lands in Phase 3 when the UHT-artefact parser ships.

### 4b.3 Implicit-deps whitelist

Six modules are treated as transitively-available and never reported as missing:

- `Core`, `CoreUObject`, `Engine`, `Projects`, `RHI`, `RenderCore`

These are near-universal — virtually every UE module already lists them or inherits them transitively. Flagging them as "missing deps" would drown real violations in noise. The list is hardcoded in Phase 2; making it configurable per-project is a deferred enhancement.

### 4b.4 Action surface

One action registers onto the existing `source` namespace from `FModuleDepRealityAdapter::RegisterActions`. The audit handler lives in `MonolithReflectionIntel` but is registered against `source_query` for caller ergonomics — agents searching for source-related tooling already find `source_query` first.

#### `source_query("audit_module_dep_reality", params)`

Scan the project for UPROPERTY / API-symbol usages whose owning module is missing from the declaring module's `Build.cs` deps. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `module_filter` | string | `Other` | no | `""` | Substring match against the **declaring** module's name. Empty scans all. |
| `include_whitelist` | bool | `Other` | no | `false` | When `true`, also reports references to whitelisted implicit-dep modules (debug aid). |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor. |

**Response:**

```json
{
  "violations": [
    {
      "declaring_module": "MonolithMesh",
      "source_path": "Source/MonolithMesh/Private/Foo.cpp",
      "source_line": 142,
      "used_type": "UNiagaraSystem",
      "missing_dep": "Niagara"
    }
  ],
  "scanned_modules": 17,
  "scanned_declarations": 4823,
  "next_cursor": "<opaque>"
}
```

Annotations: `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true`. Carries `EMonolithParamKind::Other` on all params (no path normalisation applies — `module_filter` is a name substring, not a path).

**False-positive mitigation.** The audit returns violations sorted by `(declaring_module, source_path, source_line)` so duplicates clump together — typical scan reports group related findings, making batch review tractable. Callers MAY treat `include_whitelist=true` results as advisory only.

### 4b.5 Known limitations

- **Typedef-cleared cases caught poorly.** When a UPROPERTY uses `FMyAlias` and `FMyAlias = TSoftObjectPtr<UFoo>` is declared in a header, the audit currently resolves `FMyAlias` to whatever module declares the typedef rather than chasing the underlying type. Phase 3 (UHT-artefact parser) addresses this by reading the canonicalised type from the `*.generated.h` output instead of the source declaration.
- **Template-argument metaclasses partial.** Single-argument templates (`TSoftObjectPtr<X>`, `TSubclassOf<X>`, `TArray<X>`) are handled. Multi-argument templates (`TMap<K, V>`, custom 3+ arg templates) extract only the first argument in v0.17.0.
- **Macro-hidden types invisible.** Types behind `#if WITH_*` blocks that are gated false at audit time produce false negatives. This is intentional — the audit reflects the build-as-configured, not all theoretical configurations. Pair with `risk_query("list_conditional_gates")` to spot-check gated regions.
- **No deduplication across `module_filter` paginations.** A violation appearing in two distinct source lines surfaces as two rows. This is correct — distinct call sites are distinct evidence — but callers writing release-gate reports should dedupe on `(declaring_module, missing_dep)` before presenting summary counts.

---

## 5. CppReflect Intelligence (Phase 3 — WISHLIST)

**Planned namespace:** `cppreflect_query` + cpp↔asset edge tables.

**Substrate:** `tree-sitter-unreal-cpp` for header parsing + UHT artefacts (`Intermediate/Build/.../Inc/.../*.generated.h`) for the canonical reflected surface + `IAssetRegistry` for asset cross-references.

**Planned action surface (illustrative):**

- `cppreflect_query("get_uclass", class)` — full UPROPERTY / UFUNCTION / interface inventory for a UCLASS.
- `cppreflect_query("list_uproperties", class)` — paginated UPROPERTY list. SHOULD use cursor pagination.
- `cppreflect_query("find_interface_impls", interface)` — every UCLASS implementing the given UINTERFACE.

These tables are the prerequisite substrate for Phase 4's `network_query` and the seven audit actions.

---

## 6. Network Intelligence (Phase 4 — WISHLIST)

**Planned namespace:** `network_query` + seven audit actions across `material_query` / `niagara_query` / `animation_query` / `source_query` + `pipeline_query("release_readiness")` + `pipeline_query("pr_review")` composers.

**Substrate:** Phases 1+2+3 composed — decision corpus + risk scores + reflection edges.

**Planned action surface (illustrative):**

- `network_query("audit_replicated_properties", module_filter)` — project-wide replication audit.
- `material_query("audit_orphan_materials", path_prefix)` — materials with no usage edges in the asset graph.
- `niagara_query("audit_orphan_emitters", path_prefix)` — emitters with no system parents.
- `animation_query("audit_thread_safety", anim_bp)` — AnimBP math nodes touched from `BlueprintThreadSafeUpdateAnimation`.
- `pipeline_query("pr_review", changed_files[])` — composer bundling `risk_query("get_hotspot_score")` + `risk_query("get_cochange_pairs")` + `decision_query` lookups + `source_query("audit_module_dep_reality")` into a single PR-review payload. **Deferred to Phase 4** per the Phase 2 design spec — composer-side validation needs the cppreflect tables from Phase 3 to ground type-impact predictions.
- `pipeline_query("release_readiness")` — bundles all audits + Phase 1-3 reads into a single release-gate payload.

---

## 7. Dependencies

`Plugins/Monolith/Source/MonolithReflectionIntel/MonolithReflectionIntel.Build.cs`:

| Deps | Type |
|------|------|
| `Core`, `CoreUObject`, `Engine` | `PublicDependencyModuleNames` |
| `MonolithCore`, `SQLiteCore`, `DeveloperSettings`, `Json`, `JsonUtilities`, `Projects` | `PrivateDependencyModuleNames` |

`DeveloperSettings` is its own module (NOT part of `Engine`) — required for the `UDeveloperSettings`-derived `UMonolithReflectionIntelSettings`. Documented in `.claude/rules/scoped/cpp-code.md` § Module Dependencies; LNK2019 trap if omitted.

**Build.cs unchanged from Phase 1 → Phase 2.** Phase 2 adds git-log subprocess invocation (which uses `FPlatformProcess::CreateProc` from `Core`, already linked) and regex sweeps (`FRegexPattern` from `Core`). The module-dep audit's resolver reuses the existing `Core`/`CoreUObject` reflection surface. No new module deps required.

**No dependency on `MonolithSource`** — both adapters open their own ReadOnly handles on `EngineSource.db` rather than sharing the source subsystem's in-memory handle. The Phase 2 module-dep audit reads the source-indexer's existing symbol tables but opens its own ReadOnly handle for the same reason as Phase 1 (no `GetRawDatabase()` accessor on `FMonolithSourceDatabase` as of v0.17.0).

No conditional-gate `WITH_*` macros — the module loads unconditionally and contributes 11 actions (5 `decision` + 5 `risk` + 1 `source` audit) to every install.

---

## 8. Configuration

**Editor location:** Editor Preferences → Plugins → "Monolith Reflection Intel"
**INI file:** `Config/MonolithSettings.ini`
**Section:** `[/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings]`

| Setting | Default | Category | Description |
|---------|---------|----------|-------------|
| `bEnableDecisionMining` | `true` | Decision | Mine decision records from markdown corpora during indexing. When `false`, `RunDecisionIndexerOnce` skips with a status string. |
| `DecisionMinConfidence` | `0.6` | Decision | Floor in `[0, 1]` applied at query time by `list_decisions`. Per-call `min_confidence` parameter overrides this. |
| `DecisionMarkdownRoots` | `[]` | Decision | Project-relative directory paths to scan. Empty array uses defaults: `Docs/`, `Plugins/Monolith/Docs/`, `.claude/rules/`. |
| `bEnableGitCoChangeMining` | `true` | Risk | Toggle git-log mining for the risk indexers. Setting `false` short-circuits all three Phase 2 indexers (`FGitChurnIndexer`, `FGitCoChangeIndexer`, `FConditionalGateIndexer`) at `Run()` entry. |
| `MaxCoChangeWindowCommits` | `50` | Risk | Maximum commit history window per repo to walk for co-change pair detection. Clamped `[10, 500]`. Larger windows produce more pair density at the cost of indexer runtime. |
| `MaxCommitFileCount` | `50` | Risk | Per-commit file-touch cap. Commits touching more than this many files contribute zero co-change pairs (suppresses tree-wide refactor / initial-import noise). Clamped `[5, 500]`. |
| `GitMiningNoiseFilter` | `["Saved/*", "Intermediate/*", "Binaries/*", "*.uasset", "*.umap"]` | Risk | File-pattern blacklist applied to git-log output before pair / churn aggregation. Patterns are glob-style; an entry matches any file whose project-relative path matches the glob. |

`UMonolithReflectionIntelSettings::Get()` returns the cached CDO — cheap, allocation-free.

`UDeveloperSettings::GetCategoryName()` returns `"Plugins"` so the panel groups with other Monolith settings.

---

## 9. Threading Model

- **Phase 1 indexer (`FDecisionRecordIndexer::Run`)** runs on whatever thread invoked `FMonolithReflectionIntelModule::RunDecisionIndexerOnce`. In practice that is the game thread (first-call adapter path) or whichever thread fired `FCoreUObjectDelegates::ReloadCompleteDelegate` (Live Coding fires this on the game thread). The indexer is single-threaded by construction; SQLite ops use a single `FSQLiteDatabase` handle that lives only for the duration of `Run`.
- **Phase 2 indexers (`FGitChurnIndexer`, `FGitCoChangeIndexer`, `FConditionalGateIndexer`)** are scheduled on background threads via `FRunnableThread` after first-call detection — `MonolithReflectionIntelModule.cpp` posts the work to a background runnable and the calling action returns immediately if the table is missing. **However:** the lazy-bootstrap subprocess that fires `git log` during first-ever-call indexing currently runs on the game thread inline. This is a documented trade-off — first-call latency on a fresh install (~200ms on Leviathan-scale repos) is acceptable for the simpler control flow. Subsequent reindex invocations run fully on the background thread.
- **Adapter handlers (`FDecisionQueryAdapter::*`, `FRiskQueryAdapter::*`, `FModuleDepRealityAdapter::*`)** run on the game thread under `FMonolithToolRegistry::ExecuteAction`. All eleven handlers are pure read paths against cached ReadOnly handles — no mutation, no async work, no `ParallelFor`.
- The cached ReadOnly `FSQLiteDatabase*` in each adapter's `GetRawDB` is file-scope static. Phase 1+2 adapter usage is game-thread-only, so the caches are not lock-protected. If the adapter surface ever fans out to background threads, add an `FCriticalSection` around each cache check/replace.
- No render-thread work. No `UPROPERTY(Replicated)`. No `Server`/`Client`/`NetMulticast` UFUNCTIONs. Editor-only by design.

---

## 10. Cross-References

- **Parent spec:** [`SPEC_CORE.md`](../SPEC_CORE.md) — see §3 Module Reference and §12 Action Count Summary
- **MCP reference:** `Docs/references/MCP.md` — `decision_query` row + `risk_query` row + `source_query("audit_module_dep_reality")` entry
- **C++ conventions:** `.claude/rules/scoped/cpp-code.md` — module dep gotchas (`DeveloperSettings`, `FindFilesRecursive` 6th-param, SQLite WAL trap)
- **API verification log:** `Docs/references/UE57Gotchas.md`
- **Bug class motivating the module-dep audit:** the `UPROPERTY` referencing a foreign-module type without that module being in `Build.cs` — surfaces as a confusing LNK2019 against UHT-generated `Z_Construct_*_NoRegister` symbols.
