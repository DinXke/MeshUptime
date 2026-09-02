#pragma once
// MeshUptimeCompanion — hook called from MyMesh::onMessageRecv() for every
// incoming direct message (already decrypted by the mesh stack, whether or not
// a phone app is connected over BLE). Keep this tiny so the copied MyMesh.cpp
// diff from upstream stays a single line.
// Returns true if the DM was a MeshUptime CONTROL command (a '!' command from an
// allowlisted sender) that we consumed — the caller then must NOT forward it to
// the phone app (control traffic is not chat). Returns false for everything else
// (alert DMs, plain chat), which stays visible in the app as normal.
#include <helpers/ContactInfo.h>

bool mu_on_direct_msg(const ContactInfo& from, uint32_t sender_timestamp, const char* text);
