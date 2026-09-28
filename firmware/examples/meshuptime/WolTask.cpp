#include "WolTask.h"

#include <string.h>
#include <ctype.h>
#ifdef ESP32
  #include <WiFi.h>
  #include <WiFiUdp.h>
  #include <lwip/etharp.h>   /* IP -> MAC: de ARP-tabel van de node zelf */
  #include <lwip/netif.h>
#endif

#define WOL_FILE  "/wol_mac"
#define WOL_PORT  9

static fs::FS* _fs = nullptr;
static uint8_t _mac[6];
static bool    _gezet = false;

// ------------------------------------------------------------------ opslag
static void wolSave() {
  if (_fs == nullptr) return;
  if (!_gezet) { if (_fs->exists(WOL_FILE)) _fs->remove(WOL_FILE); return; }
  fs::File f = _fs->open(WOL_FILE, "w", true);
  if (!f) return;
  for (int i = 0; i < 6; i++) f.printf("%02x", _mac[i]);
  f.print('\n');
  f.close();
}

void wol_begin(fs::FS& fs) {
  _fs = &fs;
  _gezet = false;
  if (!fs.exists(WOL_FILE)) return;
  fs::File f = fs.open(WOL_FILE, "r");
  if (!f) return;
  char line[20];
  size_t n = 0;
  while (f.available() && n < sizeof(line) - 1) {
    int ch = f.read();
    if (ch < 0 || ch == '\n' || ch == '\r') break;
    line[n++] = (char)ch;
  }
  line[n] = 0;
  f.close();
  wol_set_mac(line);        /* schrijft niet terug: zelfde inhoud */
}

// ------------------------------------------------------------- MAC lezen
bool wol_set_mac(const char* txt) {
  if (txt == nullptr) { _gezet = false; wolSave(); return true; }

  /* Scheidingstekens weg: ':', '-' en '.' mogen er allemaal in staan, want
   * iedere bron schrijft een MAC weer anders op. */
  char hex[16];
  size_t h = 0;
  for (const char* p = txt; *p && h < sizeof(hex) - 1; p++) {
    if (*p == ':' || *p == '-' || *p == '.' || *p == ' ') continue;
    hex[h++] = (char)tolower((unsigned char)*p);
  }
  hex[h] = 0;

  if (h == 0 || strcmp(hex, "uit") == 0) { _gezet = false; wolSave(); return true; }
  if (h != 12) return false;

  uint8_t nieuw[6];
  for (int i = 0; i < 6; i++) {
    char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
    char* eind = nullptr;
    long v = strtol(b, &eind, 16);
    if (eind != b + 2) return false;
    nieuw[i] = (uint8_t)v;
  }
  memcpy(_mac, nieuw, 6);
  _gezet = true;
  wolSave();
  return true;
}

void wol_mac_text(char* uit, size_t max) {
  if (max == 0) return;
  if (!_gezet) { uit[0] = 0; return; }
  snprintf(uit, max, "%02x:%02x:%02x:%02x:%02x:%02x",
           _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
}

bool wol_have_mac() { return _gezet; }

/* IP -> MAC via ARP.
 *
 * WAAROM DIT ER IS. Wake-on-LAN kan niet op IP werken: een slapende kaart
 * heeft geen IP-stack en herkent alleen zijn eigen MAC in het magic packet.
 * Maar zolang de pc nog WAKKER is, kunnen we dat MAC gewoon opvragen en
 * bewaren -- daarna werkt wekken ook als hij slaapt.
 *
 * Dit blokkeert de lus tot een seconde. Dat is hier aanvaard: het is een
 * commando dat iemand bewust intypt, geen achtergrondtaak. */
bool wol_resolve_ip(const char* ip_txt, char* uit, size_t max) {
#ifdef ESP32
  if (WiFi.status() != WL_CONNECTED) { snprintf(uit, max, "geen wifi"); return false; }

  IPAddress ip;
  if (!ip.fromString(ip_txt)) { snprintf(uit, max, "Err - geen geldig IP-adres"); return false; }

  ip4_addr_t doel;
  IP4_ADDR(&doel, ip[0], ip[1], ip[2], ip[3]);

  struct netif* nif = netif_default;
  if (nif == nullptr) { snprintf(uit, max, "geen netwerkinterface"); return false; }

  struct eth_addr* eth = nullptr;
  const ip4_addr_t* gevonden = nullptr;

  /* Eerst kijken of hij al in de tabel staat; anders vragen en even wachten. */
  if (etharp_find_addr(nif, &doel, &eth, &gevonden) < 0) {
    etharp_request(nif, &doel);
    for (int i = 0; i < 20; i++) {
      delay(50);
      if (etharp_find_addr(nif, &doel, &eth, &gevonden) >= 0) break;
      eth = nullptr;
    }
  }

  if (eth == nullptr) {
    snprintf(uit, max, "%s antwoordt niet op ARP (staat hij aan en op dit subnet?)", ip_txt);
    return false;
  }

  memcpy(_mac, eth->addr, 6);
  _gezet = true;
  wolSave();
  snprintf(uit, max, "OK - %s is %02x:%02x:%02x:%02x:%02x:%02x, bewaard",
           ip_txt, _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
  return true;
#else
  snprintf(uit, max, "alleen op ESP32");
  return false;
#endif
}

// ------------------------------------------------------------- versturen
bool wol_send(char* uit, size_t max) {
#ifdef ESP32
  if (!_gezet) { snprintf(uit, max, "geen bestemming ingesteld (wol set <mac>)"); return false; }
  if (WiFi.status() != WL_CONNECTED) { snprintf(uit, max, "geen wifi"); return false; }

  /* Het SUBNET-broadcastadres, niet 255.255.255.255: dat laatste wordt door
   * menig stack en switch geweigerd, en dan verdwijnt het pakket zonder fout. */
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress bc(ip[0] | ~mask[0], ip[1] | ~mask[1], ip[2] | ~mask[2], ip[3] | ~mask[3]);

  uint8_t pak[102];
  memset(pak, 0xFF, 6);
  for (int i = 0; i < 16; i++) memcpy(&pak[6 + i * 6], _mac, 6);

  WiFiUDP udp;
  if (!udp.begin(0)) { snprintf(uit, max, "geen udp-socket"); return false; }
  bool ok = udp.beginPacket(bc, WOL_PORT) &&
            udp.write(pak, sizeof(pak)) == sizeof(pak) &&
            udp.endPacket();
  udp.stop();

  if (!ok) { snprintf(uit, max, "verzenden mislukt"); return false; }
  snprintf(uit, max, "magic packet naar %02x:%02x:%02x:%02x:%02x:%02x via %u.%u.%u.%u:%d",
           _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5],
           bc[0], bc[1], bc[2], bc[3], WOL_PORT);
  return true;
#else
  snprintf(uit, max, "alleen op ESP32");
  return false;
#endif
}

// ------------------------------------------------------------------- CLI
bool wol_handle_command(const char* cmd, char* reply, size_t reply_max) {
  if (cmd == nullptr || memcmp(cmd, "wol", 3) != 0) return false;
  if (cmd[3] != 0 && cmd[3] != ' ') return false;      /* 'wolf' is niet van ons */

  const char* a = cmd + 3;
  while (*a == ' ') a++;

  char mac[24];

  if (*a == 0) {                       /* kaal 'wol' -> sturen */
    wol_send(reply, reply_max);
    return true;
  }

  if (strncmp(a, "ip", 2) == 0 && (a[2] == 0 || a[2] == ' ')) {
    const char* m = a + 2;
    while (*m == ' ') m++;
    if (*m == 0) { snprintf(reply, reply_max, "wol ip <adres>  (zoekt het MAC op en bewaart het)"); return true; }
    wol_resolve_ip(m, reply, reply_max);
    return true;
  }

  /* Een kaal IP-adres is ook duidelijk genoeg: cijfers en punten, geen hex. */
  {
    bool lijkt_ip = true; int punten = 0;
    for (const char* q = a; *q; q++) {
      if (*q == '.') { punten++; continue; }
      if (*q < '0' || *q > '9') { lijkt_ip = false; break; }
    }
    if (lijkt_ip && punten == 3) { wol_resolve_ip(a, reply, reply_max); return true; }
  }

  if (strncmp(a, "set", 3) == 0 && (a[3] == 0 || a[3] == ' ')) {
    const char* m = a + 3;
    while (*m == ' ') m++;
    if (*m == 0) {
      wol_mac_text(mac, sizeof(mac));
      snprintf(reply, reply_max, "wol %s", _gezet ? mac : "uit");
      return true;
    }
    if (!wol_set_mac(m)) { snprintf(reply, reply_max, "Err - mac verwacht 12 hexcijfers"); return true; }
    wol_mac_text(mac, sizeof(mac));
    snprintf(reply, reply_max, "OK - wol %s", _gezet ? mac : "uit");
    return true;
  }

  /* 'wol <mac>': onthouden EN meteen sturen -- dat is wat je bedoelt als je
   * een adres intypt. */
  if (!wol_set_mac(a)) { snprintf(reply, reply_max, "Err - mac verwacht 12 hexcijfers"); return true; }
  if (!_gezet) { snprintf(reply, reply_max, "OK - wol uit"); return true; }
  wol_send(reply, reply_max);
  return true;
}
