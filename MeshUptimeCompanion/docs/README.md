# MeshUptimeCompanion — documentatie

Companion-firmware voor de **Seeed T1000-E**, die het toestel als draagbare
annunciator/afstandsbediening op het MeshUptime-mesh zet. Zie de hoofd-repo
voor MeshUptime zelf: [../../README.md](../../README.md).

**Status: dit is ontwerpdocumentatie.** De firmware wordt parallel gebouwd;
deze bestanden leggen vast waar hij naartoe gebouwd wordt, niet wat al op een
toestel gezien is. Onzekere punten staan expliciet als zodanig gemarkeerd in
elk bestand, niet weggemoffeld.

- **[overzicht.md](overzicht.md)** — wat het is, de hardware (nRF52840,
  LR1110, RTTTL-buzzer, QMA6100P-accelerometer, groene LED, knop, GPS, kleine
  LiPo), en hoe het in het ecosysteem past: MeshUptime-node → alert-DM (met
  severity-emoji) → T1000-E speelt een tune; MeshManager → Send-DM →
  T1000-E voert een commando uit.
- **[protocol.md](protocol.md)** — de twee contracten: de severity-marker
  (🔴/🟠/🟢, met de UTF-8-bytes) en de volledige DM-/CLI-commandoset
  (`!find`, `!mute`, `!vol`, `!tune`, `!quiet`, `!gps`, `!loc`, `!cfg`,
  `!allow`, `!preset`, `!fall`, `!ping`), plus de allowlist die DM-commando's
  beveiligt.
- **[flashen.md](flashen.md)** — de sleutel-veilige flashprocedure: eerst een
  volledige back-up via de nRF52-bootloader (`CURRENT.UF2`), dan alleen de
  app-regio herschrijven, dan de public key vergelijken. Met de expliciete
  waarschuwing: **nooit** een volledige chip-erase.
- **[gebruik.md](gebruik.md)** — dagelijks gebruik: de deuntjes per severity,
  wat de knop doet, stille uren, vind-me, valdetectie, stroombudget, en
  bediening vanuit MeshManager (Send-DM-tab, companion-helpers, browser
  Web-Serial).
