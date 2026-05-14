# Archived benchmark results

This directory contains milestone benchmark results — runs that back a
published claim in a patch submission, cover letter, or release note.

Routine CI results are not committed here.

---

## File naming

```
YYYY-MM-DD-<label>.json
```

Where `<label>` identifies the event: `baseline`, `replay-opt-patch`,
`release-3.5.4`, etc.

---

## File format

Each file is a JSON object with two top-level keys:

```json
{
  "meta": {
    "date": "2026-05-14",
    "label": "baseline",
    "command": "./fork_bench --iterations=500 --warmup=50",
    "hardware": "Intel i9-11900H, 64 GB RAM",
    "os": "Windows 10.0.26200",
    "cygwin": "3.5.3-1",
    "cygwin_dll": "unpatched main branch at a1f347c0d",
    "notes": ""
  },
  "result": { ...fork-bench-v1 output... }
}
```

The `result` field contains the raw JSON output of `fork_bench`
verbatim.

---

## How to add a result

```sh
# Run the benchmark, capture output
./fork_bench --iterations=500 --warmup=50 > /tmp/result.json

# Wrap in the archive format (edit meta fields as needed)
python3 - <<'EOF'
import json, sys
result = json.load(open('/tmp/result.json'))
archive = {
    "meta": {
        "date": "YYYY-MM-DD",
        "label": "label",
        "command": "./fork_bench --iterations=500 --warmup=50",
        "hardware": "...",
        "os": "...",
        "cygwin": "...",
        "cygwin_dll": "...",
        "notes": ""
    },
    "result": result
}
json.dump(archive, sys.stdout, indent=2)
print()
EOF

# Commit
git add bench/results/YYYY-MM-DD-label.json
git commit -m "bench: archive baseline result for <purpose>"
```
