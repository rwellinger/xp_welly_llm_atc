# Experimental IFR departure scenarios (quarantined)

These three IFR-departure scenarios are **excluded from the default scenario
suite** (`make test` / `make test-scenarios`) because they currently fail
against the merged IFR feature code (PR #11, `chris3940/feat/ifr-dev`). They
were green when first added (commits `ecbe81f`, `a8a495a`) but regressed later
in the same branch.

They are kept here, not deleted, so the work is preserved and the fixes can be
verified later. Run them explicitly with:

```bash
make test-scenarios-ifr
```

## Known root causes (open — to be fixed by the IFR feature author)

- `ifr_lflp_departure_sim.json` — at an airport with **no active ATIS**
  (`no_atis: true`), the Tower should issue the IFR clearance directly. The
  redirect guard in `ground_operations.cpp::check_freq_precondition()`
  (the `REQUEST_IFR_CLEARANCE` block) instead always redirects Tower →
  Ground/Delivery, so the flow never reaches `IFR/PREDEP_CLEARANCE`. The guard
  needs an ATIS-inactive / Tower-direct exception.
- `ifr_lszh_departure_eu.json`, `ifr_lszh_departure_sim.json` — ATIS-active
  path; separate root cause(s) in the Delivery-based clearance flow, not yet
  isolated.

## Scope note

IFR is an **EU-only** feature. A hard gate in
`atc_state_machine::process()` strips IFR-only intents
(`REQUEST_IFR_CLEARANCE`, `REPORT_HOLDING_SHORT`) in non-EU profiles, so these
scenarios and the IFR flow have no effect in US/DE.
