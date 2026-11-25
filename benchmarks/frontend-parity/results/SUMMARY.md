# Frontend parity benchmark

Runs per frontend/fixture: 1 (warmup 0).
Wall time includes process startup, HTML+CSS parse, cascade, layout and IR write.
`direct` runs the binary; `run.ps1` includes PowerShell startup.

| Frontend | Fixture | Status | Mode | Wall median (ms) | Wall min | Wall max | Ops |
|---|---|---:|---|---:|---:|---:|---:|
| rust | contract | PASS | direct | 155.047 | 155.047 | 155.047 | 4 |
| rust | flex-page | PASS | direct | 32.113 | 32.113 | 32.113 | 23 |

Parity is measured against `golden/*.ir.json` with 0.01 px tolerance.
