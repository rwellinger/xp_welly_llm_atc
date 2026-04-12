# Touch and Go / Go-Around — ATC Script

Towered airport, e.g. **LSZG Grenchen**
Callsign: **Hotel Bravo Lima Uniform Kilo** (HB-LUK)

---

## Prerequisites

- Traffic pattern flow (see `inbound_pattern_atc_script.md`) must be working
- Aircraft at gate/parking or already in the pattern
- COM1 on Tower frequency (LSZG: 120.100 MHz)
- PTT key configured
- API key saved

---

## Overview: Touch and Go

Touch and go = touchdown + immediate go-around without vacating the runway.
Typical for landing practice. The pilot stays in the pattern and flies additional circuits.

```
Taxi -> Takeoff -> Pattern -> Touch and Go -> Pattern -> Touch and Go -> ... -> Full Stop Landing
```

### State Flow:

```
TOWER_CONTACT -> REQUEST_TOUCH_AND_GO -> TOUCH_AND_GO_CLEARED
TOUCH_AND_GO_CLEARED -> REPORT_POSITION -> PATTERN_ENTRY -> REPORT_FINAL -> LANDING_CLEARED
                                                          -> REQUEST_TOUCH_AND_GO -> TOUCH_AND_GO_CLEARED (next circuit)
```

---

## Procedure: Touch and Go

*Prerequisite: Taxi + Takeoff as in the traffic pattern script (steps 1-5). This continues from TOWER_CONTACT.*

### 1. Request Touch and Go (TOWER_CONTACT -> TOUCH_AND_GO_CLEARED)

*After takeoff, still on Tower frequency*

**Pilot:**
> Grenchen Tower, Hotel Bravo Lima Uniform Kilo, request touch and go runway zero six.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, runway zero six, cleared touch and go, wind calm.

**Pilot (readback):**
> Cleared touch and go runway zero six, Hotel Bravo Lima Uniform Kilo.

---

*Perform touch and go: touchdown, full power, rotate, fly the pattern*

---

### 2. Report Downwind After Touch and Go (TOUCH_AND_GO_CLEARED -> PATTERN_ENTRY)

*Abeam runway midpoint, at pattern altitude*

**Pilot:**
> Grenchen Tower, Hotel Bravo Lima Uniform Kilo, left downwind runway zero six, touch and go.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, number one, runway zero six, continue approach, report final.

**Pilot:**
> Will report final, Hotel Bravo Lima Uniform Kilo.

---

### 3a. Report Final — Full Stop (PATTERN_ENTRY -> LANDING_CLEARED)

*When you want to land (no further touch and go):*

**Pilot:**
> Hotel Bravo Lima Uniform Kilo, final runway zero six.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, runway zero six, cleared to land, wind calm.

**Pilot (readback):**
> Cleared to land runway zero six, Hotel Bravo Lima Uniform Kilo.

*Continue with runway vacated as in the traffic pattern script.*

---

### 3b. Request Another Touch and Go (PATTERN_ENTRY -> TOUCH_AND_GO_CLEARED)

*When you want to fly another circuit:*

**Pilot:**
> Hotel Bravo Lima Uniform Kilo, request touch and go runway zero six.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, runway zero six, cleared touch and go, wind calm.

**Pilot (readback):**
> Cleared touch and go runway zero six, Hotel Bravo Lima Uniform Kilo.

*Back to step 2 (report downwind after touch and go).*

---

## Procedure: Go-Around

Go-around = abort the approach and climb away. Can happen at any point in the pattern or on final.

### State Flow:

```
PATTERN_ENTRY        -> GO_AROUND -> PATTERN_ENTRY (re-enter pattern)
LANDING_CLEARED      -> GO_AROUND -> PATTERN_ENTRY (re-enter pattern)
TOUCH_AND_GO_CLEARED -> GO_AROUND -> PATTERN_ENTRY (re-enter pattern)
```

---

### 4a. Go-Around from Pattern (PATTERN_ENTRY -> PATTERN_ENTRY)

*On base or in the pattern, you decide to go around*

**Pilot:**
> Grenchen Tower, Hotel Bravo Lima Uniform Kilo, going around.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway zero six.

**Pilot (readback):**
> Fly runway heading, re-enter left downwind runway zero six, Hotel Bravo Lima Uniform Kilo.

---

### 4b. Go-Around After Landing Clearance (LANDING_CLEARED -> PATTERN_ENTRY)

*On final, already cleared to land, but go-around necessary (e.g. runway blocked)*

**Pilot:**
> Hotel Bravo Lima Uniform Kilo, going around.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway zero six.

**Pilot (readback):**
> Fly runway heading, re-enter left downwind runway zero six, Hotel Bravo Lima Uniform Kilo.

---

### 4c. Go-Around After Touch-and-Go Clearance (TOUCH_AND_GO_CLEARED -> PATTERN_ENTRY)

*You have touch-and-go clearance but decide to go around without touching down*

**Pilot:**
> Hotel Bravo Lima Uniform Kilo, going around.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, roger, fly runway heading, re-enter left downwind runway zero six.

**Pilot (readback):**
> Fly runway heading, re-enter left downwind runway zero six, Hotel Bravo Lima Uniform Kilo.

---

## Full Test Run: Multiple Circuits

A typical landing practice session with multiple touch-and-gos:

```
1. Taxi + Takeoff (traffic pattern script steps 1-5)
2. Request touch and go (step 1)
3. Perform touch and go
4. Report downwind (step 2)
5. Request another touch and go (step 3b)
6. Perform touch and go
7. Report downwind (step 2)
8. Report final — full stop (step 3a)
9. Land, vacate runway (traffic pattern script step 8)
10. Taxi to parking (traffic pattern script step 9)
```

**Expected state transitions:**
```
IDLE -> TAXI_CLEARED -> TOWER_CONTACT -> TOUCH_AND_GO_CLEARED
-> PATTERN_ENTRY -> TOUCH_AND_GO_CLEARED
-> PATTERN_ENTRY -> LANDING_CLEARED -> IDLE
```
