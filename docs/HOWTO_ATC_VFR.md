# VFR ATC Communication Guide

Standardphrasen fuer VFR-Fluege in der Simulation. Alle Beispiele verwenden das Callsign **N123AB** (November One Two Three Alpha Bravo) und Runway **26** bzw. **28L**.

---

## 1. Towered Airport — Abflug

### Ground Contact (Initial Call)

> **Pilot:** *"Springfield Ground, November One Two Three Alpha Bravo, at the south ramp, VFR departure to the north, request taxi."*
>
> **ATC:** *"November One Two Three Alpha Bravo, Springfield Ground, taxi to runway two six via Alpha."*

### Taxi Readback

> **Pilot:** *"Taxi to runway two six via Alpha, November One Two Three Alpha Bravo."*

### Ready for Departure (Holding Short)

> **Pilot:** *"Springfield Tower, November One Two Three Alpha Bravo, holding short runway two six, ready for departure."*
>
> **ATC:** *"November One Two Three Alpha Bravo, runway two six, cleared for takeoff, winds two four zero at eight."*

### Takeoff Readback

> **Pilot:** *"Cleared for takeoff runway two six, November One Two Three Alpha Bravo."*

### Frequency Change (nach Abflug)

> **ATC:** *"November One Two Three Alpha Bravo, frequency change approved, good day."*
>
> **Pilot:** *"Switching frequency, November One Two Three Alpha Bravo, good day."*

---

## 2. Towered Airport — Anflug & Landung

### Initial Call (Inbound)

> **Pilot:** *"Springfield Tower, November One Two Three Alpha Bravo, ten miles to the south, inbound for landing with information Bravo."*
>
> **ATC:** *"November One Two Three Alpha Bravo, Springfield Tower, enter left downwind runway two six, report midfield downwind."*

### Position Reports (Pattern)

> **Pilot:** *"November One Two Three Alpha Bravo, midfield left downwind runway two six."*

> **Pilot:** *"November One Two Three Alpha Bravo, turning left base runway two six."*

> **Pilot:** *"November One Two Three Alpha Bravo, final runway two six."*

### Landing Clearance

> **ATC:** *"November One Two Three Alpha Bravo, runway two six, cleared to land, winds two four zero at eight."*
>
> **Pilot:** *"Cleared to land runway two six, November One Two Three Alpha Bravo."*

### Runway Vacated

> **Pilot:** *"Springfield Ground, November One Two Three Alpha Bravo, clear of runway two six."*
>
> **ATC:** *"November One Two Three Alpha Bravo, taxi to parking via Alpha."*

---

## 3. Uncontrolled Airport (CTAF / Unicom)

Bei unkontrollierten Flugplaetzen gibt es kein ATC — Piloten melden ihre Position selbst auf der CTAF-Frequenz.

### Self-Announce — Taxi

> **Pilot:** *"Oakdale traffic, November One Two Three Alpha Bravo, taxiing to runway two eight, Oakdale."*

### Self-Announce — Departure

> **Pilot:** *"Oakdale traffic, November One Two Three Alpha Bravo, departing runway two eight, departing to the north, Oakdale."*

### Self-Announce — Inbound

> **Pilot:** *"Oakdale traffic, November One Two Three Alpha Bravo, ten miles to the south, inbound for landing, Oakdale."*

### Self-Announce — Pattern

> **Pilot:** *"Oakdale traffic, November One Two Three Alpha Bravo, entering left downwind runway two eight, Oakdale."*

> **Pilot:** *"Oakdale traffic, November One Two Three Alpha Bravo, turning left base runway two eight, Oakdale."*

> **Pilot:** *"Oakdale traffic, November One Two Three Alpha Bravo, final runway two eight, full stop, Oakdale."*

### Self-Announce — Clear of Runway

> **Pilot:** *"Oakdale traffic, November One Two Three Alpha Bravo, clear of runway two eight, Oakdale."*

---

## 4. Spezialfaelle

### Unable

Wenn eine ATC-Anweisung nicht befolgt werden kann:

> **ATC:** *"November One Two Three Alpha Bravo, turn right heading one eight zero."*
>
> **Pilot:** *"Unable, November One Two Three Alpha Bravo."*

### Go-Around

> **Pilot:** *"November One Two Three Alpha Bravo, going around."*
>
> **ATC:** *"November One Two Three Alpha Bravo, roger, fly runway heading, climb and maintain two thousand."*

### Touch and Go

> **Pilot:** *"Springfield Tower, November One Two Three Alpha Bravo, request touch and go runway two six."*
>
> **ATC:** *"November One Two Three Alpha Bravo, runway two six, cleared touch and go."*

---

## 5. Wichtige Regeln

| Regel | Beispiel |
|---|---|
| Callsign immer am Ende des Readbacks | *"Cleared to land runway two six, **N123AB**"* |
| Airport-Name am Anfang und Ende bei CTAF | *"**Oakdale** traffic, ..., **Oakdale**"* |
| ATIS-Information referenzieren | *"...with information **Bravo**"* |
| Runway als einzelne Ziffern sprechen | Runway 26 = *"two six"*, nicht *"twenty six"* |
| Suffix aussprechen | 28L = *"two eight left"* |
| "Wilco" = will comply | Nur wenn du eine Anweisung befolgst |
| "Roger" = verstanden | Nur Bestaetigung, kein Compliance |

---

## 6. Mapping zu Plugin-Intents

| Phrase | Erkannter Intent |
|---|---|
| *"Ground/Tower, [callsign]..."* | `INITIAL_CALL` |
| *"request taxi..."* | `REQUEST_TAXI` |
| *"ready for departure / holding short..."* | `READY_FOR_DEPARTURE` |
| *"downwind / base / final / crosswind..."* | `REPORT_POSITION` |
| *"inbound / landing / full stop / touch and go..."* | `REQUEST_LANDING` |
| *"clear of runway / vacated..."* | `RUNWAY_VACATED` |
| *"wilco / roger / [runway readback]..."* | `READBACK` |
| *"frequency change / switching..."* | `REQUEST_FREQUENCY` |
| *"unable..."* | `UNABLE` |
| *"traffic..."* (CTAF) | `SELF_ANNOUNCE` |
