"""Builds port/data/initial_pipeline_cache.db, the bundled seed for Aurora's pipeline cache.

    python port/tools/make_pipeline_seed.py <cache dir> [<cache dir> ...] [-o OUT]

<cache dir> is an Aurora cache directory (SMS_PORT_CACHE_DIR) filled by playing/sweeping
the game; its pipeline_cache.db (plus WAL) holds one row per pipeline config the game used.
The rows of all given caches are merged into a fresh single-file database with Aurora's
schema (only the pipeline_cache and aurora_schema tables; no Dawn blobs, which are
driver/GPU specific). Aurora opens the seed read-only at startup (resourcesPath, i.e. the
exe's folder) and merges it into the per-user cache, then compiles every known pipeline in
the background, so a first start does not stall on shader compiles.

Rows are machine independent: the pipeline config (GX state) plus the frame it was first
used; the render-target layout is applied at load time. A seed made by an older build with
a different config version is skipped by Aurora row by row, never fatal.
"""
import argparse
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("caches", nargs="+", type=Path)
    ap.add_argument("-o", "--out", type=Path, default=ROOT / "port" / "data" / "initial_pipeline_cache.db")
    a = ap.parse_args()

    out = a.out
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    tmp.unlink(missing_ok=True)

    first = a.caches[0] / "pipeline_cache.db"
    if not first.exists():
        sys.exit(f"{first} not found")
    src = sqlite3.connect(f"file:{first.as_posix()}?mode=ro", uri=True)
    schema_sql = [r[0] for r in src.execute(
        "SELECT sql FROM sqlite_master WHERE type IN ('table','index') AND sql IS NOT NULL "
        "AND (tbl_name = 'pipeline_cache' OR tbl_name = 'aurora_schema')")]
    schema_rows = src.execute("SELECT * FROM aurora_schema").fetchall()
    cols = [r[1] for r in src.execute("PRAGMA table_info(pipeline_cache)")]
    src.close()

    dst = sqlite3.connect(tmp)
    for sql in schema_sql:
        dst.execute(sql)
    for row in schema_rows:
        dst.execute(f"INSERT INTO aurora_schema VALUES ({','.join('?' * len(row))})", row)
    total = 0
    for cache in a.caches:
        db = cache / "pipeline_cache.db"
        s = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True)
        rows = s.execute(f"SELECT {','.join(cols)} FROM pipeline_cache").fetchall()
        s.close()
        # Keep the earliest first_frame_used for duplicates (Aurora sorts prewarming by it).
        for row in rows:
            rec = dict(zip(cols, row))
            cur = dst.execute("SELECT first_frame_used FROM pipeline_cache WHERE type = ? AND hash = ?",
                              (rec["type"], rec["hash"])).fetchone()
            if cur is None:
                dst.execute(f"INSERT INTO pipeline_cache ({','.join(cols)}) VALUES ({','.join('?' * len(cols))})", row)
                total += 1
            elif rec["first_frame_used"] < cur[0]:
                dst.execute("UPDATE pipeline_cache SET first_frame_used = ? WHERE type = ? AND hash = ?",
                            (rec["first_frame_used"], rec["type"], rec["hash"]))
    dst.commit()
    dst.execute("VACUUM")
    dst.close()
    tmp.replace(out)
    by_type = sqlite3.connect(out).execute("SELECT type, COUNT(*) FROM pipeline_cache GROUP BY type").fetchall()
    print(f"wrote {out} ({total} pipelines, {out.stat().st_size // 1024} KiB; by type {dict(by_type)})")


if __name__ == "__main__":
    main()
