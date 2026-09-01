#include "CH390EthernetInterface.h"
#include "CH390Config.h"

void onWiFiEvent(WiFiEvent_t event) {
  switch(event){
    case ARDUINO_EVENT_ETH_START:
      ETHERNET_DEBUG_PRINTLN("Ethernet Started");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      ETHERNET_DEBUG_PRINTLN("Ethernet Connected");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      ETHERNET_DEBUG_PRINTLN("Ethernet Disconnected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      ETHERNET_DEBUG_PRINTLN("Ethernet Got IP");
      ETHERNET_DEBUG_PRINT_IP("IP Address", CH390.localIP());
      ETHERNET_DEBUG_PRINT_IP("Subnet Mask", CH390.subnetMask());
      ETHERNET_DEBUG_PRINT_IP("Gateway", CH390.gatewayIP());
      ETHERNET_DEBUG_PRINT_IP("DNS", CH390.dnsIP());
      ETHERNET_DEBUG_PRINTLN("MAC Address: %s", CH390.macAddress().c_str());
      break;
    default:
      break;
  }
}

bool CH390EthernetInterface::begin() {

  // listen to ethernet events
  WiFi.onEvent(onWiFiEvent);

  // Init CH390 using the same board configuration as the observer uplink.
  if (!beginConfiguredCH390()) {
    ETHERNET_DEBUG_PRINTLN("Failed to initialize CH390 hardware.");
    return false;
  }

  // Start Server
  server.begin(ETHERNET_TCP_PORT);
  ETHERNET_DEBUG_PRINTLN("listening on TCP port: %d", ETHERNET_TCP_PORT);

  return true;
}

int CH390EthernetInterface::available() {
  return client.available();
}

int CH390EthernetInterface::read() {
  return client.read();
}

size_t CH390EthernetInterface::write(const uint8_t *buf, size_t size) {
  return client.write(buf, size);
}

bool CH390EthernetInterface::isConnected() const {
  return _isConnected;
}

void CH390EthernetInterface::loop() {
  
  if (server.hasClient()) {
    auto newClient = server.available();
    if (newClient) {
      IPAddress remoteIp = newClient.remoteIP();
      uint16_t remotePort = newClient.remotePort();
      ETHERNET_DEBUG_PRINTLN("New client accepted %u.%u.%u.%u:%u", remoteIp[0], remoteIp[1], remoteIp[2], remoteIp[3], remotePort);
      if (client) {
        ETHERNET_DEBUG_PRINTLN("Closing previous client");
        client.stop();
      }
      client = newClient;
      onClientConnected();
    }
  }

  _isConnected = client.connected();

}
