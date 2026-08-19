#include "NeighbourList.h"

void NeighbourList::noteAdvert(const uint8_t* pub_key, const char* name, uint8_t adv_type,
                               int8_t snr4, uint8_t hops, uint32_t now) {
  NeighbourEntry* e = NULL;

  /* Al bekend? Dan bijwerken en niet opnieuw opnemen. */
  for (int i = 0; i < _num; i++) {
    if (memcmp(_ring[i].pub_key, pub_key, PUB_KEY_SIZE) == 0) { e = &_ring[i]; break; }
  }

  if (e == NULL) {
    if (_num < MAX_NEIGHBOURS) {
      e = &_ring[_num++];
    } else {
      /* Vol: de langst niet gehoorde ingang eruit. Dat is de ingang die het
       * minst waarschijnlijk nog bestaat, en dat is hier het enige criterium --
       * deze lijst gaat over wie er NU is. */
      e = &_ring[0];
      for (int i = 1; i < MAX_NEIGHBOURS; i++) {
        if (_ring[i].heard_at < e->heard_at) e = &_ring[i];
      }
    }
    memset(e, 0, sizeof(*e));
    memcpy(e->pub_key, pub_key, PUB_KEY_SIZE);
  }

  /* De NAAM alleen overschrijven als dit advert er een droeg. Een node stuurt
   * afwisselend adverts met en zonder naam (een zero-hop advert is kort); een
   * naam die we al kenden mag niet verdwijnen omdat het laatste advert korter
   * was dan het vorige. */
  if (name != NULL && name[0] != 0) {
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = 0;
  }

  e->adv_type = adv_type;
  e->snr4     = snr4;
  e->hops     = hops;
  e->heard_at = now;
  if (e->count < 0xFFFFFFFF) e->count++;
}

const NeighbourEntry* NeighbourList::find(const uint8_t* pub_key, int key_len) const {
  if (key_len <= 0 || key_len > PUB_KEY_SIZE) return NULL;
  for (int i = 0; i < _num; i++) {
    if (memcmp(_ring[i].pub_key, pub_key, key_len) == 0) return &_ring[i];
  }
  return NULL;
}
