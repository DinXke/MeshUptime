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

/* Zendrem. 3 s per client is geen wet maar de ondergrens waarbij een gesprek nog
 * gesprek blijft; de gedeelde emmer is wat het mesh echt beschermt. */
#ifndef IRC_TX_MIN_MS
  #define IRC_TX_MIN_MS  3000
#endif
#ifndef IRC_BUCKET_MAX
  #define IRC_BUCKET_MAX  6
#endif
#ifndef IRC_BUCKET_MS
  #define IRC_BUCKET_MS  10000
#endif

/* Stille sessie afsluiten. Eerst een PING, dan pas weg -- een client die alleen
 * meeleest hoort niet weggegooid te worden omdat hij niets te zeggen had. */
#define IRC_IDLE_PING_MS   120000UL
#define IRC_IDLE_KILL_MS   180000UL

#define IRC_ACCOUNTS_FILE  "/irc_accounts"
#define IRC_SALT_LEN       8

/* Een account: nick, salt+hash, en het bot-slot dat er permanent bij hoort. */
struct IrcAccount {
  char    nick[IRC_NICK_MAX + 1];
  uint8_t salt[IRC_SALT_LEN];
  uint8_t hash[32];
  int8_t  bot;        // bot-slot 0..MAX_BOTS-1; -1 = vrije ingang
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
  char          pass[40];
  int8_t        acct;            // index in _accts; -1 = nog niet bekend
  int8_t        bot;             // bot-slot van dat account; -1 = geen
  uint32_t      chan_mask;
  unsigned long last_rx;
  unsigned long last_tx_mesh;
  bool          ping_sent;
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

  /* ---- Accountbeheer. Aangeroepen vanuit de CLI (`irc ...`) en het web. ---- */
  int  acctCount() const;
  bool acctGet(int i, char* nick, size_t nick_len, int* bot) const;
  int  acctFind(const char* nick) const;
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

  /* Gedeelde zendemmer over alle clients. */
  uint8_t       _bucket = IRC_BUCKET_MAX;
  unsigned long _bucket_at = 0;

  void loadAccounts();
  void saveAccounts();
  void hashPassword(const uint8_t* salt, const char* pw, uint8_t* out32) const;

  void pollAccept();
  void pollClient(IrcClient& c);
  void closeClient(IrcClient& c, const char* quit_reason);

  void handleLine(IrcClient& c, char* line);
  void tryRegister(IrcClient& c);
  void sendWelcome(IrcClient& c);

  /* Uitvoer. numeric() zet het servervoorvoegsel en de nick van de ontvanger er
   * zelf voor; raw() doet dat niet. Beide voegen CR-LF toe. */
  void raw(IrcClient& c, const char* fmt, ...);
  void numeric(IrcClient& c, int code, const char* fmt, ...);
  void notice(IrcClient& c, const char* fmt, ...);

  /* IRC-kanaalnaam <-> BotChannel-index. "#belimburg" <-> "belimburg". */
  int  chanIndexFor(const char* irc_name) const;
  bool chanIrcName(int idx, char* out, size_t out_len) const;
  void joinChannel(IrcClient& c, const char* name, const char* key);
  void partChannel(IrcClient& c, const char* name, const char* reason);
  void sendNames(IrcClient& c, int idx);

  void doPrivmsg(IrcClient& c, char* target, const char* text, bool is_notice);
  bool txAllowed(IrcClient& c, char* why, size_t why_len);

  /* Een mesh-naam veilig maken als IRC-nick: geen spatie, ':', '!', '@' of CR-LF. */
  static void sanitizeNick(const char* in, char* out, size_t out_len);
  static bool nickValid(const char* n);
};
