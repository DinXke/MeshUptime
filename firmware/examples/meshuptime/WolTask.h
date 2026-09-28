#pragma once

#include <Arduino.h>
#include <FS.h>

/* Wake-on-LAN: een pc wekken vanaf de mesh.
 *
 * WAAROM DIT HIER ZIT. Een node aan de USB van een slapende pc kan die niet
 * wekken: hij hangt aan een CP210x seriële brug, en die staat niet in
 * `powercfg /devicequery wake_from_any` -- een UART-brug kent geen
 * remote-wakeup. Wat wel kan, is de netwerkkant: de node zit op hetzelfde
 * subnet en mag daar een magic packet op zetten. Dat is één UDP-broadcast en
 * vraagt geen enkele medewerking van de pc behalve dat zijn netwerkkaart
 * gewapend staat.
 *
 * Het MAC-adres blijft bewaard op de filesystem, zodat 'wol' zonder argument
 * na een herstart nog steeds weet wie hij moet wekken.
 */

/* Leest het bewaarde MAC. Zonder bestand blijft het leeg en zegt 'wol' dat. */
void wol_begin(fs::FS& fs);

/* Neemt "8cc681eb9957", "8c-c6-81-eb-99-57" of met dubbele punten. Leeg of
 * "uit" wist de bestemming. false = geen twaalf hexcijfers. */
bool wol_set_mac(const char* txt);

/* Schrijft het MAC als "8c:c6:81:eb:99:57", of een lege string. */
void wol_mac_text(char* uit, size_t max);
bool wol_have_mac();

/* Zet het magic packet op de draad: zes keer 0xFF gevolgd door zestien keer
 * het MAC, als UDP-broadcast naar poort 9. Naar het SUBNET-broadcastadres en
 * niet naar 255.255.255.255: dat laatste wordt door menig stack en switch
 * stilletjes geweigerd. false met een reden in `uit` als het niet kon. */
bool wol_send(char* uit, size_t max);

/* Zoekt het MAC bij een IP op via ARP en bewaart het. Werkt alleen zolang de
 * pc nog WAKKER is -- daarna wekt het bewaarde MAC hem ook uit slaap. */
bool wol_resolve_ip(const char* ip_txt, char* uit, size_t max);

/* De CLI: 'wol', 'wol <mac>', 'wol ip <adres>', 'wol set <mac>', 'wol uit'.
 * Geeft false als het commando niet voor ons is, zodat de aanroeper kan
 * doorvallen. */
bool wol_handle_command(const char* cmd, char* reply, size_t reply_max);
