# Frontend parity benchmark

Runs per frontend/fixture: 3 (warmup 2).
Wall time includes process startup, HTML+CSS parse, cascade, layout and IR write.
`direct` runs the binary; `run.ps1` includes PowerShell startup.

| Frontend | Fixture | Status | Mode | Wall median (ms) | Wall min | Wall max | Ops |
|---|---|---:|---|---:|---:|---:|---:|
| python | contract | PASS | direct | 375.763 | 374.178 | 380.523 | 4 |
| cpp | contract | PASS | direct | 10.872 | 10.833 | 11.224 | 4 |
| rust | contract | PASS | direct | 10.155 | 9.742 | 10.479 | 4 |
| csharp | contract | PASS | direct | 29.266 | 29.178 | 29.591 | 4 |
| python | flex-page | PASS | direct | 382.721 | 378.618 | 383.748 | 23 |
| cpp | flex-page | PASS | direct | 11.612 | 11.355 | 11.855 | 23 |
| rust | flex-page | PASS | direct | 10.351 | 10.186 | 11.389 | 23 |
| csharp | flex-page | PASS | direct | 29.57 | 29.444 | 30.005 | 23 |

Parity is measured against `golden/*.ir.json` with 0.01 px tolerance.
