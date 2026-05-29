#!/usr/bin/env python3
"""
DEPRECATED: monolith_offline.py is superseded by the MonolithQueryCommandlet.
  Preferred: UnrealEditor-Cmd.exe YourProject -run=MonolithQuery [args...]
  New features in commandlet: --scope, --module, --kind, --no-header, --members-only,
    --direction, --depth, --ref-kind, --json output mode, source indexing via 'index' namespace.
  This script remains as a zero-dependency fallback (no UE installation required).

Monolith Offline CLI — query EngineSource.db and ProjectIndex.db without the editor.

Usage:
    python monolith_offline.py source <action> [params...]
    python monolith_offline.py project <action> [params...]

Source actions:
    search_source <query> [--scope all|cpp|shaders] [--limit N] [--module M] [--kind K]
    read_source <symbol> [--header] [--max-lines N] [--members-only]
    find_references <symbol> [--ref-kind K] [--limit N]
    find_callers <symbol> [--limit N]
    find_callees <symbol> [--limit N]
    get_class_hierarchy <symbol> [--direction up|down|both] [--depth N]
    get_module_info <module_name>
    get_symbol_context <symbol> [--context-lines N]
    read_file <file_path> [--start N] [--end N]

Project actions:
    search <query> [--limit N]
    find_by_type <asset_class> [--limit N] [--offset N]
    find_references <asset_path>
    get_stats
    get_asset_details <asset_path>

Reflection Intelligence actions (read EngineSource.db reflect_* tables; offline subset):
    cppreflect get_uclass <class_name> [--module M]
    network    list_replicated_classes [--limit N]
    decision   list_decisions [--status open|accepted|...] [--limit N]
    risk       list_hotspots [--limit N]

NOTE: Only namespaces backed by on-disk SQLite are servable offline (source,
project, monolith, and the read-side RI namespaces above). The live MCP server
exposes ~29 namespaces; the rest require a running editor + UObject reflection
and CANNOT be served by this tool.
"""

import sys
import os
import re
import json
import sqlite3
import argparse
from pathlib import Path

# --- Database paths ---
# The databases live in the plugin's Saved/ directory. This script lives in
# Scripts/ (tracked) — earlier it lived in Saved/ alongside the DBs. Probe both
# layouts so the script resolves the DBs regardless of where it is run from:
#   1. SCRIPT_DIR/../Saved/<db>   (Scripts/ — current tracked location)
#   2. SCRIPT_DIR/<db>            (Saved/  — legacy co-located location)
SCRIPT_DIR = Path(__file__).parent


def _resolve_db(name: str) -> Path:
    candidates = [
        SCRIPT_DIR.parent / "Saved" / name,  # Scripts/ -> ../Saved/
        SCRIPT_DIR / name,                    # legacy: co-located in Saved/
    ]
    for c in candidates:
        if c.exists():
            return c
    # Fall back to the canonical Saved/ path so the not-found error names it.
    return candidates[0]


SOURCE_DB = _resolve_db("EngineSource.db")
PROJECT_DB = _resolve_db("ProjectIndex.db")


def open_db(path: Path) -> sqlite3.Connection:
    if not path.exists():
        print(f"ERROR: Database not found: {path}", file=sys.stderr)
        sys.exit(1)
    conn = sqlite3.connect(str(path))
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=DELETE;")
    conn.execute("PRAGMA query_only=ON;")
    return conn


def escape_fts(query: str) -> str:
    """Mirror the C++ EscapeFTS() / Python _escape_fts()."""
    q = query.replace("::", " ")
    cleaned = re.sub(r'[^\w\s]', '', q)
    tokens = cleaned.split()
    if not tokens:
        return '""'
    return " ".join(f'"{t}"*' for t in tokens)


# ============================================================
# Fuzzy match — ports MonolithFuzzyMatchDetail::ScoreFuzzyMatches
# (Source/MonolithCore/Private/MonolithFuzzyMatch.cpp). Levenshtein
# distance normalised to a 0..1 score, top-N descending, stable on ties.
# ============================================================

def _levenshtein(a: str, b: str) -> int:
    if a == b:
        return 0
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(
                prev[j] + 1,       # deletion
                cur[j - 1] + 1,    # insertion
                prev[j - 1] + (ca != cb),  # substitution
            ))
        prev = cur
    return prev[-1]


def fuzzy_top(needle: str, keys, top_n: int = 3):
    """Return up to top_n keys ranked by Levenshtein-normalised score desc.
    Mirrors the live server's did_you_mean matcher."""
    if not needle or not keys or top_n <= 0:
        return []
    scored = []
    for k in keys:
        dist = _levenshtein(needle, k)
        worst = max(len(needle), len(k)) or 1
        scored.append((1.0 - dist / worst, k))
    # Stable sort: Python's sorted is stable; sort by -score keeps insertion
    # order among ties (matches Algo::StableSort in the C++ port).
    scored.sort(key=lambda t: -t[0])
    return [k for _, k in scored[:top_n]]


def did_you_mean_suffix(needle: str, keys) -> str:
    sugg = fuzzy_top(needle, list(keys), 3)
    if not sugg:
        return ""
    return " did_you_mean: " + ", ".join(sugg)


# ============================================================
# Namespace registry — which namespaces this OFFLINE tool serves, and
# which are LIVE-ONLY (require a running editor). The unknown-namespace
# error lists all offline-servable namespaces + flags the boundary.
# ============================================================

# Populated after action classes are defined (see ACTIONS_BY_NS below).
OFFLINE_NAMESPACES = ["source", "project", "monolith", "cppreflect", "network", "decision", "risk"]


# ============================================================
# Source actions
# ============================================================

class SourceActions:
    def __init__(self):
        self.db = open_db(SOURCE_DB)

    def get_file_path(self, file_id: int) -> str:
        row = self.db.execute("SELECT path FROM files WHERE id = ?", (file_id,)).fetchone()
        return row["path"] if row else "<unknown>"

    def short_path(self, full_path: str) -> str:
        # Try to shorten to Engine-relative
        markers = ["Engine\\Source\\", "Engine/Source/", "Engine\\Shaders\\", "Engine/Shaders/"]
        for m in markers:
            idx = full_path.find(m)
            if idx >= 0:
                return full_path[idx:]
        return full_path

    def read_file_lines(self, file_path: str, start: int, end: int) -> str:
        try:
            with open(file_path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
        except FileNotFoundError:
            return f"[File not found: {file_path}]"
        start = max(1, start)
        end = min(len(lines), end)
        result = []
        for i in range(start - 1, end):
            result.append(f"{i+1:5d} | {lines[i].rstrip()}")
        return "\n".join(result)

    def search_source(self, args):
        query = args.query
        limit = args.limit
        module = getattr(args, 'module', None) or ""
        kind = getattr(args, 'kind', None) or ""
        fts_q = escape_fts(query)

        parts = []

        # Symbol FTS search
        sql = """SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start,
                        s.line_end, s.access, s.signature, s.docstring
                 FROM symbols_fts f JOIN symbols s ON s.id = f.rowid"""
        conditions = ["symbols_fts MATCH ?"]
        params = [fts_q]

        if module:
            sql += " JOIN files fi ON fi.id = s.file_id JOIN modules m ON m.id = fi.module_id"
            conditions.append("m.name = ?")
            params.append(module)
        if kind:
            conditions.append("s.kind = ?")
            params.append(kind)

        sql += " WHERE " + " AND ".join(conditions)
        sql += f" ORDER BY bm25(symbols_fts) LIMIT {limit}"

        rows = self.db.execute(sql, params).fetchall()
        if rows:
            parts.append("=== Symbol Matches ===")
            for r in rows:
                fp = self.short_path(self.get_file_path(r["file_id"]))
                parts.append(f"  [{r['kind']}] {r['qualified_name']} ({fp}:{r['line_start']})")
                if r["signature"]:
                    parts.append(f"         {r['signature']}")

        # Source line FTS search
        src_sql = """SELECT sf.file_id, sf.line_number, sf.text
                     FROM source_fts sf"""
        src_conditions = ["source_fts MATCH ?"]
        src_params = [fts_q]

        if module:
            src_sql += " JOIN files fi ON fi.id = sf.file_id JOIN modules m ON m.id = fi.module_id"
            src_conditions.append("m.name = ?")
            src_params.append(module)

        src_sql += " WHERE " + " AND ".join(src_conditions)
        src_sql += f" ORDER BY bm25(source_fts) LIMIT {limit}"

        src_rows = self.db.execute(src_sql, src_params).fetchall()
        if src_rows:
            parts.append("\n=== Source Line Matches ===")
            seen = set()
            for r in src_rows:
                key = (r["file_id"], r["line_number"])
                if key in seen:
                    continue
                seen.add(key)
                fp = self.short_path(self.get_file_path(r["file_id"]))
                text = r["text"].strip()[:120]
                parts.append(f"  {fp}:{r['line_number']}")
                parts.append(f"    {text}")

        print("\n".join(parts) if parts else f"No results found for '{query}'.")

    def read_source(self, args):
        symbol = args.symbol
        include_header = args.header
        max_lines = args.max_lines
        members_only = args.members_only

        # Exact name lookup, then FTS fallback
        rows = self.db.execute(
            "SELECT * FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC", (symbol,)
        ).fetchall()
        if not rows:
            fts_q = escape_fts(symbol)
            rows = self.db.execute(
                "SELECT s.* FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? ORDER BY bm25(symbols_fts) LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not rows:
            print(f"No symbol found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        parts = []
        seen = set()
        for r in rows:
            key = (r["file_id"], r["line_start"], r["line_end"])
            if key in seen:
                continue
            seen.add(key)
            fp = self.get_file_path(r["file_id"])
            if not include_header and fp.endswith(".h"):
                continue
            header = f"--- {self.short_path(fp)} (lines {r['line_start']}-{r['line_end']}) ---"
            source = self.read_file_lines(fp, r["line_start"], r["line_end"])
            parts.append(f"{header}\n{source}")

        result = "\n\n".join(parts) if parts else f"Found symbol '{symbol}' but could not read source."
        if max_lines and max_lines > 0:
            lines = result.split("\n")
            if len(lines) > max_lines:
                remaining = len(lines) - max_lines
                result = "\n".join(lines[:max_lines]) + f"\n[...truncated, {remaining} more lines]"
        print(result)

    def find_references(self, args):
        symbol = args.symbol
        ref_kind = getattr(args, 'ref_kind', None) or ""
        limit = args.limit

        sym_rows = self.db.execute("SELECT id, name FROM symbols WHERE name = ?", (symbol,)).fetchall()
        if not sym_rows:
            fts_q = escape_fts(symbol)
            sym_rows = self.db.execute(
                "SELECT s.id, s.name FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not sym_rows:
            print(f"No symbol found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        lines = []
        for sym in sym_rows:
            if ref_kind:
                refs = self.db.execute(
                    """SELECT r.ref_kind, r.line, s.name as from_name, f.path
                       FROM "references" r JOIN symbols s ON s.id = r.from_symbol_id
                       JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = ? LIMIT ?""",
                    (sym["id"], ref_kind, limit)
                ).fetchall()
            else:
                refs = self.db.execute(
                    """SELECT r.ref_kind, r.line, s.name as from_name, f.path
                       FROM "references" r JOIN symbols s ON s.id = r.from_symbol_id
                       JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? LIMIT ?""",
                    (sym["id"], limit)
                ).fetchall()
            for ref in refs:
                lines.append(f"[{ref['ref_kind']}] {self.short_path(ref['path'])}:{ref['line']} (from {ref['from_name']})")

        print("\n".join(lines) if lines else f"No references found for '{symbol}'.")

    def find_callers(self, args):
        symbol = args.symbol
        limit = args.limit

        sym_rows = self.db.execute("SELECT id FROM symbols WHERE name = ? AND kind = 'function'", (symbol,)).fetchall()
        if not sym_rows:
            fts_q = escape_fts(symbol)
            sym_rows = self.db.execute(
                "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? AND s.kind = 'function' LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not sym_rows:
            print(f"No function found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        lines = []
        for sym in sym_rows:
            refs = self.db.execute(
                """SELECT s.name as from_name, f.path, r.line
                   FROM "references" r JOIN symbols s ON s.id = r.from_symbol_id
                   JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = 'call' LIMIT ?""",
                (sym["id"], limit)
            ).fetchall()
            for ref in refs:
                lines.append(f"{ref['from_name']} -- {self.short_path(ref['path'])}:{ref['line']}")

        print("\n".join(lines) if lines else f"No callers found for '{symbol}'.")

    def find_callees(self, args):
        symbol = args.symbol
        limit = args.limit

        sym_rows = self.db.execute("SELECT id FROM symbols WHERE name = ? AND kind = 'function'", (symbol,)).fetchall()
        if not sym_rows:
            fts_q = escape_fts(symbol)
            sym_rows = self.db.execute(
                "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? AND s.kind = 'function' LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not sym_rows:
            print(f"No function found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        lines = []
        for sym in sym_rows:
            refs = self.db.execute(
                """SELECT s.name as to_name, f.path, r.line
                   FROM "references" r JOIN symbols s ON s.id = r.to_symbol_id
                   JOIN files f ON f.id = r.file_id WHERE r.from_symbol_id = ? AND r.ref_kind = 'call' LIMIT ?""",
                (sym["id"], limit)
            ).fetchall()
            for ref in refs:
                lines.append(f"{ref['to_name']} -- {self.short_path(ref['path'])}:{ref['line']}")

        print("\n".join(lines) if lines else f"No callees found for '{symbol}'.")

    def get_class_hierarchy(self, args):
        symbol = args.symbol
        direction = args.direction
        depth = args.depth

        sym_rows = self.db.execute(
            "SELECT id, name, file_id FROM symbols WHERE name = ? AND kind IN ('class','struct') ORDER BY (line_end > line_start) DESC",
            (symbol,)
        ).fetchall()
        if not sym_rows:
            fts_q = escape_fts(symbol)
            sym_rows = self.db.execute(
                "SELECT s.id, s.name, s.file_id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? AND s.kind IN ('class','struct') LIMIT 1",
                (fts_q,)
            ).fetchall()
        if not sym_rows:
            print(f"No class/struct found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        sym = sym_rows[0]
        fp = self.short_path(self.get_file_path(sym["file_id"]))
        lines = [f"{sym['name']} ({fp})"]

        visited = set()

        def walk_up(sid, indent, max_d):
            if indent > max_d or sid in visited:
                return
            visited.add(sid)
            parents = self.db.execute(
                "SELECT s.id, s.name FROM inheritance i JOIN symbols s ON s.id = i.parent_id WHERE i.child_id = ?",
                (sid,)
            ).fetchall()
            for p in parents:
                lines.append(f"{'  ' * indent}<- {p['name']}")
                walk_up(p["id"], indent + 1, max_d)

        def walk_down(sid, indent, max_d):
            if indent > max_d or sid in visited:
                return
            visited.add(sid)
            children = self.db.execute(
                "SELECT s.id, s.name FROM inheritance i JOIN symbols s ON s.id = i.child_id WHERE i.parent_id = ?",
                (sid,)
            ).fetchall()
            for c in children:
                lines.append(f"{'  ' * indent}-> {c['name']}")
                walk_down(c["id"], indent + 1, max_d)

        if direction in ("up", "both"):
            lines.append("\nAncestors:")
            count_before = len(lines)
            visited.clear()
            walk_up(sym["id"], 1, depth)
            if len(lines) == count_before:
                lines.append("  (none)")

        if direction in ("down", "both"):
            lines.append("\nDescendants:")
            count_before = len(lines)
            visited.clear()
            walk_down(sym["id"], 1, depth)
            if len(lines) == count_before:
                lines.append("  (none)")

        print("\n".join(lines))

    def get_module_info(self, args):
        module_name = args.module_name
        mod = self.db.execute("SELECT id, name, path, module_type FROM modules WHERE name = ?", (module_name,)).fetchone()
        if not mod:
            print(f"No module found matching '{module_name}'.", file=sys.stderr)
            sys.exit(1)

        file_count = self.db.execute("SELECT COUNT(*) as c FROM files WHERE module_id = ?", (mod["id"],)).fetchone()["c"]
        kind_rows = self.db.execute(
            "SELECT s.kind, COUNT(*) as cnt FROM symbols s JOIN files f ON f.id = s.file_id WHERE f.module_id = ? GROUP BY s.kind",
            (mod["id"],)
        ).fetchall()

        lines = [
            f"Module: {mod['name']}",
            f"Path: {self.short_path(mod['path'])}",
            f"Type: {mod['module_type']}",
            f"Files: {file_count}",
            "",
            "Symbol counts by kind:"
        ]
        for kr in sorted(kind_rows, key=lambda r: r["kind"]):
            lines.append(f"  {kr['kind']}: {kr['cnt']}")

        key_classes = self.db.execute(
            "SELECT s.name, s.line_start FROM symbols s JOIN files f ON f.id = s.file_id JOIN modules m ON m.id = f.module_id WHERE m.name = ? AND s.kind = 'class' LIMIT 20",
            (module_name,)
        ).fetchall()
        if key_classes:
            lines.extend(["", "Key classes:"])
            for c in key_classes:
                lines.append(f"  {c['name']} (line {c['line_start']})")

        print("\n".join(lines))

    def get_symbol_context(self, args):
        symbol = args.symbol
        ctx_lines = args.context_lines

        rows = self.db.execute("SELECT * FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC", (symbol,)).fetchall()
        if not rows:
            fts_q = escape_fts(symbol)
            rows = self.db.execute(
                "SELECT s.* FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not rows:
            print(f"No symbol found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        parts = []
        for i, r in enumerate(rows):
            if i >= 3:
                break
            fp = self.get_file_path(r["file_id"])
            ctx_start = max(1, r["line_start"] - ctx_lines)
            ctx_end = r["line_end"] + ctx_lines

            header = f"--- {r['qualified_name']} ---"
            info = [f"File: {self.short_path(fp)} (lines {r['line_start']}-{r['line_end']})"]
            if r["signature"]:
                info.append(f"Signature: {r['signature']}")
            if r["docstring"]:
                info.append(f"Docstring: {r['docstring']}")
            source = self.read_file_lines(fp, ctx_start, ctx_end)
            parts.append(f"{header}\n" + "\n".join(info) + f"\n\n{source}")

        print("\n\n".join(parts))

    def read_file(self, args):
        file_path = args.file_path
        start = args.start
        end = args.end

        resolved = None
        if os.path.isfile(file_path):
            resolved = file_path
        else:
            normalized = file_path.replace("/", "\\")
            row = self.db.execute("SELECT path FROM files WHERE path = ?", (normalized,)).fetchone()
            if row:
                resolved = row["path"]
            else:
                row = self.db.execute("SELECT path FROM files WHERE path LIKE ? LIMIT 1", (f"%{normalized}",)).fetchone()
                if row:
                    resolved = row["path"]

        if not resolved:
            print(f"No file found matching '{file_path}'.", file=sys.stderr)
            sys.exit(1)

        if end <= 0:
            end = start + 199

        header = f"--- {self.short_path(resolved)} (lines {start}-{end}) ---"
        source = self.read_file_lines(resolved, start, end)
        print(f"{header}\n{source}")


# ============================================================
# Project actions
# ============================================================

class ProjectActions:
    def __init__(self):
        self.db = open_db(PROJECT_DB)

    def search(self, args):
        query = args.query
        limit = args.limit

        # Search assets FTS
        results = []
        try:
            rows = self.db.execute(
                f"""SELECT a.package_path, a.asset_name, a.asset_class, a.module_name,
                           snippet(fts_assets, 2, '>>>', '<<<', '...', 32) as ctx, rank
                    FROM fts_assets f JOIN assets a ON a.id = f.rowid
                    WHERE fts_assets MATCH ? ORDER BY rank LIMIT {limit}""",
                (query,)
            ).fetchall()
            for r in rows:
                results.append({
                    "asset_path": r["package_path"],
                    "asset_name": r["asset_name"],
                    "asset_class": r["asset_class"],
                    "module_name": r["module_name"],
                    "match_context": r["ctx"],
                    "rank": r["rank"],
                })
        except sqlite3.OperationalError:
            pass

        # Search nodes FTS
        try:
            node_rows = self.db.execute(
                f"""SELECT a.package_path, a.asset_name, a.asset_class, a.module_name,
                           snippet(fts_nodes, 0, '>>>', '<<<', '...', 32) as ctx, f.rank
                    FROM fts_nodes f JOIN nodes n ON n.id = f.rowid
                    JOIN assets a ON a.id = n.asset_id
                    WHERE fts_nodes MATCH ? ORDER BY f.rank LIMIT {limit}""",
                (query,)
            ).fetchall()
            for r in node_rows:
                results.append({
                    "asset_path": r["package_path"],
                    "asset_name": r["asset_name"],
                    "asset_class": r["asset_class"],
                    "module_name": r["module_name"],
                    "match_context": r["ctx"],
                    "rank": r["rank"],
                })
        except sqlite3.OperationalError:
            pass

        results.sort(key=lambda x: x["rank"])
        results = results[:limit]

        print(json.dumps({"success": True, "count": len(results), "results": results}, indent=2))

    def find_by_type(self, args):
        asset_class = args.asset_class
        limit = args.limit
        offset = args.offset

        rows = self.db.execute(
            "SELECT package_path, asset_name, asset_class, module_name, description FROM assets WHERE asset_class = ? LIMIT ? OFFSET ?",
            (asset_class, limit, offset)
        ).fetchall()

        results = [dict(r) for r in rows]
        print(json.dumps({"success": True, "count": len(results), "results": results}, indent=2))

    def find_references(self, args):
        asset_path = args.asset_path
        asset = self.db.execute("SELECT id FROM assets WHERE package_path = ?", (asset_path,)).fetchone()
        if not asset:
            print(json.dumps({"success": False, "error": f"Asset not found: {asset_path}"}))
            return

        aid = asset["id"]

        # Depends on
        deps = self.db.execute(
            """SELECT a.package_path, a.asset_class, d.dependency_type
               FROM dependencies d JOIN assets a ON a.id = d.target_asset_id WHERE d.source_asset_id = ?""",
            (aid,)
        ).fetchall()

        # Referenced by
        refs = self.db.execute(
            """SELECT a.package_path, a.asset_class, d.dependency_type
               FROM dependencies d JOIN assets a ON a.id = d.source_asset_id WHERE d.target_asset_id = ?""",
            (aid,)
        ).fetchall()

        print(json.dumps({
            "success": True,
            "depends_on": [{"path": r["package_path"], "class": r["asset_class"], "type": r["dependency_type"]} for r in deps],
            "referenced_by": [{"path": r["package_path"], "class": r["asset_class"], "type": r["dependency_type"]} for r in refs],
        }, indent=2))

    def get_stats(self, args):
        tables = ["assets", "nodes", "connections", "variables", "parameters", "dependencies", "actors", "tags", "configs", "datatable_rows"]
        stats = {}
        for t in tables:
            try:
                row = self.db.execute(f"SELECT COUNT(*) as c FROM {t}").fetchone()
                stats[t] = row["c"]
            except sqlite3.OperationalError:
                stats[t] = 0

        # Class breakdown
        breakdown = {}
        try:
            rows = self.db.execute("SELECT asset_class, COUNT(*) as cnt FROM assets GROUP BY asset_class ORDER BY cnt DESC LIMIT 20").fetchall()
            for r in rows:
                breakdown[r["asset_class"]] = r["cnt"]
        except sqlite3.OperationalError:
            pass

        stats["asset_class_breakdown"] = breakdown

        # Module breakdown
        try:
            mod_rows = self.db.execute(
                "SELECT CASE WHEN module_name = '' THEN 'Project' ELSE module_name END as mod, COUNT(*) as cnt FROM assets GROUP BY module_name ORDER BY cnt DESC"
            ).fetchall()
            stats["module_breakdown"] = {r["mod"]: r["cnt"] for r in mod_rows}
        except sqlite3.OperationalError:
            stats["module_breakdown"] = {}

        print(json.dumps(stats, indent=2))

    def get_asset_details(self, args):
        asset_path = args.asset_path
        asset = self.db.execute(
            "SELECT * FROM assets WHERE package_path = ?", (asset_path,)
        ).fetchone()
        if not asset:
            print(json.dumps({"error": f"Asset not found: {asset_path}"}))
            return

        details = dict(asset)
        aid = asset["id"]

        # Nodes
        nodes = self.db.execute(
            "SELECT node_type, node_name, node_class FROM nodes WHERE asset_id = ?", (aid,)
        ).fetchall()
        details["nodes"] = [dict(n) for n in nodes]

        # Variables
        variables = self.db.execute(
            "SELECT var_name, var_type, category, default_value, is_exposed, is_replicated FROM variables WHERE asset_id = ?",
            (aid,)
        ).fetchall()
        details["variables"] = [dict(v) for v in variables]

        # Parameters
        params = self.db.execute(
            "SELECT param_name, param_type, param_group, default_value FROM parameters WHERE asset_id = ?",
            (aid,)
        ).fetchall()
        details["parameters"] = [dict(p) for p in params]

        print(json.dumps(details, indent=2, default=str))


# ============================================================
# Reflection Intelligence actions (read-only, EngineSource.db reflect_* tables)
#
# These mirror the live RI adapters (Source/MonolithReflectionIntel/Private/
# {CppReflect,Network,Decision,Risk}/F*QueryAdapter.cpp) but serve only the
# representative read actions that need nothing but on-disk SQLite. Table/column
# names verified against the *Schema.cpp CREATE TABLE statements.
#
# FIELD-NAME SOURCE OF TRUTH: the live F*QueryAdapter.cpp handlers (their
# SetStringField/SetNumberField/SetBoolField calls) define the canonical JSON
# row field names. Offline output MUST stay byte-identical to live — when a live
# handler renames a field, mirror it here AND in Tools/MonolithQuery/
# monolith_query.cpp. See FNetworkQueryAdapter::HandleListReplicatedClasses
# (replicated_property_count), FCppReflectQueryAdapter::HandleGetUClass
# (uproperty/ufunction cpp_module + bool blueprint_callable),
# FDecisionQueryAdapter::RowToJson (rationale + source_mtime).
# ============================================================

class ReflectionActions:
    """Shared base — all RI tables live in EngineSource.db."""

    def __init__(self):
        self.db = open_db(SOURCE_DB)

    def _lookup_class_source_line(self, class_name: str) -> int:
        """Item C — best-effort source-line auto-join, mirroring the live
        FCppReflectQueryAdapter::LookupClassSourceLine. UHT artefacts discard the
        header line, so reflect_* rows carry source_line=0. The same EngineSource.db
        symbol index (already powering source actions) has the real line. Name-only,
        kind-restricted to class/struct. Returns 0 on miss ("0 means unknown")."""
        if not class_name:
            return 0
        row = self.db.execute(
            "SELECT line_start FROM symbols WHERE name = ? AND kind IN ('class','struct') LIMIT 1",
            (class_name,)
        ).fetchone()
        return row["line_start"] if row else 0

    # ---- cppreflect get_uclass ----
    def get_uclass(self, args):
        class_name = args.class_name
        module = getattr(args, "module", None) or ""
        sql = "SELECT class_name, module_name, parent_class, source_path, source_line, flags FROM reflect_uclasses WHERE class_name = ?"
        params = [class_name]
        if module:
            sql += " AND module_name = ?"
            params.append(module)
        row = self.db.execute(sql, params).fetchone()
        if not row:
            print(json.dumps({"success": False, "error": f"UCLASS not found: {class_name}",
                              "note": "Not in reflection index — run UBT then rebuild_reflection_index, or check the name."}, indent=2))
            return
        uclass = dict(row)
        # Item C — auto-join the real header line when the stored value is 0.
        if not uclass.get("source_line"):
            uclass["source_line"] = self._lookup_class_source_line(uclass["class_name"])
        # Field names mirror FCppReflectQueryAdapter::HandleGetUClass: each
        # uproperty carries cpp_module; blueprint_callable is a bool on ufunctions.
        props = self.db.execute(
            "SELECT property_name, property_type, cpp_module, blueprint_visibility, specifiers FROM reflect_uproperties WHERE owning_class = ? AND cpp_module = ?",
            (uclass["class_name"], uclass["module_name"])
        ).fetchall()
        funcs = self.db.execute(
            "SELECT function_name, return_type, blueprint_callable, cpp_module, specifiers FROM reflect_ufunctions WHERE owning_class = ? AND cpp_module = ?",
            (uclass["class_name"], uclass["module_name"])
        ).fetchall()
        uclass["uproperties"] = [dict(p) for p in props]
        uclass["ufunctions"] = [
            {"function_name": f["function_name"], "return_type": f["return_type"],
             "blueprint_callable": bool(f["blueprint_callable"]), "cpp_module": f["cpp_module"],
             "specifiers": f["specifiers"]}
            for f in funcs
        ]
        print(json.dumps({"success": True, "uclass": uclass}, indent=2))

    # ---- network list_replicated_classes ----
    def list_replicated_classes(self, args):
        limit = args.limit
        # Field name mirrors FNetworkQueryAdapter::HandleListReplicatedClasses:
        # the per-class count is "replicated_property_count" (NOT "replicated_count").
        rows = self.db.execute(
            """SELECT owning_class, cpp_module, COUNT(*) AS replicated_property_count
               FROM reflect_replicated_properties
               GROUP BY owning_class, cpp_module
               ORDER BY replicated_property_count DESC, owning_class LIMIT ?""",
            (limit,)
        ).fetchall()
        results = [dict(r) for r in rows]
        print(json.dumps({"success": True, "count": len(results), "classes": results}, indent=2))

    # ---- decision list_decisions ----
    def list_decisions(self, args):
        status = getattr(args, "status", None) or ""
        limit = args.limit
        # Field names + column set mirror FDecisionQueryAdapter::RowToJson:
        # each row carries rationale + source_mtime in addition to the basics.
        cols = "decision_id, title, status, source_path, source_line, confidence, rationale, source_mtime"
        if status:
            rows = self.db.execute(
                f"SELECT {cols} FROM decision_records WHERE status = ? ORDER BY source_path LIMIT ?",
                (status, limit)
            ).fetchall()
        else:
            rows = self.db.execute(
                f"SELECT {cols} FROM decision_records ORDER BY source_path LIMIT ?",
                (limit,)
            ).fetchall()
        results = [dict(r) for r in rows]
        print(json.dumps({"success": True, "count": len(results), "decisions": results}, indent=2))

    # ---- risk list_hotspots ----
    def list_hotspots(self, args):
        limit = args.limit
        rows = self.db.execute(
            """SELECT file_path, churn, complexity_proxy, score
               FROM risk_hotspot_scores ORDER BY score DESC, file_path LIMIT ?""",
            (limit,)
        ).fetchall()
        results = [dict(r) for r in rows]
        print(json.dumps({"success": True, "count": len(results), "hotspots": results}, indent=2))


# ============================================================
# CLI setup
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description="Monolith Offline CLI — query source/project databases without the editor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    sub = parser.add_subparsers(dest="namespace", required=True)

    # --- source namespace ---
    src = sub.add_parser("source", help="Query engine source database")
    src_sub = src.add_subparsers(dest="action", required=True)

    # search_source
    p = src_sub.add_parser("search_source", help="Full-text search across UE source")
    p.add_argument("query")
    p.add_argument("--scope", default="all", choices=["all", "cpp", "shaders"])
    p.add_argument("--limit", type=int, default=20)
    p.add_argument("--module", default="")
    p.add_argument("--kind", default="", help="Filter by symbol kind (class, function, enum, etc.)")

    # read_source
    p = src_sub.add_parser("read_source", help="Get source code for a symbol")
    p.add_argument("symbol")
    p.add_argument("--header", action="store_true", default=True, help="Include header declarations")
    p.add_argument("--no-header", dest="header", action="store_false")
    p.add_argument("--max-lines", type=int, default=0)
    p.add_argument("--members-only", action="store_true")

    # find_references
    p = src_sub.add_parser("find_references", help="Find all references to a symbol")
    p.add_argument("symbol")
    p.add_argument("--ref-kind", default="")
    p.add_argument("--limit", type=int, default=50)

    # find_callers
    p = src_sub.add_parser("find_callers", help="Find all callers of a function")
    p.add_argument("symbol")
    p.add_argument("--limit", type=int, default=50)

    # find_callees
    p = src_sub.add_parser("find_callees", help="Find all functions called by a function")
    p.add_argument("symbol")
    p.add_argument("--limit", type=int, default=50)

    # get_class_hierarchy
    p = src_sub.add_parser("get_class_hierarchy", help="Show inheritance tree for a class")
    p.add_argument("symbol")
    p.add_argument("--direction", default="both", choices=["up", "down", "both"])
    p.add_argument("--depth", type=int, default=5)

    # get_module_info
    p = src_sub.add_parser("get_module_info", help="Get module statistics")
    p.add_argument("module_name")

    # get_symbol_context
    p = src_sub.add_parser("get_symbol_context", help="Get symbol with surrounding context lines")
    p.add_argument("symbol")
    p.add_argument("--context-lines", type=int, default=10)

    # read_file
    p = src_sub.add_parser("read_file", help="Read source lines from a file")
    p.add_argument("file_path")
    p.add_argument("--start", type=int, default=1)
    p.add_argument("--end", type=int, default=0)

    # --- project namespace ---
    prj = sub.add_parser("project", help="Query project index database")
    prj_sub = prj.add_subparsers(dest="action", required=True)

    # search
    p = prj_sub.add_parser("search", help="Full-text search across project assets")
    p.add_argument("query")
    p.add_argument("--limit", type=int, default=50)

    # find_by_type
    p = prj_sub.add_parser("find_by_type", help="Find assets by class type")
    p.add_argument("asset_class")
    p.add_argument("--limit", type=int, default=50)
    p.add_argument("--offset", type=int, default=0)

    # find_references
    p = prj_sub.add_parser("find_references", help="Find asset dependencies")
    p.add_argument("asset_path")

    # get_stats
    p = prj_sub.add_parser("get_stats", help="Get index statistics")

    # get_asset_details
    p = prj_sub.add_parser("get_asset_details", help="Get full details for an asset")
    p.add_argument("asset_path")

    # --- cppreflect namespace (offline RI subset) ---
    cpr = sub.add_parser("cppreflect", help="Reflection Intelligence: C++ UCLASS structure")
    cpr_sub = cpr.add_subparsers(dest="action", required=True)
    p = cpr_sub.add_parser("get_uclass", help="One UCLASS row + its UPROPERTYs and UFUNCTIONs")
    p.add_argument("class_name")
    p.add_argument("--module", default="")

    # --- network namespace (offline RI subset) ---
    net = sub.add_parser("network", help="Reflection Intelligence: replication audit")
    net_sub = net.add_subparsers(dest="action", required=True)
    p = net_sub.add_parser("list_replicated_classes", help="Classes with replicated UPROPERTYs")
    p.add_argument("--limit", type=int, default=50)

    # --- decision namespace (offline RI subset) ---
    dec = sub.add_parser("decision", help="Reflection Intelligence: decision records")
    dec_sub = dec.add_subparsers(dest="action", required=True)
    p = dec_sub.add_parser("list_decisions", help="List markdown ADR/decision records")
    p.add_argument("--status", default="")
    p.add_argument("--limit", type=int, default=50)

    # --- risk namespace (offline RI subset) ---
    rsk = sub.add_parser("risk", help="Reflection Intelligence: git hotspot signals")
    rsk_sub = rsk.add_subparsers(dest="action", required=True)
    p = rsk_sub.add_parser("list_hotspots", help="Top files by churn x complexity score")
    p.add_argument("--limit", type=int, default=50)

    # Intercept an unknown namespace BEFORE argparse so we can emit the live
    # server's error format + did_you_mean suggestions + the offline boundary.
    if len(sys.argv) >= 2 and not sys.argv[1].startswith("-"):
        requested_ns = sys.argv[1]
        if requested_ns not in OFFLINE_NAMESPACES:
            offline_list = ", ".join(f"'{n}'" for n in OFFLINE_NAMESPACES)
            msg = f"Unknown namespace: {requested_ns} (offline-servable: {offline_list})"
            msg += did_you_mean_suffix(requested_ns, OFFLINE_NAMESPACES)
            print(f"ERROR: {msg}", file=sys.stderr)
            print("NOTE: this offline tool serves only namespaces backed by on-disk SQLite. "
                  "The live MCP server exposes ~29 namespaces; the rest are LIVE-ONLY "
                  "(require a running editor + UObject reflection).", file=sys.stderr)
            sys.exit(2)

    args = parser.parse_args()

    if args.namespace == "source":
        sa = SourceActions()
        action_map = {
            "search_source": sa.search_source,
            "read_source": sa.read_source,
            "find_references": sa.find_references,
            "find_callers": sa.find_callers,
            "find_callees": sa.find_callees,
            "get_class_hierarchy": sa.get_class_hierarchy,
            "get_module_info": sa.get_module_info,
            "get_symbol_context": sa.get_symbol_context,
            "read_file": sa.read_file,
        }
        action_map[args.action](args)
    elif args.namespace == "project":
        pa = ProjectActions()
        action_map = {
            "search": pa.search,
            "find_by_type": pa.find_by_type,
            "find_references": pa.find_references,
            "get_stats": pa.get_stats,
            "get_asset_details": pa.get_asset_details,
        }
        action_map[args.action](args)
    elif args.namespace in ("cppreflect", "network", "decision", "risk"):
        ra = ReflectionActions()
        action_map = {
            "get_uclass": ra.get_uclass,
            "list_replicated_classes": ra.list_replicated_classes,
            "list_decisions": ra.list_decisions,
            "list_hotspots": ra.list_hotspots,
        }
        action_map[args.action](args)


if __name__ == "__main__":
    main()
