#pragma once
// MeshUptimeCompanion — hook called from MyMesh::onMessageRecv() for every
// incoming direct message (already decrypted by the mesh stack, whether or not
// a phone app is connected over BLE). Keep this tiny so the copied MyMesh.cpp
// diff from upstream stays a single line.
#include <helpers/ContactInfo.h>

void mu_on_direct_msg(const ContactInfo& from, uint32_t sender_timestamp, const char* text);
