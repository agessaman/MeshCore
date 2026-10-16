#pragma once

#include <stdint.h>

// Operator-facing labels for the SDK error codes the observer reports through
// `get wifi.status`, `get mqttN.diag` and the web config panel.
//
// Pure lookup: no Arduino or ESP headers, so the table is host-testable. The
// numeric values below are the installed SDK's; MQTTBridge.cpp static_asserts
// every one of them against the real enum/#define, so a framework bump either
// keeps these labels honest or fails the build. Codes with no entry return
// nullptr and the caller prints the raw number — an unknown code must never be
// given a neighbouring code's name.
namespace MQTTErrorLabels {

// esp_wifi_types.h wifi_err_reason_t. Values below 200 are 802.11 reason codes
// forwarded from the AP; 200+ are Espressif's own local failures.
enum WifiReason : uint16_t {
  kWifiAuthExpire            = 2,
  kWifiAssocExpire           = 4,
  kWifiAssocLeave            = 8,
  kWifi4WayHandshakeTimeout  = 15,
  kWifiGroupCipherInvalid    = 18,
  kWifiCipherSuiteRejected   = 24,
  kWifiMissingAcks           = 34,
  kWifiTimeout               = 39,
  kWifiInvalidPmkid          = 49,
  kWifiBssTransitionDisassoc = 12,
  kWifiBeaconTimeout         = 200,
  kWifiNoApFound             = 201,
  kWifiAuthFail              = 202,
  kWifiAssocFail             = 203,
  kWifiHandshakeTimeout      = 204,
  kWifiConnectionFail        = 205,
  kWifiApTsfReset            = 206,
  kWifiRoaming               = 207,
  kWifiSaQueryTimeout        = 209,
};

// esp_tls_errors.h, ESP_ERR_ESP_TLS_BASE (0x8000) + offset.
enum TlsError : int32_t {
  kTlsCannotResolveHostname = 0x8001,
  kTlsCannotCreateSocket    = 0x8002,
  kTlsUnsupportedProtoFamily= 0x8003,
  kTlsFailedConnectToHost   = 0x8004,
  kTlsSocketSetoptFailed    = 0x8005,
  kTlsConnectionTimeout     = 0x8006,
  kTlsTcpClosedFin          = 0x8008,
  kTlsMbedtlsCertPartlyOk   = 0x8010,
  kTlsMbedtlsSetHostname    = 0x8012,
  kTlsMbedtlsX509ParseFailed= 0x8015,
  kTlsMbedtlsSslSetupFailed = 0x8017,
  kTlsMbedtlsSslWriteFailed = 0x8018,
  kTlsMbedtlsHandshakeFailed= 0x801A,
};

// MQTT 3.1.1 CONNACK return codes (esp_mqtt_error_codes.connect_return_code).
enum ConnackCode : uint8_t {
  kConnackAccepted        = 0,
  kConnackBadProtocol     = 1,
  kConnackIdRejected      = 2,
  kConnackServerUnavail   = 3,
  kConnackBadCredentials  = 4,
  kConnackNotAuthorized   = 5,
};

// Short enough to sit inside a LoRa-bounded diagnostic reply next to the raw code.
static inline const char* wifiReason(uint8_t reason) {
  switch (reason) {
    case kWifiAuthExpire:            return "auth expired";
    case kWifiAssocExpire:           return "assoc expired";
    case kWifiAssocLeave:            return "AP disconnected";
    case kWifiBssTransitionDisassoc: return "AP steered us away";
    case kWifi4WayHandshakeTimeout:  return "4-way handshake timeout";
    case kWifiGroupCipherInvalid:    return "group cipher mismatch";
    case kWifiCipherSuiteRejected:   return "cipher suite rejected";
    case kWifiMissingAcks:           return "AP saw no acks";
    case kWifiTimeout:               return "802.11 timeout";
    case kWifiInvalidPmkid:          return "invalid PMKID";
    case kWifiBeaconTimeout:         return "beacon lost";
    case kWifiNoApFound:             return "SSID not found";
    case kWifiAuthFail:              return "auth failed (check password)";
    case kWifiAssocFail:             return "association failed";
    case kWifiHandshakeTimeout:      return "handshake timeout";
    case kWifiConnectionFail:        return "connect failed";
    case kWifiApTsfReset:            return "AP restarted";
    case kWifiRoaming:               return "roaming";
    case kWifiSaQueryTimeout:        return "SA query timeout (PMF)";
    default:                         return nullptr;
  }
}

static inline const char* tlsError(int32_t err) {
  switch (err) {
    case kTlsCannotResolveHostname:  return "DNS failed";
    case kTlsCannotCreateSocket:     return "socket error";
    case kTlsUnsupportedProtoFamily: return "unsupported protocol family";
    case kTlsFailedConnectToHost:    return "connect failed";
    case kTlsSocketSetoptFailed:     return "socket setopt failed";
    case kTlsConnectionTimeout:      return "connection timeout";
    case kTlsTcpClosedFin:           return "server closed connection";
    case kTlsMbedtlsCertPartlyOk:    return "cert chain partly parsed";
    case kTlsMbedtlsSetHostname:     return "SNI hostname rejected";
    case kTlsMbedtlsX509ParseFailed: return "cert parse failed";
    case kTlsMbedtlsSslSetupFailed:  return "TLS setup failed";
    case kTlsMbedtlsSslWriteFailed:  return "TLS write failed";
    case kTlsMbedtlsHandshakeFailed: return "TLS handshake failed";
    default:                         return nullptr;
  }
}

// The broker answered and refused: this is what an operator needs instead of a
// transport error, which is why the bridge keeps the CONNACK code separately.
static inline const char* connackReason(uint8_t code) {
  switch (code) {
    case kConnackBadProtocol:    return "protocol rejected";
    case kConnackIdRejected:     return "client id rejected";
    case kConnackServerUnavail:  return "server unavailable";
    case kConnackBadCredentials: return "bad user/password";
    case kConnackNotAuthorized:  return "not authorized";
    default:                     return nullptr;
  }
}

} // namespace MQTTErrorLabels
