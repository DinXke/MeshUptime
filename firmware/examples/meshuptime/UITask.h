#pragma once

#include <helpers/ui/DisplayDriver.h>
#include <helpers/CommonCLI.h>

/* UITask -- het OLED-scherm (Heltec V3, 128x64 SSD1306).
 *
 * Drie schermsoorten:
 *  - bootscherm: MeshUptime-branding + MeshCore-versie (BOOT_SCREEN_MILLIS lang);
 *  - STORING-scherm: staat er iets neer (monitor down / netvoeding weg / wifi weg
 *    / batterij laag), dan krijgt dat voorrang op de slideshow;
 *  - slideshow: draait rustig door telemetrie-/statuspagina's (radio, voeding,
 *    en per monitor up/down + ping-ms).
 *
 * De sensor-/monitortoestand komt uit dezelfde `sensors` (MonitorSensors) die ook
 * de alerts aanstuurt, zodat scherm en alarm nooit uit elkaar lopen.
 */
class UITask {
  DisplayDriver* _display;
  unsigned long _next_read, _next_refresh, _auto_off, _next_slide;
  int _prevBtnState;
  NodePrefs* _node_prefs;
  char _version_info[48];
  int  _page;

  void renderCurrScreen();
  int  collectAlerts(char lines[][22], int max_lines);   // aantal neer-items
public:
  UITask(DisplayDriver& display) : _display(&display) {
    _next_read = _next_refresh = _next_slide = 0; _page = 0;
  }
  void begin(NodePrefs* node_prefs, const char* build_date, const char* firmware_version);

  void loop();
};
