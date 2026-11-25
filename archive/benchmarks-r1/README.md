Archived R1 IR generation benchmark: `bench.py` + `verify_ir.py` compared the retired display-list IR (`--out page.ir.json`) against frozen goldens, with timings sidecars.
Retired because the frontend no longer emits R1; the canonical path is covered by `wn-cli test all`, `bench pilot` and the visual/offscreen pixel suites.
