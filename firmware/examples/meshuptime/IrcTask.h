#pragma once

#include <Arduino.h>
#if defined(ESP32)
  #include <WiFi.h>
#endif

#include "RoomMesh.h"

/* ============================================================================
 * IrcTask -- een echte IRC-server OP de node, met de bot-slots als identiteit.
 *
 * WAT DIT IS
 *
 * Een klassieke IRC-client (HexChat, irssi, mIRC, WeeChat, Textual) verbindt met
 * deze node op poort 6667 en praat daarna gewoon op het mesh. De vertaling:
 *
 *    IRC                          MeshCore
 *    ---------------------------  --------------------------------------------
 *    account (PASS bij inloggen)  een VAST bot-slot -> eigen sleutelpaar
 *    #kanaal zonder sleutel       hashtag-/publiek group-channel (naam-afgeleid)
 *    #kanaal met MODE +k          group-channel met een expliciet 16/32-byte geheim
 *    PRIVMSG #kanaal              group-datagram, geflood
 *    PRIVMSG <nick>               DM (ECDH vanaf het bot-sleutelpaar)
 *    WHOIS <nick>                 pubkey, SNR/RSSI en hopcount van de laatste hoor
 *
 * WAAROM DE BOT-SLOTS EN NIET DE ROOMS
 *
 * RoomMesh draagt drie soorten identiteit op een radio: rooms (server-rol,
 * anderen loggen IN en posten), snodes (telemetriecontacten) en bots (CHAT-
 * identiteiten met een eigen sleutelpaar dat DM's KAN INITIEREN en kanalen
 * meeleest). Een IRC-gebruiker is een chat-deelnemer, geen server -- dus een bot.
 * De machinerie bestond al (botSendTo, de kanaaltabel, de per-bot advertenties);
 * deze klasse is de IRC-kant ervoor, niet een tweede identiteitsstelsel.
 *
 * VASTE TOEWIJZING, GEEN POOL. Elk account heeft PERMANENT hetzelfde bot-slot, ook
 * als niemand ingelogd is. Dat is een bewuste keuze en geen implementatiegemak: de
 * pubkey van een IRC-gebruiker is zijn adres op het mesh. Zou een slot bij logout
 * vrijkomen en later aan iemand anders gaan, dan komen DM's die onderweg waren bij
 * de verkeerde persoon aan, en zouden contacten die je toegevoegd hebben opeens met
 * een ander praten. De identiteit blijft dus staan; de IRC-sessie is er alleen de
 * toetsenbordkant van. Het aantal accounts is daarmee hard begrensd op MAX_BOTS.
 *
 * WAT DIT NIET IS
 *
 * Geen IRC-netwerk: geen server-naar-server-koppeling, geen services (NickServ/
 * ChanServ), geen ban-lijsten, geen channel-operators. Registratie kan NIET over
 * IRC -- accounts maak je met `irc user add` op de CLI (serieel of via het web),
 * omdat een account een bot-slot claimt en dat een beheerbeslissing is.
 *
 * SYNCHROON EN NIET-BLOKKEREND. Zelfde regel als WebTask: geen async-stack. loop()
 * doet per ronde hooguit een accept en een leesronde over de open sockets, en keert
 * altijd terug -- de radio moet bediend blijven.
 *
 * ------------------------------------------------------------------------
 * AIRTIME IS DE ECHTE BEPERKING, NIET HET PROTOCOL
 *
 * Een matig druk IRC-kanaal produceert meer verkeer dan een LoRa-mesh kan dragen.
 * EU868 kent 1% duty cycle en een MeshCore-tekstbericht is maximaal 160 tekens.
 * Zonder rem is deze brug een zendmachine die het eigen netwerk platlegt. Daarom:
 *
 *   - per client minstens IRC_TX_MIN_MS tussen twee mesh-verzendingen;
 *   - een gedeelde emmer (IRC_BUCKET_MAX berichten, hervult per IRC_BUCKET_MS) over
 *     ALLE clients samen, want de radio is er ook maar een;
 *   - tekst wordt op BOT_MAX_TEXT_LEN geknipt, inclusief het "<naam>: "-voorvoegsel;
 *   - JOIN/PART/QUIT/TOPIC/NAMES gaan NOOIT het mesh op. Die zijn puur lokaal.
 *
 * Wat geweigerd wordt komt terug als NOTICE met de wachttijd erbij, niet als stilte:
 * een client die niet weet dat zijn regel weggegooid is, typt hem opnieuw.
 *
 * ------------------------------------------------------------------------
 * BEVEILIGING -- LEES DIT VOOR JE HEM OPENZET
 *
 * IRC is onversleuteld. Het wachtwoord gaat in leesbare vorm over de lijn (PASS) en
 * alles wat je typt ook. Draai dit op een vertrouwd LAN of achter een VPN; zet
 * poort 6667 NOOIT open naar het internet. Er zit met opzet geen TLS in: een
 * ESP32-S3 die naast mesh, WiFi en de webserver ook nog TLS-sessies moet dragen
 * heeft de heap niet, en een halve TLS is erger dan geen.
 *
 * Wachtwoorden staan als sha256(salt || wachtwoord) met een eigen 8-byte salt per
 * account in "/irc_accounts". De PRIVATE SLEUTELS van de bots staan waar ze al
 * stonden ("/bot_id_i") en gaan nooit over de IRC-verbinding -- ook niet op verzoek.
 * Wie de node fysiek of via het web in handen heeft, heeft de sleutels; dat is de
 * grens van wat dit ding kan beloven.
 * ==========================================================================*/

#ifndef IRC_PORT
  #define IRC_PORT  6667
#endif

/* Een client per bot-slot: meer sessies dan identiteiten heeft geen zin, want een
 * account is aan een slot gebonden. Elke client kost ~700 byte aan buffers. */
#ifndef IRC_MAX_CLIENTS
  #define IRC_MAX_CLIENTS  MAX_BOTS
#endif

/* RFC 1459 zegt 512 inclusief CR-LF. Wij lezen niet meer en kappen de rest van een
 * te lange regel weg tot de volgende LF -- niet de verbinding sluiten, want een
 * client die eenmaal te veel plakt hoort daar niet voor van het net te vliegen. */
#define IRC_LINE_MAX      512

#ifndef IRC_NICK_MAX
  #define IRC_NICK_MAX  20
#endif

/* ZENDREM -- EN WAAROM EEN BERICHTENTELLER NIET GENOEG IS.
 *
 * De eerste versie telde berichten: zes per emmer, een erbij per tien seconden.
 * Dat is geen bescherming maar een tempo. Nagerekend met de radio-instellingen van
 * deze node (869,618 MHz, BW 62,5 kHz, SF8, CR4/8) kost een vol bericht ~1,6 s
 * lucht; zes per minuut is dan ~16% zendtijd, terwijl de sub-band 869,4-869,65 MHz
 * op 10% duty cycle staat. Een client die stug elke drie seconden iets stuurt --
 * een sensor, een statusscript, een brug die iemand aanzet en vergeet -- zat dus
 * BOVEN de wettelijke grens, en de repeaters die het floodverkeer herhalen deden er
 * nog een schep bovenop.
 *
 * Daarom is de echte rem nu een LUCHTBEGROTING op de meting van de radio zelf
 * (Dispatcher::getTotalAirTime, dus inclusief adverts, alerts en doorgegeven
 * pakketten). Komt de node boven IRC_DUTY_PCT over het laatste uur, dan mag IRC
 * niet meer zenden -- het chatverkeer wijkt voor het echte werk van de node.
 * IRC_DUTY_PCT staat bewust ver onder de 10%: er moet ruimte overblijven voor de
 * bewaking, en het floodverkeer kost elders op het mesh nog eens hetzelfde.
 *
 * De berichtenteller en de 3 s per client blijven ernaast staan: die begrenzen
 * pieken, de luchtbegroting begrenst het gemiddelde. */
#ifndef IRC_TX_MIN_MS
  #define IRC_TX_MIN_MS  3000
#endif
#ifndef IRC_BUCKET_MAX
  #define IRC_BUCKET_MAX  6
#endif
#ifndef IRC_BUCKET_MS
  #define IRC_BUCKET_MS  10000
#endif
#ifndef IRC_DUTY_PCT
  #define IRC_DUTY_PCT  2          /* procent zendtijd over het venster hieronder */
#endif
#ifndef IRC_DUTY_WINDOW_MS
  #define IRC_DUTY_WINDOW_MS  (60UL * 60UL * 1000UL)   /* 1 uur */
#endif
/* DE AANLOOP TELT NIET MEE. Bij het opstarten zendt de node zijn advert de lucht
 * in, en gemeten is dat ~9 s zendtijd in de eerste twintig seconden. Zou dat in de
 * begroting vallen, dan mag IRC daarna acht minuten niets -- terwijl er niemand
 * gechat heeft. Het nulpunt schuift daarom mee tot de node IRC_DUTY_GRACE_MS
 * draait; pas daarna begint het venster echt te tellen. De piekremmen (3 s per
 * client, de emmer) gelden in die tijd gewoon. */
#ifndef IRC_DUTY_GRACE_MS
  #define IRC_DUTY_GRACE_MS  90000UL
#endif

/* HERHALINGSREM. Precies het geval dat dit moet vangen: een gekoppelde server of
 * sensor die elke zoveel seconden dezelfde regel het kanaal in duwt. Twee keer
 * exact hetzelfde binnen dit venster wordt geweigerd, per gebruiker en per doel. */
#ifndef IRC_REPEAT_MS
  #define IRC_REPEAT_MS  (10UL * 60UL * 1000UL)   /* 10 minuten */
#endif

/* Stille sessie afsluiten. Eerst een PING, dan pas weg -- een client die alleen
 * meeleest hoort niet weggegooid te worden omdat hij niets te zeggen had. */
#define IRC_IDLE_PING_MS   120000UL
#define IRC_IDLE_KILL_MS   180000UL

#define IRC_ACCOUNTS_FILE  "/irc_accounts"
#define IRC_SALT_LEN       8

/* GEEMULEERDE LEDENLIJST (v2.9.0). Een MeshCore-kanaal HEEFT geen ledenlijst: er
 * is geen join, geen aanwezigheid, alleen wie toevallig zendt. IRC-clients tonen
 * een leeg /NAMES dan als een leeg kanaal, en dat leest als "er is hier niemand"
 * terwijl er een heel mesh meeluistert.
 *
 * Daarom onthouden we per kanaal wie we hebben HOREN zenden en presenteren we die
 * als leden: bij de eerste keer een JOIN naar de kijkende clients, na
 * IRC_SEEN_TTL_MS stilte een PART. Wat je ziet is dus "recent gehoord", en dat is
 * iets anders dan lidmaatschap:
 *
 *   - wie meeleest maar nooit zendt, verschijnt NOOIT;
 *   - wie een naam verzint, verschijnt WEL -- een group-pakket draagt geen
 *     handtekening per afzender, dus de naam is onbewezen tekst;
 *   - de PART is een gok op stilte, geen vertrek.
 *
 * De 366-regel bij /NAMES zegt dat er met zoveel woorden bij. */
#ifndef IRC_SEEN_PER_CHAN
  #define IRC_SEEN_PER_CHAN  10
#endif
#ifndef IRC_SEEN_TTL_MS
  #define IRC_SEEN_TTL_MS  (45UL * 60UL * 1000UL)   /* 45 min stilte -> PART */
#endif

struct IrcSeen {
  char          nick[IRC_NICK_MAX + 1];
  unsigned long last;      // millis() van de laatste keer gehoord; 0 = leeg
};

/* TERUGSPOELEN NA HET INLOGGEN (v2.9.0). Een LoRa-mesh heeft geen geschiedenis:
 * wie niet verbonden was, heeft het gemist. Voor een IRC-gebruiker is dat vreemd
 * -- hij logt in en ziet een leeg kanaal terwijl er de hele dag gepraat is.
 *
 * Daarom een GEDEELDE ringbuffer van de laatste berichten, kanaalregels en DM's
 * door elkaar. Je krijgt terug wat er langskwam TERWIJL JE WEG WAS: per account
 * onthouden we de RTC-tijd van het laatste uitloggen, en alles van na dat moment
 * komt bij het inloggen (je DM's) of bij JOIN (dat kanaal) alsnog binnen. Wie
 * nooit eerder inlogde krijgt wat er nog in de ring zit.
 *
 * IRC_LOG_TTL_S is de bovengrens daarop: was je een week weg, dan is het geen
 * gemist gesprek meer maar archief, en daar is deze node de plek niet voor.
 *
 * GEDEELD en niet per client: de inhoud is voor iedereen dezelfde, en per client
 * zou het geheugen met het aantal sessies meegroeien. Puur RAM -- een herstart
 * wist hem, en dat is eerlijk: dit is een radio met een chatserver erop, geen
 * logserver. Kosten: IRC_LOG_MAX x ~190 byte, bij 32 dus ~6 kB voor allemaal. */
#ifndef IRC_LOG_MAX
  #define IRC_LOG_MAX  32
#endif
#ifndef IRC_LOG_TTL_S
  #define IRC_LOG_TTL_S  (12UL * 3600UL)   /* 12 uur */
#endif

#define IRC_LOG_DM  (-1)   /* chan-waarde voor een DM */

/* ANTWOORDEN (v2.9.0) -- en waarom het er op de draad zo simpel uitziet.
 *
 * MeshCore v1.17.0 heeft GEEN antwoordveld. De hele tekstlaag is drie types
 * (TXT_TYPE_PLAIN / _CLI_DATA / _SIGNED_PLAIN, TxtDataHelpers.h) en verder niets:
 * geen bericht-id, geen reply_to, geen thread. Wat wij verzinnen moet dus LEESBARE
 * TEKST zijn, anders ziet iemand met de MeshCore-app op zijn telefoon ruis.
 *
 * Daarom twee lagen, en de onderste is de echte:
 *
 *  - OP DE DRAAD een compacte quote: ">Jan: wat is de.. 869.618". Werkt in elke
 *    client, ook in de app. De ".. " erachter is het scheidingsteken, ALTIJD, ook
 *    als de quote niet afgekapt is -- zo is hij deterministisch terug te vinden.
 *
 *    De NAAM van wie je citeert staat erbij, en dat is geen sier. In een kanaal
 *    draagt elk bericht al "<naam>: " als gewone tekst, dus de lezer ziet wie er
 *    ANTWOORDT -- maar zonder naam in de quote niet aan wie. Vragen twee mensen
 *    iets soortgelijks, dan is het antwoord niet meer thuis te brengen. In een DM
 *    laten we hem weg: daar zijn maar twee partijen en is hij verspilde airtime.
 *  - OP DE IRC-VERBINDING de IRCv3-tags `message-tags` en `server-time`. Wij geven
 *    elk bericht een `msgid`, een antwoord komt binnen met `+draft/reply=<msgid>`,
 *    en een binnenkomende mesh-regel die met '>' begint koppelen we terug aan de
 *    ringbuffer. Dat is echte threading in je client, het degradeert naar leesbare
 *    tekst op het mesh, en het kost GEEN airtime: tags gaan alleen over TCP.
 *
 * Wat we NIET doen: een kort id op de draad zetten ("~a3 tekst"). Compact voor ons,
 * maar in de app leest het als ruis -- en dat is precies de lezer die de context
 * niet heeft. */
#ifndef IRC_QUOTE_LEN
  #define IRC_QUOTE_LEN  16        /* tekens van het origineel in de quote */
#endif
#ifndef IRC_QUOTE_NAME_LEN
  #define IRC_QUOTE_NAME_LEN  8    /* tekens van de naam ervoor (0 = geen naam) */
#endif
#define IRC_QUOTE_SEP  ".. "       /* scheidt de quote van het antwoord */

struct IrcLogEntry {
  uint32_t ts;                      /* RTC-seconden; 0 = leeg slot */
  uint32_t id;                      /* msgid, oplopend; 0 = leeg */
  int8_t   chan;                    /* BotChannel-index, of IRC_LOG_DM */
  int8_t   bot;                     /* bij een DM: voor welk bot-slot */
  char     nick[IRC_NICK_MAX + 1];
  char     text[BOT_MAX_TEXT_LEN];
};

/* Een account: nick, salt+hash, en het bot-slot dat er permanent bij hoort.
 * last_off is de RTC-tijd van het laatste uitloggen en staat ALLEEN in RAM: hij
 * hoort bij de ringbuffer, en die overleeft een herstart ook niet. Hem bij elke
 * logout naar flash schrijven zou een wisbeurt kosten voor niets. */
struct IrcAccount {
  char     nick[IRC_NICK_MAX + 1];
  uint8_t  salt[IRC_SALT_LEN];
  uint8_t  hash[32];
  int8_t   bot;        // bot-slot 0..MAX_BOTS-1; -1 = vrije ingang
  uint32_t last_off;   // RTC-s van het laatste uitloggen; 0 = nooit ingelogd
};

/* Een open sessie. chan_mask is een bit per BotChannel-index, dus MAX_CHANNELS
 * mag niet boven 32 zonder deze mask te verbreden. */
struct IrcClient {
  WiFiClient    sock;
  char          in[IRC_LINE_MAX];
  uint16_t      in_len;
  bool          overflow;        // regel te lang -> slikken tot de volgende LF
  char          nick[IRC_NICK_MAX + 1];
  char          user[12];
  bool          have_pass;
  bool          registered;
  /* CAP-onderhandeling. cap_pending betekent: de client is met CAP begonnen, dus
   * we mogen hem NIET registreren tot er CAP END komt -- doen we dat wel, dan
   * mist hij het welkom of krijgt hij het dubbel. */
  bool          cap_pending;
  bool          cap_tags;      // message-tags: msgid + draft/reply
  bool          cap_time;      // server-time: echte tijd op teruggespeelde regels
  char          pass[40];
  int8_t        acct;            // index in _accts; -1 = nog niet bekend
  int8_t        bot;             // bot-slot van dat account; -1 = geen
  uint32_t      chan_mask;
  unsigned long last_rx;
  unsigned long last_tx_mesh;
  bool          ping_sent;
  uint32_t      last_hash;       // FNV van doel+tekst van het vorige bericht
  unsigned long last_hash_at;
};

class IrcTask {
public:
  /* De mesh en de firmwareversie. Zonder setNode() start de server niet: een
   * IRC-server zonder mesh eronder is een chatserver die nergens heen praat, en
   * dat is verwarrender dan een dichte poort. */
  void begin(RoomMesh* node, const char* firmware_version);
  void loop();

  bool isRunning() const { return _running; }
  int  numClients() const;

  /* ---- Mesh -> IRC. RoomMesh roept deze aan; zie de vooruitverklaring daar. ---- */

  /* Een gelezen kanaalbericht. chan_idx is de BotChannel-index, sender de naam
   * uit het "<naam>: "-voorvoegsel (leeg als het bericht er geen had). */
  void onMeshChannelText(int chan_idx, const char* sender, const char* text,
                         float snr, int rssi, int hops);

  /* Een DM aan bot `bot_idx`. sender_name mag leeg zijn; dan wordt de pubkey-
   * prefix de nick. */
  void onMeshDm(int bot_idx, const uint8_t* sender_pub, const char* sender_name,
                const char* text, float snr, int rssi, int hops);

  /* EEN MESH-NAAM VEILIG MAKEN ALS IRC-NICK. Publiek en statisch, want RoomMesh
   * gebruikt hem ook: wie in zijn client `/msg NodeNet_Gateway` typt, moet de
   * contact "NodeNet Gateway" uit de naamtabel te pakken krijgen. Als de mangeling
   * op twee plaatsen anders zou zijn, is de nick die je ziet niet de nick die
   * werkt -- en dat is precies het soort fout dat niemand meer terugvindt. */
  static void sanitizeNick(const char* in, char* out, size_t out_len);

  /* ---- Accountbeheer. Aangeroepen vanuit de CLI (`irc ...`) en het web. ---- */
  int  acctCount() const;
  bool acctGet(int i, char* nick, size_t nick_len, int* bot) const;
  int  acctFind(const char* nick) const;
  int  acctFindByBot(int bot) const;              // welk account hoort bij dit slot; -1 = geen
  bool acctOnline(int i) const;                   // is er nu een sessie op account i
  int  port() const { return IRC_PORT; }
  /* 0 = ok, -1 = tabel vol, -2 = ongeldige nick/wachtwoord, -3 = nick bestaat al,
   * -4 = bot-slot bestaat niet of is al vergeven. */
  int  acctAdd(const char* nick, const char* password, int bot);
  int  acctSetPassword(const char* nick, const char* password);
  int  acctDel(const char* nick);

private:
  RoomMesh*   _node = nullptr;
  const char* _fw = nullptr;
  WiFiServer  _server{IRC_PORT};
  bool        _running = false;

  IrcAccount  _accts[MAX_BOTS];
  IrcClient   _cl[IRC_MAX_CLIENTS];

  IrcLogEntry _log[IRC_LOG_MAX];
  uint16_t    _log_wr = 0;
  uint32_t    _msg_seq = 0;

  IrcSeen     _seen[MAX_CHANNELS][IRC_SEEN_PER_CHAN];
  unsigned long _seen_next_sweep = 0;

  /* Gedeelde zendemmer over alle clients. */
  uint8_t       _bucket = IRC_BUCKET_MAX;
  unsigned long _bucket_at = 0;
  /* Nulpunt van het luchtvenster: wanneer, en hoeveel de radio toen al gezonden had. */
  unsigned long _duty_ref_at = 0;
  unsigned long _duty_ref_air = 0;
  bool          _duty_armed = false;

  void loadAccounts();
  void saveAccounts();
  void hashPassword(const uint8_t* salt, const char* pw, uint8_t* out32) const;

  void pollAccept();
  void pollClient(IrcClient& c);
  void closeClient(IrcClient& c, const char* quit_reason);

  void handleLine(IrcClient& c, char* line);
  void tryRegister(IrcClient& c);
  void sendWelcome(IrcClient& c);
  /* De ingebouwde help (RPL_HELPSTART/HELPTXT/ENDOFHELP, 704/705/706). Zonder
   * onderwerp een overzicht, met een onderwerp de uitleg erbij. Dit staat er omdat
   * deze server een half dozijn dingen doet die op een gewoon IRC-netwerk niet
   * bestaan -- airtime-rem, quote-antwoorden, een geemuleerde ledenlijst -- en die
   * moet je kunnen opzoeken zonder de repo open te hebben. */
  void sendHelp(IrcClient& c, const char* topic);

  /* Uitvoer. numeric() zet het servervoorvoegsel en de nick van de ontvanger er
   * zelf voor; raw() doet dat niet. Beide voegen CR-LF toe. */
  void raw(IrcClient& c, const char* fmt, ...);
  void numeric(IrcClient& c, int code, const char* fmt, ...);
  void notice(IrcClient& c, const char* fmt, ...);
  /* Een NOTICE die in het venster van `target` landt: bij een kanaal dus in dat
   * kanaalvenster, waar de regel stond die je net probeerde te sturen. Bij een DM
   * valt hij terug op notice() -- een NOTICE met de naam van de tegenpartij ervoor
   * zou in het queryvenster landen, maar dan doen we alsof die persoon iets zei. */
  void noticeTo(IrcClient& c, const char* target, const char* fmt, ...);

  /* Hoeveel tekens er in dit doel passen. Bij een kanaal gaat "<botnaam>: " er nog
   * voor, en dat telt mee in de 160 van een MeshCore-tekstbericht. */
  size_t meshRoomFor(const IrcClient& c, const char* target) const;

  /* IRC-KANAALNAAM <-> KANAALTABEL.
   *
   * Dit is NIET "de '#' eraf knippen", en die aanname was fout: op de node heet
   * een hashtag-kanaal letterlijk "#dinx". De '#' hoort BIJ de naam en gaat mee in
   * sha256(naam), dus wie hem eraf haalt en het kanaal opnieuw aanmaakt krijgt een
   * ANDER geheim en praat stil in het niets.
   *
   * En een mesh-kanaal mag heten wat het wil -- "NodeNet FireMesh Limbur" staat er
   * echt in -- terwijl een IRC-kanaalnaam geen spatie, komma of ':' mag dragen.
   * Die tekens worden '_'. Dat is niet omkeerbaar, dus we zoeken NIET door terug te
   * vertalen maar door de IRC-vorm van elke ingang te vergelijken.
   *
   * Botsen twee kanalen daardoor op dezelfde IRC-naam ("A B" en "A_B"), dan krijgen
   * ZE ALLEBEI de kanaalhash achteraan ("#A_B~3f"). Allebei, want anders hangt het
   * van de tabelvolgorde af wie de kale naam houdt en verspringt die bij de
   * volgende import. De hash staat ook in /LIST, dus hij is over te typen. */
  int  chanIndexFor(const char* irc_name) const;
  bool chanIrcName(int idx, char* out, size_t out_len) const;
  bool chanIrcNameRaw(int idx, char* out, size_t out_len) const;   // zonder botsingsachtervoegsel
  void joinChannel(IrcClient& c, const char* name, const char* key);
  void partChannel(IrcClient& c, const char* name, const char* reason);
  void sendNames(IrcClient& c, int idx);

  /* Ledenlijst per kanaal. seenTouch() zet een JOIN voor wie nieuw is; seenExpire()
   * draait in loop() en zet de PART voor wie te lang stil was. */
  void seenTouch(int chan_idx, const char* nick);
  void seenExpire();
  void seenBroadcast(int chan_idx, const char* nick, bool joining);

  /* De ringbuffer. logAdd() bewaart, replay*() speelt terug naar een client. */
  /* Retour: de index in de ring, of -1. De beller heeft hem nodig om het msgid
   * mee te sturen dat hij zojuist heeft laten aanmaken. */
  int  logAdd(int chan, int bot, const char* nick, const char* text);
  int  logFindById(uint32_t id) const;
  /* De ring-ingang waarvan de tekst met `snip` begint -- zo koppelen we een
   * binnenkomende ">quote.. " terug aan het origineel. -1 = niet gevonden. */
  /* `nick` mag NULL zijn. Staat hij er wel, dan wint een ingang van die afzender;
   * anders valt hij terug op de nieuwste met dezelfde tekst. */
  int  logFindBySnippet(const char* snip, const char* nick) const;

  /* Een PRIVMSG met de juiste IRCv3-tags ervoor. `e` mag NULL zijn (dan geen
   * msgid), `reply_to` is 0 of het msgid waarop dit een antwoord is. */
  void sendMsg(IrcClient& c, const IrcLogEntry* e, const char* nick,
               const char* target, const char* text, uint32_t reply_to);
  void isoTime(uint32_t ts, char* out, size_t out_len) const;
  void replayChannel(IrcClient& c, int chan_idx);
  void replayDms(IrcClient& c);
  /* Vanaf welk moment voor deze client, met IRC_LOG_TTL_S als bovengrens. */
  uint32_t replaySince(const IrcClient& c) const;
  /* "[14:03] ", of leeg als de klok niet gesynct is -- liever geen tijd dan een
   * verzonnen tijd uit 2024, de terugvalwaarde van een ESP32 zonder RTC-batterij. */
  void logStamp(uint32_t ts, char* out, size_t out_len) const;

  void doPrivmsg(IrcClient& c, char* target, const char* text, bool is_notice,
                 const char* tags);

  /* WAT ER RICHTING HET MESH MAG. Deze drie staan tussen de IRC-lijn en de radio:
   *  - meshClean() haalt eruit wat op een mesh geen betekenis heeft (IRC-kleuren,
   *    vetdruk, controltekens) en knipt op een UTF-8-grens af;
   *  - isRepeat() weigert exact dezelfde regel binnen IRC_REPEAT_MS;
   *  - dutyOk() kijkt naar de gemeten zendtijd van de radio.
   * Alles wat naar botSay()/botSendTo() gaat is hier langs geweest. */
  static size_t meshClean(const char* in, char* out, size_t out_len);
  bool isRepeat(IrcClient& c, const char* target, const char* body);
  bool dutyOk(int payload_len, int chunks, char* why, size_t why_len);
  /* Begint deze mesh-regel met ">origineel.. "? Dan het msgid van het origineel,
   * met `body` op het antwoord gezet. 0 = geen quote herkend. */
  uint32_t quoteLookup(const char* text, const char** body) const;
  bool txAllowed(IrcClient& c, char* why, size_t why_len);

  static bool nickValid(const char* n);
};
