# Bouwen

MeshUptime is een **zelfstandig PlatformIO-project**. MeshCore is geen boom waar
je onze bestanden in kopieert, maar een **gepinde afhankelijkheid**: een
git-submodule onder `firmware/vendor/MeshCore`, vastgezet op tag
`companion-v1.17.0`.

Er zijn twee wegen. De eerste is de voorkeur; de tweede blijft tijdens de
overgang bestaan zodat het flashen niet breekt.

---

## Weg 1 — het zelfstandige project (voorkeur)

Eén bron: deze repo. Geen kopieerstappen, geen twee-bomen-sync.

```sh
git clone https://github.com/DinXke/MeshUptime.git
cd MeshUptime
git submodule update --init firmware/vendor/MeshCore
cd firmware
python -m platformio run -e meshuptime
```

Flashen (schrijft alleen de programmapartitie, niet SPIFFS — identiteit en
instellingen blijven staan):

```sh
python -m platformio run -e meshuptime -t upload --upload-port COM4
```

Wat er onder de motorkap gebeurt:

- **`firmware/platformio.ini`** is het projectbestand. De `[platformio]`-sectie
  wijst PlatformIO's mappen (`src_dir`, `boards_dir`, `lib_dir`, `include_dir`)
  naar de submodule en laadt via `extra_configs` MeshCore's eigen env-definities
  plus de heltec_v3-variant. Onze env `meshuptime` erft van `Heltec_lora32_v3`,
  precies zoals in de oude bouwkopie.
- De **variants-patch** (`firmware/patches/0001-...patch`) wordt door de
  pre-build hook **`firmware/apply_patches.py`** idempotent op de submodule
  aangebracht. De gevendorde MeshCore-boom blijft in de repo dus onaangeraakt op
  de gepinde commit; de patch leeft pas in de build. Draai je twee keer, dan
  gebeurt er de tweede keer niets (`al aangebracht, overgeslagen`).
- Een paar vlaggen zijn t.o.v. de oude env aangepast omdat `src_dir` nu naar de
  submodule wijst en enkele MeshCore-paden project-relatief zijn: de include
  `-I vendor/MeshCore/variants/heltec_v3`, de `build_src_filter` naar
  `+<../../../examples/meshuptime>`, en de OTA-lib met een expliciet
  `file://`-pad. Dit is puur build-structuur; het **gedrag** van de firmware is
  ongewijzigd. Zie de commentaren in `firmware/platformio.ini`.

### WiFi-gegevens

Staan **niet** in de repo. `firmware/platformio.ini` draagt plaatshouders; de op
het toestel opgeslagen wifi-instelling is toch de baas (zie
`firmware/wifi.ini.voorbeeld`). Wil je bij het bouwen echte waarden meegeven,
zet ze dan in een lokale, niet-gecommitte `firmware/platformio.local.ini` (die
match `*.local.ini` in `.gitignore`). Zet **nooit** echte gegevens in
`platformio.ini` of `platformio.ci.ini`.

### Waarom submodule en niet `lib_deps`

MeshCore's build leunt volledig op zijn eigen `platformio.ini`: de env-boom
`arduino_base → esp32_base → sensor_base → Heltec_lora32_v3`, de `variants/` en de
`boards/`. Als PlatformIO-library (via `library.json` + `build_as_lib.py`) levert
MeshCore alleen bronbestanden — niets van die env-, variant- of boardmachinerie.
Onze env erft rechtstreeks van `Heltec_lora32_v3`; dat kan alleen als MeshCore's
config in het spel is. `lib_deps` zou ons dwingen die hele machinerie zelf na te
bouwen: precies de drift die we kwijt willen. Een submodule houdt de complete
boom op de gepinde commit in de repo. Empirisch getest: de submodule-weg bouwt,
de lib_deps-weg zou een grote handmatige reconstructie vergen.

### De patch: hook, niet handmatig editen

De patch vervangt drie regels IN `variants/heltec_v3/target.{h,cpp}` (de
hardgecodeerde `EnvironmentSensorManager sensors`). Die bestanden compileren mee
via MeshCore's eigen `build_src_filter`; ze zonder de upstream-bestanden uit te
sluiten overschrijven kan niet, en uitsluiten-plus-dupliceren zou net de drift
terugbrengen. Daarom brengt de hook de kleine patch bij de build aan i.p.v. dat
we onze eigen kopie van target.cpp meeslepen. De prijs: na een build staat de
submodule "dirty" in `git status` (de patch is aangebracht). Dat is bewust; een
`git -C firmware/vendor/MeshCore checkout -- .` zet hem terug.

---

## Weg 2 — de losse bouwkopie (legacy, tijdens de overgang)

De oude manier: een aparte MeshCore-checkout (bv. `C:\Users\Public\MeshUptime-build`)
op de tag, met de firmware erin en een handgeschreven `platformio.local.ini`.
Deze weg blijft werken zolang de flash-workflow er nog op leunt. Kort:

1. Een MeshCore-checkout op `companion-v1.17.0`.
2. `git apply` van `firmware/patches/0001-...patch` op die checkout.
3. `firmware/examples/meshuptime/*` in `examples/meshuptime/` van de checkout.
4. Een `platformio.local.ini` (met je echte wifi) — MeshCore leest die via
   `extra_configs`. `firmware/platformio.ci.ini` is de plaatshouder-spiegel
   daarvan.
5. `pio run -e meshuptime` in de checkout.

Nadeel — en de reden dat weg 1 de voorkeur heeft: dit is de twee-bomen-sync die
gebroken builds gaf toen meerdere mensen/agents tegelijk werkten. Zodra het
flashen op weg 1 draait, kan deze kopie (en `platformio.ci.ini`) weg.

---

## CI

`.github/workflows/build.yml` bouwt **weg 1** tegen meerdere upstream-tags:

- **`companion-v1.17.0`** (gepind) — MOET groen zijn.
- **de laatste upstream companion-tag** (dynamisch opgezocht) — informatief,
  `continue-on-error`. Faalt die, dan heeft een MeshCore-release iets veranderd:
  zichtbaar als waarschuwing en in de samenvatting, zonder de required build rood
  te maken. Zo wordt een upgrade een vink vooraf in plaats van een verrassing.

Laatst bekende cijfers op een verse checkout (moeten hiermee overeenkomen; wijken
ze af, dan is er iets niet meegekomen): **RAM 32,0 %** (104.904 van 327.680 bytes),
**flash 41,4 %** (1.385.037 van 3.342.336 bytes).
