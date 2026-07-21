#pragma once

// Adapters that bind a MeshCore variant's ACL admin list and CLI to the
// MQTTBridge remote-command hooks. Shared by the repeater and room-server
// examples so the wiring stays identical between them.

#ifdef WITH_MQTT_BRIDGE

#include "helpers/bridges/MQTTBridge.h"
#include "helpers/ClientACL.h"
#include "helpers/CommonCLI.h"
#include <string.h>

// Authorizes a remote command's signing key against the variant's ACL: the key
// must belong to a known client flagged as admin.
class ACLAdminAuthCallbacks : public MQTTBridgeACLCallbacks {
  ClientACL* _acl;
public:
  explicit ACLAdminAuthCallbacks(ClientACL* acl = nullptr) : _acl(acl) {}
  bool isPublicKeyAdmin(const uint8_t* pubkey, size_t key_len) override {
    if (!_acl) return false;
    ClientInfo* client = _acl->getClient(pubkey, (int)key_len);
    return client != nullptr && client->isAdmin();
  }
};

// Runs an authorized remote command through the variant's CLI. CommonCLI mutates
// its command buffer, so the const command is copied into a local first.
class CLICommandExecutor : public MQTTBridgeCommandExecutor {
  CommonCLI* _cli;
public:
  explicit CLICommandExecutor(CommonCLI* cli = nullptr) : _cli(cli) {}
  void handleCommand(uint32_t sender_timestamp, const char* command, char* reply) override {
    if (!_cli) return;
    char buf[256];
    size_t n = strlen(command);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, command, n);
    buf[n] = '\0';
    _cli->handleCommand(sender_timestamp, buf, reply);
  }
};

#endif  // WITH_MQTT_BRIDGE
