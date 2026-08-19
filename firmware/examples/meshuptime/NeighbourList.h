#pragma once

#include <Arduino.h>   // needed for PlatformIO
#include <MeshCore.h>

/* Wie er ONLANGS in de lucht was.
 *
 * MeshCore bewaart nergens van wie het adverts hoort. Voor de toegangslijst is
 * dat precies het ontbrekende stuk: een publieke sleutel van 64 hextekens is
 * niet iets dat iemand overtypt, en zonder deze lijst is de enige weg naar een
 * nieuwe ingang "log eerst met het beheerderswachtwoord in". Deze lijst maakt
 * van dat probleem een keuzelijstje.
 *
 * NIET IN SPIFFS, EN DAT IS DE HELE REDEN DAT DEZE KLASSE ZO KLEIN IS. Wat hier
 * staat is een bewering over het HEDEN: "deze node was net te horen". Een
 * bewaarde versie zou na een herstart beweren dat een node in de buurt is die er
 * al een week niet meer is, en dat is erger dan een lege lijst -- een lege lijst
 * liegt niet. Daar komt bij dat elke schrijfactie flash kost en dat adverts met
 * tientallen per uur langskomen.
 *
 * AFWIJKING VAN "RING", met opzet: een strikte FIFO-ring zou dezelfde node bij
 * elk advert opnieuw opnemen en na twaalf adverts van één drukke buur zou de
 * lijst twaalf keer diezelfde buur zijn. Hier wordt op publieke sleutel
 * SAMENGEVOEGD (naam, signaal en tijd worden bijgewerkt, de teller loopt op) en
 * bij een volle lijst valt de LANGST NIET GEHOORDE ingang eruit. Dat is wat een
 * ring hier moet doen: twaalf verschillende buren tonen, niet twaalf adverts.
 *
 * Statisch: één vast blok van MAX_NEIGHBOURS ingangen, geen new, geen malloc,
 * geen String. Kost ongeveer 12 x 68 = 816 byte RAM.
 */

#ifndef MAX_NEIGHBOURS
  #define MAX_NEIGHBOURS  12
#endif

/* 24 en niet 32: een advert is hoogstens MAX_ADVERT_DATA_SIZE (32) byte en
 * daarvan gaan er 1 naar de vlaggen en 8 naar lengte/breedte. Er blijven dus
 * ten hoogste 23 tekens naam over, plus de afsluitende nul. Ruimer maken zou
 * alleen RAM kosten voor tekens die er niet in kunnen zitten. */
#define NB_NAME_LEN   24

struct NeighbourEntry {
  uint8_t  pub_key[PUB_KEY_SIZE];
  char     name[NB_NAME_LEN];   // leeg als het advert geen naam droeg
  uint32_t heard_at;            // RTC-seconden van het LAATSTE advert
  uint32_t count;               // hoeveel adverts van deze node we zagen
  int8_t   snr4;                // SNR x 4, zoals mesh::Packet het bewaart
  uint8_t  hops;                // padlengte bij ontvangst; 0 = rechtstreeks gehoord
  uint8_t  adv_type;            // ADV_TYPE_* uit AdvertDataHelpers.h
};

class NeighbourList {
public:
  NeighbourList() { memset(_ring, 0, sizeof(_ring)); _num = 0; }

  /* Eén gehoord advert opnemen. Wordt aangeroepen vanuit de ontvangstlus, dus:
   * geen allocatie, geen bestandssysteem, geen lus over meer dan MAX_NEIGHBOURS.
   * 'name' mag NULL zijn (advert zonder naam). */
  void noteAdvert(const uint8_t* pub_key, const char* name, uint8_t adv_type,
                  int8_t snr4, uint8_t hops, uint32_t now);

  int getNumEntries() const { return _num; }

  /* Op index; NULL bij een index buiten de lijst. De volgorde is die van
   * opname en dus NIET op tijd gesorteerd -- sorteren doet de pagina, want daar
   * kost het niets en hier zou het per advert werk zijn. */
  const NeighbourEntry* getEntryByIdx(int idx) const {
    return (idx >= 0 && idx < _num) ? &_ring[idx] : NULL;
  }

  /* Zoekt op (deel van een) publieke sleutel. Bedoeld om bij een ingang in de
   * toegangslijst de naam uit een advert te kunnen tonen: ClientInfo draagt geen
   * naam, en zonder naam is een lijst van hexsleutels onleesbaar. */
  const NeighbourEntry* find(const uint8_t* pub_key, int key_len) const;

private:
  NeighbourEntry _ring[MAX_NEIGHBOURS];
  int _num;
};
