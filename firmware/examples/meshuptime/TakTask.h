#pragma once

#include <Arduino.h>
#include <FS.h>

/* TAK: posities uit de mesh op de kaart van ATAK/WinTAK/iTAK (v2.25.0).
 *
 * WAT HET DOET. Elke positie die deze node leert -- een advert met lat/lon van
 * een andere node, of een #LOC van een eigen companion (T1000-E) -- gaat als
 * Cursor-on-Target (CoT, XML) over één blijvende TCP-verbinding naar een
 * TAK-server, bv. OpenTAKServer op poort 8088. Die zet ze door naar elke
 * ATAK-client. Er gaat niets over de radio: dit is een uitgang, geen zender.
 *
 * WAAROM OP DE NODE EN NIET OP EEN HOST. De node HOORT de posities al; een brug
 * op een pc zou via COM4 of TCP 5000 moeten meeluisteren. COM4 openen reset de
 * node en poort 5000 laat maar één client toe. Zo kost het niets.
 *
 * DE VIER REGELS
 *
 * 1. NOOIT WACHTEN. Alles loopt over een lwIP-socket met O_NONBLOCK, net als
 *    PushTask. Een TAK-server die hapert mag de radio geen milliseconde kosten;
 *    loopt de ring vol, dan valt de oudste positie en dat wordt geteld.
 *
 * 2. EEN NODE IS ÉÉN PLAATS IN DE RING. Hoort de node dezelfde afzender twee
 *    keer voordat hij kon verzenden, dan telt alleen de nieuwste positie. Een
 *    oude positie doorsturen na een nieuwere zou het spoor terug doen springen.
 *
 * 3. ALLES WAT BINNENKOMT GAAT WEG. Een TAK-server stuurt elk CoT-bericht naar
 *    ELKE client, dus ook naar ons. Dat lezen we elke ronde weg (begrensd);
 *    anders loopt ons ontvangstvenster vol en gooit de server ons eruit.
 *
 * 4. GEEN TIJD, GEEN BERICHT. CoT draagt een absolute tijd en een vervaltijd.
 *    Zonder NTP-sync (klok onder TIME_FLOOR) sturen we niets: een positie met
 *    tijd 1970 is al vervallen voor ze aankomt.
 *
 * ADRES. Alleen een IPv4-adres, geen hostnaam: dit is voor een server op het
 * eigen LAN, en zonder DNS hoeft er ook niets opgezocht te worden.
 */

#define TAK_CFG_PATH        "/tak.cfg"
#define TAK_PORT_DEFAULT    8088
#define TAK_STALE_DEFAULT   30      /* minuten tot een positie grijs wordt */
#define TAK_STALE_MIN       2
#define TAK_STALE_MAX       1440

/* Soort bron, voor het CoT-type en de tekst in de opmerking. */
enum TakSrc : uint8_t {
  TAK_SRC_ADVERT    = 0,   /* advert van een andere node, met lat/lon      */
  TAK_SRC_COMPANION = 1,   /* #LOC van een eigen companion (T1000-E)       */
  TAK_SRC_TEST      = 2,   /* 'tak test' -- om de keten na te lopen        */
};

void tak_begin(fs::FS& fs);
void tak_loop();

/* Een positie aanbieden. Kopieert alleen; verzenden gebeurt in tak_loop().
 * Veilig als TAK uit staat -- dan valt ze meteen weg.
 *   pub       : minstens 6 bytes publieke sleutel (voor de uid)
 *   adv_type  : ADV_TYPE_* uit de advert (0 als onbekend)
 *   batt      : batterij in %, -1 = onbekend
 *   snr4      : SNR x4 zoals de mesh het bijhoudt, of INT16_MIN
 *   hops      : aantal hops, of -1
 *   sos       : companion meldt een SOS (of een val) -- dan gaat er ook een
 *               noodmelding mee, zodat ATAK het niet alleen als stip toont
 *   ts        : UTC-epoch waarop de positie gehoord werd */
void tak_queue_pos(const uint8_t* pub, const char* name, TakSrc src, uint8_t adv_type,
                   double lat, double lon, int batt, int snr4, int hops,
                   bool sos, uint32_t ts);

bool tak_enabled();

/* De CLI: 'tak', 'tak on|off', 'tak host <ip>[:poort]', 'tak port <n>',
 * 'tak adverts on|off', 'tak stale <min>', 'tak test <lat> <lon>'.
 * false = niet voor ons, zodat de aanroeper kan doorvallen. */
bool tak_handle_command(const char* cmd, char* reply, size_t reply_max, uint32_t now);
