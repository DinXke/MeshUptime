#pragma once

#include <Arduino.h>
#include <FS.h>

#include "RepeaterCli.h"   /* RCLI_PASS_MAX, RCLI_JOB_BUF */

class PushTask;

/* ============================================================================
 * Poller -- MeshUptime als POLLER van de MeshManager-opdrachtwachtrij (v2.6.0).
 *
 * WAT DIT DOET, EN WAAROM HET HOME ASSISTANT UIT DE KETEN HAALT
 *
 * MeshManager kon al opdrachten in een wachtrij zetten voor "een repeater die
 * alleen iets anders kan bereiken" (db.request_settings / request_refresh). Tot
 * nu toe leegde HOME ASSISTANT die wachtrij: het polde /api/v1/commands, sprak de
 * repeater aan via een companion-node en pushte de antwoorden terug. Drie
 * schakels (HA, de integratie, de companion) tussen twee dozen die op hetzelfde
 * dak staan.
 *
 * Deze module doet exact dat werk, maar dan op de node zelf:
 *
 *   1. elke poll_secs een GET {push.url}/api/v1/commands (Bearer = het
 *      sensorpush-token; de server aanvaardt dat token sinds kort ook op deze
 *      route -- serverkant, niet onze zorg);
 *   2. voor elk `settings`-verzoek: EEN sessie via RepeaterCli (eenmaal inloggen,
 *      dan de N commando's), met de param->commando-vertaling van de oude
 *      HA-pusher ("cmd:X" -> X letterlijk, anders P -> "get P");
 *   3. elk antwoord (of het uitblijven ervan) terug via PushTask naar
 *      POST /api/v1/repeater_settings -- null bij "geen antwoord".
 *
 * ------------------------------------------------------------------------
 * CLEAR-ON-READ: WAT JE OPHAALT IS WEG
 *
 * /api/v1/commands is clear-on-read (db.pop_settings_requests wist de wachtrij op
 * het moment van uitreiken). Wat de poll teruggeeft bestaat daarna NERGENS meer.
 * Dus: alles wat binnenkomt moet OFWEL afgehandeld OFWEL expliciet gemeld worden
 * als niet-afgehandeld -- stil laten vallen is precies de fout die dit project
 * bestrijdt. Twee gevolgen in het ontwerp:
 *
 *  - we pollen ALLEEN als de wachtrij hieronder (_pending) LEEG is, zodat er
 *    plaats is om te bewaren wat we ophalen. Anders zouden we clear-on-read
 *    uitvoeren en de opgehaalde verzoeken meteen kwijt zijn;
 *  - komt er tóch meer terug dan _pending draagt (POLLER_PENDING_MAX repeaters),
 *    dan wordt de rest GETELD en gelogd, niet stil vergeten.
 *
 * ------------------------------------------------------------------------
 * VEILIGHEID EN ZENDTIJD -- DE DAK-REPEATER MAG NOOIT ONBEREIKBAAR WORDEN
 *
 *  - EEN sessie tegelijk (RepeaterCli is single-session); een nieuw verzoek
 *    wacht tot de vorige klaar is. De pollerlus blokkeert nooit -- alles gebeurt
 *    stapsgewijs vanuit loop(), naast de bewaking, die voorgaat.
 *  - GEEN automatische herhaling van muterende commando's (RepeaterCli::
 *    isMutating -> een poging). Een herhaling zou het commando OPNIEUW uitvoeren.
 *  - GEVAARLIJKE commando's komen NIET uit de wachtrij de lucht in. clkreboot,
 *    reboot, erase, set radio/freq, poweroff/shutdown, start ota en alles met
 *    prv.key worden GEWEIGERD (null gepusht + gelogd), want op afstand is daar
 *    geen bevestiging voor te geven en een fout maakt een node op een dak
 *    onbereikbaar. In de praktijk stuurt MeshManager alleen leescommando's
 *    (de cli_params-lijst) plus `cmd:filter count`/`cmd:region`; deze zeef is de
 *    gordel voor het geval iemand de lijst uitbreidt.
 *  - refresh-verzoeken (REQ_TYPE_GET_STATUS, een ANDER protocol) worden in deze
 *    versie NIET uitgevoerd: ze worden gelogd als "niet ondersteund" en vallen
 *    weg. Bekende beperking; staat in de docs.
 *
 * ------------------------------------------------------------------------
 * WACHTWOORDEN PER DOEL
 *
 * Om als admin op een repeater in te loggen moet de node diens
 * beheerderswachtwoord kennen. Dat staat in een klein persistent tabelletje
 * (/rep_targets.cfg, cap POLLER_MAX_TARGETS): pubkey-prefix -> wachtwoord, plus
 * EEN standaardwachtwoord als terugval. Beheer via de web-GUI (POST) en
 * /repeater_targets.json (GET). Wachtwoorden komen NOOIT terug in een GET -- de
 * JSON zegt alleen "set: ja/nee", net als de wifi- en web-credential.
 *
 * Onbekend doel zonder wachtwoord (geen match én geen standaard) -> het verzoek
 * wordt NIET uitgevoerd; elke gevraagde parameter gaat als null terug (zodat
 * MeshManager "gevraagd, geen antwoord" ziet) en er komt een logregel.
 * ==========================================================================*/

#define POLLER_CFG_PATH       "/poller.cfg"
#define POLLER_TARGETS_PATH   "/rep_targets.cfg"

#define POLLER_MAX_TARGETS    8
#define POLLER_PENDING_MAX    4       /* repeaters met een wachtend settings-verzoek */

#define POLL_SECS_DEFAULT     30
#define POLL_SECS_MIN         10
#define POLL_SECS_MAX         3600

/* Eerste poll niet meteen bij boot: wifi, tijd-sync en de eerste advert-ronde
 * mogen eerst hun beurt hebben. Vijftien seconden is ruim daarvoor en
 * onmerkbaar voor de gebruiker. */
#define POLLER_FIRST_DELAY_MS 15000UL

/* De pubkey-prefix zoals hij in de wachtrij en in de doeltabel staat: 12 hex
 * (6 byte) is de minimale, MeshManager stuurt precies dat. We laten tot 64 toe
 * zodat iemand de volle sleutel kan opslaan als de node de repeater nooit als
 * advert hoorde. */
#define POLLER_PREFIX_MAX     (64 + 1)

class Poller {
public:
  Poller() { reset(); }

  /* fs: voor de twee configbestanden. push: de HTTP-weg (poll-GET + antwoord-
   * POST). rcli: de mesh-weg (login + commando's). Alle drie leven even lang als
   * de node; Poller bewaart alleen pointers. */
  void begin(fs::FS* fs, PushTask* push, RepeaterCli* rcli);

  /* Kort en niet-blokkerend; hoort in loop() naast the_mesh.loop(). */
  void loop();

  /* ---- config (web-GUI) ---- */
  bool     enabled() const   { return _on; }
  uint16_t pollSecs() const  { return _poll_secs; }
  void     setEnabled(bool on);
  void     setPollSecs(uint16_t s);

  /* ---- doel-wachtwoorden (web-GUI) ----
   * setTarget: prefix (12..64 hex) -> wachtwoord (<=15). Leeg wachtwoord verwijdert
   * de ingang. setDefaultPass: het terugval-wachtwoord (leeg wist het). */
  bool setTarget(const char* prefix_hex, const char* password);
  bool delTarget(const char* prefix_hex);
  void setDefaultPass(const char* password);
  int  targetCount() const { return _ntargets; }
  bool defaultPassSet() const { return _default_pass[0] != 0; }
  /* Voor /repeater_targets.json: de prefix en of er een wachtwoord staat -- NOOIT
   * het wachtwoord zelf. */
  bool targetAt(int idx, char* prefix_out, size_t out_len) const;

  /* ---- status (/poller.json + statuspagina) ---- */
  uint32_t    lastPollAgeSecs() const { return _last_poll ? (millis() - _last_poll) / 1000 : 0; }
  bool        everPolled() const  { return _last_poll != 0; }
  uint32_t    processedCount() const { return _processed; }
  uint32_t    droppedCount() const   { return _dropped; }
  uint8_t     pendingCount() const   { return _pending_count; }
  int         lastRefreshDropped() const { return _last_refresh_dropped; }
  const char* lastNote() const    { return _note; }

private:
  fs::FS*      _fs;
  PushTask*    _push;
  RepeaterCli* _rcli;

  bool     _on;
  uint16_t _poll_secs;
  unsigned long _next_poll;
  unsigned long _last_poll;   // millis van de laatste GELUKTE poll (0 = nooit)

  /* Doel-wachtwoorden. Prefix als hex-tekst (zo staat hij in de wachtrij), niet
   * als bytes: de vergelijking is een tekst-prefixmatch en dat is precies wat we
   * willen -- een opgeslagen "e3d3f4d7edd0" moet een gevraagde "e3d3f4d7edd0"
   * dekken. Wachtwoord max 15 tekens (sendLogin kapt daar af). */
  struct Target {
    char prefix[POLLER_PREFIX_MAX];
    char pass[RCLI_PASS_MAX];
  };
  Target  _targets[POLLER_MAX_TARGETS];
  int     _ntargets;
  char    _default_pass[RCLI_PASS_MAX];

  /* Wachtende settings-verzoeken uit de poll (clear-on-read, dus vastgehouden tot
   * verwerkt). Elk: de doel-prefix + de parameterlijst als CSV zoals de server ze
   * gaf. */
  struct Pending {
    char prefix[POLLER_PREFIX_MAX];
    char params[RCLI_JOB_BUF];
  };
  Pending _pending[POLLER_PENDING_MAX];
  uint8_t _pending_head;   // volgende om te verwerken
  uint8_t _pending_count;

  uint32_t _processed;             // verwerkte settings-verzoeken (gestart of geweigerd)
  uint32_t _dropped;               // verzoeken die _pending niet meer in konden
  int      _last_refresh_dropped;  // refresh-verzoeken in de laatste poll (niet ondersteund)
  char     _note[96];              // korte, mensleesbare laatste-actie-regel

  void reset();
  void loadConfig();
  void saveConfig();
  void loadTargets();
  void saveTargets();

  /* De prefix -> het wachtwoord: eerst een exacte/prefix-match in de tabel, anders
   * het standaardwachtwoord, anders nullptr (onbekend doel). */
  const char* passwordFor(const char* prefix_hex) const;

  /* De poll-callback (statische thunk -> deze instance). Ontleedt het body. */
  static void pollThunk(void* ctx, const char* body);
  void onPollBody(const char* body);

  /* Een settings-verzoek in _pending zetten (clear-on-read: mag niet verloren
   * gaan). false = geen plaats meer (geteld in _dropped). */
  bool pushPending(const char* prefix, const char* params_csv);

  /* Het kop-verzoek uit _pending proberen te starten. Doet niets als RepeaterCli
   * bezig is. Weigert (null + log) bij onbekend wachtwoord of gevaarlijke params. */
  void startNextPending();

  /* Mag deze parameter (na param->commando-vertaling) uit de WACHTRIJ de lucht in?
   * false voor de gevaarlijke set (clkreboot/reboot/erase/set radio/...). */
  static bool paramAllowedFromQueue(const char* param);
};
