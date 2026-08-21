#pragma once

/* ============================================================================
 * IWebNode -- de smalle interface tussen WebTask en "de node".
 *
 * WAAROM DIT BESTAAT
 *
 * WebTask was rechtstreeks aan SensorMesh gekoppeld (SensorMesh* _acl). De
 * room-variant gebruikt geen SensorMesh maar RoomMesh, en die moet dezelfde
 * webinterface (nodebeheer via /cli, /cfg.json, toegangsbeheer, buurtlijst)
 * kunnen aandrijven. In plaats van WebTask twee keer te bouwen of van type te
 * laten wisselen, praat WebTask nu tegen deze interface. SensorMesh EN RoomMesh
 * implementeren hem allebei.
 *
 * KLEIN GEHOUDEN MET OPZET: precies de methoden die WebTask op `_acl` aanriep,
 * niet meer. Elke methode erbij is er een die beide klassen moeten blijven
 * leveren.
 *
 * Namen die anders met de basisklasse zouden botsen (getSelfId, getRole,
 * getRTCClock, handleCommand) hebben hier een EIGEN naam (getSelfPubKey,
 * getRoleName, nowSecs, handleCommandWeb), zodat er geen dubbelzinnigheid is met
 * de gelijknamige, niet-virtuele methoden op mesh::Mesh / CommonCLICallbacks.
 * ==========================================================================*/

#include <Arduino.h>
#include <helpers/ClientACL.h>   // ClientInfo
#include <helpers/CommonCLI.h>   // NodePrefs

class NeighbourList;

class IWebNode {
public:
  virtual ~IWebNode() {}

  /* Nodebeheer: /cli geeft een opdrachtregel door aan de CLI van de node. */
  virtual void        handleCommandWeb(uint32_t sender_timestamp, char* command, char* reply) = 0;

  /* Leeskant voor /cfg.json en de beheerpagina. */
  virtual NodePrefs*  getNodePrefs() = 0;
  virtual const char* getRoleName() = 0;
  virtual const uint8_t* getSelfPubKey() = 0;   // PUB_KEY_SIZE byte
  virtual uint32_t    nowSecs() = 0;            // RTC-tijd in seconden

  /* Toegangsbeheer. */
  virtual bool        getAclStrict() const = 0;
  virtual void        setAclStrict(bool on) = 0;
  virtual int         getAclCount() = 0;
  virtual ClientInfo* getAclEntry(int idx) = 0;
  virtual bool        aclSetPerms(const uint8_t* pubkey, uint8_t perms) = 0;
  virtual bool        aclRemove(const uint8_t* pubkey, int key_len) = 0;
  virtual int         aclCountMatching(const uint8_t* pubkey, int key_len) = 0;

  /* Buurtlijst en het naar-voren-trekken van een sensorleesronde. */
  virtual const NeighbourList& getNeighbours() const = 0;
  virtual void        requestSensorReadNow() = 0;
};
