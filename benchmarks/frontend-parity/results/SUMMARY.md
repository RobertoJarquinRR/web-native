# Frontend parity benchmark

Runs per frontend/fixture: 3 (warmup 2).
Wall time includes process startup, HTML+CSS parse, cascade, layout and IR write.
`direct` runs the binary; `run.ps1` includes PowerShell startup.

| Frontend | Fixture | Status | Mode | Wall median (ms) | Wall min | Wall max | Ops |
|---|---|---:|---|---:|---:|---:|---:|
| python | contract | PASS | direct | 386.318 | 385.925 | 418.976 | 4 |
| cpp | contract | PASS | direct | 11.756 | 10.981 | 12.567 | 4 |
| rust | contract | PASS | direct | 10.158 | 10.132 | 10.496 | 4 |
| csharp | contract | PASS | direct | 30.211 | 29.572 | 30.374 | 4 |
| python | flex-page | PASS | direct | 451.296 | 431.557 | 474.47 | 23 |
| cpp | flex-page | PASS | direct | 14.265 | 13.384 | 16.867 | 23 |
| rust | flex-page | PASS | direct | 11.831 | 11.317 | 12.513 | 23 |
| csharp | flex-page | PASS | direct | 31.459 | 31.248 | 34.528 | 23 |

Parity is measured against `golden/*.ir.json` with 0.01 px tolerance.
