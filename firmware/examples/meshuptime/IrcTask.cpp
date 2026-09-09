#include "IrcTask.h"

#if defined(ESP32)

#include <SPIFFS.h>
#include <esp_system.h>
#include <stdarg.h>

/* Het servervoorvoegsel in numerieke antwoorden. IRC-clients tonen dit als de
 * naam van het netwerk, dus de nodenaam is hier de juiste waarde en niet een
 * verzonnen "irc.meshuptime". */
static const char* IRC_HOST_FALLBACK = "meshuptime";

/* De pseudo-host achter een mesh-nick. Clients tonen "nick!user@host" in /whois
 * en in join-meldingen; "mesh" maakt in een oogopslag zichtbaar dat de afzender
 * van de radio komt en niet van een tweede IRC-verbinding. */
#define IRC_MESH_USERHOST  "mesh@mesh"

/* ------------------------------------------------------------------------ */
/*  Kleine hulpjes                                                           */
/* ------------------------------------------------------------------------ */

void IrcTask::sanitizeNick(const char* in, char* out, size_t out_len) {
  size_t o = 0;
  if (!in) { out[0] = 0; return; }
  for (const char* p = in; *p && o + 1 < out_len; p++) {
    unsigned char ch = (unsigned char)*p;
    /* NIET-ASCII WEGLATEN, NIET ONDERSTREPEN. Een naam als "🇧🇪BE-HSS-DinX" begint met
     * een vlag-emoji van acht UTF-8-bytes; die stuk voor stuk vervangen gaf
     * "________BE-HSS-DinX" -- onleesbaar, en niet over te typen in een /msg. */
    if (ch >= 0x7f) continue;
    /* Wat het IRC-frame zou breken of een tweede parameter zou beginnen. Een reeks
     * daarvan wordt EEN underscore, zodat "Jan  de  Boer" niet "Jan__de__Boer" wordt. */
    bool bad = (ch <= ' ') || ch == ':' || ch == '!' || ch == '@' || ch == ',' ||
               ch == '*' || ch == '?' || ch == '#';
    if (bad) { if (o > 0 && out[o - 1] == '_') continue; out[o++] = '_'; continue; }
    out[o++] = (char)ch;
  }
  while (o > 0 && out[o - 1] == '_') o--;      /* geen underscore op het eind */
  out[o] = 0;
  size_t lead = 0; while (out[lead] == '_') lead++;
  if (lead) memmove(out, out + lead, o - lead + 1);
  if (out[0] == 0) { out[0] = '?'; out[1] = 0; }
}

bool IrcTask::nickValid(const char* n) {
  if (!n || !n[0] || strlen(n) > IRC_NICK_MAX) return false;
  for (const char* p = n; *p; p++) {
    char ch = *p;
    bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '[' ||
              ch == ']' || ch == '\\' || ch == '`' || ch == '^' || ch == '{' || ch == '}';
    if (!ok) return false;
  }
  /* Een nick die met een cijfer begint botst met de numerieke antwoorden. */
  return !(n[0] >= '0' && n[0] <= '9');
}

/* Een IRC-parameterlijst splitsen. Retourneert het aantal parameters; de laatste
 * parameter mag met ':' beginnen en loopt dan tot het einde van de regel. */
static int ircSplit(char* line, char** argv, int max_args) {
  int n = 0;
  char* p = line;
  while (*p && n < max_args) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (*p == ':') { argv[n++] = p + 1; break; }   // trailing parameter
    argv[n++] = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
  }
  return n;
}

/* ------------------------------------------------------------------------ */
/*  Levensloop                                                               */
/* ------------------------------------------------------------------------ */

void IrcTask::begin(RoomMesh* node, const char* firmware_version) {
  _node = node;
  _fw   = firmware_version;
  if (!_node) return;   // zie de uitleg bij begin() in de header

  memset(_accts, 0, sizeof(_accts));
  for (int i = 0; i < MAX_BOTS; i++) _accts[i].bot = -1;
  for (int i = 0; i < IRC_MAX_CLIENTS; i++) {
    _cl[i].in_len = 0; _cl[i].overflow = false;
    _cl[i].nick[0] = 0; _cl[i].user[0] = 0; _cl[i].pass[0] = 0;
    _cl[i].have_pass = false; _cl[i].registered = false;
    _cl[i].cap_pending = false; _cl[i].cap_tags = false; _cl[i].cap_time = false;
    _cl[i].acct = -1; _cl[i].bot = -1; _cl[i].chan_mask = 0;
    _cl[i].ping_sent = false;
  }
  memset(_seen, 0, sizeof(_seen));
  memset(_log, 0, sizeof(_log));
  loadAccounts();

  _server.begin();
  _server.setNoDelay(true);
  _bucket = IRC_BUCKET_MAX;
  _bucket_at = millis();
  _running = true;
  Serial.printf("[irc] server op poort %d, %d account(s)\n", IRC_PORT, acctCount());
}

int IrcTask::numClients() const {
  int n = 0;
  for (int i = 0; i < IRC_MAX_CLIENTS; i++)
    if (const_cast<WiFiClient&>(_cl[i].sock).connected()) n++;
  return n;
}

void IrcTask::loop() {
  if (!_running) return;

  /* De emmer hervullen. Eén bericht per IRC_BUCKET_MS erbij, tot het maximum. */
  unsigned long now = millis();
  while ((long)(now - _bucket_at) >= IRC_BUCKET_MS) {
    _bucket_at += IRC_BUCKET_MS;
    if (_bucket < IRC_BUCKET_MAX) _bucket++;
  }

  pollAccept();
  for (int i = 0; i < IRC_MAX_CLIENTS; i++) pollClient(_cl[i]);

  /* De geemuleerde ledenlijst opruimen. Eens per 30 s is ruim genoeg voor een
   * TTL van drie kwartier, en het houdt loop() goedkoop. */
  if ((long)(now - _seen_next_sweep) >= 0) { _seen_next_sweep = now + 30000UL; seenExpire(); }
}

void IrcTask::pollAccept() {
  WiFiClient nc = _server.available();
  if (!nc) return;

  for (int i = 0; i < IRC_MAX_CLIENTS; i++) {
    if (_cl[i].sock.connected()) continue;
    IrcClient& c = _cl[i];
    c.sock = nc;
    c.sock.setNoDelay(true);
    c.in_len = 0; c.overflow = false;
    c.nick[0] = 0; c.user[0] = 0; c.pass[0] = 0;
    c.have_pass = false; c.registered = false;
    c.cap_pending = false; c.cap_tags = false; c.cap_time = false;
    c.acct = -1; c.bot = -1; c.chan_mask = 0;
    c.last_rx = millis(); c.last_tx_mesh = 0; c.ping_sent = false;
    return;
  }

  /* Vol. Zeg waarom -- een client die zonder uitleg de deur dicht krijgt, wordt
   * als "server stuk" gerapporteerd terwijl er niets stuk is. */
  nc.printf("ERROR :Alle %d sessies bezet (één per bot-slot)\r\n", IRC_MAX_CLIENTS);
  nc.stop();
}

void IrcTask::closeClient(IrcClient& c, const char* quit_reason) {
  /* Het moment van weggaan onthouden, zodat de volgende sessie precies vanaf hier
   * kan terugspoelen. Ook bij een harde disconnect: pollClient() roept ons dan
   * alsnog aan zodra de socket dood blijkt. */
  if (c.registered && c.acct >= 0)
    _accts[c.acct].last_off = _node->getRTCClock()->getCurrentTime();
  if (c.sock.connected()) {
    if (quit_reason && quit_reason[0]) c.sock.printf("ERROR :%s\r\n", quit_reason);
    c.sock.flush();
    c.sock.stop();
  }
  c.registered = false;
  c.chan_mask = 0;
  c.in_len = 0;
  c.nick[0] = 0;
  c.acct = -1;
  c.bot = -1;
}

void IrcTask::pollClient(IrcClient& c) {
  if (!c.sock.connected()) { if (c.registered) closeClient(c, NULL); return; }

  unsigned long now = millis();

  /* Lezen. Begrensd per ronde: de radio mag niet wachten op een client die een
   * bestand in het venster plakt. */
  int budget = 256;
  while (c.sock.available() && budget-- > 0) {
    char ch = (char)c.sock.read();
    c.last_rx = now;
    c.ping_sent = false;
    if (ch == '\n' || ch == '\r') {
      if (c.in_len == 0) { c.overflow = false; continue; }
      c.in[c.in_len] = 0;
      c.in_len = 0;
      if (c.overflow) { c.overflow = false; continue; }   // rest van een te lange regel
      handleLine(c, c.in);
      if (!c.sock.connected()) return;
      continue;
    }
    if (c.in_len + 1 >= IRC_LINE_MAX) { c.overflow = true; c.in_len = 0; continue; }
    c.in[c.in_len++] = ch;
  }

  /* Stilte. Eerst een PING, en pas als die onbeantwoord blijft de sessie weg. */
  if (c.registered) {
    if (!c.ping_sent && (now - c.last_rx) > IRC_IDLE_PING_MS) {
      raw(c, "PING :%s", _node->getNodeName());
      c.ping_sent = true;
    } else if ((now - c.last_rx) > IRC_IDLE_KILL_MS) {
      closeClient(c, "Ping timeout");
    }
  } else if ((now - c.last_rx) > 60000UL) {
    closeClient(c, "Registration timeout");
  }
}

/* ------------------------------------------------------------------------ */
/*  Uitvoer                                                                  */
/* ------------------------------------------------------------------------ */

void IrcTask::raw(IrcClient& c, const char* fmt, ...) {
  if (!c.sock.connected()) return;
  char buf[IRC_LINE_MAX];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(buf) - 3) n = sizeof(buf) - 3;
  buf[n++] = '\r'; buf[n++] = '\n'; buf[n] = 0;
  c.sock.write((const uint8_t*)buf, n);
}

void IrcTask::numeric(IrcClient& c, int code, const char* fmt, ...) {
  char tail[IRC_LINE_MAX - 64];
  va_list ap; va_start(ap, fmt);
  vsnprintf(tail, sizeof(tail), fmt, ap);
  va_end(ap);
  const char* host = _node ? _node->getNodeName() : IRC_HOST_FALLBACK;
  if (!host || !host[0]) host = IRC_HOST_FALLBACK;
  raw(c, ":%s %03d %s %s", host, code, c.nick[0] ? c.nick : "*", tail);
}

void IrcTask::notice(IrcClient& c, const char* fmt, ...) {
  char tail[IRC_LINE_MAX - 64];
  va_list ap; va_start(ap, fmt);
  vsnprintf(tail, sizeof(tail), fmt, ap);
  va_end(ap);
  const char* host = _node ? _node->getNodeName() : IRC_HOST_FALLBACK;
  if (!host || !host[0]) host = IRC_HOST_FALLBACK;
  raw(c, ":%s NOTICE %s :%s", host, c.nick[0] ? c.nick : "*", tail);
}

/* ------------------------------------------------------------------------ */
/*  Accounts                                                                 */
/* ------------------------------------------------------------------------ */

void IrcTask::hashPassword(const uint8_t* salt, const char* pw, uint8_t* out32) const {
  /* De salt gaat als SLEUTEL mee in dezelfde sha256-helper die de rest van deze
   * firmware gebruikt; zo komt er geen tweede hash-implementatie bij. */
  mesh::Utils::sha256(out32, 32, (const uint8_t*)pw, (int)strlen(pw), salt, IRC_SALT_LEN);
}

void IrcTask::loadAccounts() {
  File f = SPIFFS.open(IRC_ACCOUNTS_FILE, "r");
  if (!f) return;
  int n = 0;
  while (f.available() && n < MAX_BOTS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    /* nick \t salthex \t hashhex \t botidx */
    int t1 = line.indexOf('\t');
    int t2 = t1 < 0 ? -1 : line.indexOf('\t', t1 + 1);
    int t3 = t2 < 0 ? -1 : line.indexOf('\t', t2 + 1);
    if (t3 < 0) continue;
    String nick = line.substring(0, t1);
    String sh   = line.substring(t1 + 1, t2);
    String hh   = line.substring(t2 + 1, t3);
    int bot     = line.substring(t3 + 1).toInt();
    if (nick.length() == 0 || nick.length() > IRC_NICK_MAX) continue;
    if (sh.length() != IRC_SALT_LEN * 2 || hh.length() != 64) continue;
    if (bot < 0 || bot >= MAX_BOTS) continue;
    IrcAccount& a = _accts[n];
    StrHelper::strncpy(a.nick, nick.c_str(), sizeof(a.nick));
    if (!mesh::Utils::fromHex(a.salt, IRC_SALT_LEN, sh.c_str())) continue;
    if (!mesh::Utils::fromHex(a.hash, 32, hh.c_str())) continue;
    a.bot = (int8_t)bot;
    n++;
  }
  f.close();
}

void IrcTask::saveAccounts() {
  File f = SPIFFS.open(IRC_ACCOUNTS_FILE, "w");
  if (!f) { Serial.println("[irc] kan " IRC_ACCOUNTS_FILE " niet schrijven"); return; }
  f.println("# nick\tsalt\tsha256(salt,wachtwoord)\tbot-slot -- NIET met de hand bewerken");
  char sh[IRC_SALT_LEN * 2 + 1], hh[65];
  for (int i = 0; i < MAX_BOTS; i++) {
    if (_accts[i].bot < 0 || !_accts[i].nick[0]) continue;
    mesh::Utils::toHex(sh, _accts[i].salt, IRC_SALT_LEN);
    mesh::Utils::toHex(hh, _accts[i].hash, 32);
    f.printf("%s\t%s\t%s\t%d\n", _accts[i].nick, sh, hh, (int)_accts[i].bot);
  }
  f.close();
}

int IrcTask::acctCount() const {
  int n = 0;
  for (int i = 0; i < MAX_BOTS; i++) if (_accts[i].bot >= 0 && _accts[i].nick[0]) n++;
  return n;
}

bool IrcTask::acctGet(int i, char* nick, size_t nick_len, int* bot) const {
  if (i < 0 || i >= MAX_BOTS || _accts[i].bot < 0 || !_accts[i].nick[0]) return false;
  if (nick) StrHelper::strncpy(nick, _accts[i].nick, nick_len);
  if (bot) *bot = _accts[i].bot;
  return true;
}

int IrcTask::acctFind(const char* nick) const {
  if (!nick) return -1;
  for (int i = 0; i < MAX_BOTS; i++)
    if (_accts[i].bot >= 0 && strcasecmp(_accts[i].nick, nick) == 0) return i;
  return -1;
}

int IrcTask::acctFindByBot(int bot) const {
  if (bot < 0) return -1;
  for (int i = 0; i < MAX_BOTS; i++)
    if (_accts[i].bot == bot && _accts[i].nick[0]) return i;
  return -1;
}

bool IrcTask::acctOnline(int i) const {
  for (int k = 0; k < IRC_MAX_CLIENTS; k++)
    if (_cl[k].registered && _cl[k].acct == i) return true;
  return false;
}

int IrcTask::acctAdd(const char* nick, const char* password, int bot) {
  if (!nickValid(nick)) return -2;
  if (!password || strlen(password) < 6) return -2;
  if (acctFind(nick) >= 0) return -3;
  if (!_node || bot < 0 || bot >= MAX_BOTS || !_node->webBotSlotUsed(bot)) return -4;
  for (int i = 0; i < MAX_BOTS; i++)
    if (_accts[i].bot == bot) return -4;   // slot al vergeven; zie de vaste-toewijzing-regel

  for (int i = 0; i < MAX_BOTS; i++) {
    if (_accts[i].bot >= 0) continue;
    IrcAccount& a = _accts[i];
    StrHelper::strncpy(a.nick, nick, sizeof(a.nick));
    for (int k = 0; k < IRC_SALT_LEN; k++) a.salt[k] = (uint8_t)(esp_random() & 0xff);
    hashPassword(a.salt, password, a.hash);
    a.bot = (int8_t)bot;
    saveAccounts();
    return 0;
  }
  return -1;
}

int IrcTask::acctSetPassword(const char* nick, const char* password) {
  int i = acctFind(nick);
  if (i < 0) return -2;
  if (!password || strlen(password) < 6) return -2;
  for (int k = 0; k < IRC_SALT_LEN; k++) _accts[i].salt[k] = (uint8_t)(esp_random() & 0xff);
  hashPassword(_accts[i].salt, password, _accts[i].hash);
  saveAccounts();
  return 0;
}

int IrcTask::acctDel(const char* nick) {
  int i = acctFind(nick);
  if (i < 0) return -2;
  /* Een open sessie van dit account eerst netjes wegsturen. */
  for (int k = 0; k < IRC_MAX_CLIENTS; k++)
    if (_cl[k].acct == i) closeClient(_cl[k], "Account verwijderd");
  memset(&_accts[i], 0, sizeof(IrcAccount));
  _accts[i].bot = -1;
  saveAccounts();
  return 0;
}

/* ------------------------------------------------------------------------ */
/*  Registratie                                                              */
/* ------------------------------------------------------------------------ */

void IrcTask::tryRegister(IrcClient& c) {
  if (c.registered || !c.nick[0] || !c.user[0]) return;
  if (c.cap_pending) return;            /* CAP END nog niet gezien */

  if (!c.have_pass) {
    numeric(c, 464, ":Wachtwoord vereist. Stel het in je client in als serverwachtwoord (PASS).");
    closeClient(c, "Geen wachtwoord");
    return;
  }
  int a = acctFind(c.nick);
  if (a < 0) {
    numeric(c, 464, ":Onbekend account '%s'. Accounts maakt de beheerder met 'irc user add'.", c.nick);
    closeClient(c, "Onbekend account");
    return;
  }
  uint8_t want[32];
  hashPassword(_accts[a].salt, c.pass, want);
  if (memcmp(want, _accts[a].hash, 32) != 0) {
    numeric(c, 464, ":Verkeerd wachtwoord");
    closeClient(c, "Verkeerd wachtwoord");
    return;
  }
  /* Eén sessie per account: een tweede login zou twee toetsenborden op één
   * mesh-identiteit zetten en dan is niet meer te zien wie wat verstuurde. */
  for (int k = 0; k < IRC_MAX_CLIENTS; k++)
    if (&_cl[k] != &c && _cl[k].registered && _cl[k].acct == a)
      closeClient(_cl[k], "Elders ingelogd op dit account");

  c.acct = (int8_t)a;
  c.bot  = _accts[a].bot;
  memset(c.pass, 0, sizeof(c.pass));   // niet langer bewaren dan nodig
  c.registered = true;
  sendWelcome(c);
}

void IrcTask::sendWelcome(IrcClient& c) {
  const char* host = _node->getNodeName();
  if (!host || !host[0]) host = IRC_HOST_FALLBACK;
  char pub[PUB_KEY_SIZE * 2 + 1] = {0};
  _node->webBotSlotPubHex(c.bot, pub, sizeof(pub));
  const char* bname = _node->webBotSlotName(c.bot);

  numeric(c, 1, ":Welkom op het mesh, %s -- je bent hier de MeshCore-identiteit '%s'", c.nick, bname);
  numeric(c, 2, ":Draait op %s, %s", host, _fw ? _fw : "MeshUptime");
  numeric(c, 3, ":Bot-slot %d, pubkey %.12s...", c.bot, pub);
  /* RPL_MYINFO is <server> <versie> <usermodes> <chanmodes>; daar hoort geen proza
   * in, sommige clients parsen deze regel. Wij kennen geen usermodes en op een
   * kanaal alleen +k. */
  numeric(c, 4, "%s %s - k", host, _fw ? _fw : "MeshUptime");
  numeric(c, 5, "CHANTYPES=# PREFIX= NICKLEN=%d TOPICLEN=0 CHANMODES=k :zijn ondersteund",
          IRC_NICK_MAX);

  numeric(c, 375, ":- %s bericht van de dag -", host);
  numeric(c, 372, ":- Je praat op een LoRa-mesh. Airtime is schaars: hooguit één");
  numeric(c, 372, ":- bericht per %lu s, en %d tekens per bericht.",
          (unsigned long)(IRC_TX_MIN_MS / 1000), (int)BOT_MAX_TEXT_LEN);
  numeric(c, 372, ":- JOIN #naam         hashtag-kanaal: de sleutel volgt uit de naam");
  numeric(c, 372, ":- JOIN #naam sleutel prive-kanaal met een eigen sleutel (16/32 byte hex)");
  numeric(c, 372, ":- JOIN #Public       het standaardkanaal -- vaste sleutel, GEEN hashtag");
  numeric(c, 372, ":- antwoorden         wordt op het mesh '>origineel.. jouw tekst',");
  numeric(c, 372, ":-                    want MeshCore kent geen antwoordveld");
  numeric(c, 372, ":- PRIVMSG nick       DM vanaf jouw eigen sleutelpaar");
  numeric(c, 372, ":- WHOIS nick         pubkey, SNR en hopcount van de laatste hoor");
  numeric(c, 372, ":- Anderen voegen je toe met deze link:");
  char uri[160];
  if (_node->webBotSlotJoinUri(c.bot, uri, sizeof(uri))) numeric(c, 372, ":-   %s", uri);
  numeric(c, 376, ":Einde van het bericht van de dag");

  notice(c, "Let op: IRC is onversleuteld. Alleen op een vertrouwd netwerk gebruiken.");
  replayDms(c);
}

/* ------------------------------------------------------------------------ */
/*  Kanalen                                                                  */
/* ------------------------------------------------------------------------ */

bool IrcTask::chanIrcNameRaw(int idx, char* out, size_t out_len) const {
  char name[24]; int bits; bool en, derived, pub; uint8_t hash;
  if (!_node->channelGet(idx, name, &bits, &en, &hash, &derived, &pub)) return false;
  const char* p = name;
  if (*p == '#') p++;                 /* geen "##dinx" */
  size_t o = 0;
  if (o + 1 < out_len) out[o++] = '#';
  for (; *p && o + 1 < out_len; p++) {
    unsigned char ch = (unsigned char)*p;
    if (ch >= 0x7f) continue;                  /* emoji weg, niet onderstrepen */
    /* Wat RFC 1459 verbiedt in een kanaalnaam: spatie, komma, ':' en BEL. Een reeks
     * ervan wordt EEN underscore; twee kanalen die daardoor toch dezelfde IRC-naam
     * krijgen, krijgen allebei de hash erachter (zie chanIrcName). */
    bool bad = ch <= ' ' || ch == ',' || ch == ':' || ch == 7;
    if (bad) { if (o > 1 && out[o - 1] == '_') continue; out[o++] = '_'; continue; }
    out[o++] = (char)ch;
  }
  while (o > 1 && out[o - 1] == '_') o--;
  out[o] = 0;
  return o > 1;
}

bool IrcTask::chanIrcName(int idx, char* out, size_t out_len) const {
  if (!chanIrcNameRaw(idx, out, out_len)) return false;
  /* Botsing? Dan de kanaalhash erachter -- bij ALLEBEI, zie de header. */
  char other[32];
  for (int j = 0; j < MAX_CHANNELS; j++) {
    if (j == idx) continue;
    if (!chanIrcNameRaw(j, other, sizeof(other))) continue;
    if (strcasecmp(other, out) != 0) continue;
    char nm[24]; int bits; bool en, derived, pub; uint8_t hash;
    if (_node->channelGet(idx, nm, &bits, &en, &hash, &derived, &pub)) {
      size_t o = strlen(out);
      snprintf(out + o, out_len - o, "~%02x", hash);
    }
    break;
  }
  return true;
}

/* Zoeken door te vergelijken met de IRC-vorm van elke ingang: de mangeling hoeft
 * dan niet omkeerbaar te zijn. O(n) over hoogstens MAX_CHANNELS. */
int IrcTask::chanIndexFor(const char* irc_name) const {
  if (!irc_name || irc_name[0] != '#' || !irc_name[1]) return -1;
  char cn[36];
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!chanIrcName(i, cn, sizeof(cn))) continue;
    if (strcasecmp(cn, irc_name) == 0) return i;
  }
  return -1;
}

/* ------------------------------------------------------------------------ */
/*  Terugspoelen                                                              */
/* ------------------------------------------------------------------------ */

int IrcTask::logAdd(int chan, int bot, const char* nick, const char* text) {
  if (!text || !text[0]) return -1;
  uint32_t now = _node->getRTCClock()->getCurrentTime();
  if (now == 0) now = 1;                 /* 0 betekent "leeg slot" */
  int idx = _log_wr;
  IrcLogEntry& e = _log[idx];
  _log_wr = (uint16_t)((_log_wr + 1) % IRC_LOG_MAX);
  e.ts = now;
  e.id = ++_msg_seq;
  e.chan = (int8_t)chan;
  e.bot = (int8_t)bot;
  StrHelper::strncpy(e.nick, (nick && nick[0]) ? nick : "mesh", sizeof(e.nick));
  StrHelper::strncpy(e.text, text, sizeof(e.text));
  return idx;
}

int IrcTask::logFindById(uint32_t id) const {
  if (!id) return -1;
  for (int i = 0; i < IRC_LOG_MAX; i++)
    if (_log[i].ts != 0 && _log[i].id == id) return i;
  return -1;
}

int IrcTask::logFindBySnippet(const char* snip) const {
  if (!snip || !snip[0]) return -1;
  size_t n = strlen(snip);
  /* Nieuwste eerst: dezelfde vraag wordt op een mesh vaker gesteld, en dan is het
   * laatste voorkomen bijna altijd het bedoelde. */
  for (int k = IRC_LOG_MAX; k > 0; k--) {
    const IrcLogEntry& e = _log[(_log_wr + k - 1) % IRC_LOG_MAX];
    if (e.ts == 0) continue;
    if (strncasecmp(e.text, snip, n) == 0) return (_log_wr + k - 1) % IRC_LOG_MAX;
  }
  return -1;
}

void IrcTask::isoTime(uint32_t ts, char* out, size_t out_len) const {
  out[0] = 0;
  if (ts < 1735689600UL) return;         /* klok niet gesynct; zie logStamp() */
  time_t t = (time_t)ts;
  struct tm tmv;
  if (!gmtime_r(&t, &tmv)) return;
  snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* Een PRIVMSG met de tags die DEZE client gevraagd heeft. Wie geen message-tags
 * negotieerde krijgt precies wat hij altijd kreeg -- de tags weglaten is hier geen
 * verlies maar de hele reden dat dit degradeert. */
void IrcTask::sendMsg(IrcClient& c, const IrcLogEntry* e, const char* nick,
                      const char* target, const char* text, uint32_t reply_to) {
  char tags[96]; tags[0] = 0;
  int t = 0;
  if (c.cap_tags && e && e->id)
    t += snprintf(tags + t, sizeof(tags) - t, "%smsgid=m%lx", t ? ";" : "@", (unsigned long)e->id);
  if (c.cap_tags && reply_to)
    t += snprintf(tags + t, sizeof(tags) - t, "%s+draft/reply=m%lx", t ? ";" : "@", (unsigned long)reply_to);
  if (c.cap_time && e && e->ts) {
    char iso[32]; isoTime(e->ts, iso, sizeof(iso));
    if (iso[0]) t += snprintf(tags + t, sizeof(tags) - t, "%stime=%s", t ? ";" : "@", iso);
  }
  if (t) raw(c, "%s :%s!%s PRIVMSG %s :%s", tags, nick, IRC_MESH_USERHOST, target, text);
  else   raw(c, ":%s!%s PRIVMSG %s :%s", nick, IRC_MESH_USERHOST, target, text);
}

void IrcTask::logStamp(uint32_t ts, char* out, size_t out_len) const {
  out[0] = 0;
  /* Onder deze grens staat de klok op de fabriekswaarde en is een tijdstip een
   * leugen. Zelfde gedachte als TIME_FLOOR, hier lokaal: 1 januari 2025. */
  if (ts < 1735689600UL) return;
  time_t t = (time_t)ts;
  struct tm tmv;
  if (!localtime_r(&t, &tmv)) return;
  snprintf(out, out_len, "[%02d:%02d] ", tmv.tm_hour, tmv.tm_min);
}

uint32_t IrcTask::replaySince(const IrcClient& c) const {
  uint32_t now = _node->getRTCClock()->getCurrentTime();
  uint32_t oldest = (now > IRC_LOG_TTL_S) ? (now - IRC_LOG_TTL_S) : 0;
  uint32_t since = (c.acct >= 0) ? _accts[c.acct].last_off : 0;
  return since > oldest ? since : oldest;
}

/* De ring op volgorde van schrijven aflopen: begin bij de plek waar we hierna
 * zouden schrijven (de oudste) en loop rond. Zo komt het gesprek in de juiste
 * volgorde binnen en niet omgekeerd. */
void IrcTask::replayChannel(IrcClient& c, int chan_idx) {
  char cname[32];
  if (!chanIrcName(chan_idx, cname, sizeof(cname))) return;
  uint32_t since = replaySince(c);
  int n = 0;
  for (int k = 0; k < IRC_LOG_MAX; k++) {
    const IrcLogEntry& e = _log[(_log_wr + k) % IRC_LOG_MAX];
    if (e.ts == 0 || e.chan != chan_idx || e.ts <= since) continue;
    if (n == 0)
      raw(c, ":%s NOTICE %s :--- gemist sinds je laatste sessie ---",
          _node->getNodeName(), cname);
    /* Met server-time zet de client zelf het juiste tijdstip voor de regel; dan is
     * ons eigen "[uu:mm]" dubbelop en laten we het weg. */
    if (c.cap_time) {
      sendMsg(c, &e, e.nick, cname, e.text, 0);
    } else {
      char st[12]; logStamp(e.ts, st, sizeof(st));
      char line[BOT_MAX_TEXT_LEN + 16];
      snprintf(line, sizeof(line), "%s%s", st, e.text);
      sendMsg(c, &e, e.nick, cname, line, 0);
    }
    n++;
  }
  if (n) raw(c, ":%s NOTICE %s :--- einde, hierna is het live ---",
             _node->getNodeName(), cname);
}

void IrcTask::replayDms(IrcClient& c) {
  uint32_t since = replaySince(c);
  int n = 0;
  for (int k = 0; k < IRC_LOG_MAX; k++) {
    const IrcLogEntry& e = _log[(_log_wr + k) % IRC_LOG_MAX];
    if (e.ts == 0 || e.chan != IRC_LOG_DM || e.bot != c.bot || e.ts <= since) continue;
    if (n == 0) notice(c, "DM's die binnenkwamen terwijl je weg was:");
    if (c.cap_time) {
      sendMsg(c, &e, e.nick, c.nick, e.text, 0);
    } else {
      char st[12]; logStamp(e.ts, st, sizeof(st));
      char line[BOT_MAX_TEXT_LEN + 16];
      snprintf(line, sizeof(line), "%s%s", st, e.text);
      sendMsg(c, &e, e.nick, c.nick, line, 0);
    }
    n++;
  }
}

/* Naar iedereen die dit kanaal volgt: nick komt binnen of gaat weg. */
void IrcTask::seenBroadcast(int chan_idx, const char* nick, bool joining) {
  char cname[32];
  if (!chanIrcName(chan_idx, cname, sizeof(cname))) return;
  for (int k = 0; k < IRC_MAX_CLIENTS; k++) {
    if (!_cl[k].registered || !(_cl[k].chan_mask & (1UL << chan_idx))) continue;
    if (joining) raw(_cl[k], ":%s!%s JOIN %s", nick, IRC_MESH_USERHOST, cname);
    else raw(_cl[k], ":%s!%s PART %s :stil sinds %lu min", nick, IRC_MESH_USERHOST,
             cname, (unsigned long)(IRC_SEEN_TTL_MS / 60000UL));
  }
}

void IrcTask::seenTouch(int chan_idx, const char* nick) {
  if (chan_idx < 0 || chan_idx >= MAX_CHANNELS || !nick || !nick[0]) return;
  IrcSeen* row = _seen[chan_idx];
  unsigned long now = millis();
  if (now == 0) now = 1;   // 0 betekent "leeg"

  int free_i = -1, oldest = 0;
  for (int i = 0; i < IRC_SEEN_PER_CHAN; i++) {
    if (row[i].last == 0) { if (free_i < 0) free_i = i; continue; }
    if (strcasecmp(row[i].nick, nick) == 0) { row[i].last = now; return; }   // al lid
    if (row[i].last < row[oldest].last) oldest = i;
  }
  /* Vol -> de langst stille eruit. Die krijgt een PART, anders blijft hij in de
   * ledenlijst van de client staan terwijl wij hem niet meer kennen. */
  int idx = free_i;
  if (idx < 0) { idx = oldest; seenBroadcast(chan_idx, row[idx].nick, false); }
  StrHelper::strncpy(row[idx].nick, nick, sizeof(row[idx].nick));
  row[idx].last = now;
  seenBroadcast(chan_idx, nick, true);
}

void IrcTask::seenExpire() {
  unsigned long now = millis();
  for (int c = 0; c < MAX_CHANNELS; c++)
    for (int i = 0; i < IRC_SEEN_PER_CHAN; i++) {
      if (_seen[c][i].last == 0) continue;
      if ((now - _seen[c][i].last) < IRC_SEEN_TTL_MS) continue;
      seenBroadcast(c, _seen[c][i].nick, false);
      _seen[c][i].last = 0;
      _seen[c][i].nick[0] = 0;
    }
}

void IrcTask::sendNames(IrcClient& c, int idx) {
  char cname[32];
  if (!chanIrcName(idx, cname, sizeof(cname))) return;

  /* Wie "in" het kanaal zit is op een mesh niet te weten: er is geen ledenlijst,
   * alleen wie toevallig zendt. We tonen daarom de lokale IRC-sessies die het
   * kanaal volgen. Dat is eerlijk -- een verzonnen ledenlijst uit de buurtlijst
   * zou nodes tonen die het kanaal misschien niet eens hebben. */
  char line[IRC_LINE_MAX - 64];
  int o = 0; line[0] = 0;
  for (int k = 0; k < IRC_MAX_CLIENTS; k++) {
    if (!_cl[k].registered || !(_cl[k].chan_mask & (1UL << idx))) continue;
    o += snprintf(line + o, sizeof(line) - o, "%s%s", o ? " " : "", _cl[k].nick);
    if (o > (int)sizeof(line) - 32) break;
  }
  /* En de nodes die we in dit kanaal hebben HOREN zenden. */
  if (idx < MAX_CHANNELS)
    for (int i = 0; i < IRC_SEEN_PER_CHAN; i++) {
      if (_seen[idx][i].last == 0) continue;
      if (o > (int)sizeof(line) - 32) break;
      o += snprintf(line + o, sizeof(line) - o, "%s%s", o ? " " : "", _seen[idx][i].nick);
    }
  numeric(c, 353, "= %s :%s", cname, line);
  numeric(c, 366, "%s :Einde van /NAMES -- dit is wie er GEHOORD is, niet wie er is: "
                  "meelezers verschijnen nooit en een naam in een kanaal is onbewezen", cname);
}

void IrcTask::joinChannel(IrcClient& c, const char* name, const char* key) {
  if (!name || name[0] != '#' || !name[1]) { numeric(c, 403, "%s :Geen geldige kanaalnaam", name ? name : ""); return; }
  /* EERST opzoeken, DAN pas de lengte keuren. Een kanaal dat al op de node staat
   * mag een naam hebben die wij zelf niet meer zouden aanmaken -- hem weigeren op
   * een grens die alleen over aanmaken gaat, sluit je buiten je eigen kanaal. */
  int idx = chanIndexFor(name);
  if (idx < 0) {
    if (strlen(name) > 23) {
      numeric(c, 403, "%s :Kanaalnaam te lang om aan te maken (hoogstens 22 tekens na de #)", name);
      return;
    }
    /* Nieuw kanaal. De naam gaat MET de '#' de tabel in: dat is de vorm die de
     * MeshCore-apps gebruiken, en bij een hashtag-kanaal zit hij in het geheim
     * (sha256 over de hele naam). Hem weglaten zou een kanaal opleveren dat
     * niemand anders kan lezen. */
    int rc = _node->channelAdd(name, (key && key[0]) ? key : NULL, true);
    if (rc == -3) {
      numeric(c, 403, "%s :De kanaaltabel van de node is vol (%d slots). Ruim er een "
                      "op met 'channel del <naam>' voor je een nieuw kanaal joint.",
              name, (int)MAX_CHANNELS);
      return;
    }
    if (rc < 0) {
      numeric(c, 403, "%s :Kan het kanaal niet toevoegen (sleutel moet 32 of 64 hex zijn), code %d", name, rc);
      return;
    }
    idx = chanIndexFor(name);
    if (idx < 0) { numeric(c, 403, "%s :Kanaal niet gevonden na toevoegen", name); return; }
  }
  if (idx >= 32) { numeric(c, 403, "%s :Kanaalindex buiten bereik", name); return; }

  if (c.chan_mask & (1UL << idx)) return;   // al gejoind; stil
  c.chan_mask |= (1UL << idx);

  char cname[32]; chanIrcName(idx, cname, sizeof(cname));
  /* De JOIN echoën naar iedereen die het kanaal volgt -- ook lokaal, want het
   * mesh draagt geen aanwezigheid. */
  for (int k = 0; k < IRC_MAX_CLIENTS; k++)
    if (_cl[k].registered && (_cl[k].chan_mask & (1UL << idx)))
      raw(_cl[k], ":%s!%s JOIN %s", c.nick, IRC_MESH_USERHOST, cname);

  char nm[24]; int bits; bool en, derived, pub; uint8_t hash;
  _node->channelGet(idx, nm, &bits, &en, &hash, &derived, &pub);
  numeric(c, 332, "%s :%s-kanaal, %d-bit sleutel, kanaalhash %02X%s", cname,
          pub ? "publiek" : (derived ? "hashtag" : "prive"), bits, hash,
          en ? "" : " (UITGESCHAKELD -- 'channel on' op de CLI)");
  sendNames(c, idx);
  replayChannel(c, idx);
}

void IrcTask::partChannel(IrcClient& c, const char* name, const char* reason) {
  int idx = chanIndexFor(name);
  if (idx < 0 || !(c.chan_mask & (1UL << idx))) {
    numeric(c, 442, "%s :Je volgt dat kanaal niet", name ? name : "");
    return;
  }
  char cname[32]; chanIrcName(idx, cname, sizeof(cname));
  for (int k = 0; k < IRC_MAX_CLIENTS; k++)
    if (_cl[k].registered && (_cl[k].chan_mask & (1UL << idx)))
      raw(_cl[k], ":%s!%s PART %s :%s", c.nick, IRC_MESH_USERHOST, cname,
          reason && reason[0] ? reason : "");
  c.chan_mask &= ~(1UL << idx);
  /* Het kanaal zelf blijft op de node staan. PART is "ik lees even niet mee",
   * niet "gooi de sleutel weg" -- dat laatste is 'channel del' op de CLI. */
}

/* ------------------------------------------------------------------------ */
/*  Antwoorden                                                               */
/* ------------------------------------------------------------------------ */

/* De waarde van een tag uit "k=v;k2=v2". Retour NULL als hij er niet staat. */
static const char* ircTagValue(const char* tags, const char* key, char* buf, size_t buf_len) {
  size_t klen = strlen(key);
  for (const char* p = tags; p && *p; ) {
    const char* end = strchr(p, ';');
    size_t seg = end ? (size_t)(end - p) : strlen(p);
    if (seg > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
      size_t vlen = seg - klen - 1;
      if (vlen >= buf_len) vlen = buf_len - 1;
      memcpy(buf, p + klen + 1, vlen);
      buf[vlen] = 0;
      return buf;
    }
    p = end ? end + 1 : NULL;
  }
  return NULL;
}

/* ">origineel.. antwoord" bouwen. De quote wordt op IRC_QUOTE_LEN afgekapt en het
 * scheidingsteken staat er ALTIJD, ook als er niets afviel -- anders is bij het
 * teruglezen niet te zien waar de quote ophoudt. */
static void ircBuildQuote(const char* orig, const char* reply, char* out, size_t out_len) {
  char snip[IRC_QUOTE_LEN + 1];
  size_t n = 0;
  for (const char* p = orig; *p && n < IRC_QUOTE_LEN; p++) {
    if ((unsigned char)*p < ' ') break;          /* geen controltekens in de quote */
    snip[n++] = *p;
  }
  snip[n] = 0;
  snprintf(out, out_len, ">%s%s%s", snip, IRC_QUOTE_SEP, reply);
}

/* Begint deze mesh-regel met een quote? Zo ja: het origineel opzoeken in de ring en
 * het msgid teruggeven, met `body` op het antwoord gezet. 0 = geen quote. */
uint32_t IrcTask::quoteLookup(const char* text, const char** body) const {
  *body = text;
  if (!text || text[0] != '>') return 0;
  const char* sep = strstr(text + 1, IRC_QUOTE_SEP);
  if (!sep) return 0;
  char snip[IRC_QUOTE_LEN + 2];
  size_t n = (size_t)(sep - (text + 1));
  if (n == 0 || n >= sizeof(snip)) return 0;
  memcpy(snip, text + 1, n); snip[n] = 0;
  int i = logFindBySnippet(snip);
  if (i < 0) return 0;
  *body = sep + strlen(IRC_QUOTE_SEP);
  return _log[i].id;
}

/* ------------------------------------------------------------------------ */
/*  Zenden                                                                   */
/* ------------------------------------------------------------------------ */

bool IrcTask::txAllowed(IrcClient& c, char* why, size_t why_len) {
  unsigned long now = millis();
  if (c.last_tx_mesh && (now - c.last_tx_mesh) < IRC_TX_MIN_MS) {
    unsigned long wait = (IRC_TX_MIN_MS - (now - c.last_tx_mesh) + 999) / 1000;
    snprintf(why, why_len, "Te snel achter elkaar -- nog %lu s. LoRa-airtime is gedeeld.", wait);
    return false;
  }
  if (_bucket == 0) {
    snprintf(why, why_len, "De zendemmer van de node is leeg (%d per %lu s, over alle gebruikers). Probeer zo opnieuw.",
             (int)IRC_BUCKET_MAX, (unsigned long)(IRC_BUCKET_MS / 1000));
    return false;
  }
  return true;
}

void IrcTask::doPrivmsg(IrcClient& c, char* target, const char* text, bool is_notice,
                        const char* tags) {
  if (!target || !target[0]) { numeric(c, 411, ":Geen ontvanger opgegeven"); return; }
  if (!text || !text[0]) { numeric(c, 412, ":Geen tekst om te versturen"); return; }

  /* NOTICE gaat NOOIT het mesh op. Clients en bots sturen er automatische dingen
   * mee (away-antwoorden, CTCP-replies) en die horen geen airtime te kosten. */
  if (is_notice) return;

  /* CTCP (\001...\001) evenmin: ACTION zou nog kunnen, maar VERSION/PING/TIME
   * zijn client-onderhandeling en geen mesh-verkeer. Alleen ACTION laten we door
   * als gewone tekst met een sterretje ervoor. */
  char body[BOT_MAX_TEXT_LEN + 8];
  if (text[0] == '\001') {
    if (strncasecmp(text + 1, "ACTION ", 7) == 0) {
      const char* act = text + 8;
      size_t n = strlen(act);
      if (n && act[n - 1] == '\001') n--;
      snprintf(body, sizeof(body), "* %.*s", (int)n, act);
    } else {
      return;   // stil negeren; geen airtime
    }
  } else {
    StrHelper::strncpy(body, text, sizeof(body));
  }

  /* Een antwoord uit de client (+draft/reply=<msgid>) wordt hier een quote op de
   * draad. Kan het origineel niet meer gevonden worden -- uit de ring gerold --
   * dan gaat het bericht gewoon zonder quote weg; dat is beter dan weigeren. */
  char tagbuf[40];
  const char* rt = tags ? ircTagValue(tags, "+draft/reply", tagbuf, sizeof(tagbuf)) : NULL;
  if (!rt && tags) rt = ircTagValue(tags, "draft/reply", tagbuf, sizeof(tagbuf));
  if (rt) {
    uint32_t id = (uint32_t)strtoul(rt[0] == 'm' ? rt + 1 : rt, NULL, 16);
    int oi = logFindById(id);
    if (oi >= 0) {
      char q[BOT_MAX_TEXT_LEN + 8];
      ircBuildQuote(_log[oi].text, body, q, sizeof(q));
      StrHelper::strncpy(body, q, sizeof(body));
    } else {
      notice(c, "Het bericht waarop je antwoordt is uit de buffer gerold; "
                "je regel gaat zonder quote de lucht in.");
    }
  }

  char why[128];
  if (!txAllowed(c, why, sizeof(why))) { notice(c, "%s", why); return; }

  if (target[0] == '#') {
    int idx = chanIndexFor(target);
    if (idx < 0) { numeric(c, 403, "%s :Onbekend kanaal", target); return; }
    if (!(c.chan_mask & (1UL << idx))) { numeric(c, 404, "%s :Je volgt dat kanaal niet", target); return; }

    int rc = _node->botSay(c.bot, idx, body);
    if (rc < 0) { notice(c, "Versturen mislukt (code %d)", rc); return; }
    c.last_tx_mesh = millis();
    if (_bucket) _bucket--;

    /* Lokale weergalm: het mesh stuurt ons eigen group-pakket niet terug, dus de
     * andere sessies op deze node zouden het bericht anders nooit zien. */
    char cname[32]; chanIrcName(idx, cname, sizeof(cname));
    /* Ook onze EIGEN regel in de ring: wie later joint hoort het gesprek te zien
     * zoals het gevoerd is, niet met alleen de andere kant erin. */
    int mi = logAdd(idx, -1, _node->webBotSlotName(c.bot), body);
    const IrcLogEntry* me = (mi >= 0) ? &_log[mi] : NULL;
    /* Ook de afzender krijgt het msgid terug, zodat zijn eigen regel een geldig
     * antwoorddoel is -- in een echt IRC-netwerk komt je eigen bericht ook langs. */
    if (c.cap_tags && me) sendMsg(c, me, c.nick, cname, body, 0);
    for (int k = 0; k < IRC_MAX_CLIENTS; k++)
      if (&_cl[k] != &c && _cl[k].registered && (_cl[k].chan_mask & (1UL << idx)))
        sendMsg(_cl[k], me, c.nick, cname, body, 0);
    return;
  }

  /* DM. De nick oplossen naar een pubkey: bekende companion, buur uit de
   * advert-lijst, of gewoon een hex-pubkey als nick. */
  uint8_t pub[PUB_KEY_SIZE];
  char resolved[32];
  int rr = _node->ircResolveNick(target, pub, resolved, sizeof(resolved));
  if (rr < 0) {
    numeric(c, 401, "%s :Onbekende nick. Gebruik de nodenaam, of plak de volledige pubkey als nick.", target);
    return;
  }
  if (rr == 1)
    notice(c, "Meerdere nodes heten '%s'; ik gebruik %s. Plak de pubkey om zeker te zijn.", target, resolved);

  int rc = _node->botSendTo(c.bot, pub, body);
  if (rc < 0) { notice(c, "DM versturen mislukt (code %d)", rc); return; }
  c.last_tx_mesh = millis();
  if (_bucket) _bucket--;
  notice(c, "DM de lucht in naar %s (geen leesbevestiging op dit pad)", resolved);
}

/* ------------------------------------------------------------------------ */
/*  Commando's                                                               */
/* ------------------------------------------------------------------------ */

void IrcTask::handleLine(IrcClient& c, char* line) {
  /* IRCv3-tags staan VOOR het commando: "@k=v;k2=v2 PRIVMSG #x :hoi". We knippen
   * ze eraf en bewaren ze; de rest van deze functie ziet een gewone regel. */
  const char* tags = "";
  if (*line == '@') {
    char* sp = strchr(line, ' ');
    if (!sp) return;                    /* alleen tags, geen commando */
    *sp = 0;
    tags = line + 1;
    line = sp + 1;
    while (*line == ' ') line++;
  }

  char* argv[8];
  int argc = ircSplit(line, argv, 8);
  if (argc == 0) return;
  char* cmd = argv[0];
  for (char* p = cmd; *p; p++) *p = toupper((unsigned char)*p);

  /* --- Voor registratie --- */
  if (!strcmp(cmd, "CAP")) {
    const char* host = _node->getNodeName();
    if (argc >= 2 && !strcasecmp(argv[1], "LS")) {
      c.cap_pending = true;             /* wachten met registreren tot CAP END */
      raw(c, ":%s CAP * LS :message-tags server-time", host);
    } else if (argc >= 2 && !strcasecmp(argv[1], "LIST")) {
      char have[48]; snprintf(have, sizeof(have), "%s%s%s",
          c.cap_tags ? "message-tags" : "", (c.cap_tags && c.cap_time) ? " " : "",
          c.cap_time ? "server-time" : "");
      raw(c, ":%s CAP * LIST :%s", host, have);
    } else if (argc >= 2 && !strcasecmp(argv[1], "REQ")) {
      c.cap_pending = true;
      /* Alles-of-niets, zoals de specificatie eist: kennen we er een niet, dan NAK
       * op het HELE verzoek en zetten we ook de andere niet aan. */
      const char* want = argc >= 3 ? argv[2] : "";
      bool tagsq = strstr(want, "message-tags") != NULL;
      bool timeq = strstr(want, "server-time") != NULL;
      char rest[128]; StrHelper::strncpy(rest, want, sizeof(rest));
      bool unknown = false;
      for (char* p = strtok(rest, " "); p; p = strtok(NULL, " "))
        if (strcmp(p, "message-tags") && strcmp(p, "server-time")) unknown = true;
      if (unknown || (!tagsq && !timeq)) { raw(c, ":%s CAP * NAK :%s", host, want); return; }
      if (tagsq) c.cap_tags = true;
      if (timeq) c.cap_time = true;
      raw(c, ":%s CAP * ACK :%s", host, want);
    } else if (argc >= 2 && !strcasecmp(argv[1], "END")) {
      c.cap_pending = false;
      tryRegister(c);
    }
    return;
  }
  if (!strcmp(cmd, "PASS")) {
    if (c.registered) { numeric(c, 462, ":Je bent al ingelogd"); return; }
    if (argc < 2) { numeric(c, 461, "PASS :Te weinig parameters"); return; }
    StrHelper::strncpy(c.pass, argv[1], sizeof(c.pass));
    c.have_pass = true;
    return;
  }
  if (!strcmp(cmd, "NICK")) {
    if (argc < 2) { numeric(c, 431, ":Geen nick opgegeven"); return; }
    if (!nickValid(argv[1])) { numeric(c, 432, "%s :Ongeldige nick", argv[1]); return; }
    if (c.registered) {
      /* Van nick wisselen zou van MeshCore-identiteit wisselen betekenen, en die
       * zit vast aan het account. Weigeren met de reden erbij. */
      numeric(c, 484, "%s :Je nick is aan je mesh-identiteit gebonden en kan niet wijzigen", argv[1]);
      return;
    }
    StrHelper::strncpy(c.nick, argv[1], sizeof(c.nick));
    tryRegister(c);
    return;
  }
  if (!strcmp(cmd, "USER")) {
    if (c.registered) { numeric(c, 462, ":Je bent al ingelogd"); return; }
    if (argc < 2) { numeric(c, 461, "USER :Te weinig parameters"); return; }
    StrHelper::strncpy(c.user, argv[1], sizeof(c.user));
    tryRegister(c);
    return;
  }
  if (!strcmp(cmd, "QUIT")) { closeClient(c, argc >= 2 ? argv[1] : "Tot ziens"); return; }
  if (!strcmp(cmd, "PING")) { raw(c, ":%s PONG %s :%s", _node->getNodeName(), _node->getNodeName(), argc >= 2 ? argv[1] : ""); return; }
  if (!strcmp(cmd, "PONG")) return;

  if (!c.registered) { numeric(c, 451, ":Je bent nog niet ingelogd"); return; }

  /* --- Na registratie --- */
  if (!strcmp(cmd, "JOIN")) {
    if (argc < 2) { numeric(c, 461, "JOIN :Te weinig parameters"); return; }
    /* "JOIN #a,#b sleutel1,sleutel2" -- de komma-vorm die elke client kent. */
    char* chans = argv[1];
    char* keys  = argc >= 3 ? argv[2] : NULL;
    char* cs = chans;
    while (cs && *cs) {
      char* cnext = strchr(cs, ','); if (cnext) *cnext++ = 0;
      char* k = NULL;
      if (keys && *keys) { k = keys; char* knext = strchr(keys, ','); if (knext) { *knext = 0; keys = knext + 1; } else keys = NULL; }
      joinChannel(c, cs, k);
      cs = cnext;
    }
    return;
  }
  if (!strcmp(cmd, "PART")) {
    if (argc < 2) { numeric(c, 461, "PART :Te weinig parameters"); return; }
    char* cs = argv[1];
    while (cs && *cs) {
      char* cnext = strchr(cs, ','); if (cnext) *cnext++ = 0;
      partChannel(c, cs, argc >= 3 ? argv[2] : NULL);
      cs = cnext;
    }
    return;
  }
  if (!strcmp(cmd, "PRIVMSG") || !strcmp(cmd, "NOTICE")) {
    if (argc < 3) { numeric(c, 461, "%s :Te weinig parameters", cmd); return; }
    doPrivmsg(c, argv[1], argv[2], cmd[0] == 'N', tags);
    return;
  }
  if (!strcmp(cmd, "NAMES")) {
    if (argc >= 2) { int i = chanIndexFor(argv[1]); if (i >= 0) sendNames(c, i); else numeric(c, 403, "%s :Onbekend kanaal", argv[1]); }
    else for (int i = 0; i < _node->webChannelMax(); i++) if (c.chan_mask & (1UL << i)) sendNames(c, i);
    return;
  }
  if (!strcmp(cmd, "LIST")) {
    numeric(c, 321, "Kanaal :Gebruikers  Onderwerp");
    for (int i = 0; i < _node->webChannelMax(); i++) {
      char nm[24]; int bits; bool en, derived, pub; uint8_t hash;
      if (!_node->channelGet(i, nm, &bits, &en, &hash, &derived, &pub)) continue;
      char cn[36];
      if (!chanIrcName(i, cn, sizeof(cn))) continue;
      int here = 0;
      for (int k = 0; k < IRC_MAX_CLIENTS; k++) if (_cl[k].registered && (_cl[k].chan_mask & (1UL << i))) here++;
      /* De naam op de node erbij als de IRC-vorm ervan afwijkt -- anders zie je
       * "#NodeNet_FireMesh_Limbur" en vind je hem nergens terug in 'channel list'. */
      bool mangled = (strcasecmp(cn + 1, nm) != 0) && (strcasecmp(cn + 1, nm[0] == '#' ? nm + 1 : nm) != 0);
      numeric(c, 322, "%s %d :%s, %d-bit, hash %02X%s%s%s", cn, here,
              pub ? "publiek" : (derived ? "hashtag" : "prive"), bits, hash,
              en ? "" : ", uit", mangled ? ", op de node: " : "", mangled ? nm : "");
    }
    numeric(c, 323, ":Einde van /LIST");
    return;
  }
  if (!strcmp(cmd, "TOPIC")) {
    if (argc < 2) { numeric(c, 461, "TOPIC :Te weinig parameters"); return; }
    if (argc >= 3) { numeric(c, 482, "%s :Een mesh-kanaal heeft geen onderwerp", argv[1]); return; }
    int idx = chanIndexFor(argv[1]);
    if (idx < 0) { numeric(c, 403, "%s :Onbekend kanaal", argv[1]); return; }
    char nm[24]; int bits; bool en, derived, pub; uint8_t hash;
    _node->channelGet(idx, nm, &bits, &en, &hash, &derived, &pub);
    numeric(c, 332, "%s :%s-kanaal, %d-bit sleutel, kanaalhash %02X", argv[1],
            pub ? "publiek" : (derived ? "hashtag" : "prive"), bits, hash);
    return;
  }
  if (!strcmp(cmd, "MODE")) {
    if (argc < 2) { numeric(c, 461, "MODE :Te weinig parameters"); return; }
    if (argv[1][0] != '#') { numeric(c, 221, "+"); return; }
    int idx = chanIndexFor(argv[1]);
    if (idx < 0) { numeric(c, 403, "%s :Onbekend kanaal", argv[1]); return; }
    if (argc >= 3) { numeric(c, 482, "%s :Kanaalinstellingen gaan via de node-CLI ('channel ...'), niet via MODE", argv[1]); return; }
    char nm[24]; int bits; bool en, derived, pub; uint8_t hash;
    _node->channelGet(idx, nm, &bits, &en, &hash, &derived, &pub);
    /* De sleutel zelf NOOIT teruggeven -- IRC is onversleuteld en een MODE-antwoord
     * belandt in elke client-log. Alleen of er een is. */
    numeric(c, 324, "%s %s", argv[1], derived ? "+" : "+k");
    return;
  }
  if (!strcmp(cmd, "WHOIS")) {
    if (argc < 2) { numeric(c, 431, ":Geen nick opgegeven"); return; }
    /* Eerst de lokale sessies. */
    for (int k = 0; k < IRC_MAX_CLIENTS; k++) {
      if (!_cl[k].registered || strcasecmp(_cl[k].nick, argv[1]) != 0) continue;
      char pub[PUB_KEY_SIZE * 2 + 1] = {0};
      _node->webBotSlotPubHex(_cl[k].bot, pub, sizeof(pub));
      numeric(c, 311, "%s %s %s * :%s (bot-slot %d op deze node)", _cl[k].nick,
              IRC_MESH_USERHOST, "mesh", _node->webBotSlotName(_cl[k].bot), _cl[k].bot);
      numeric(c, 320, "%s :pubkey %s", _cl[k].nick, pub);
      numeric(c, 318, "%s :Einde van /WHOIS", _cl[k].nick);
      return;
    }
    /* Anders: een mesh-node uit de buurtlijst of de companion-store. */
    char info[200];
    if (_node->ircWhois(argv[1], info, sizeof(info))) {
      numeric(c, 311, "%s %s * :mesh-node", argv[1], IRC_MESH_USERHOST);
      numeric(c, 320, "%s :%s", argv[1], info);
      numeric(c, 318, "%s :Einde van /WHOIS", argv[1]);
    } else {
      numeric(c, 401, "%s :Die node heb ik nog niet gehoord", argv[1]);
      numeric(c, 318, "%s :Einde van /WHOIS", argv[1]);
    }
    return;
  }
  if (!strcmp(cmd, "WHO")) {
    if (argc >= 2 && argv[1][0] == '#') {
      int idx = chanIndexFor(argv[1]);
      if (idx >= 0)
        for (int k = 0; k < IRC_MAX_CLIENTS; k++)
          if (_cl[k].registered && (_cl[k].chan_mask & (1UL << idx)))
            numeric(c, 352, "%s mesh mesh %s %s H :0 %s", argv[1], _node->getNodeName(),
                    _cl[k].nick, _node->webBotSlotName(_cl[k].bot));
      numeric(c, 315, "%s :Einde van /WHO", argv[1]);
    } else {
      numeric(c, 315, "%s :Einde van /WHO", argc >= 2 ? argv[1] : "*");
    }
    return;
  }
  if (!strcmp(cmd, "ISON")) {
    char out[200]; int o = 0; out[0] = 0;
    for (int i = 1; i < argc; i++)
      for (int k = 0; k < IRC_MAX_CLIENTS; k++)
        if (_cl[k].registered && !strcasecmp(_cl[k].nick, argv[i]))
          o += snprintf(out + o, sizeof(out) - o, "%s%s", o ? " " : "", _cl[k].nick);
    numeric(c, 303, ":%s", out);
    return;
  }
  if (!strcmp(cmd, "MOTD")) { sendWelcome(c); return; }
  if (!strcmp(cmd, "AWAY")) { numeric(c, argc >= 2 ? 306 : 305, ":Afwezigheid bestaat niet op het mesh"); return; }
  if (!strcmp(cmd, "USERHOST")) { numeric(c, 302, ":"); return; }
  if (!strcmp(cmd, "LUSERS")) {
    numeric(c, 251, ":%d van %d sessies bezet, %d account(s), %d kanalen",
            numClients(), IRC_MAX_CLIENTS, acctCount(), _node->webChannelCount());
    return;
  }

  numeric(c, 421, "%s :Onbekend commando", cmd);
}

/* ------------------------------------------------------------------------ */
/*  Mesh -> IRC                                                              */
/* ------------------------------------------------------------------------ */

void IrcTask::onMeshChannelText(int chan_idx, const char* sender, const char* text,
                                float snr, int rssi, int hops) {
  if (!_running || chan_idx < 0 || chan_idx >= 32 || !text || !text[0]) return;

  /* Van onszelf? Dan is het al lokaal weergalmd bij het verzenden. Zou hij hier
   * alsnog binnenkomen (een repeater die ons eigen pakket terugkaatst), dan zou
   * de afzender zijn eigen regel dubbel zien. */
  for (int k = 0; k < MAX_BOTS; k++) {
    if (!_node->webBotSlotUsed(k)) continue;
    const char* bn = _node->webBotSlotName(k);
    if (bn && bn[0] && sender && strcasecmp(bn, sender) == 0) return;
  }

  char nick[IRC_NICK_MAX + 1];
  sanitizeNick(sender && sender[0] ? sender : "mesh", nick, sizeof(nick));
  char cname[32];
  if (!chanIrcName(chan_idx, cname, sizeof(cname))) return;

  /* Eerst de ledenlijst bijwerken: wie nu zendt is "aanwezig", en de JOIN moet
   * VOOR het bericht komen -- een PRIVMSG van iemand die de client niet in het
   * kanaal ziet staan, laten sommige clients in een apart venster belanden. */
  seenTouch(chan_idx, nick);
  /* Een binnenkomende regel die met een quote begint koppelen we terug aan het
   * origineel in de ring; clients met message-tags krijgen er dan echte threading
   * van. De quote blijft WEL in de tekst staan -- wie de tag niet snapt (en dat is
   * ook iedereen in de MeshCore-app) moet hem gewoon kunnen lezen. */
  const char* body = text;
  uint32_t rt = quoteLookup(text, &body);

  int li = logAdd(chan_idx, -1, nick, text);
  const IrcLogEntry* le = (li >= 0) ? &_log[li] : NULL;

  for (int k = 0; k < IRC_MAX_CLIENTS; k++) {
    IrcClient& c = _cl[k];
    if (!c.registered || !(c.chan_mask & (1UL << chan_idx))) continue;
    sendMsg(c, le, nick, cname, text, rt);
    /* Het signaalrapport apart, als NOTICE in hetzelfde kanaalvenster. Zo blijft
     * de gespreksregel schoon en is toch te zien hoe het pakket binnenkwam. */
    raw(c, ":%s!%s NOTICE %s :[%s SNR %.1f dB, RSSI %d dBm, %d hop%s]",
        nick, IRC_MESH_USERHOST, cname, nick, snr, rssi, hops, hops == 1 ? "" : "s");
  }
}

void IrcTask::onMeshDm(int bot_idx, const uint8_t* sender_pub, const char* sender_name,
                       const char* text, float snr, int rssi, int hops) {
  if (!_running || bot_idx < 0 || !text || !text[0]) return;

  char nick[IRC_NICK_MAX + 1];
  if (sender_name && sender_name[0]) {
    sanitizeNick(sender_name, nick, sizeof(nick));
  } else {
    /* Geen naam bekend -> de pubkey-prefix als nick. Kort genoeg om te typen en
     * uniek genoeg om niet met een andere node te verwarren. */
    char hex[PUB_KEY_SIZE * 2 + 1];
    mesh::Utils::toHex(hex, sender_pub, PUB_KEY_SIZE);
    snprintf(nick, sizeof(nick), "%.12s", hex);
  }

  const char* dbody = text;
  uint32_t drt = quoteLookup(text, &dbody);
  int dli = logAdd(IRC_LOG_DM, bot_idx, nick, text);
  const IrcLogEntry* dle = (dli >= 0) ? &_log[dli] : NULL;

  for (int k = 0; k < IRC_MAX_CLIENTS; k++) {
    IrcClient& c = _cl[k];
    if (!c.registered || c.bot != bot_idx) continue;
    sendMsg(c, dle, nick, c.nick, text, drt);
    raw(c, ":%s!%s NOTICE %s :[SNR %.1f dB, RSSI %d dBm, %d hop%s]",
        nick, IRC_MESH_USERHOST, c.nick, snr, rssi, hops, hops == 1 ? "" : "s");
  }
}

#endif  /* ESP32 */
