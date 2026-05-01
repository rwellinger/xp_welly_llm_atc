# Prompt — `make sanitize` in einem X-Plane-Plugin-Repo nachziehen

Verwendung: in eines der anderen Plugin-Repos (`xp_pilot`, `xp_swiss_vfr`,
`xp_welly_atc`) wechseln und den folgenden Block an Claude geben.

---

## Aufgabe

Bau einen `make sanitize`-Target ein, der die SDK-freien Teile des Codes
unter **AddressSanitizer + UndefinedBehaviorSanitizer** baut und ausführt.
Ziel ist eine schnelle, automatisierbare Memory-Bug-Sicherung in CI —
kein Ersatz für Instruments.app im laufenden Sim.

## Kontext / Constraints

- macOS, Apple Silicon (ARM64). Toolchain ist Homebrew clang / Apple clang.
- LeakSanitizer ist auf macOS ARM64 **nicht supported** → mit
  `ASAN_OPTIONS=detect_leaks=0` ausschalten, sonst bricht ASan beim Start ab.
  Echte Leak-Suche läuft separat über Instruments.app im X-Plane-Prozess.
- Die `.xpl` Plugin-MODULE-Library **darf nicht** instrumentiert werden:
  ASan im X-Plane-Prozess ist auf macOS ARM64 fragil (dyld + Code-Signing).
- Vendored Libs (alles unter `vendor/`, `sdk/`, ggf. `spikes/.../third_party/`)
  **bleiben unsanitized** — bauen oft nicht clean unter Sanitizer und sind
  ohnehin nicht unser Code.
- Nur folgendes wird instrumentiert:
  1. die SDK-freie Engine-OBJECT-Library (oder das Äquivalent — die TUs
     die der headless CLI/REPL und die Catch2-Tests teilen)
  2. das headless CLI/REPL-Tool
  3. die Catch2-Test-Executable

## Vorgehen

1. **Repo erkunden**:
   - `CMakeLists.txt` lesen — welche Targets gibt es? Welche OBJECT-Lib
     teilen Plugin + headless Tool + Tests? Welche `add_subdirectory`-Aufrufe
     ziehen Vendor-Code rein?
   - `Makefile` lesen — welche `.PHONY`-Targets, wie sieht `lint`/`clean`/
     `help` aus? Welcher Build-Dir-Convention folgt das Repo (`build/`,
     `build-lint/` …)?
   - `tests/CMakeLists.txt` (oder Äquivalent) lesen.
   - Wenn das Repo **kein** headless Tool hat, ist `make sanitize` über die
     Tests allein OK — bei nur einem .xpl-Target ohne Tests macht das
     Feature keinen Sinn, dann zurückmelden statt einbauen.

2. **CMakeLists.txt** — Option + Flags-Variable definieren, am besten
   direkt nach den anderen `option(...)`-Zeilen oben:

   ```cmake
   option(<PROJECT>_SANITIZE "Enable ASan + UBSan on engine/CLI/tests" OFF)
   if(<PROJECT>_SANITIZE)
       set(<PROJECT>_SANITIZER_FLAGS
           -fsanitize=address,undefined
           -fno-omit-frame-pointer
           -fno-sanitize-recover=all
       )
       message(STATUS "Sanitizer build: ASan + UBSan on engine, CLI, tests")
   endif()
   ```

   `<PROJECT>` durch den Repo-Prefix ersetzen (z.B. `XP_SWISS_VFR`).

3. **CMakeLists.txt** — Flags an die drei Target-Klassen anhängen.
   Compile-Options auf alle drei, Link-Options nur auf die Executables:

   ```cmake
   if(<PROJECT>_SANITIZE)
       target_compile_options(<engine_object_lib> PRIVATE
           ${<PROJECT>_SANITIZER_FLAGS} -O1 -g)
   endif()

   if(<PROJECT>_SANITIZE)
       target_compile_options(<cli_or_repl_target> PRIVATE
           ${<PROJECT>_SANITIZER_FLAGS} -O1 -g)
       target_link_options(<cli_or_repl_target> PRIVATE
           ${<PROJECT>_SANITIZER_FLAGS})
   endif()
   ```

   Wenn die Tests in `tests/CMakeLists.txt` definiert sind, dort denselben
   Block für das Test-Target. `<PROJECT>_SANITIZER_FLAGS` ist im
   Sub-Verzeichnis sichtbar (Parent-Scope-Variable).

   Das **Plugin-MODULE-Target** (`add_library(<plugin> MODULE …)`) bekommt
   **keine** Sanitizer-Flags — explizit nichts dort einbauen.

4. **Makefile** — neuen Target ergänzen, `.PHONY` und `clean` updaten,
   `help` ergänzen:

   ```make
   .PHONY: ... sanitize ...

   help:
       …
       @echo "  make sanitize          Build CLI + tests with ASan+UBSan and run them"
       @echo "  make clean             Remove build/, build-lint/ and build-sanitize/"
       …

   sanitize: <same prerequisites as build/test targets>
       @echo "=== Configuring sanitizer build (ASan + UBSan) ==="
       cmake -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -D<PROJECT>_SANITIZE=ON -Wno-dev
       @echo "=== Building <cli> + <tests> with ASan + UBSan ==="
       cmake --build build-sanitize --target <cli_target> <test_target> --parallel
       @echo ""
       @echo "=== Running unit tests under ASan + UBSan ==="
       @ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_stacktrace=1 \
        UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
            ./build-sanitize/<test_target>
       @echo ""
       @echo "=== Running scenario tests under ASan + UBSan ==="    # nur falls scenarios existieren
       @ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_stacktrace=1 \
        UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
            ./build-sanitize/<cli_target> run testscripts/*.json
       @echo ""
       @echo "Sanitizer run clean."

   clean:
       rm -rf build/ build-lint/ build-sanitize/
   ```

   Begründung als Kommentar am Sanitize-Target ergänzen (warum nur
   CLI/Tests, nicht das `.xpl` — Verweis auf Instruments.app).

5. **Verifizieren**: `make sanitize` ausführen. Erwartet:
   - Build-Phase ohne Fehler
   - Tests laufen bis Ende, alle Assertions PASS
   - Exit 0
   - Keine `==<pid>==ERROR: AddressSanitizer:` oder
     `runtime error:` Zeilen im Output

   Falls ASan tatsächlich Findings zeigt: **diese sind echte Bugs**, nicht
   "False Positives" abtun. Stack-Trace lesen, root-cause fixen.

6. **CLAUDE.md**: einen Satz im Build-System-Block ergänzen, dass
   `make sanitize` existiert und was es abdeckt (analog zur `make lint`-Zeile).

## Was NICHT zu tun ist

- Keine `add_compile_options`/`add_link_options` auf Top-Level — würde
  Vendor-Code mit-instrumentieren.
- Kein `-fsanitize=thread` parallel — TSan ist mit ASan inkompatibel.
  Falls Threading-Bugs ein Thema sind, dafür separaten Target
  `make sanitize-thread` (in dieser Aufgabe nicht eingebaut).
- LeakSanitizer **nicht** versuchen einzuschalten (`detect_leaks=1` nicht
  setzen) — auf macOS ARM64 nicht supported, bricht beim Start ab.
- Kein neues README oder eigenes `SANITIZE.md` schreiben — die `make help`
  Zeile + ein CLAUDE.md-Hinweis reichen.
- Nicht den Plugin-MODULE-Target mit-instrumentieren, auch nicht
  "optional über ein zweites Flag". Dafür gibt's Instruments.app.
