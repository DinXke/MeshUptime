# Beveiligingsaudit — MeshUptime, MeshStats-server en repeaterfirmware

Datum: 2026-08-20
Reikwijdte (read-only audit, geen broncode gewijzigd):

- `MeshStats/server/app` — FastAPI + SQLite, publiek op meshmanager.net (zwaarst gewogen)
- `MeshUptime/firmware/examples/meshuptime` — ESP32-nodefirmware
- `MeshStats/firmware/examples/simple_repeater` — repeaterfirmware (`MeshManagerNet.cpp`)

> LET OP: beide firmwarebomen werden tijdens de audit door andere agents bewerkt
> (onbevestigde wijzigingen in de werkboom). De bevindingen hieronder gaan over
> code die op het moment van lezen aanwezig en compleet was. De serverkant is
> stabiel.

---

## English summary

The public FastAPI server is in good shape. Every state-changing `/admin` route
passes through both `require_perm` (authorisation, audited) and `check_csrf`;
there is even a test that enforces this and an explicit allow-list of the few
exempt routes. All SQL is parameterised — the only f-string SQL uses column and
table names drawn from internal constants or a whitelist (`search.FIELDS`),
never user input. Sessions are HMAC-signed with a password-stamp revocation,
cookies are `HttpOnly`/`SameSite=Lax`/`Secure`, login is rate-limited on two
buckets, bodies are capped by streaming middleware, and a solid CSP and security
headers are set.

The one server issue that deserves action is **SSRF combined with fleet-wide
credential exfiltration** (H1): the `ota_host`/`sensor_host` fields are only
validated for URL *scheme*, not for destination, and the server then connects to
that host **carrying the shared `FW_NODE_USER`/`FW_NODE_PASS` Basic-auth
credentials**. A user who holds the delegatable, node-scoped
`node.beheeradres` permission on a single node can point it at a host they
control and capture the credential pair that writes firmware and config to
*every* node.

On the firmware side the two build-flag default secrets are the headline:
the mesh admin password defaults to `"password"` (H2) and the node web UI uses
HTTP Basic over plaintext HTTP with default `admin`/`meshcore` (M1). Both are
documented in the code, but the consequences are fleet-level. The buffer classes
that were already hardened on your side (enlarged `temp[262]`/`g_reply[320]`,
the `sensor set` length guard, the `409` radio boundary) hold up, and I found no
*new* unbounded copy on an externally-reachable path.

---

## Bevindingen per ernst

| Ernst | Aantal |
|-------|--------|
| Kritiek | 0 |
| Hoog | 2 |
| Middel | 2 |
| Laag | 3 |

---

## HOOG

### H1 — SSRF + lekken van de vloot-brede firmwaresleutel via `ota_host`/`sensor_host` (server)

**Bestanden en regels**
- `MeshStats/server/app/firmware.py:353` — `_url()` (enige hostvalidatie)
- `firmware.py:301` — `_auth_header()` hangt `Authorization: Basic <FW_NODE_USER:FW_NODE_PASS>` aan
- `firmware.py:313` — `probe()`; `nodeconfig.py:534` — `_open()`; `firmware.py:329/387` — `push()/download`
- Instelpunten: `routes_admin.py:1773` `save_ota`, `routes_admin.py:1356` `save_sensor_host`
- Triggers: `routes_admin.py:1796` `probe_node` (`node.uitvragen`), `sensor_poll` (1388), `start_upgrade` (1820)
- Rechtdefinitie: `rbac.py:157` `node.beheeradres` = klasse *merkbaar*, per node te delegeren

**Wat er mis is.** `_url()` is bewust toegeeflijk over de vorm en streng over het
schema (alleen `http`/`https`), maar controleert het *doel* niet. Er is nergens
een private-IP-/allowlist-filter (geen `ipaddress.is_private`, geen hostwhitelist).
Het adres wordt door een mens getypt achter een login, maar zodra de server
verbinding maakt met dat adres, stuurt hij daar **de Basic-auth-inloggegevens
`FW_NODE_USER`/`FW_NODE_PASS` in mee** (`_auth_header()` wordt onvoorwaardelijk
aangehangen in `probe`, `_open` en `push`). Die inloggegevens zijn één gedeeld
paar voor de hele vloot: ermee kun je op elke node `/api/fw` (firmware schrijven)
en `/api/cfg` (instellingen) aanspreken.

**Concreet scenario.** Een gebruiker die enkel `node.beheeradres` op één node
gekregen heeft (een *merkbaar*-recht, uitdrukkelijk bedoeld om te delegeren —
geen serverbeheerder):
1. zet `sensor_host` (of `ota_host`) van díé node op `http://mijn-server.example:80`;
2. laat `sensor_poll`/`probe_node` uitvoeren (of wacht tot de achtergrondlus of
   een collega dat doet);
3. de server verbindt met `mijn-server.example` en stuurt
   `Authorization: Basic base64(admin:<FW_NODE_PASS>)` mee;
4. de aanvaller leest de header en bezit nu de sleutel waarmee firmware en
   configuratie naar **alle** nodes geschreven wordt.

Daarnaast is het klassieke SSRF-gevolg aanwezig: het adres mag ook
`http://10.10.30.1/…`, `http://127.0.0.1:<poort>/…` of een ander intern doel zijn.
De server draait op een LXC in een intern netwerk; foutteksten en respons-timing
laten interne poort-/hostverkenning toe. (AWS-metadata `169.254.169.254` is op
deze LXC waarschijnlijk niet relevant, maar het interne netwerk wel.)

**Zekerheid.** Bewezen ontwerp-eigenschap (geen doelvalidatie, credentials altijd
meegestuurd). De exploiteerbaarheid hangt af van of `node.beheeradres` ooit aan
een niet-serverbeheerder gegeven wordt; het rechtenmodel is er expliciet op
gebouwd dat dat mag, dus dit is een echte grens die overschreden wordt.

**Voorgestelde reparatie.**
1. In `_url()` (of een nieuwe `_resolve_and_check()`): weiger hostnamen/IP's die
   naar een niet-toegestaan bereik wijzen. Los de hostnaam op en controleer élk
   opgelost adres met `ipaddress.ip_address(...).is_private/​is_loopback/​is_link_local/​is_reserved`;
   sta alleen het bereik toe waar de nodes echt in zitten (bv. de LAN-prefix), of
   werk met een expliciete allowlist van toegestane node-adressen. Doe de check ná
   DNS-resolutie en gebruik dat opgeloste adres voor de verbinding, om
   DNS-rebinding uit te sluiten.
2. Stuur `_auth_header()` niet naar een willekeurig adres: verbind eerst en stuur
   de credentials pas na bevestiging dat het doel binnen het toegestane bereik
   valt (met punt 1 is dat gedekt). Overweeg per-node-credentials in plaats van
   één vlootsleutel, zodat één gelekt paar niet de hele vloot opent.

---

### H2 — Mesh-beheerderswachtwoord staat standaard op `"password"` (nodefirmware)

**Bestanden en regels**
- `MeshUptime/firmware/examples/meshuptime/SensorMesh.cpp:31` — `#define ADMIN_PASSWORD "password"` (indien niet via bouwvlag gezet)
- `SensorMesh.cpp:951` — `StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, …)` bij initialisatie
- `SensorMesh.cpp:417` — `handleLoginReq`: `strcmp((char*)data, _prefs.password)` bepaalt beheerderstoegang

**Wat er mis is.** Als `ADMIN_PASSWORD` niet bij het bouwen wordt overschreven,
staat het mesh-beheerderswachtwoord op `"password"`. `handleLoginReq` kent bij een
kloppend wachtwoord `PERM_ACL_ADMIN` toe (`SensorMesh.cpp:435`), en daarmee is de
volledige CLI over het mesh bereikbaar (`onPeerDataRecv` → `handleCommand`,
`SensorMesh.cpp:782/819`) inclusief `setperm`, radio-instellingen enz.

**Concreet scenario.** Het mesh is publiek. Iedere node die de publieke sleutel
van deze node kent, stuurt een `ANON_REQ` met wachtwoord `"password"`; bij een
niet-gewijzigde build wordt hij admin en kan hij de node herconfigureren of van de
lucht halen. De webpagina waarschuwt zelf al voor deze stand
(`WebTask.cpp:3398` `pw_def`), wat bevestigt dat het een bekende toestand is.

**Zekerheid.** Bewezen: het is een gebakken standaard. Het gevolg is hoog; het
intreden ervan hangt af van de operator (of de bouwvlag gezet is en of het
wachtwoord daarna gewijzigd is).

**Voorgestelde reparatie.** Laat de firmware bij een niet-overschreven of nog op
de standaard staand wachtwoord de mesh-CLI-loginweg weigeren (of dwing eenmalig
wijzigen af) in plaats van hem te aanvaarden. Minstens: bouw nooit een
distributie-image met de standaardwaarde, en laat `handleLoginReq` de
strcmp-vergelijking constant-tijd doen (klein, maar gratis mee te nemen).

---

## MIDDEL

### M1 — Node-webinterface: HTTP Basic over onversleuteld HTTP, standaard `admin`/`meshcore` (nodefirmware)

**Bestanden en regels**
- `WebTask.cpp:47-51` — `#define WEB_USER "admin"`, `#define WEB_PASS "meshcore"` (indien niet via bouwvlag gezet)
- `WebTask.cpp:2276` — `requireAuth()` → `_server->authenticate(WEB_USER, WEB_PASS)`
- `WebTask.cpp:36` — de code documenteert zelf dat dit Basic zonder TLS is

**Wat er mis is.** De webinterface (met o.a. `POST /cli`, dat CLI-opdrachten naar
de node stuurt) beveiligt zich met HTTP Basic over gewoon HTTP. De inloggegevens
gaan als base64 (dus leesbaar) over de draad, en staan standaard op
`admin`/`meshcore`. Wie op het LAN meeleest, heeft het wachtwoord en daarmee de
volledige node-CLI.

**Concreet scenario.** De node hangt aan wifi; iemand op hetzelfde netwerk (of een
gecompromitteerd apparaat daarop) leest de Basic-header af het eerste verzoek en
kan daarna zelf `POST /cli` doen — inclusief instellingen wijzigen. `requireAuth()`
zit consequent op elke route (goed), maar de vertrouwelijkheid van het wachtwoord
zelf is er niet.

**Zekerheid.** Bekend en gedocumenteerd; gevolg is beperkt tot wie al op het LAN
zit. Reëel omdat het wachtwoord ook nog een gebakken standaard heeft.

**Voorgestelde reparatie.** Documenteer dat de bouwvlaggen `WEB_USER`/`WEB_PASS`
gezet *moeten* worden; overweeg de node te weigeren te starten met de
standaardwaarde. Structureel: de node op een vertrouwd segment houden en de
webinterface niet op onvertrouwde netwerken blootstellen (TLS op een ESP32 is
zwaar; segmentatie is de praktische weg). `POST /cli` weigert al de gevaarlijkste
opdrachten (`prv.key`, `start ota`, `poweroff`) — dat is goed en moet zo blijven.

### M2 — Geen ratelimiet op de mesh-CLI-antwoordweg als zendtijdversterker (nodefirmware)

**Bestand en regel**
- `SensorMesh.cpp:782-833` — `onPeerDataRecv`, `TXT_TYPE_CLI_DATA`-tak

**Wat er mis is.** Elke CLI-opdracht van een admin over het mesh levert een
antwoordpakket op via `sendFlood`/`sendDirect`. Er is een replay-bescherming
(`sender_timestamp > from->last_timestamp`), maar geen snelheidsbegrenzing op de
hoeveelheid antwoorden. De DM-weg (`DmCommands.cpp:494` `DM_DENY_COOLDOWN_MS`)
heeft die cooldown wél voor geweigerde afzenders; de admin-CLI-weg niet.

**Concreet scenario.** Een gecompromitteerde of kwaadwillende admin-node (staat al
in de ACL) stuurt in hoog tempo CLI-opdrachten; de node antwoordt op elk, wat op
een gedeelde LoRa-band zendtijd opsoupeert. Beperkt: vereist reeds
admin-lidmaatschap.

**Zekerheid.** Vermoeden op basis van code-lezen; het gevolg (zendtijd) is reëel
maar de dader moet al admin zijn. Nakijken of een hogere laag dit al begrenst.

**Voorgestelde reparatie.** Een minimale interval tussen twee CLI-antwoorden per
afzender, in dezelfde geest als `MQTT_CMD_MIN_GAP_MS` in de repeater
(`MeshManagerNet.cpp:2711`).

---

## LAAG

### L1 — `GET /admin/logout` verandert toestand zonder CSRF (server)

`routes_admin.py:224` — `logout` is een `GET` die de sessiekoek wist. Een externe
pagina kan `<img src=".../admin/logout">` gebruiken om een beheerder uit te
loggen. Onschuldig (geen dataverlies), maar het is een toestandswijziging via GET,
tegen de eigen regel elders in de code. Reparatie: naar `POST` met `check_csrf`,
of accepteren als bewuste uitzondering en in `ROUTES_ZONDER_RECHTENCONTROLE`-stijl
documenteren.

### L2 — `handleRequest` leest `payload[0..9]` zonder `payload_len`-controle (nodefirmware)

`SensorMesh.cpp:245-247` (`REQ_TYPE_GET_AVG_MIN_MAX`) en `279-280`
(`REQ_TYPE_GET_ACCESS_LIST`) lezen vaste offsets uit `payload` zonder te toetsen
dat `payload_len` groot genoeg is. `payload` wijst in `data[5..]`, een buffer van
`MAX_PACKET_PAYLOAD`, dus de lees valt binnen de fysieke buffer (hoogstens stale/
nul-bytes) — geen geheugenfout, wel slordig. De aanroeper is bovendien een
ACL-lid. In de `PAYLOAD_TYPE_REQ`-tak (`SensorMesh.cpp:758`) wordt `len-5`
doorgegeven terwijl er geen `len >= 5`-controle vlak ervoor staat; bij `len < 5`
wordt `len-5` als `size_t` een enorm getal — momenteel ongebruikt in
`handleRequest`, maar een valstrik voor later. Reparatie: controleer `payload_len`
per req-type vóór de offset-lezingen, en toets `len >= 5` in de REQ-tak.

### L3 — Niet-constante-tijd wachtwoordvergelijking op de mesh-login (nodefirmware)

`SensorMesh.cpp:417` gebruikt `strcmp` voor het beheerderswachtwoord. Over LoRa is
een timing-aanval praktisch irrelevant (hoge latentie, ruis), dus laag. Meenemen
bij de reparatie van H2 met een constante-tijd-vergelijking.

---

## Wat GOED zit (niet slopen bij reparaties)

**Server**
- **SQL volledig geparametriseerd.** De enige f-string-SQL gebruikt kolom-/
  tabelnamen uit interne constanten (`COLUMN_RENAMES`, `COLUMN_MIGRATIONS`,
  `_VISIBILITY_COLUMNS`) of uit een whitelist (`search.FIELDS`/`SORTS`,
  `db.py:1379/1423`, `routes_admin.py:1544`), nooit uit gebruikersinvoer.
- **Autorisatie + CSRF op élke schrijvende `/admin`-route.** `require_perm`
  (audited) én `check_csrf` staan op alle POST-handlers; `test_rechten.py` dwingt
  dit af tegen een expliciete uitzonderingslijst (`ROUTES_ZONDER_RECHTENCONTROLE`,
  `routes_admin.py`). De risicogestuurde poort (`node.instelling.gewoon/merkbaar/
  ingrijpend`) neemt de zwaarste klasse als veilige aanname bij een onbekende
  parameter.
- **Sessies en cookies.** HMAC-ondertekend met `password_stamp`-intrekking
  (`auth.py`), `HttpOnly`+`SameSite=Lax`+`Secure` (via `_secure`, dat
  `x-forwarded-proto` respecteert), legacy-koeken worden bij login gewist.
  Constante-tijdvergelijkingen via `auth.eq`, dummy-hash tegen user-enumeratie.
- **Ratelimiet** op login met twee emmers (IP + gebruiker), plafond op het aantal
  bijgehouden sleutels (`ratelimit.py`), en `client_ip` telt X-Forwarded-For van
  rechts met `TRUSTED_PROXY_HOPS`.
- **Resource-grenzen.** `BodySizeLimitMiddleware` (streaming, dekt ook chunked),
  `MAX_BODY_BYTES`, query-`LIMIT`-plafonds, retentie met rij- én byteplafond.
- **Headers/CSP.** `X-Content-Type-Options`, `X-Frame-Options: DENY`,
  `Referrer-Policy`, strak CSP met `frame-ancestors 'none'`, `object-src 'none'`.
- **Geheimen uit het audittrail** gehouden (`ota_host`/`sensor_host` bewust niet
  gelogd, `routes_admin.py:1385/1791`), `NO_REMOTE` blokkeert radio-parameters op
  afstand (`nodeconfig.py:209`).

**Firmware**
- **Vergrote buffers houden stand.** `temp[262]` met reply op offset 5
  (`SensorMesh.cpp:817`) en `g_reply[320]` (`WebTask.cpp:184`) vangen de upstream
  `sensor list`-sprintf ruim op; `composeMsgPacket` weigert bovendien te lange
  antwoorden, dus het ergste is een uitgebleven antwoord.
- **De `sensor set`-lengtegrens** (`WebTask.cpp:3564`, max 59 tekens achter
  `sensor set `) dekt de ongebonden `strcpy(tmp, &command[11])` in CommonCLI's
  68-byte-buffer af — de bekende reparatie zit erin.
- **De 409-radiogrens.** `POST /cli` weigert `set radio`/`set freq` zonder
  `confirm=radio` (`WebTask.cpp:3549`) en de browser vraagt twee keer; typen is
  geen sluipweg. `NO_REMOTE` doet hetzelfde aan de serverkant.
- **Invoerkeuring.** `validName`/`validHost` (`MonitorSensors.cpp:1700/1718`)
  beperken tekenset én lengte; interval-cast is bewaakt (`addMonitor`,
  `MonitorSensors.cpp:1919`), dus de oude `uint16_t`-wrap bij 86400 kan hier niet
  meer optreden.
- **De MQTT-opdrachtweg in de repeater is netjes begrensd.** `mqttOnMessage`
  weigert `len >= MQTT_CMD_MAX` (`MeshManagerNet.cpp:2485`); `mqttRunCommand`
  splitst en valideert het woord en de parameter met eigen lengtegrenzen; er is
  een `MQTT_CMD_MIN_GAP_MS`-cooldown.
- **De onbekende-pakketweg is defensief.** `meshmanager_on_raw_packet`
  (`MeshManagerNet.cpp:3060`) en `meshmanager_on_monitor_response` (3704) klemmen
  `len` tegen `MQTT_RX_MAX_LEN`/`sizeof(_mon_reply)` vóór `memcpy`; de
  monitor-antwoordparsers (`monSettingsReply`/`monWriteReply`/`monClockReply`,
  status/nbr-kopieën op 5390/5427) klemmen `n` telkens tegen de doelbuffer;
  `PacketFilter::pf_allow` (`PacketFilter.cpp:391`) toetst `payload_type` en
  `payload_len >= 1` vóór het lezen van `payload[0]`.
- **DM-weg.** `DmCommands::handleDm` (`DmCommands.cpp:460`) kopieert naar een vaste
  `cmd[64]` met eigen nulafsluiting, `appendText` en `startReply` bewaken hun
  grenzen (`out_path_len <= MAX_PATH_SIZE`), en er is een deny-cooldown.

**Configuratie**
- De `.gitignore` van MeshUptime houdt inloggegevens uit de repo; op de server
  staat het secret in `data/secret.key` met `chmod 600` (`config.py:get_secret`).
