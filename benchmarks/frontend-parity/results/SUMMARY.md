# Frontend parity benchmark

Runs per frontend/fixture: 1 (warmup 0).
Wall time includes process startup, HTML+CSS parse, cascade, layout and IR write.
`direct` runs the binary; `run.ps1` includes PowerShell startup.

| Frontend | Fixture | Status | Mode | Wall median (ms) | Wall min | Wall max | Ops |
|---|---|---:|---|---:|---:|---:|---:|
| rust | contract | PASS | direct | 16.074 | 16.074 | 16.074 | 4 |
| rust | flex-page | PASS | direct | 18.194 | 18.194 | 18.194 | 23 |

Parity is measured against `golden/*.ir.json` with 0.01 px tolerance.
