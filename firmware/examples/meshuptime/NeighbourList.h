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
 * TWEE DOELEN, en dat verklaart de GROOTTE (v2.3.0). (1) "Wie is er nu?" voor de
 * toegangslijst-kiezer, en (2) NAAMRESOLUTIE pubkey->naam voor de bot-`path`
 * (repeaternamen), de DM-afzendernaam en de kanaal-diagnose. De MeshCore-app toont
 * overal meteen namen omdat zij een GROTE contactdatabase bijhoudt (naam uit elk
 * gehoord advert); een kleine lijst liet ons terugvallen op de hex-pubkey. Daarom
 * is de capaciteit nu fors: elke gehoorde node onthouden we met pubkey + naam.
 *
 * RAM I.P.V. FLASH, bewust. Adverts komen met tientallen per uur langs; per advert
 * naar flash schrijven zou de flash verslijten (dat was de oorspronkelijke reden
 * om NIET te bewaren). Een periodieke snapshot zou kunnen, maar de namen stromen
 * na een herstart binnen enkele minuten vanzelf weer binnen zodra de adverts
 * langskomen -- de kost (code + flash-slijtage) weegt niet op tegen die paar
 * minuten. Zie het eindrapport; blijft een afweging die later kan kantelen.
 *
 * AFWIJKING VAN "RING", met opzet: een strikte FIFO-ring zou dezelfde node bij
 * elk advert opnieuw opnemen. Hier wordt op publieke sleutel SAMENGEVOEGD (naam,
 * signaal en tijd bijgewerkt, teller op) en bij een volle lijst valt de LANGST
 * NIET GEHOORDE ingang eruit (LRU op laatst-gehoord). Zo tonen we verschillende
 * nodes, niet dezelfde advert-storm.
 *
 * Statisch: één vast blok van MAX_NEIGHBOURS ingangen, geen new, geen malloc,
 * geen String. Bij 200 x 68 byte ~= 13,6 kB RAM (de bewuste ruil voor namen).
 */

#ifndef MAX_NEIGHBOURS
  #define MAX_NEIGHBOURS  200
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
