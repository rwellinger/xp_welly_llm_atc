# IFR Ground Phase — Taxi Routing

## Goal

Replace the current generic taxi template ("taxi to holding point runway 2 via Alpha, QNH 1013") with ATC-computed, airport-aware routing: "taxi to holding point Alpha 3, via Alpha, Bravo, hold short runway 28".

Routing is derived from the `apt.dat` taxiway graph already bundled with X-Plane — no external database needed.

---

## Prerequisites

- `apt.dat` row codes 1201/1202/1204 parsed into a per-airport taxiway graph (see below)
- IFR Slice 1+2 fully working (clearance → taxi → holding point) — **done** as of `feat/ifr-dev`

---

## Phase 1 — apt.dat taxiway graph

### Row codes to parse (in `xplane_context_runtime.cpp`, `build_towered_cache()`)

| Code | Meaning | Key fields |
|------|---------|-----------|
| `1201` | Taxiway node | lat, lon, usage (`taxiway`/`both`/`vehicle`), name (e.g. `A1`, `RWY28 hold`) |
| `1202` | Taxiway edge | from-node id, to-node id, one-way flag, taxiway name (e.g. `A`, `B`) |
| `1204` | Active zone | edge id, runway number — marks edges that cross a runway (hot spots) |

### New data structures (add to `src/data/airspace_db.hpp` or new `src/data/taxiway_graph.hpp`)

```cpp
struct TaxiwayNode {
    uint32_t id;
    double lat, lon;
    std::string name;   // "A1", "RWY28 hold", "" if unnamed
};

struct TaxiwayEdge {
    uint32_t from, to;
    std::string taxiway;   // "A", "B", ""
    bool one_way = false;
    bool active_zone = false;  // crosses a runway
};

struct TaxiwayGraph {
    std::vector<TaxiwayNode> nodes;
    std::vector<TaxiwayEdge> edges;
    // adjacency index built after parse
};

// Per-airport, populated alongside RunwayInfo in build_towered_cache()
std::unordered_map<std::string, TaxiwayGraph> taxiway_graphs;
```

### Parsing notes

- Nodes and edges are airport-local (appear between the airport header row and the next `1` row)
- Node IDs are integers local to the file — reset per airport, so store them relative to the airport block
- `1204` rows reference edge indices — link them during the same parse pass
- Only parse `usage != "vehicle"` nodes (exclude ground vehicle routes)

---

## Phase 2 — Shortest-path routing

### Algorithm

Dijkstra on the taxiway graph, edge weight = Haversine distance between node endpoints.

```cpp
// src/data/taxiway_graph.hpp
struct TaxiRoute {
    std::vector<uint32_t> node_ids;
    std::vector<std::string> taxiways;   // deduplicated, in order: ["A", "B"]
    std::string holding_point_name;      // e.g. "Alpha 3" (from node name)
};

std::optional<TaxiRoute> find_route(
    const TaxiwayGraph& g,
    double from_lat, double from_lon,   // aircraft position
    const std::string& runway_number    // "28", "28L"
);
```

Start node: nearest node to aircraft position (Haversine, `usage == taxiway | both`).  
End node: node whose `name` matches `"RWY{runway} hold"` or nearest node tagged as `active_zone` leading to the target runway.

### Route → ATC phrase

```cpp
// In ground_operations::build_vars(), replace hardcoded taxi route:
{"taxi_route", route.taxiways.empty()
    ? std::string{"holding point"}
    : "holding point " + route.holding_point_name + ", via " + join(route.taxiways, ", ")},
```

Template then becomes:
```
"{callsign}, taxi to {taxi_route}, QNH {qnh}."
→ "One Romeo Charlie, taxi to holding point Alpha 3, via Alpha, Bravo, QNH 1013."
```

Fallback when no graph available (airport not in apt.dat or parse failed): keep current generic phrase.

---

## Phase 3 — Active zone warnings

When the computed route crosses a runway (`active_zone == true` on any edge), append a hold-short instruction:

```
"One Romeo Charlie, taxi to holding point Alpha 3, via Alpha, hold short runway 10 at Bravo."
```

This requires detecting which active zone the route crosses and naming the crossing runway. Use the `1204` runway number field.

---

## Template changes

No new JSON keys needed — `{taxi_route}` replaces the existing hardcoded string in `build_vars()`. The template itself stays:

```json
"REQUEST_TAXI": {
    "response": "{callsign}, taxi to {taxi_route}, QNH {qnh}.",
    ...
}
```

---

## Integration with IFR ground phase

IFR taxi instruction is identical in structure to VFR — same template, same `{taxi_route}` variable. The IFR squawk and SID don't change the routing. The improvement applies to both VFR and IFR automatically.

---

## Testing

- Unit test: `tests/test_taxiway_graph.cpp` — parse a minimal apt.dat snippet, verify shortest path
- Scenario: extend `testscripts/ifr_lszh_departure_eu.json` step 4 to `expect: "via Alpha"` (or whatever LSZH's actual graph yields — update after parsing real apt.dat)
- Fallback: a scenario with a fictional airport (no graph) must still produce a valid taxi instruction

---

## Open questions

1. **apt.dat availability in atc_repl**: the headless REPL has no `XPLANE_ROOT` — taxiway graphs would be empty, scenarios fall back gracefully. No issue.
2. **Graph size**: LSZH has ~200 nodes. Memory cost is negligible. Build index at parse time.
3. **Moving aircraft start position**: at cold-start the aircraft may be on a parking stand (row `1300` `startup` location). Phase C could use stand name as the origin label. Out of scope for Phase 1.
