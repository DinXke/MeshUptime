#include "UITask.h"
#include <Arduino.h>
#include <helpers/CommonCLI.h>
#include <target.h>              // board, radio_driver, en de globale `sensors`
#include "MonitorSensors.h"
#include "Branding.h"

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "v1.17.0"   // MeshCore-versie; branding staat in Branding.h
#endif

#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif

#define AUTO_OFF_MILLIS      0       // 0 = scherm blijft aan (bewakingsnode)
#define BOOT_SCREEN_MILLIS   4000
#define SLIDE_MS             4000    // rustig doorschuiven
#define REFRESH_MS           1000

/* Layout. UI_TOP > 0: NIETS op y=0 -- op de Heltec V3 sneed de bovenrand soms een
 * regel af. Alles wordt bovendien als BLOK verticaal gecentreerd, zodat er noch
 * boven noch onder iets buiten de 64 px valt. */
#define UI_TOP    3
#define LINE_H    11
#define SCR_W     128
#define SCR_H     64

#ifdef ROOM_SERVER_VARIANT
  #define NODE_TYPE_STR  "< Room Server >"
#else
  #define NODE_TYPE_STR  "< Sensor >"
#endif

void UITask::begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version) {
  (void)firmware_version;   // branding komt uit Branding.h, niet uit de meegegeven tekst
  _prevBtnState = HIGH;
  _auto_off = AUTO_OFF_MILLIS ? (millis() + AUTO_OFF_MILLIS) : 0;
  _node_prefs = node_prefs;
  _display->turnOn();
  snprintf(_version_info, sizeof(_version_info), "%s", build_date);
}

/* Verzamelt de neer-staande items (voor het STORING-scherm en om te beslissen of
 * de slideshow onderbroken wordt). Geeft het aantal terug; vult hoogstens
 * max_lines regels van <=21 tekens. */
int UITask::collectAlerts(char lines[][22], int max_lines) {
  int n = 0;
  float v = (float)board.getBattMilliVolts() / 1000.0f;
  if (v < 3.6f && n < max_lines) { snprintf(lines[n++], 22, "accu laag %.2fV", v); }
  /* fixedIsDown() (gedebounced) i.p.v. de rauwe isMains()/isWifiOnline(): het
   * STORING-scherm en het alarmpad tonen zo DEZELFDE stabiele toestand -- geen
   * "storing weg" op het scherm terwijl er net een "terug" de deur uit ging. */
  if (sensors.fixedIsDown(MonitorSensors::FIXED_POWER) && n < max_lines) { snprintf(lines[n++], 22, "netvoeding weg"); }
  if (sensors.fixedIsDown(MonitorSensors::FIXED_WIFI) && n < max_lines) { snprintf(lines[n++], 22, "wifi weg"); }
  for (int i = 0; i < MonitorSensors::MAX_MONITORS && n < max_lines; i++) {
    if (sensors.monitorUsed(i) && sensors.monitorSeeded(i) &&
        !sensors.monitorsPaused() && !sensors.monitorIsUp(i)) {
      snprintf(lines[n++], 22, "%s neer", sensors.monitorName(i));
    }
  }
  return n;
}

/* Tekent een titel (gecentreerd) plus regels (links uitgelijnd), als BLOK
 * verticaal gecentreerd binnen 64 px. */
static void drawBlock(DisplayDriver* d, const char* title, char lines[][22], int nlines) {
  int rows = (title ? 1 : 0) + nlines;
  int total = rows * LINE_H;
  int y = (SCR_H - total) / 2;
  if (y < UI_TOP) y = UI_TOP;

  d->setTextSize(1);
  if (title) {
    d->setColor(UIColor::corp_blue);
    uint16_t w = d->getTextWidth(title);
    int x = (SCR_W - (int)w) / 2; if (x < 0) x = 0;
    d->setCursor(x, y);
    d->print(title);
    y += LINE_H;
  }
  d->setColor(UIColor::primary_txt);
  for (int i = 0; i < nlines; i++) {
    d->setCursor(4, y);
    d->print(lines[i]);
    y += LINE_H;
  }
}

void UITask::renderCurrScreen() {
  char lines[6][22];

  // ---- bootscherm: MeshUptime-branding + MeshCore-versie ----
  if (millis() < BOOT_SCREEN_MILLIS) {
    snprintf(lines[0], 22, "MeshUptime %s", MESHUPTIME_VERSION);
    snprintf(lines[1], 22, "by %s", MESHUPTIME_AUTHOR);
    snprintf(lines[2], 22, "MeshCore %s", FIRMWARE_VERSION);
    snprintf(lines[3], 22, "%s", NODE_TYPE_STR);
    drawBlock(_display, NULL, lines, 4);
    return;
  }

  // ---- STORING-scherm: heeft voorrang op de slideshow ----
  int na = collectAlerts(lines, 5);
  if (na > 0) {
    char title[22];
    snprintf(title, sizeof(title), "STORING (%d)", na);
    drawBlock(_display, title, lines, na);
    return;
  }

  // ---- slideshow: bereken het aantal pagina's en wrap _page ----
  int nused = 0;
  for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) if (sensors.monitorUsed(i)) nused++;
  int mon_pages = (nused + 2) / 3;             // 3 monitors per pagina
  int total_pages = 2 + mon_pages;             // 0=info, 1=voeding, rest=monitors
  int page = (total_pages > 0) ? (_page % total_pages) : 0;

  if (page == 0) {
    char title[22];
    snprintf(title, 22, "%s", _node_prefs->node_name);
    snprintf(lines[0], 22, "%s", NODE_TYPE_STR);
    snprintf(lines[1], 22, "MeshCore %s", FIRMWARE_VERSION);
    snprintf(lines[2], 22, "F%.3f SF%d", _node_prefs->freq, (int)_node_prefs->sf);
    snprintf(lines[3], 22, "BW%.0f CR%d", _node_prefs->bw, (int)_node_prefs->cr);
    drawBlock(_display, title, lines, 4);
  } else if (page == 1) {
    float v = (float)board.getBattMilliVolts() / 1000.0f;
    snprintf(lines[0], 22, "accu   %.2f V", v);
    snprintf(lines[1], 22, "net    %s", sensors.isMains() ? "aan" : "UIT");
    snprintf(lines[2], 22, "wifi   %s", sensors.isWifiOnline() ? "online" : "WEG");
    drawBlock(_display, "Voeding / net", lines, 3);
  } else {
    int mp = page - 2;                 // welke monitorpagina
    int shown = 0, skipped = 0;
    int start = mp * 3;
    char title[22];
    snprintf(title, 22, "Monitors %d/%d", mp + 1, mon_pages);
    for (int i = 0; i < MonitorSensors::MAX_MONITORS && shown < 3; i++) {
      if (!sensors.monitorUsed(i)) continue;
      if (skipped < start) { skipped++; continue; }
      const char* st;
      if (sensors.monitorsPaused()) st = "pauze";
      else if (!sensors.monitorSeeded(i)) st = "?";
      else st = sensors.monitorIsUp(i) ? "op" : "NEER";
      snprintf(lines[shown], 22, "%-9.9s %s %lu", sensors.monitorName(i), st,
               (unsigned long)sensors.monitorPingMs(i));
      shown++;
    }
    if (shown == 0) snprintf(lines[shown++], 22, "(geen monitors)");
    drawBlock(_display, title, lines, shown);
  }
}

void UITask::loop() {
#ifdef PIN_USER_BTN
  if (millis() >= _next_read) {
    int btnState = digitalRead(PIN_USER_BTN);
    if (btnState != _prevBtnState) {
      if (btnState == USER_BTN_PRESSED) {
        if (_display->isOn()) {
          _page++;                 // knopdruk = volgende pagina
          _next_slide = millis() + SLIDE_MS;
        } else {
          _display->turnOn();
        }
        if (AUTO_OFF_MILLIS) _auto_off = millis() + AUTO_OFF_MILLIS;
      }
      _prevBtnState = btnState;
    }
    _next_read = millis() + 200;
  }
#endif

  if (millis() >= _next_slide) {
    _page++;
    _next_slide = millis() + SLIDE_MS;
  }

  if (_display->isOn()) {
    if (millis() >= _next_refresh) {
      _display->startFrame();
      renderCurrScreen();
      _display->endFrame();
      _next_refresh = millis() + REFRESH_MS;
    }
    if (AUTO_OFF_MILLIS && _auto_off && millis() > _auto_off) {
      _display->turnOff();
    }
  }
}
