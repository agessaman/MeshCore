#include "MyMesh.h"
#include <algorithm>

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef DEFAULT_ADVERT_INTERVAL_MINUTES
  #define DEFAULT_ADVERT_INTERVAL_MINUTES 2
#endif
#ifndef DEFAULT_FLOOD_ADVERT_INTERVAL_HOURS
  #define DEFAULT_FLOOD_ADVERT_INTERVAL_HOURS 47
#endif
#ifndef DEFAULT_AGC_RESET_INTERVAL_SECONDS
  #define DEFAULT_AGC_RESET_INTERVAL_SECONDS 0
#endif
#ifndef DEFAULT_RX_DELAY_BASE
  #define DEFAULT_RX_DELAY_BASE 0.0f
#endif
#ifndef DEFAULT_MULTI_ACKS
  #define DEFAULT_MULTI_ACKS 0
#endif
#ifndef DEFAULT_PATH_HASH_MODE
  #define DEFAULT_PATH_HASH_MODE 0
#endif
#ifndef DEFAULT_LOOP_DETECT
  #define DEFAULT_LOOP_DETECT LOOP_DETECT_OFF
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000

#define FLOOD_CHANNEL_BLOCK_FILE      "/flood_ch_block"
#define DEFAULT_FLOOD_CHANNEL_BLOCK_NAME  "#wardriving"
#ifndef DEFAULT_FLOOD_CHANNEL_BLOCK_HOPS
  #define DEFAULT_FLOOD_CHANNEL_BLOCK_HOPS 4
#endif

#ifndef REPEATERS_CHANNEL_KEY_HEX
  #define REPEATERS_CHANNEL_KEY_HEX "89db441e2814dccf0dbd2e8cc5f501a3"
#endif
#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif

#define LOW_BATTERY_MIN_VALID_MV       1000
#define LOW_BATTERY_CHECK_INTERVAL     (60UL * 1000UL)
#define LOW_BATTERY_WARN_INTERVAL      (24UL * 60UL * 60UL * 1000UL)
#define LOW_BATTERY_CRITICAL_INTERVAL  (12UL * 60UL * 60UL * 1000UL)

static const char* skipLocalSpaces(const char* text) {
  while (text != NULL && *text == ' ') text++;
  return text;
}

static bool selectorIsEmpty(const char* text) {
  text = skipLocalSpaces(text);
  return text == NULL || *text == 0;
}

static bool selectorIsAll(const char* text) {
  text = skipLocalSpaces(text);
  if (text == NULL || memcmp(text, "all", 3) != 0) {
    return false;
  }
  text += 3;
  while (*text == ' ') text++;
  return *text == 0;
}

static bool parsePositiveSelector(const char* text, int& value) {
  text = skipLocalSpaces(text);
  if (text == NULL || *text == 0) {
    return false;
  }

  uint32_t n = 0;
  bool saw_digit = false;
  while (*text >= '0' && *text <= '9') {
    saw_digit = true;
    n = (n * 10) + (uint32_t)(*text - '0');
    if (n > 32767) {
      return false;
    }
    text++;
  }
  while (*text == ' ') text++;
  if (!saw_digit || n == 0 || *text != 0) {
    return false;
  }
  value = (int)n;
  return true;
}

static bool bwMatches(float bw, float allowed) {
  float diff = bw - allowed;
  if (diff < 0.0f) diff = -diff;
  return diff <= 0.001f;
}

static bool isValidLoRaBandwidth(float bw) {
#if defined(USE_LR1110)
  return bwMatches(bw, 62.5f)
      || bwMatches(bw, 125.0f)
      || bwMatches(bw, 250.0f)
      || bwMatches(bw, 500.0f);
#elif defined(USE_LLCC68) || defined(USE_SX1272)
  return bwMatches(bw, 125.0f)
      || bwMatches(bw, 250.0f)
      || bwMatches(bw, 500.0f);
#else
  return bwMatches(bw, 7.8f)
      || bwMatches(bw, 10.4f)
      || bwMatches(bw, 15.6f)
      || bwMatches(bw, 20.8f)
      || bwMatches(bw, 31.25f)
      || bwMatches(bw, 41.7f)
      || bwMatches(bw, 62.5f)
      || bwMatches(bw, 125.0f)
      || bwMatches(bw, 250.0f)
      || bwMatches(bw, 500.0f);
#endif
}

static bool isValidScheduledRadioParams(float freq, float bw, uint8_t sf, uint8_t cr) {
  return freq >= 150.0f && freq <= 2500.0f
      && isValidLoRaBandwidth(bw)
      && sf >= 5 && sf <= 12
      && cr >= 5 && cr <= 8;
}

static bool buildRepeatersChannel(mesh::GroupChannel& channel) {
  const char* hex = REPEATERS_CHANNEL_KEY_HEX;
  size_t hex_len = strlen(hex);
  if (!(hex_len == 32 || hex_len == 64)) return false;
  for (size_t i = 0; i < hex_len; i++) {
    if (!mesh::Utils::isHexChar(hex[i])) return false;
  }

  memset(channel.secret, 0, sizeof(channel.secret));
  size_t key_len = hex_len / 2;
  if (!mesh::Utils::fromHex(channel.secret, key_len, hex)) return false;

  mesh::Utils::sha256(channel.hash, sizeof(channel.hash), channel.secret, key_len);
  return true;
}

static File openFloodChannelBlockRead(FILESYSTEM* fs, const char* filename) {
#if defined(RP2040_PLATFORM)
  return fs->open(filename, "r");
#else
  return fs->open(filename);
#endif
}

static File openFloodChannelBlockWrite(FILESYSTEM* fs, const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove(filename);
  return fs->open(filename, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return fs->open(filename, "w");
#else
  return fs->open(filename, "w", true);
#endif
}

static uint8_t batteryPercentFromMilliVolts(uint16_t batt_mv) {
  const int min_mv = BATT_MIN_MILLIVOLTS;
  const int max_mv = BATT_MAX_MILLIVOLTS;
  if (max_mv <= min_mv) return 100;

  int pct = (((int)batt_mv - min_mv) * 100) / (max_mv - min_mv);
  if (pct < 0) return 0;
  if (pct > 100) return 100;
  return (uint8_t)pct;
}

static bool parseBatteryAlertPercent(const char* value, uint8_t min_value, uint8_t max_value, uint8_t& result) {
  if (value == NULL || *value == 0) {
    return false;
  }

  uint16_t parsed = 0;
  while (*value) {
    if (*value < '0' || *value > '9') {
      return false;
    }
    parsed = (uint16_t)(parsed * 10 + (*value - '0'));
    if (parsed > max_value) {
      return false;
    }
    value++;
  }
  if (parsed < min_value) {
    return false;
  }

  result = (uint8_t)parsed;
  return true;
}

static void formatFixed3(char* dest, size_t dest_len, float value) {
  long scaled = (long)(value * 1000.0f + (value >= 0.0f ? 0.5f : -0.5f));
  long whole = scaled / 1000;
  long decimals = scaled % 1000;
  if (decimals < 0) decimals = -decimals;
  snprintf(dest, dest_len, "%ld.%03ld", whole, decimals);
}

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->snr = (int8_t)(snr * 4);
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (strcmp((char *)data, _prefs.password) != 0) {  // admin pw bypasses ACL (allows upgrade)
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~PERM_ACL_ROLE_MASK;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = OUT_PATH_UNKNOWN;  // need to rediscover out_path
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = (client->isAdmin() || client->isRegionMgr()) ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data & 63;
    reply_path_hash_size = (*data >> 6) + 1;
    data++;

    memcpy(reply_path, data, ((uint8_t)reply_path_len) * reply_path_hash_size);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data & 63;
    reply_path_hash_size = (*data >> 6) + 1;
    data++;

    memcpy(reply_path, data, ((uint8_t)reply_path_len) * reply_path_hash_size);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data & 63;
    reply_path_hash_size = (*data >> 6) + 1;
    data++;

    memcpy(reply_path, data, ((uint8_t)reply_path_len) * reply_path_hash_size);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
#if MAX_NEIGHBOURS
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        if (neighbour->heard_timestamp > 0) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }
#endif

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){
        
        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

#if MAX_NEIGHBOURS
        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;
#endif

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

static uint8_t max_loop_minimal[] =  { 0, /* 1-byte */  4, /* 2-byte */  2, /* 3-byte */  1 };
static uint8_t max_loop_moderate[] = { 0, /* 1-byte */  2, /* 2-byte */  1, /* 3-byte */  1 };
static uint8_t max_loop_strict[] =   { 0, /* 1-byte */  1, /* 2-byte */  1, /* 3-byte */  1 };

bool MyMesh::isLooped(const mesh::Packet* packet, const uint8_t max_counters[]) {
  uint8_t hash_size = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  uint8_t n = 0;
  const uint8_t* path = packet->path;
  while (hash_count > 0) {      // count how many times this node is already in the path
    if (self_id.isHashMatch(path, hash_size)) n++;
    hash_count--;
    path += hash_size;
  }
  return n >= max_counters[hash_size];
}

void MyMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  if (recv_pkt_region && !recv_pkt_region->isWildcard()) {  // if _request_ packet scope is known, send reply with same scope
    TransportKey scope;
    if (region_map.getTransportKeysFor(*recv_pkt_region, &scope, 1) > 0) {
      sendFloodScoped(scope, packet, delay_millis, path_hash_size);
    } else {
      sendFlood(packet, delay_millis, path_hash_size);  // send un-scoped
    }
  } else {
    sendFlood(packet, delay_millis, path_hash_size);  // send un-scoped
  }
}

static bool directPathsEqual(const uint8_t* a_path, uint8_t a_len, const uint8_t* b_path, uint8_t b_len) {
  if (!mesh::Packet::isValidPathLen(a_len) || !mesh::Packet::isValidPathLen(b_len) || a_len != b_len) {
    return false;
  }
  uint8_t hash_count = a_len & 63;
  uint8_t hash_size = (a_len >> 6) + 1;
  uint8_t byte_len = hash_count * hash_size;
  return byte_len == 0 || memcmp(a_path, b_path, byte_len) == 0;
}

void MyMesh::sendClientReply(ClientInfo* client, mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  if (packet == NULL) {
    return;
  }
  if (client == NULL || !mesh::Packet::isValidPathLen(client->out_path_len)) {
    sendFloodReply(packet, delay_millis, path_hash_size);
    return;
  }

  mesh::Packet* alt = NULL;
  if (mesh::Packet::isValidPathLen(client->alt_path_len)
      && !directPathsEqual(client->out_path, client->out_path_len, client->alt_path, client->alt_path_len)) {
    alt = obtainNewPacket();
    if (alt != NULL) {
      *alt = *packet;
    } else {
      MESH_DEBUG_PRINTLN("sendClientReply: altpath packet pool empty");
    }
  }

  sendDirect(packet, client->out_path, client->out_path_len, delay_millis);
  if (alt != NULL) {
    uint8_t direct_retry_enabled = _prefs.direct_retry_enabled;
    _prefs.direct_retry_enabled = 0;
    sendDirect(alt, client->alt_path, client->alt_path_len, delay_millis);
    _prefs.direct_retry_enabled = direct_retry_enabled;
  }
}

uint8_t MyMesh::resolveFloodChannelBlockHops(uint8_t max_hops) const {
  return max_hops == FLOOD_CHANNEL_BLOCK_HOPS_INHERIT ? _prefs.flood_channel_block_max_hops : max_hops;
}

bool MyMesh::floodChannelBlockHopApplies(const mesh::Packet* packet, uint8_t max_hops) const {
  if (packet == NULL) {
    return false;
  }
  max_hops = resolveFloodChannelBlockHops(max_hops);
  return max_hops == FLOOD_CHANNEL_BLOCK_HOPS_ALL || packet->getPathHashCount() > max_hops;
}

bool MyMesh::floodChannelDataHopApplies(const mesh::Packet* packet) const {
  if (packet == NULL) {
    return false;
  }
  uint8_t max_hops = _prefs.flood_channel_data_max_hops;
  return max_hops == FLOOD_CHANNEL_BLOCK_HOPS_ALL || packet->getPathHashCount() > max_hops;
}

bool MyMesh::floodChannelBlockMatches(const FloodChannelBlockEntry& entry, const mesh::Packet* packet) const {
  if (!entry.active || packet == NULL || !packet->isRouteFlood()) {
    return false;
  }
  if (!floodChannelBlockHopApplies(packet, entry.max_hops)) {
    return false;
  }
  uint8_t type = packet->getPayloadType();
  if (type != PAYLOAD_TYPE_GRP_TXT && type != PAYLOAD_TYPE_GRP_DATA) {
    return false;
  }
  if (packet->payload_len <= PATH_HASH_SIZE + CIPHER_MAC_SIZE || packet->payload[0] != entry.hash_prefix[0]) {
    return false;
  }

  uint8_t data[MAX_PACKET_PAYLOAD];
  int len = mesh::Utils::MACThenDecrypt(entry.secret, data, &packet->payload[PATH_HASH_SIZE],
                                        packet->payload_len - PATH_HASH_SIZE);
  return len > 0;
}

bool MyMesh::shouldBlockFloodChannelForward(const mesh::Packet* packet) const {
  for (int i = 0; i < FLOOD_CHANNEL_BLOCK_SLOTS; i++) {
    if (floodChannelBlockMatches(flood_channel_blocks[i], packet)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: flood.channel.block matched slot=%d name=%s hops=%d",
                         i + 1, flood_channel_blocks[i].name, packet->getPathHashCount());
      return true;
    }
  }
  return false;
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood()) {
    if (packet->getPathHashCount() >= _prefs.flood_max) return false;
    if (packet->getRouteType() == ROUTE_TYPE_FLOOD && packet->getPathHashCount() >= _prefs.flood_max_unscoped) return false;
    if (packet->getPayloadType() == PAYLOAD_TYPE_ADVERT && packet->getPathHashCount() >= _prefs.flood_max_advert) return false;
    if (!_prefs.flood_channel_data_enabled
        && packet->getPayloadType() == PAYLOAD_TYPE_GRP_DATA
        && floodChannelDataHopApplies(packet)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: flood.channel.data off, blocking GRP_DATA hops=%d",
                         packet->getPathHashCount());
      return false;
    }
    if (shouldBlockFloodChannelForward(packet)) return false;
  }
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code, or wildcard not allowed for FLOOD packet");
    return false;
  }
  if (packet->isRouteFlood() && _prefs.loop_detect != LOOP_DETECT_OFF) {
    const uint8_t* maximums;
    if (_prefs.loop_detect == LOOP_DETECT_MINIMAL) {
      maximums = max_loop_minimal;
    } else if (_prefs.loop_detect == LOOP_DETECT_MODERATE) {
      maximums = max_loop_moderate;
    } else {
      maximums = max_loop_strict;
    }
    if (isLooped(packet, maximums)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: FLOOD packet loop detected!");
      return false;
    }
  }
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 1) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 0) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

bool MyMesh::extractDirectRetryPrefix(const mesh::Packet* packet, uint8_t* prefix, uint8_t& prefix_len) const {
  if (packet == NULL || !packet->isRouteDirect() || packet->getPathHashCount() == 0) {
    return false;
  }
  prefix_len = packet->getPathHashSize();
  memcpy(prefix, packet->path, prefix_len);
  return true;
}

static bool isDirectShortcutPayload(const mesh::Packet* packet) {
  if (packet == NULL || !packet->isRouteDirect()) {
    return false;
  }

  switch (packet->getPayloadType()) {
    case PAYLOAD_TYPE_PATH:
    case PAYLOAD_TYPE_REQ:
    case PAYLOAD_TYPE_RESPONSE:
    case PAYLOAD_TYPE_TXT_MSG:
    case PAYLOAD_TYPE_ANON_REQ:
      return true;
    default:
      return false;
  }
}

bool MyMesh::maybeShortCircuitDirect(mesh::Packet* packet) {
  if (!isDirectShortcutPayload(packet)) {
    return false;
  }

  uint8_t hash_size = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES || hash_count < 3) {
    return false;
  }

  int self_idx = -1;
  for (uint8_t i = 1; i + 1 < hash_count; i++) {
    if (self_id.isHashMatch(&packet->path[i * hash_size], hash_size)) {
      self_idx = i;
      break;
    }
  }
  if (self_idx < 1) {
    return false;
  }

  const SimpleMeshTables* tables = static_cast<const SimpleMeshTables*>(getTables());
  if (tables == NULL) {
    return false;
  }

  const uint8_t* previous_hop = &packet->path[(self_idx - 1) * hash_size];
  const uint8_t* next_hop = &packet->path[(self_idx + 1) * hash_size];
  if (tables->findRecentRepeaterByHash(previous_hop, hash_size) == NULL
      || tables->findRecentRepeaterByHash(next_hop, hash_size) == NULL) {
    return false;
  }

  uint8_t remaining_count = hash_count - (uint8_t)self_idx;
  memmove(packet->path, &packet->path[self_idx * hash_size], remaining_count * hash_size);
  packet->setPathHashCount(remaining_count);
  MESH_DEBUG_PRINTLN("direct shortcut: skipped %u planned hop(s), remaining=%u",
                     (uint32_t)self_idx,
                     (uint32_t)remaining_count);
  return true;
}

int8_t MyMesh::getDirectRetryMinSNRX4() const {
  switch (active_sf) {
    case 7: return -30;
    case 8: return -40;
    case 9: return -50;
    case 10: return -60;
    case 11: return -70;
    case 12: return -80;
    default: return -60;
  }
}

uint8_t MyMesh::getDirectRetryCodingRateForSNR(int8_t snr_x4) const {
  if (!_prefs.direct_retry_cr_enabled) return 0;
  if (snr_x4 >= _prefs.direct_retry_cr4_snr_x4) return 4;
  if (snr_x4 >= _prefs.direct_retry_cr5_snr_x4) return 5;
  if (snr_x4 <= _prefs.direct_retry_cr8_snr_x4) return 8;
  if (snr_x4 >= _prefs.direct_retry_cr7_snr_x4) return 7;
  return 7;
}

uint8_t MyMesh::getDirectRetryConfiguredMaxAttempts() const {
  return constrain(_prefs.direct_retry_attempts, 1, 15);
}

uint32_t MyMesh::getDirectRetryAttemptStepMillis() const {
  return _prefs.direct_retry_step_ms;
}

bool MyMesh::allowDirectRetry(const mesh::Packet* packet, const uint8_t* next_hop_hash, uint8_t next_hop_hash_len) const {
  (void)packet;
  if (!_prefs.direct_retry_enabled) {
    return false;
  }
  if (!_prefs.direct_retry_recent_enabled) {
    return true;
  }
  if (next_hop_hash == NULL || next_hop_hash_len == 0) {
    return true;
  }
  const SimpleMeshTables* tables = static_cast<const SimpleMeshTables*>(getTables());
  const SimpleMeshTables::RecentRepeaterInfo* repeater = tables != NULL
      ? tables->findRecentRepeaterByHash(next_hop_hash, next_hop_hash_len)
      : NULL;

  if (repeater == NULL) {
    // Retry unknown repeaters too. If they fail, onDirectRetryFailed() seeds the
    // recent-repeater table below the +3.00 dB starting point.
    return true;
  }
  int16_t retry_floor_x4 = (int16_t)getDirectRetryMinSNRX4() + (int16_t)_prefs.direct_retry_snr_margin_x4;
  return (int16_t)repeater->snr_x4 >= retry_floor_x4;
}

void MyMesh::configureDirectRetryPacket(mesh::Packet* retry, const mesh::Packet* original, uint8_t retry_attempt) {
  int8_t snr_x4 = 12;  // unknown repeaters start at +3.00 dB
  const SimpleMeshTables* tables = static_cast<const SimpleMeshTables*>(getTables());
  if (tables != NULL) {
    uint8_t prefix[MAX_HASH_SIZE];
    uint8_t prefix_len = 0;
    if (extractDirectRetryPrefix(original, prefix, prefix_len)) {
      const SimpleMeshTables::RecentRepeaterInfo* repeater = tables->findRecentRepeaterByHash(prefix, prefix_len);
      if (repeater != NULL) {
        snr_x4 = repeater->snr_x4;
      }
    }
  }

  retry->tx_cr = getDirectRetryCodingRateForAttempt(getDirectRetryCodingRateForSNR(snr_x4), retry_attempt);
}

uint32_t MyMesh::getDirectRetryEchoDelay(const mesh::Packet* packet) const {
  uint32_t base_wait_millis = constrain((uint32_t)_prefs.direct_retry_base_ms, (uint32_t)10, (uint32_t)5000);
  if (packet == NULL) {
    return base_wait_millis;
  }

  // Approximate LoRa line rate in kilobits/sec from the live radio params the repeater is using now.
  float kbps = (((float)active_sf) * active_bw * ((float)active_cr)) / ((float)(1UL << active_sf));
  if (kbps <= 0.0f) {
    return base_wait_millis;
  }

  // Wait roughly long enough for our TX, the next hop's receive/forward window, and its echo back.
  uint32_t bits = ((uint32_t)packet->getRawLength()) * 8;
  uint8_t payload_type = packet->getPayloadType();
  float length_factor = 6.0f;
  if (payload_type == PAYLOAD_TYPE_TRACE || payload_type == PAYLOAD_TYPE_ANON_REQ) {
    length_factor = 4.0f;
  } else if (payload_type == PAYLOAD_TYPE_TXT_MSG) {
    length_factor = 7.0f;
  }
  uint32_t scaled_wait_millis = (uint32_t)((((float)bits) * length_factor) / kbps);
  return base_wait_millis + scaled_wait_millis;
}

static uint8_t decodeDirectRetryTraceHashSize(uint8_t flags, uint8_t route_bytes) {
  uint8_t code = flags & 0x03;
  uint8_t size_pow2 = (uint8_t)(1U << code);
  uint8_t size_linear = (uint8_t)(code + 1U);

  bool pow2_ok = size_pow2 > 0 && (route_bytes % size_pow2) == 0;
  bool linear_ok = size_linear > 0 && (route_bytes % size_linear) == 0;

  if (pow2_ok && !linear_ok) return size_pow2;
  if (linear_ok && !pow2_ok) return size_linear;
  if (pow2_ok) return size_pow2;
  return size_linear;
}

uint8_t MyMesh::getDirectRetryMaxAttempts(const mesh::Packet* packet) const {
  if (packet != NULL && packet->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
    return 21;
  }

  uint8_t configured_attempts = getDirectRetryConfiguredMaxAttempts();
  uint8_t total_hops = 0;

  if (packet != NULL) {
    if (packet->isRouteDirect() && packet->getPayloadType() == PAYLOAD_TYPE_TRACE && packet->payload_len >= 9) {
      uint8_t route_bytes = packet->payload_len - 9;
      uint8_t hash_size = decodeDirectRetryTraceHashSize(packet->payload[8], route_bytes);
      if (hash_size > 0) {
        total_hops = (uint8_t)(route_bytes / hash_size);
      }
    } else {
      total_hops = packet->getPathHashCount();
    }
  }

  uint8_t path_cap = 15;
  if (total_hops <= 3) {
    path_cap = 8;
  } else if (total_hops == 4) {
    path_cap = 12;
  }

  return configured_attempts < path_cap ? configured_attempts : path_cap;
}

uint32_t MyMesh::getDirectRetryAttemptDelay(const mesh::Packet* packet, uint8_t attempt_idx) {
  uint32_t retry_delay = getDirectRetryEchoDelay(packet) + ((uint32_t)attempt_idx * getDirectRetryAttemptStepMillis());
  if (packet == NULL) {
    return retry_delay;
  }
  return getDirectRetransmitDelay(packet) + retry_delay;
}

static void formatDirectRetryTarget(char* dest, size_t dest_len, const uint8_t* target_hash, uint8_t target_hash_len) {
  if (dest == NULL || dest_len == 0) {
    return;
  }
  if (target_hash == NULL || target_hash_len == 0 || target_hash_len > MAX_HASH_SIZE) {
    StrHelper::strncpy(dest, "-", dest_len);
    return;
  }

  size_t hex_len = (size_t)target_hash_len * 2;
  if (dest_len <= hex_len) {
    StrHelper::strncpy(dest, "-", dest_len);
    return;
  }

  mesh::Utils::toHex(dest, target_hash, target_hash_len);
  dest[hex_len] = 0;
}

static uint8_t getRetryLogCodingRate(const mesh::Packet* packet, uint8_t default_cr) {
  if (packet != NULL && packet->tx_cr >= 4 && packet->tx_cr <= 8) {
    return packet->tx_cr;
  }
  return default_cr;
}

static uint16_t getRetryLogPreambleLength(const mesh::Packet* packet, uint16_t default_preamble_len) {
  if (packet == NULL || default_preamble_len <= 16 || !(packet->tx_cr == 4 || packet->tx_cr == 5)) {
    return default_preamble_len;
  }

  bool has_direct_path = packet->getPathHashCount() > 0
      || (packet->getPayloadType() == PAYLOAD_TYPE_TRACE && packet->payload_len > 9);
  if (packet->isRouteDirect() && has_direct_path) {
    return 16;
  }
  return default_preamble_len;
}

void MyMesh::onDirectRetryEvent(const char* event, const mesh::Packet* packet, uint32_t delay_millis, uint8_t retry_attempt,
                                const uint8_t* target_hash, uint8_t target_hash_len, int16_t payload_type) {
  char type_label[8];
  char target_label[(MAX_HASH_SIZE * 2) + 1];
  const char* route_label = packet != NULL ? (packet->isRouteDirect() ? "D" : "F") : "D";
  if (packet != NULL) {
    snprintf(type_label, sizeof(type_label), "%u", (uint32_t)packet->getPayloadType());
  } else if (payload_type >= 0) {
    snprintf(type_label, sizeof(type_label), "%u", (uint32_t)payload_type);
  } else {
    strcpy(type_label, "?");
  }
  formatDirectRetryTarget(target_label, sizeof(target_label), target_hash, target_hash_len);
  uint8_t log_cr = getRetryLogCodingRate(packet, getDefaultTxCodingRate());
  uint16_t log_preamble_len = getRetryLogPreambleLength(packet, radio_driver.getDefaultPreambleLength());

#if MESH_DEBUG
  MESH_DEBUG_PRINTLN("direct retry %s attempt=%u delay=%lu type=%s route=%s target=%s cr=%u preamble_len=%u",
                     event ? event : "?",
                     (uint32_t)retry_attempt,
                     (unsigned long)delay_millis,
                     type_label,
                     route_label,
                     target_label,
                     (uint32_t)log_cr,
                     (uint32_t)log_preamble_len);
#endif
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": direct retry %s attempt=%u delay=%lu type=%s route=%s target=%s cr=%u preamble_len=%u\n",
               event ? event : "?",
               (uint32_t)retry_attempt,
               (unsigned long)delay_millis,
               type_label,
               route_label,
               target_label,
               (uint32_t)log_cr,
               (uint32_t)log_preamble_len);
      f.close();
    }
  }
}

void MyMesh::onDirectRetryFailed(const uint8_t* next_hop_hash, uint8_t next_hop_hash_len) {
  if (next_hop_hash == NULL || next_hop_hash_len == 0) {
    return;
  }

  SimpleMeshTables* tables = static_cast<SimpleMeshTables*>(getTables());
  if (tables != NULL) {
    if (!tables->decrementRecentRepeaterSnrX4(next_hop_hash, next_hop_hash_len, 1)) {
      tables->setRecentRepeater(next_hop_hash, next_hop_hash_len, 11);
    }
  }
}

void MyMesh::onDirectRetrySucceeded(const uint8_t* next_hop_hash, uint8_t next_hop_hash_len, int8_t snr_x4) {
  if (next_hop_hash == NULL || next_hop_hash_len == 0) {
    return;
  }

  SimpleMeshTables* tables = static_cast<SimpleMeshTables*>(getTables());
  if (tables != NULL) {
    tables->setRecentRepeater(next_hop_hash, next_hop_hash_len, snr_x4);
  }
}

bool MyMesh::hasFloodRetryPrefixes() const {
  for (int i = 0; i < FLOOD_RETRY_PREFIX_SLOTS; i++) {
    const uint8_t* configured = _prefs.flood_retry_prefixes[i];
    if (configured[0] != 0 || configured[1] != 0 || configured[2] != 0) {
      return true;
    }
  }
  return false;
}

bool MyMesh::floodRetryLastHopMatches(const mesh::Packet* packet) const {
  if (packet == NULL || packet->getPathHashCount() == 0) {
    return false;
  }

  uint8_t hash_size = packet->getPathHashSize();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES) {
    return false;
  }

  const uint8_t* heard_prefix = &packet->path[(packet->getPathHashCount() - 1) * hash_size];
  for (int i = 0; i < FLOOD_RETRY_PREFIX_SLOTS; i++) {
    const uint8_t* configured = _prefs.flood_retry_prefixes[i];
    if ((configured[0] != 0 || configured[1] != 0 || configured[2] != 0)
        && memcmp(configured, heard_prefix, hash_size) == 0) {
      return true;
    }
  }

  return false;
}

bool MyMesh::floodRetryPrefixMatches(const mesh::Packet* packet) const {
  if (packet == NULL || packet->getPathHashCount() == 0) {
    return false;
  }

  uint8_t hash_size = packet->getPathHashSize();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES) {
    return false;
  }

  const uint8_t* path = packet->path;
  for (int hop = 0; hop < packet->getPathHashCount(); hop++) {
    for (int i = 0; i < FLOOD_RETRY_PREFIX_SLOTS; i++) {
      const uint8_t* configured = _prefs.flood_retry_prefixes[i];
      if ((configured[0] != 0 || configured[1] != 0 || configured[2] != 0)
          && memcmp(configured, path, hash_size) == 0) {
        return true;
      }
    }
    path += hash_size;
  }

  return false;
}

bool MyMesh::floodRetryPrefixIgnored(const uint8_t* prefix, uint8_t prefix_len) const {
  if (prefix == NULL || prefix_len == 0 || prefix_len > MAX_ROUTE_HASH_BYTES) {
    return false;
  }
  for (int i = 0; i < FLOOD_RETRY_IGNORE_PREFIXES; i++) {
    const uint8_t* ignored = _prefs.flood_retry_ignore_prefixes[i];
    if ((ignored[0] != 0 || ignored[1] != 0 || ignored[2] != 0)
        && memcmp(ignored, prefix, prefix_len) == 0) {
      return true;
    }
  }
  return false;
}

uint8_t MyMesh::floodRetryEffectivePathLength(const mesh::Packet* packet, uint8_t max_hops) const {
  if (packet == NULL || !packet->isRouteFlood() || packet->getPathHashCount() == 0) {
    return 0;
  }

  uint8_t hash_size = packet->getPathHashSize();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES) {
    return packet->getPathHashCount();
  }

  uint8_t hop_count = packet->getPathHashCount();
  if (max_hops < hop_count) {
    hop_count = max_hops;
  }

  uint8_t effective_len = 0;
  const uint8_t* path = packet->path;
  for (uint8_t hop = 0; hop < hop_count; hop++) {
    if (!floodRetryPrefixIgnored(path, hash_size)) {
      effective_len++;
    }
    path += hash_size;
  }
  return effective_len;
}

bool MyMesh::floodRetryPrefixFresh(const uint8_t* prefix, uint8_t prefix_len) const {
  const SimpleMeshTables* tables = static_cast<const SimpleMeshTables*>(getTables());
  if (tables == NULL) {
    return false;
  }
  const auto* recent = tables->findRecentRepeaterByHash(prefix, prefix_len);
  if (recent == NULL || recent->last_heard_millis == 0) {
    return false;
  }
  return (uint32_t)(millis() - recent->last_heard_millis) <= 3600000UL;
}

static const uint8_t FLOOD_RETRY_BRIDGE_OTHER_BUCKET = FLOOD_RETRY_BRIDGE_BUCKETS;

static uint8_t floodRetryBucketMask(uint8_t bucket) {
  if (bucket >= 8) {
    return 0;
  }
  return (uint8_t)(1U << bucket);
}

int MyMesh::floodRetryBucketForPrefix(const uint8_t* prefix, uint8_t prefix_len, bool require_fresh,
                                      bool include_other) const {
  if (prefix == NULL || prefix_len == 0 || prefix_len > MAX_ROUTE_HASH_BYTES) {
    return -1;
  }
  if (floodRetryPrefixIgnored(prefix, prefix_len)) {
    return -1;
  }
  if (require_fresh && !floodRetryPrefixFresh(prefix, prefix_len)) {
    return -1;
  }
  for (int bucket = 0; bucket < FLOOD_RETRY_BRIDGE_BUCKETS; bucket++) {
    for (int i = 0; i < FLOOD_RETRY_BUCKET_PREFIXES; i++) {
      const uint8_t* configured = _prefs.flood_retry_bridge_buckets[bucket][i];
      if ((configured[0] != 0 || configured[1] != 0 || configured[2] != 0)
          && memcmp(configured, prefix, prefix_len) == 0) {
        return bucket;
      }
    }
  }
  if (include_other) {
    return FLOOD_RETRY_BRIDGE_OTHER_BUCKET;
  }
  return -1;
}

int MyMesh::floodRetryBucketForPathHop(const uint8_t* prefix, uint8_t prefix_len, uint8_t hop,
                                       uint8_t progress_marker) const {
  return floodRetryBucketForPrefix(prefix, prefix_len, hop < progress_marker, true);
}

int MyMesh::floodRetrySourceBucket(const mesh::Packet* packet) const {
  if (packet == NULL) {
    return -1;
  }
  uint8_t hash_size = packet->getPathHashSize();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES) {
    return -1;
  }
  if (packet->getPathHashCount() < 2) {
    return FLOOD_RETRY_BRIDGE_OTHER_BUCKET;
  }
  const uint8_t* source_prefix = &packet->path[(packet->getPathHashCount() - 2) * hash_size];
  return floodRetryBucketForPrefix(source_prefix, hash_size, true, true);
}

uint8_t MyMesh::floodRetryBridgeTargetMask(uint8_t source_bucket) const {
  uint8_t mask = 0;
  for (int bucket = 0; bucket < FLOOD_RETRY_BRIDGE_BUCKETS; bucket++) {
    if (bucket == source_bucket) {
      continue;
    }
    for (int i = 0; i < FLOOD_RETRY_BUCKET_PREFIXES; i++) {
      const uint8_t* configured = _prefs.flood_retry_bridge_buckets[bucket][i];
      if ((configured[0] != 0 || configured[1] != 0 || configured[2] != 0)
          && !floodRetryPrefixIgnored(configured, FLOOD_RETRY_PREFIX_LEN)
          && floodRetryPrefixFresh(configured, FLOOD_RETRY_PREFIX_LEN)) {
        mask |= floodRetryBucketMask((uint8_t)bucket);
        break;
      }
    }
  }
  if (source_bucket != FLOOD_RETRY_BRIDGE_OTHER_BUCKET) {
    mask |= floodRetryBucketMask(FLOOD_RETRY_BRIDGE_OTHER_BUCKET);
  }
  return mask;
}

uint8_t MyMesh::floodRetryBridgeHeardMask(const mesh::Packet* packet, uint8_t source_bucket,
                                          uint8_t progress_marker) const {
  if (packet == NULL || packet->getPathHashCount() == 0) {
    return 0;
  }
  uint8_t hash_size = packet->getPathHashSize();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES) {
    return 0;
  }

  uint8_t mask = 0;
  const uint8_t* path = packet->path;
  for (int hop = 0; hop < packet->getPathHashCount(); hop++) {
    if (progress_marker > 0 && hop == progress_marker - 1) {
      path += hash_size;
      continue;
    }
    int bucket = floodRetryBucketForPathHop(path, hash_size, (uint8_t)hop, progress_marker);
    if (bucket >= 0 && bucket != source_bucket) {
      mask |= floodRetryBucketMask((uint8_t)bucket);
    }
    path += hash_size;
  }
  return mask;
}

MyMesh::FloodRetryBridgeState* MyMesh::floodRetryBridgeStateFor(const mesh::Packet* packet, bool create) const {
  if (packet == NULL) {
    return NULL;
  }

  uint8_t key[MAX_HASH_SIZE];
  packet->calculatePacketHash(key);
  FloodRetryBridgeState* free_slot = NULL;
  for (int i = 0; i < MAX_FLOOD_RETRY_SLOTS; i++) {
    if (flood_retry_bridge_states[i].active
        && memcmp(flood_retry_bridge_states[i].key, key, MAX_HASH_SIZE) == 0) {
      return &flood_retry_bridge_states[i];
    }
    if (!flood_retry_bridge_states[i].active && free_slot == NULL) {
      free_slot = &flood_retry_bridge_states[i];
    }
  }
  if (!create || free_slot == NULL) {
    return NULL;
  }

  int source_bucket = floodRetrySourceBucket(packet);
  if (source_bucket < 0) {
    return NULL;
  }

  uint8_t target_mask = floodRetryBridgeTargetMask((uint8_t)source_bucket);
  if (target_mask == 0) {
    return NULL;
  }

  uint8_t progress_marker = packet->getPathHashCount();
  uint8_t heard_mask = floodRetryBridgeHeardMask(packet, (uint8_t)source_bucket, progress_marker) & target_mask;
  if ((heard_mask & target_mask) == target_mask) {
    return NULL;
  }

  memset(free_slot, 0, sizeof(*free_slot));
  memcpy(free_slot->key, key, sizeof(free_slot->key));
  free_slot->source_bucket = (uint8_t)source_bucket;
  free_slot->target_mask = target_mask;
  free_slot->heard_mask = heard_mask;
  free_slot->progress_marker = progress_marker;
  free_slot->active = true;
  return free_slot;
}

bool MyMesh::allowFloodRetry(const mesh::Packet* packet) const {
  if (_prefs.disable_fwd || constrain(_prefs.flood_retry_attempts, 0, 15) == 0) {
    return false;
  }
  if (packet != NULL && packet->getPayloadType() == PAYLOAD_TYPE_ADVERT && !_prefs.flood_retry_advert_enabled) {
    return false;
  }
  if (!_prefs.flood_retry_bridge_enabled) {
    return true;
  }
  FloodRetryBridgeState* state = floodRetryBridgeStateFor(packet, true);
  if (state == NULL) {
    return false;
  }
  if ((state->heard_mask & state->target_mask) == state->target_mask) {
    state->active = false;
    return false;
  }
  return true;
}

void MyMesh::clearFloodRetryBridgeState(const mesh::Packet* packet) {
  FloodRetryBridgeState* state = floodRetryBridgeStateFor(packet, false);
  if (state != NULL) {
    state->active = false;
  }
}

void MyMesh::refreshFloodRetryHeardRecent(const mesh::Packet* packet) {
  if (packet == NULL || !packet->isRouteFlood() || packet->getPathHashCount() == 0) {
    return;
  }

  uint8_t hash_size = packet->getPathHashSize();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES) {
    return;
  }

  SimpleMeshTables* tables = static_cast<SimpleMeshTables*>(getTables());
  if (tables == NULL) {
    return;
  }
  const uint8_t* path = packet->path;
  if (_prefs.flood_retry_bridge_enabled) {
    FloodRetryBridgeState* state = floodRetryBridgeStateFor(packet, false);
    if (state != NULL) {
      for (int hop = 0; hop < packet->getPathHashCount(); hop++) {
        if (state->progress_marker > 0 && hop == state->progress_marker - 1) {
          path += hash_size;
          continue;
        }
        int bucket = floodRetryBucketForPathHop(path, hash_size, (uint8_t)hop, state->progress_marker);
        uint8_t bucket_mask = bucket >= 0 ? floodRetryBucketMask((uint8_t)bucket) : 0;
        if (bucket >= 0 && bucket != state->source_bucket && (state->target_mask & bucket_mask)) {
          tables->setRecentRepeater(path, hash_size, packet->_snr, false, true);
        }
        path += hash_size;
      }
      return;
    }
  }

  const uint8_t* heard_prefix = &packet->path[(packet->getPathHashCount() - 1) * hash_size];
  tables->setRecentRepeater(heard_prefix, hash_size, packet->_snr, false, true);
}

void MyMesh::formatFloodRetryPath(char* dest, size_t dest_len, const mesh::Packet* packet) const {
  if (dest == NULL || dest_len == 0) {
    return;
  }
  dest[0] = 0;

  if (packet == NULL || packet->getPathHashCount() == 0) {
    StrHelper::strncpy(dest, "-", dest_len);
    return;
  }

  uint8_t hash_size = packet->getPathHashSize();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES) {
    StrHelper::strncpy(dest, "invalid", dest_len);
    return;
  }

  char* out = dest;
  size_t remaining = dest_len;
  const uint8_t* path = packet->path;
  for (int hop = 0; hop < packet->getPathHashCount(); hop++) {
    size_t needed = (hop > 0 ? 1 : 0) + ((size_t)hash_size * 2) + 1;
    if (remaining < needed) {
      if (remaining > 4) {
        strcpy(out, "...");
      }
      return;
    }
    if (hop > 0) {
      *out++ = '>';
      remaining--;
    }
    mesh::Utils::toHex(out, path, hash_size);
    out += (size_t)hash_size * 2;
    remaining -= (size_t)hash_size * 2;
    path += hash_size;
  }
}

bool MyMesh::formatFloodRetryHeard(char* dest, size_t dest_len, const mesh::Packet* packet) const {
  if (dest == NULL || dest_len == 0 || packet == NULL || packet->getPathHashCount() == 0) {
    return false;
  }
  dest[0] = 0;

  uint8_t hash_size = packet->getPathHashSize();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES) {
    return false;
  }

  char* out = dest;
  size_t remaining = dest_len;
  bool first = true;

  if (_prefs.flood_retry_bridge_enabled) {
    FloodRetryBridgeState* state = floodRetryBridgeStateFor(packet, false);
    if (state == NULL) {
      return false;
    }
    const uint8_t* path = packet->path;
    for (int hop = 0; hop < packet->getPathHashCount(); hop++) {
      if (state->progress_marker > 0 && hop == state->progress_marker - 1) {
        path += hash_size;
        continue;
      }
      int bucket = floodRetryBucketForPathHop(path, hash_size, (uint8_t)hop, state->progress_marker);
      uint8_t bucket_mask = bucket >= 0 ? floodRetryBucketMask((uint8_t)bucket) : 0;
      if (bucket >= 0 && bucket != state->source_bucket && (state->target_mask & bucket_mask)) {
        char bucket_label[8];
        if ((uint8_t)bucket == FLOOD_RETRY_BRIDGE_OTHER_BUCKET) {
          strcpy(bucket_label, "other");
        } else {
          snprintf(bucket_label, sizeof(bucket_label), "b%d", bucket + 1);
        }
        size_t needed = (first ? 0 : 1) + strlen(bucket_label) + 1 + ((size_t)hash_size * 2) + 1;
        if (remaining < needed) {
          if (remaining > 4) {
            strcpy(out, "...");
          }
          return dest[0] != 0;
        }
        if (!first) {
          *out++ = ',';
          remaining--;
        }
        int n = snprintf(out, remaining, "%s:", bucket_label);
        if (n < 0 || (size_t)n >= remaining) {
          return dest[0] != 0;
        }
        out += n;
        remaining -= n;
        mesh::Utils::toHex(out, path, hash_size);
        out += (size_t)hash_size * 2;
        remaining -= (size_t)hash_size * 2;
        first = false;
      }
      path += hash_size;
    }
    return dest[0] != 0;
  }

  const uint8_t* heard_prefix = &packet->path[(packet->getPathHashCount() - 1) * hash_size];
  if (remaining < ((size_t)hash_size * 2) + 1) {
    return false;
  }
  mesh::Utils::toHex(out, heard_prefix, hash_size);
  return true;
}

void MyMesh::onFloodRetryEvent(const char* event, const mesh::Packet* packet, uint32_t delay_millis, uint8_t retry_attempt) {
  if (event == NULL || packet == NULL) {
    return;
  }

  bool clear_bridge_state = _prefs.flood_retry_bridge_enabled
      && (strcmp(event, "good") == 0 || strcmp(event, "failure") == 0 || strcmp(event, "failed_all_tries") == 0
          || strncmp(event, "dropped_", 8) == 0);

  if (clear_bridge_state && strcmp(event, "failure") == 0) {
    clearFloodRetryBridgeState(packet);
  }

  if (strcmp(event, "failure") == 0) {
    return;
  }

  const char* time_label = "time_ms";
  if (strcmp(event, "queued") == 0 || strcmp(event, "dropped_queue_full") == 0) {
    time_label = "wait_ms";
  } else if (strcmp(event, "resent") == 0 || strcmp(event, "failed_all_tries") == 0
      || strcmp(event, "failure") == 0 || strncmp(event, "dropped_", 8) == 0) {
    time_label = "elapsed_ms";
  } else if (strcmp(event, "good") == 0) {
    time_label = "echo_ms";
  }

  char path_log[208];
  char heard_log[96];
  char heard_suffix[112];
  formatFloodRetryPath(path_log, sizeof(path_log), packet);
  heard_suffix[0] = 0;
  if (strcmp(event, "good") == 0 && formatFloodRetryHeard(heard_log, sizeof(heard_log), packet)) {
    refreshFloodRetryHeardRecent(packet);
    snprintf(heard_suffix, sizeof(heard_suffix), ", heard=%s", heard_log);
  }
  uint8_t log_cr = getRetryLogCodingRate(packet, getDefaultTxCodingRate());
  uint16_t log_preamble_len = getRetryLogPreambleLength(packet, radio_driver.getDefaultPreambleLength());

  MESH_DEBUG_PRINTLN("flood retry %s (retry=%u, type=%d, route=%s, payload_len=%d, hop=%u, path=%s%s, %s=%lu, cr=%u, preamble_len=%u)",
                     event,
                     (unsigned int)retry_attempt,
                     (uint32_t)packet->getPayloadType(),
                     packet->isRouteDirect() ? "D" : "F",
                     (uint32_t)packet->payload_len,
                     (unsigned int)packet->getPathHashCount(),
                     path_log,
                     heard_suffix,
                     time_label,
                     (unsigned long)delay_millis,
                     (uint32_t)log_cr,
                     (uint32_t)log_preamble_len);

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": FLOOD RETRY %s (retry=%u, type=%d, route=%s, payload_len=%d, hop=%u, path=%s%s, %s=%lu, cr=%u, preamble_len=%u)\n",
               event,
               (unsigned int)retry_attempt,
               (uint32_t)packet->getPayloadType(),
               packet->isRouteDirect() ? "D" : "F",
               (uint32_t)packet->payload_len,
               (unsigned int)packet->getPathHashCount(),
               path_log,
               heard_suffix,
               time_label,
               (unsigned long)delay_millis,
               (uint32_t)log_cr,
               (uint32_t)log_preamble_len);
      f.close();
    }
  }

  if (clear_bridge_state) {
    clearFloodRetryBridgeState(packet);
  }
}

bool MyMesh::hasFloodRetryTargetPrefix(const mesh::Packet* packet) const {
  if (_prefs.flood_retry_bridge_enabled) {
    return false;
  }
  return floodRetryPrefixMatches(packet);
}

uint8_t MyMesh::getFloodRetryMaxPathLength(const mesh::Packet* packet) const {
  uint8_t gate = _prefs.flood_retry_max_path;
  if (gate == FLOOD_RETRY_PATH_GATE_DISABLED) {
    return FLOOD_RETRY_PATH_GATE_DISABLED;
  }
  if (gate > 63) {
    gate = FLOOD_RETRY_ROOFTOP_MAX_PATH;
  }

  uint8_t raw_hops = packet != NULL ? packet->getPathHashCount() : 0;
  uint8_t effective_hops = floodRetryEffectivePathLength(packet);
  uint8_t ignored_hops = raw_hops > effective_hops ? raw_hops - effective_hops : 0;
  uint16_t adjusted_gate = (uint16_t)gate + ignored_hops;
  return adjusted_gate > 63 ? 63 : (uint8_t)adjusted_gate;
}

uint8_t MyMesh::getFloodRetryMaxAttempts(const mesh::Packet* packet) const {
  if (_prefs.disable_fwd) {
    return 0;
  }

  uint8_t attempts = constrain(_prefs.flood_retry_attempts, 0, 15);
  uint16_t scaled_attempts = attempts;
  uint8_t hops = packet != NULL ? packet->getPathHashCount() : 0;
  if (hops == 1) {
    scaled_attempts = (uint16_t)attempts * 2U;
  } else if (hops == 2) {
    scaled_attempts = (((uint16_t)attempts * 3U) + 1U) / 2U;
  }
  return scaled_attempts > 15 ? 15 : (uint8_t)scaled_attempts;
}

bool MyMesh::isFloodRetryEchoTarget(const mesh::Packet* packet, uint8_t progress_marker) const {
  if (packet == NULL || !packet->isRouteFlood()) {
    return false;
  }
  if (_prefs.flood_retry_bridge_enabled) {
    FloodRetryBridgeState* state = floodRetryBridgeStateFor(packet, false);
    if (state == NULL) {
      return false;
    }
    state->heard_mask |= floodRetryBridgeHeardMask(packet, state->source_bucket, state->progress_marker) & state->target_mask;
    return (state->heard_mask & state->target_mask) == state->target_mask;
  }
  if (packet->getPathHashCount() == 0) {
    return false;
  }
  uint8_t hash_size = packet->getPathHashSize();
  if (hash_size == 0 || hash_size > MAX_ROUTE_HASH_BYTES) {
    return false;
  }
  const uint8_t* heard_prefix = &packet->path[(packet->getPathHashCount() - 1) * hash_size];
  if (floodRetryPrefixIgnored(heard_prefix, hash_size)) {
    return false;
  }
  if (hasFloodRetryPrefixes()) {
    return floodRetryLastHopMatches(packet);
  }
  return true;
}

static void formatLocalSnrX4(char* dest, size_t dest_len, int16_t snr_x4) {
  int16_t v = snr_x4;
  const char* sign = "";
  if (v < 0) {
    sign = "-";
    v = -v;
  }
  snprintf(dest, dest_len, "%s%d.%02d", sign, v / 4, (v % 4) * 25);
  size_t len = strlen(dest);
  if (len > 3 && dest[len - 1] == '0') {
    dest[len - 1] = 0;
  }
}

static bool parseRecentRepeatersPageCommand(const char* command, int& page) {
  if (strncmp(command, "get ", 4) != 0) {
    return false;
  }

  const char* cursor = command + 4;
  if (strncmp(cursor, "recent.repeater", 15) != 0) {
    return false;
  }
  cursor += 15;

  if (*cursor == 's') {
    cursor++;
  }
  if (*cursor == 0) {
    return false;
  }
  if (*cursor != ' ') {
    return false;
  }

  while (*cursor == ' ') cursor++;
  if (strncmp(cursor, "page", 4) == 0 && (cursor[4] == 0 || cursor[4] == ' ')) {
    cursor += 4;
    while (*cursor == ' ') cursor++;
  }

  page = 1;
  if (*cursor) page = atoi(cursor);
  if (page < 1) page = 1;
  return true;
}

void MyMesh::formatRecentRepeatersReply(char *reply, int page) {
  const SimpleMeshTables* tables = static_cast<const SimpleMeshTables*>(getTables());
  if (tables == NULL) {
    strcpy(reply, "Error: unsupported");
    return;
  }
  int count = tables->getRecentRepeaterCount();
  if (count <= 0) {
    strcpy(reply, "> -none-");
    return;
  }

  const int page_size = 10;
  int pages = (count + page_size - 1) / page_size;
  if (page < 1) page = 1;
  if (page > pages) page = pages;

  int len = snprintf(reply, 160, "> %d/%d", page, pages);
  int start = (page - 1) * page_size;
  for (int i = 0; i < page_size && len < 150; i++) {
    const SimpleMeshTables::RecentRepeaterInfo* info = tables->getRecentRepeaterBySortedIdx(start + i);
    if (info == NULL) break;
    char prefix[MAX_ROUTE_HASH_BYTES * 2 + 1];
    char snr[12];
    mesh::Utils::toHex(prefix, info->prefix, info->prefix_len);
    prefix[info->prefix_len * 2] = 0;
    formatLocalSnrX4(snr, sizeof(snr), info->snr_x4);
    len += snprintf(&reply[len], 160 - len, "\n%s,%s%s",
                    prefix,
                    snr[0] == '-' ? "" : " ",
                    snr);
  }
}

void MyMesh::printRecentRepeatersSerial() {
  const SimpleMeshTables* tables = static_cast<const SimpleMeshTables*>(getTables());
  if (tables == NULL) {
    Serial.println("Error: unsupported");
    return;
  }

  int count = tables->getRecentRepeaterCount();
  Serial.printf("Recent repeaters (%d):\n", count);
  if (count <= 0) {
    Serial.println("-none-");
    return;
  }

  for (int i = 0; i < count; i++) {
    const SimpleMeshTables::RecentRepeaterInfo* info = tables->getRecentRepeaterBySortedIdx(i);
    if (info == NULL) break;
    char prefix[MAX_ROUTE_HASH_BYTES * 2 + 1];
    char snr[12];
    mesh::Utils::toHex(prefix, info->prefix, info->prefix_len);
    prefix[info->prefix_len * 2] = 0;
    formatLocalSnrX4(snr, sizeof(snr), info->snr_x4);
    Serial.printf("%s,%s%s\n", prefix, snr[0] == '-' ? "" : " ", snr);
  }
}

bool MyMesh::setRecentRepeater(const uint8_t* prefix, uint8_t prefix_len, int8_t snr_x4) {
  SimpleMeshTables* tables = static_cast<SimpleMeshTables*>(getTables());
  return tables != NULL && tables->setRecentRepeater(prefix, prefix_len, snr_x4);
}

void MyMesh::clearRecentRepeaters() {
  SimpleMeshTables* tables = static_cast<SimpleMeshTables*>(getTables());
  if (tables != NULL) {
    tables->clearRecentRepeaters();
  }
}

mesh::DispatcherAction MyMesh::onRecvPacket(mesh::Packet* pkt) {
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  return Mesh::onRecvPacket(pkt);
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    data[len] = 0;  // ensure null terminator
    uint8_t reply_len;

    reply_path_len = -1;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    if (packet->isRouteFlood()) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else if (reply_path_len < 0) {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      if (reply) sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    } else {
      mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
      uint8_t path_len = ((reply_path_hash_size - 1) << 6) | (reply_path_len & 63);
      if (reply) sendDirect(reply, reply_path,  path_len, SERVER_RESPONSE_DELAY);
    }
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->getPathHashCount() == 0 && !isShare(packet)) {
    AdvertDataParser parser(app_data, app_data_len);
    if (parser.isValid() && parser.getType() == ADV_TYPE_REPEATER) { // just keep neigbouring Repeaters
      putNeighbour(id, timestamp, packet->getSNR());
    }
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        sendClientReply(client, reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && (client->isAdmin() || client->isRegionMgr())) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        sendClientReply(client, ack, TXT_ACK_DELAY, packet->getPathHashSize());
      }

      uint8_t temp[166];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
        handleCommand(sender_timestamp, client, command, reply);
      }
      int text_len = strlen(reply);
      if (text_len > 0) {
        uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
        if (timestamp == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          timestamp++;
        }
        memcpy(temp, &timestamp, 4);        // mostly an extra blob to help make packet_hash unique
        temp[4] = (TXT_TYPE_CLI_DATA << 2); // NOTE: legacy was: TXT_TYPE_PLAIN

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        sendClientReply(client, reply, CLI_REPLY_DELAY_MILLIS, packet->getPathHashSize());
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    // store a copy of path, for sendDirect()
    if (client->out_path_len != OUT_PATH_FORCE_FLOOD) {
      client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    }
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  } else if (type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6) {
    uint8_t node_type = packet->payload[0] & 0x0F;
    if (node_type != ADV_TYPE_REPEATER) {
      return;
    }
    if (packet->payload_len < 6 + PUB_KEY_SIZE) {
      MESH_DEBUG_PRINTLN("onControlDataRecv: DISCOVER_RESP pubkey too short: %d", (uint32_t)packet->payload_len);
      return;
    }

    if (pending_discover_tag == 0 || millisHasNowPassed(pending_discover_until)) {
      pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    if (tag != pending_discover_tag) {
      return;
    }

    mesh::Identity id(&packet->payload[6]);
    if (id.matches(self_id)) {
      return;
    }
    putNeighbour(id, rtc_clock.getCurrentTime(), packet->getSNR());
  }
}

void MyMesh::sendNodeDiscoverReq() {
  uint8_t data[10];
  data[0] = CTL_TYPE_NODE_DISCOVER_REQ; // prefix_only=0
  data[1] = (1 << ADV_TYPE_REPEATER);
  getRNG()->random(&data[2], 4); // tag
  memcpy(&pending_discover_tag, &data[2], 4);
  pending_discover_until = futureMillis(60000);
  uint32_t since = 0;
  memcpy(&data[6], &since, 4);

  auto pkt = createControlData(data, sizeof(data));
  if (pkt) {
    sendZeroHop(pkt);
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180)   // max 4 every 3 minutes
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  next_battery_alert_check = 0;
  last_battery_alert_sent = 0;
  battery_alert_sent = false;
  dirty_contacts_expiry = 0;
  active_bw = 0.0f;
  active_sf = 0;
  active_cr = 0;
  memset(scheduled_radio_settings, 0, sizeof(scheduled_radio_settings));
  _logging = false;
  region_load_active = false;
  memset(flood_retry_bridge_states, 0, sizeof(flood_retry_bridge_states));
  recv_pkt_region = NULL;
  memset(flood_channel_blocks, 0, sizeof(flood_channel_blocks));

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  memset(&_prefs, 0, sizeof(_prefs));
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = DEFAULT_RX_DELAY_BASE;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.3f; // was 0.2
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = DEFAULT_ADVERT_INTERVAL_MINUTES / 2;
  _prefs.flood_advert_interval = DEFAULT_FLOOD_ADVERT_INTERVAL_HOURS;
  _prefs.flood_max = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert = 8;
  _prefs.interference_threshold = 0; // disabled
  _prefs.cad_enabled = 0;            // hardware CAD before TX (off by default; 'set cad on')
  _prefs.agc_reset_interval = DEFAULT_AGC_RESET_INTERVAL_SECONDS / 4;
  _prefs.multi_acks = DEFAULT_MULTI_ACKS;
  _prefs.path_hash_mode = DEFAULT_PATH_HASH_MODE;
  _prefs.loop_detect = DEFAULT_LOOP_DETECT;
  _prefs.retry_preset = RETRY_PRESET_ROOFTOP;
  _prefs.direct_retry_attempts = DIRECT_RETRY_ROOFTOP_COUNT;
  _prefs.direct_retry_base_ms = DIRECT_RETRY_ROOFTOP_BASE_MS;
  _prefs.direct_retry_step_ms = DIRECT_RETRY_ROOFTOP_STEP_MS;
  _prefs.direct_retry_snr_margin_x4 = DIRECT_RETRY_ROOFTOP_MARGIN_X4;
  _prefs.direct_retry_cr4_snr_x4 = DIRECT_RETRY_CR4_MIN_SNR_X4_DEFAULT;
  _prefs.direct_retry_cr5_snr_x4 = DIRECT_RETRY_CR5_MIN_SNR_X4_DEFAULT;
  _prefs.direct_retry_cr7_snr_x4 = DIRECT_RETRY_CR7_MIN_SNR_X4_DEFAULT;
  _prefs.direct_retry_cr8_snr_x4 = DIRECT_RETRY_CR8_MAX_SNR_X4_DEFAULT;
  _prefs.direct_retry_enabled = 1;
  _prefs.direct_retry_cr_enabled = 1;
  _prefs.direct_retry_prefs_magic[0] = DIRECT_RETRY_PREFS_MAGIC_0;
  _prefs.direct_retry_prefs_magic[1] = DIRECT_RETRY_PREFS_MAGIC_1;
  _prefs.direct_retry_recent_enabled = DIRECT_RETRY_RECENT_DEFAULT;
  _prefs.flood_retry_attempts = FLOOD_RETRY_ROOFTOP_COUNT;
  _prefs.flood_retry_max_path = FLOOD_RETRY_ROOFTOP_MAX_PATH;
  _prefs.flood_retry_bridge_enabled = 0;
  _prefs.flood_retry_advert_enabled = FLOOD_RETRY_ADVERT_DEFAULT;
  _prefs.flood_channel_data_enabled = 1;
  _prefs.flood_channel_block_max_hops = FLOOD_CHANNEL_BLOCK_HOPS_ALL;
  _prefs.flood_channel_data_max_hops = FLOOD_CHANNEL_BLOCK_HOPS_ALL;
  _prefs.battery_alert_enabled = 0;
  _prefs.battery_alert_low_percent = BATTERY_ALERT_LOW_PERCENT_DEFAULT;
  _prefs.battery_alert_critical_percent = BATTERY_ALERT_CRITICAL_PERCENT_DEFAULT;

  // bridge defaults
  _prefs.bridge_enabled = 1;    // enabled
  _prefs.bridge_delay   = 500;  // milliseconds
  _prefs.bridge_pkt_src = 0;    // logTx
  _prefs.bridge_baud = 115200;  // baud rate
  _prefs.bridge_channel = 1;    // channel 1

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default;
#endif
#endif
  _prefs.radio_fem_rxgain = 1;

  pending_discover_tag = 0;
  pending_discover_until = 0;

  memset(default_scope.key, 0, sizeof(default_scope.key));
}

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();
  _fs = fs;
  // load persisted prefs
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
  // TODO: key_store.begin();
  region_map.load(_fs);
  loadFloodChannelBlocks();

  // establish default-scope
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
#ifdef DEFAULT_FLOOD_SCOPE_NAME
      r = region_map.findByName(DEFAULT_FLOOD_SCOPE_NAME);
      if (r == NULL) {
        r = region_map.putRegion(DEFAULT_FLOOD_SCOPE_NAME, 0);  // auto-create the default scope region
        if (r) { r->flags = 0; }   // Allow-flood
      }
      if (r) {
        region_map.setDefaultRegion(r);
        region_map.getTransportKeysFor(*r, &default_scope, 1);
      }
#endif
    }
  }

#if defined(WITH_BRIDGE)
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif

  applySavedRadioParams();
  radio_driver.setTxPower(_prefs.tx_power_dbm);

  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",
                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");
  board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain);

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

void MyMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;  // REVISIT: set to 'home' Region, for sender/return region?
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

bool MyMesh::sendRepeatersFloodText(const char* text) {
  if (text == NULL || *text == 0) return false;

  mesh::GroupChannel channel;
  if (!buildRepeatersChannel(channel)) {
    return false;
  }

  uint8_t temp[MAX_PACKET_PAYLOAD];
  uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
  memcpy(temp, &timestamp, 4);
  temp[4] = (TXT_TYPE_PLAIN << 2);

  const size_t max_data_len = MAX_PACKET_PAYLOAD - CIPHER_BLOCK_SIZE;
  const size_t prefix_cap = max_data_len > 5 ? max_data_len - 5 + 1 : 0;
  char node_name[sizeof(_prefs.node_name)];
  StrHelper::strncpy(node_name, _prefs.node_name, sizeof(node_name));
  for (char* p = node_name; *p; p++) {
    if (*p == ':') *p = ';';
  }
  int prefix_written = prefix_cap > 0
      ? snprintf((char*)&temp[5], prefix_cap, "%s: ", node_name)
      : -1;
  if (prefix_written < 0) {
    return false;
  }

  size_t prefix_len = (size_t)prefix_written;
  if (prefix_len >= prefix_cap) {
    prefix_len = prefix_cap - 1;
  }

  size_t text_len = strlen(text);
  size_t max_text_len = max_data_len - 5 - prefix_len;
  if (text_len > max_text_len) {
    text_len = max_text_len;
  }
  memcpy(&temp[5 + prefix_len], text, text_len);

  auto pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, temp, 5 + prefix_len + text_len);
  if (pkt == NULL) {
    return false;
  }

  sendFloodScoped(default_scope, pkt, 0, _prefs.path_hash_mode + 1);
  return true;
}

void MyMesh::checkBatteryAlert() {
  if (!_prefs.battery_alert_enabled) {
    battery_alert_sent = false;
    return;
  }

  if (next_battery_alert_check && !millisHasNowPassed(next_battery_alert_check)) {
    return;
  }
  next_battery_alert_check = futureMillis(LOW_BATTERY_CHECK_INTERVAL);

  uint16_t batt_mv = board.getBattMilliVolts();
  uint8_t batt_pct = batteryPercentFromMilliVolts(batt_mv);
  if (batt_mv <= LOW_BATTERY_MIN_VALID_MV || batt_pct >= _prefs.battery_alert_low_percent) {
    battery_alert_sent = false;
    return;
  }

  unsigned long interval = batt_pct <= _prefs.battery_alert_critical_percent
      ? LOW_BATTERY_CRITICAL_INTERVAL
      : LOW_BATTERY_WARN_INTERVAL;
  if (battery_alert_sent && !millisHasNowPassed(last_battery_alert_sent + interval)) {
    return;
  }

  char text[96];
  snprintf(text, sizeof(text), "LOW BATTERY %u%% (%u mV)", (uint32_t)batt_pct, (uint32_t)batt_mv);
  if (sendRepeatersFloodText(text)) {
    battery_alert_sent = true;
    last_battery_alert_sent = millis();
  }
}

void MyMesh::applyRadioParams(float freq, float bw, uint8_t sf, uint8_t cr) {
  radio_driver.setParams(freq, bw, sf, cr);
  active_bw = bw;
  active_sf = sf;
  active_cr = cr;
}

void MyMesh::applySavedRadioParams() {
  applyRadioParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
}

bool MyMesh::hasStartedScheduledTempRadio() const {
  for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
    const ScheduledRadioSetting& setting = scheduled_radio_settings[i];
    if (setting.active && setting.temporary && setting.started) {
      return true;
    }
  }
  return false;
}

int MyMesh::findFreeScheduledRadioSlot() const {
  for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
    if (!scheduled_radio_settings[i].active) {
      return i;
    }
  }
  return -1;
}

int MyMesh::countScheduledRadioSettings(bool temporary) const {
  int count = 0;
  for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
    const ScheduledRadioSetting& setting = scheduled_radio_settings[i];
    if (setting.active && setting.temporary == temporary) {
      count++;
    }
  }
  return count;
}

int MyMesh::findScheduledRadioSettingByIndex(bool temporary, int wanted) const {
  bool used[MAX_SCHEDULED_RADIO_SETTINGS] = {};
  for (int rank = 1; rank <= wanted; rank++) {
    int best = -1;
    for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
      const ScheduledRadioSetting& setting = scheduled_radio_settings[i];
      if (!setting.active || setting.temporary != temporary || used[i]) {
        continue;
      }
      if (best < 0 || setting.start_time < scheduled_radio_settings[best].start_time
          || (setting.start_time == scheduled_radio_settings[best].start_time && i < best)) {
        best = i;
      }
    }
    if (best < 0) {
      return -1;
    }
    used[best] = true;
    if (rank == wanted) {
      return best;
    }
  }
  return -1;
}

int MyMesh::getScheduledRadioSettingIndex(bool temporary, int slot_idx) const {
  int count = countScheduledRadioSettings(temporary);
  for (int i = 1; i <= count; i++) {
    if (findScheduledRadioSettingByIndex(temporary, i) == slot_idx) {
      return i;
    }
  }
  return -1;
}

bool MyMesh::scheduledRadioConflicts(bool temporary, uint32_t start_time, uint32_t end_time) const {
  for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
    const ScheduledRadioSetting& setting = scheduled_radio_settings[i];
    if (!setting.active) {
      continue;
    }
    if (temporary) {
      if (setting.temporary && start_time < setting.end_time && end_time > setting.start_time) {
        return true;
      }
      if (!setting.temporary && setting.start_time >= start_time && setting.start_time < end_time) {
        return true;
      }
    } else {
      if (!setting.temporary && setting.start_time == start_time) {
        return true;
      }
      if (setting.temporary && start_time >= setting.start_time && start_time < setting.end_time) {
        return true;
      }
    }
  }
  return false;
}

void MyMesh::clearScheduledRadioSetting(int idx, bool restore_if_started) {
  if (idx < 0 || idx >= MAX_SCHEDULED_RADIO_SETTINGS) {
    return;
  }
  bool restore_radio = restore_if_started
      && scheduled_radio_settings[idx].active
      && scheduled_radio_settings[idx].temporary
      && scheduled_radio_settings[idx].started;
  scheduled_radio_settings[idx].active = false;
  scheduled_radio_settings[idx].started = false;
  if (restore_radio && !hasStartedScheduledTempRadio()) {
    applySavedRadioParams();
  }
}

void MyMesh::formatScheduledRadioDuration(char* dest, size_t dest_len, uint32_t target_time) const {
  uint32_t now = getRTCClock()->getCurrentTime();
  if (target_time <= now) {
    StrHelper::strncpy(dest, "now", dest_len);
    return;
  }

  uint32_t seconds = target_time - now;
  uint32_t days = seconds / 86400;
  seconds %= 86400;
  uint32_t hours = seconds / 3600;
  seconds %= 3600;
  uint32_t minutes = seconds / 60;
  seconds %= 60;

  if (days > 0) {
    snprintf(dest, dest_len, "%lud%luh", (unsigned long)days, (unsigned long)hours);
  } else if (hours > 0) {
    snprintf(dest, dest_len, "%luh%lum", (unsigned long)hours, (unsigned long)minutes);
  } else if (minutes > 0) {
    snprintf(dest, dest_len, "%lum%lus", (unsigned long)minutes, (unsigned long)seconds);
  } else {
    snprintf(dest, dest_len, "%lus", (unsigned long)seconds);
  }
}

void MyMesh::formatRadioParamTuple(char* dest, size_t dest_len, const ScheduledRadioSetting& setting) const {
  char freq[16];
  char bw[16];
  formatFixed3(freq, sizeof(freq), setting.freq);
  StrHelper::strncpy(bw, StrHelper::ftoa3(setting.bw), sizeof(bw));
  snprintf(dest, dest_len, "%s,%s,%u,%u", freq, bw, (uint32_t)setting.sf, (uint32_t)setting.cr);
}

void MyMesh::formatScheduledRadioSetting(char* reply, int setting_idx, int display_idx) const {
  const ScheduledRadioSetting& setting = scheduled_radio_settings[setting_idx];
  char params[40];
  char delay[16];
  formatRadioParamTuple(params, sizeof(params), setting);

  if (setting.temporary) {
    if (setting.started) {
      formatScheduledRadioDuration(delay, sizeof(delay), setting.end_time);
      snprintf(reply, 160, "> %d:%s@%lu-%lu active ends in %s",
               display_idx,
               params,
               (unsigned long)setting.start_time,
               (unsigned long)setting.end_time,
               delay);
    } else {
      formatScheduledRadioDuration(delay, sizeof(delay), setting.start_time);
      snprintf(reply, 160, "> %d:%s@%lu-%lu starts in %s",
               display_idx,
               params,
               (unsigned long)setting.start_time,
               (unsigned long)setting.end_time,
               delay);
    }
  } else {
    formatScheduledRadioDuration(delay, sizeof(delay), setting.start_time);
    snprintf(reply, 160, "> %d:%s@%lu in %s",
             display_idx,
             params,
             (unsigned long)setting.start_time,
             delay);
  }
}

void MyMesh::addScheduledRadioParams(bool temporary, float freq, float bw, uint8_t sf, uint8_t cr,
                                     uint32_t start_time, uint32_t end_time, char* reply) {
  uint32_t now = getRTCClock()->getCurrentTime();
  if (!isValidScheduledRadioParams(freq, bw, sf, cr)) {
    strcpy(reply, "Error, invalid radio params");
    return;
  }
  if (start_time <= now) {
    strcpy(reply, "Error: start is in the past");
    return;
  }
  if (temporary && end_time <= now) {
    strcpy(reply, "Error: end is in the past");
    return;
  }
  if (temporary && end_time <= start_time) {
    strcpy(reply, "Error: end must be after start");
    return;
  }
  if (countScheduledRadioSettings(temporary) >= MAX_SCHEDULED_RADIO_SETTINGS_PER_TYPE) {
    snprintf(reply, 160, "Error: max %d queued", MAX_SCHEDULED_RADIO_SETTINGS_PER_TYPE);
    return;
  }
  if (scheduledRadioConflicts(temporary, start_time, end_time)) {
    strcpy(reply, "Error: schedule conflict");
    return;
  }

  int slot = findFreeScheduledRadioSlot();
  if (slot < 0) {
    strcpy(reply, "Error: queue full");
    return;
  }

  scheduled_radio_settings[slot].active = true;
  scheduled_radio_settings[slot].temporary = temporary;
  scheduled_radio_settings[slot].started = false;
  scheduled_radio_settings[slot].freq = freq;
  scheduled_radio_settings[slot].bw = bw;
  scheduled_radio_settings[slot].sf = sf;
  scheduled_radio_settings[slot].cr = cr;
  scheduled_radio_settings[slot].start_time = start_time;
  scheduled_radio_settings[slot].end_time = temporary ? end_time : 0;

  char delay[16];
  formatScheduledRadioDuration(delay, sizeof(delay), start_time);
  snprintf(reply, 160, "OK - %s %d in %s",
           temporary ? "tempradioat" : "radioat",
           getScheduledRadioSettingIndex(temporary, slot),
           delay);
}

void MyMesh::formatScheduledRadioParams(bool temporary, const char* selector, char* reply) {
  if (selectorIsEmpty(selector) || selectorIsAll(selector)) {
    int count = countScheduledRadioSettings(temporary);
    if (count == 0) {
      strcpy(reply, "> -none-");
      return;
    }

    int len = snprintf(reply, 160, "> ");
    for (int display_idx = 1; display_idx <= count && len < 159; display_idx++) {
      int idx = findScheduledRadioSettingByIndex(temporary, display_idx);
      if (idx < 0) {
        break;
      }
      char params[40];
      formatRadioParamTuple(params, sizeof(params), scheduled_radio_settings[idx]);
      int written;
      if (temporary) {
        written = snprintf(&reply[len], 160 - len, "%s%d:%s@%lu-%lu",
                           display_idx == 1 ? "" : " ",
                           display_idx,
                           params,
                           (unsigned long)scheduled_radio_settings[idx].start_time,
                           (unsigned long)scheduled_radio_settings[idx].end_time);
      } else {
        written = snprintf(&reply[len], 160 - len, "%s%d:%s@%lu",
                           display_idx == 1 ? "" : " ",
                           display_idx,
                           params,
                           (unsigned long)scheduled_radio_settings[idx].start_time);
      }
      if (written < 0 || written >= 160 - len) {
        reply[159] = 0;
        break;
      }
      len += written;
    }
    return;
  }

  int wanted = 0;
  if (!parsePositiveSelector(selector, wanted)) {
    strcpy(reply, temporary ? "Error, use: get tempradioat [n]" : "Error, use: get radioat [n]");
    return;
  }

  int idx = findScheduledRadioSettingByIndex(temporary, wanted);
  if (idx < 0) {
    strcpy(reply, "Error: not found");
    return;
  }
  formatScheduledRadioSetting(reply, idx, wanted);
}

void MyMesh::deleteScheduledRadioParams(bool temporary, const char* selector, char* reply) {
  if (selectorIsEmpty(selector) || selectorIsAll(selector)) {
    int deleted = 0;
    bool restore_radio = false;
    for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
      ScheduledRadioSetting& setting = scheduled_radio_settings[i];
      if (setting.active && setting.temporary == temporary) {
        restore_radio = restore_radio || (setting.temporary && setting.started);
        setting.active = false;
        setting.started = false;
        deleted++;
      }
    }
    if (restore_radio && !hasStartedScheduledTempRadio()) {
      applySavedRadioParams();
    }
    snprintf(reply, 160, "OK - deleted %d", deleted);
    return;
  }

  int wanted = 0;
  if (!parsePositiveSelector(selector, wanted)) {
    strcpy(reply, temporary ? "Error, use: del tempradioat [n]" : "Error, use: del radioat [n]");
    return;
  }

  int idx = findScheduledRadioSettingByIndex(temporary, wanted);
  if (idx < 0) {
    strcpy(reply, "Error: not found");
    return;
  }
  clearScheduledRadioSetting(idx, true);
  strcpy(reply, "OK");
}

void MyMesh::processScheduledRadioSettings() {
  uint32_t now = getRTCClock()->getCurrentTime();
  bool saved_params_changed = false;
  bool temp_ended = false;

  while (true) {
    int due_idx = -1;
    for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
      const ScheduledRadioSetting& setting = scheduled_radio_settings[i];
      if (!setting.active || setting.temporary || now < setting.start_time) {
        continue;
      }
      if (due_idx < 0 || setting.start_time < scheduled_radio_settings[due_idx].start_time
          || (setting.start_time == scheduled_radio_settings[due_idx].start_time && i < due_idx)) {
        due_idx = i;
      }
    }
    if (due_idx < 0) {
      break;
    }

    ScheduledRadioSetting& setting = scheduled_radio_settings[due_idx];
    _prefs.freq = setting.freq;
    _prefs.bw = setting.bw;
    _prefs.sf = setting.sf;
    _prefs.cr = setting.cr;
    savePrefs();
    setting.active = false;
    setting.started = false;
    saved_params_changed = true;
  }

  if (saved_params_changed && !hasStartedScheduledTempRadio()) {
    applySavedRadioParams();
  }

  for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
    ScheduledRadioSetting& setting = scheduled_radio_settings[i];
    if (setting.active && setting.temporary && setting.started && now >= setting.end_time) {
      setting.active = false;
      setting.started = false;
      temp_ended = true;
    }
  }

  if (temp_ended && !hasStartedScheduledTempRadio()) {
    applySavedRadioParams();
  }

  for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
    ScheduledRadioSetting& setting = scheduled_radio_settings[i];
    if (setting.active && setting.temporary && !setting.started && now >= setting.start_time) {
      if (now >= setting.end_time) {
        setting.active = false;
      } else {
        applyRadioParams(setting.freq, setting.bw, setting.sf, setting.cr);
        setting.started = true;
      }
    }
  }
}

bool MyMesh::isMillisTimerDue(unsigned long timestamp) const {
  return timestamp && millisHasNowPassed(timestamp);
}

bool MyMesh::hasScheduledRadioWorkDue() const {
  uint32_t now = getRTCClock()->getCurrentTime();
  for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
    const ScheduledRadioSetting& setting = scheduled_radio_settings[i];
    if (!setting.active) {
      continue;
    }
    if (!setting.temporary && now >= setting.start_time) {
      return true;
    }
    if (setting.temporary) {
      if (!setting.started && now >= setting.start_time) {
        return true;
      }
      if (setting.started && now >= setting.end_time) {
        return true;
      }
    }
  }
  return false;
}

uint32_t MyMesh::limitSleepToMillisTimer(unsigned long timestamp, uint32_t sleep_secs) const {
  if (!timestamp || sleep_secs == 0) {
    return sleep_secs;
  }
  unsigned long now = millis();
  if ((long)(now - timestamp) >= 0) {
    return 0;
  }
  unsigned long remaining_ms = timestamp - now;
  uint32_t remaining_secs = (remaining_ms + 999UL) / 1000UL;
  return remaining_secs < sleep_secs ? remaining_secs : sleep_secs;
}

uint32_t MyMesh::limitSleepToRtcTime(uint32_t timestamp, uint32_t sleep_secs) const {
  if (!timestamp || sleep_secs == 0) {
    return sleep_secs;
  }
  uint32_t now = getRTCClock()->getCurrentTime();
  if (now >= timestamp) {
    return 0;
  }
  uint32_t remaining_secs = timestamp - now;
  return remaining_secs < sleep_secs ? remaining_secs : sleep_secs;
}

uint32_t MyMesh::limitSleepToScheduledRadioWork(uint32_t sleep_secs) const {
  for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
    const ScheduledRadioSetting& setting = scheduled_radio_settings[i];
    if (!setting.active) {
      continue;
    }
    if (!setting.temporary || !setting.started) {
      sleep_secs = limitSleepToRtcTime(setting.start_time, sleep_secs);
    }
    if (setting.temporary && setting.started) {
      sleep_secs = limitSleepToRtcTime(setting.end_time, sleep_secs);
    }
  }
  return sleep_secs;
}

uint32_t MyMesh::getPowerSaveSleepSeconds(uint32_t max_secs) const {
  if (max_secs == 0 || hasPendingWork()) {
    return 0;
  }

  uint32_t sleep_secs = max_secs;
  sleep_secs = limitSleepToMillisTimer(next_flood_advert, sleep_secs);
  sleep_secs = limitSleepToMillisTimer(next_local_advert, sleep_secs);
  sleep_secs = limitSleepToMillisTimer(dirty_contacts_expiry, sleep_secs);
  if (_prefs.battery_alert_enabled) {
    sleep_secs = limitSleepToMillisTimer(next_battery_alert_check, sleep_secs);
  }
  sleep_secs = limitSleepToScheduledRadioWork(sleep_secs);
  return sleep_secs;
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  for (int i = 0; i < MAX_SCHEDULED_RADIO_SETTINGS; i++) {
    if (scheduled_radio_settings[i].active && scheduled_radio_settings[i].temporary) {
      scheduled_radio_settings[i].active = false;
      scheduled_radio_settings[i].started = false;
    }
  }

  int slot = findFreeScheduledRadioSlot();
  if (slot < 0) {
    return;
  }

  uint32_t start_time = getRTCClock()->getCurrentTime() + 2; // give CLI reply time to be sent first
  scheduled_radio_settings[slot].active = true;
  scheduled_radio_settings[slot].temporary = true;
  scheduled_radio_settings[slot].started = false;
  scheduled_radio_settings[slot].freq = freq;
  scheduled_radio_settings[slot].bw = bw;
  scheduled_radio_settings[slot].sf = sf;
  scheduled_radio_settings[slot].cr = cr;
  scheduled_radio_settings[slot].start_time = start_time;
  scheduled_radio_settings[slot].end_time = start_time + ((uint32_t)timeout_mins * 60);
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_driver.setTxPower(power_dbm);
}

bool MyMesh::setRxBoostedGain(bool enable) {
  return radio_driver.setRxBoostedGainMode(enable);
}

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
    }
  }
#endif
}

void MyMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}

bool MyMesh::saveRegions() {
  return region_map.save(_fs);
}

void MyMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) {
    region_map.getTransportKeysFor(*r, &default_scope, 1);
  } else {
    memset(default_scope.key, 0, sizeof(default_scope.key));
  }
}

void MyMesh::clearFloodChannelBlockEntry(FloodChannelBlockEntry& entry) {
  memset(&entry, 0, sizeof(entry));
}

void MyMesh::deriveFloodChannelBlockPrefix(const uint8_t* secret, uint8_t key_len,
                                           uint8_t prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN]) const {
  mesh::Utils::sha256(prefix, FLOOD_CHANNEL_BLOCK_PREFIX_LEN, secret, key_len);
}

void MyMesh::seedDefaultFloodChannelBlocks() {
  auto& entry = flood_channel_blocks[0];
  clearFloodChannelBlockEntry(entry);
  entry.active = true;
  entry.key_len = CIPHER_KEY_SIZE;
  entry.max_hops = DEFAULT_FLOOD_CHANNEL_BLOCK_HOPS;
  mesh::Utils::sha256(entry.secret, CIPHER_KEY_SIZE,
                      (const uint8_t*)DEFAULT_FLOOD_CHANNEL_BLOCK_NAME,
                      strlen(DEFAULT_FLOOD_CHANNEL_BLOCK_NAME));
  memset(&entry.secret[CIPHER_KEY_SIZE], 0, PUB_KEY_SIZE - CIPHER_KEY_SIZE);
  deriveFloodChannelBlockPrefix(entry.secret, entry.key_len, entry.hash_prefix);
  StrHelper::strncpy(entry.name, DEFAULT_FLOOD_CHANNEL_BLOCK_NAME, sizeof(entry.name));
}

void MyMesh::loadFloodChannelBlocks() {
  memset(flood_channel_blocks, 0, sizeof(flood_channel_blocks));
  if (_fs == NULL) {
    return;
  }
  if (!_fs->exists(FLOOD_CHANNEL_BLOCK_FILE)) {
    seedDefaultFloodChannelBlocks();
    saveFloodChannelBlocks();
    return;
  }

  File file = openFloodChannelBlockRead(_fs, FLOOD_CHANNEL_BLOCK_FILE);
  if (!file) {
    return;
  }

  uint8_t magic[4];
  uint8_t count = 0;
  bool success = file.read(magic, sizeof(magic)) == sizeof(magic)
      && memcmp(magic, "FCB2", sizeof(magic)) == 0
      && file.read(&count, sizeof(count)) == sizeof(count);

  for (int i = 0; success && i < count && i < FLOOD_CHANNEL_BLOCK_SLOTS; i++) {
    uint8_t active = 0;
    uint8_t key_len = 0;
    uint8_t max_hops = FLOOD_CHANNEL_BLOCK_HOPS_INHERIT;
    uint8_t hash_prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN];
    uint8_t secret[PUB_KEY_SIZE];
    char name[FLOOD_CHANNEL_BLOCK_NAME_LEN];

    success = file.read(&active, sizeof(active)) == sizeof(active);
    success = success && file.read(&key_len, sizeof(key_len)) == sizeof(key_len);
    success = success && file.read(&max_hops, sizeof(max_hops)) == sizeof(max_hops);
    success = success && file.read(hash_prefix, sizeof(hash_prefix)) == sizeof(hash_prefix);
    success = success && file.read(secret, sizeof(secret)) == sizeof(secret);
    success = success && file.read((uint8_t*)name, sizeof(name)) == sizeof(name);
    if (!success) {
      break;
    }

    name[sizeof(name) - 1] = 0;
    if (active && (key_len == CIPHER_KEY_SIZE || key_len == PUB_KEY_SIZE) && name[0] != 0) {
      auto& entry = flood_channel_blocks[i];
      entry.active = true;
      entry.key_len = key_len;
      entry.max_hops = (max_hops == FLOOD_CHANNEL_BLOCK_HOPS_ALL
          || max_hops == FLOOD_CHANNEL_BLOCK_HOPS_INHERIT
          || (max_hops >= 1 && max_hops <= 7)) ? max_hops : FLOOD_CHANNEL_BLOCK_HOPS_INHERIT;
      memcpy(entry.secret, secret, sizeof(entry.secret));
      if (entry.key_len == CIPHER_KEY_SIZE) {
        memset(&entry.secret[CIPHER_KEY_SIZE], 0, PUB_KEY_SIZE - CIPHER_KEY_SIZE);
      }
      deriveFloodChannelBlockPrefix(entry.secret, entry.key_len, entry.hash_prefix);
      StrHelper::strncpy(entry.name, name, sizeof(entry.name));
    }
  }

  file.close();
}

bool MyMesh::saveFloodChannelBlocks() {
  if (_fs == NULL) {
    return false;
  }

  File file = openFloodChannelBlockWrite(_fs, FLOOD_CHANNEL_BLOCK_FILE);
  if (!file) {
    return false;
  }

  const uint8_t magic[4] = {'F', 'C', 'B', '2'};
  uint8_t count = FLOOD_CHANNEL_BLOCK_SLOTS;
  bool success = file.write(magic, sizeof(magic)) == sizeof(magic);
  success = success && file.write(&count, sizeof(count)) == sizeof(count);

  for (int i = 0; success && i < FLOOD_CHANNEL_BLOCK_SLOTS; i++) {
    const auto& entry = flood_channel_blocks[i];
    uint8_t active = entry.active ? 1 : 0;
    uint8_t max_hops = entry.active ? entry.max_hops : FLOOD_CHANNEL_BLOCK_HOPS_INHERIT;
    success = file.write(&active, sizeof(active)) == sizeof(active);
    success = success && file.write(&entry.key_len, sizeof(entry.key_len)) == sizeof(entry.key_len);
    success = success && file.write(&max_hops, sizeof(max_hops)) == sizeof(max_hops);
    success = success && file.write(entry.hash_prefix, sizeof(entry.hash_prefix)) == sizeof(entry.hash_prefix);
    success = success && file.write(entry.secret, sizeof(entry.secret)) == sizeof(entry.secret);
    success = success && file.write((const uint8_t*)entry.name, sizeof(entry.name)) == sizeof(entry.name);
  }

  file.close();
  return success;
}

static void trimFloodChannelBlockSelector(const char* selector, char* dest, size_t dest_len) {
  selector = skipLocalSpaces(selector);
  StrHelper::strncpy(dest, selector == NULL ? "" : selector, dest_len);
  size_t len = strlen(dest);
  while (len > 0 && dest[len - 1] == ' ') {
    dest[--len] = 0;
  }
}

static bool parseFloodChannelBlockPrefixSelector(const char* selector,
                                                 uint8_t prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN]) {
  char text[16];
  trimFloodChannelBlockSelector(selector, text, sizeof(text));
  if (strlen(text) != FLOOD_CHANNEL_BLOCK_PREFIX_LEN * 2) {
    return false;
  }
  for (int i = 0; i < FLOOD_CHANNEL_BLOCK_PREFIX_LEN * 2; i++) {
    if (!mesh::Utils::isHexChar(text[i])) {
      return false;
    }
  }
  return mesh::Utils::fromHex(prefix, FLOOD_CHANNEL_BLOCK_PREFIX_LEN, text);
}

static void formatFloodChannelBlockHops(char* dest, uint8_t max_hops) {
  if (max_hops == FLOOD_CHANNEL_BLOCK_HOPS_ALL) {
    strcpy(dest, "h=all");
  } else if (max_hops == FLOOD_CHANNEL_BLOCK_HOPS_INHERIT) {
    strcpy(dest, "h=def");
  } else {
    sprintf(dest, "h>%u", (unsigned int)max_hops);
  }
}

int MyMesh::findFloodChannelBlockBySelector(const char* selector) const {
  uint8_t prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN];
  if (parseFloodChannelBlockPrefixSelector(selector, prefix)) {
    for (int i = 0; i < FLOOD_CHANNEL_BLOCK_SLOTS; i++) {
      const auto& entry = flood_channel_blocks[i];
      if (entry.active && memcmp(entry.hash_prefix, prefix, sizeof(entry.hash_prefix)) == 0) {
        return i;
      }
    }
    return -1;
  }

  int index = 0;
  if (parsePositiveSelector(selector, index)) {
    return (index >= 1 && index <= FLOOD_CHANNEL_BLOCK_SLOTS) ? index - 1 : -1;
  }

  char name[FLOOD_CHANNEL_BLOCK_NAME_LEN];
  trimFloodChannelBlockSelector(selector, name, sizeof(name));
  if (name[0] == 0) {
    return -1;
  }
  for (int i = 0; i < FLOOD_CHANNEL_BLOCK_SLOTS; i++) {
    const auto& entry = flood_channel_blocks[i];
    if (entry.active && strcmp(entry.name, name) == 0) {
      return i;
    }
  }
  return -1;
}

int MyMesh::findFloodChannelBlockSlot(const uint8_t prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN], const char* name) const {
  int free_slot = -1;
  for (int i = 0; i < FLOOD_CHANNEL_BLOCK_SLOTS; i++) {
    const auto& entry = flood_channel_blocks[i];
    if (entry.active) {
      if (memcmp(entry.hash_prefix, prefix, FLOOD_CHANNEL_BLOCK_PREFIX_LEN) == 0 || strcmp(entry.name, name) == 0) {
        return i;
      }
    } else if (free_slot < 0) {
      free_slot = i;
    }
  }
  return free_slot;
}

void MyMesh::formatFloodChannelBlockDetail(char* reply, int idx) const {
  if (idx < 0 || idx >= FLOOD_CHANNEL_BLOCK_SLOTS) {
    strcpy(reply, "Err - not found");
    return;
  }

  const auto& entry = flood_channel_blocks[idx];
  if (!entry.active) {
    snprintf(reply, 150, "> %d empty", idx + 1);
    return;
  }

  char prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN * 2 + 1];
  char hops[8];
  mesh::Utils::toHex(prefix, entry.hash_prefix, FLOOD_CHANNEL_BLOCK_PREFIX_LEN);
  formatFloodChannelBlockHops(hops, entry.max_hops);
  snprintf(reply, 150, "> %d %s %u %s %s", idx + 1, prefix, (unsigned int)entry.key_len * 8, hops, entry.name);
}

void MyMesh::setFloodChannelBlock(int index, const uint8_t* secret, uint8_t key_len,
                                  const char* name, uint8_t max_hops, char* reply) {
  if ((key_len != CIPHER_KEY_SIZE && key_len != PUB_KEY_SIZE) || secret == NULL || name == NULL || name[0] == 0) {
    strcpy(reply, "Err - bad params");
    return;
  }
  if (index < 0 || index > FLOOD_CHANNEL_BLOCK_SLOTS) {
    snprintf(reply, 160, "Err - index 1-%d", FLOOD_CHANNEL_BLOCK_SLOTS);
    return;
  }
  if (max_hops != FLOOD_CHANNEL_BLOCK_HOPS_ALL
      && max_hops != FLOOD_CHANNEL_BLOCK_HOPS_INHERIT
      && (max_hops < 1 || max_hops > 7)) {
    strcpy(reply, "Err - bad hops");
    return;
  }

  uint8_t prefix[FLOOD_CHANNEL_BLOCK_PREFIX_LEN];
  deriveFloodChannelBlockPrefix(secret, key_len, prefix);
  int slot = index > 0 ? index - 1 : findFloodChannelBlockSlot(prefix, name);
  if (slot < 0 || slot >= FLOOD_CHANNEL_BLOCK_SLOTS) {
    strcpy(reply, "Err - block list full");
    return;
  }

  auto& entry = flood_channel_blocks[slot];
  clearFloodChannelBlockEntry(entry);
  entry.active = true;
  entry.key_len = key_len;
  entry.max_hops = max_hops;
  memcpy(entry.secret, secret, PUB_KEY_SIZE);
  if (entry.key_len == CIPHER_KEY_SIZE) {
    memset(&entry.secret[CIPHER_KEY_SIZE], 0, PUB_KEY_SIZE - CIPHER_KEY_SIZE);
  }
  deriveFloodChannelBlockPrefix(entry.secret, entry.key_len, entry.hash_prefix);
  StrHelper::strncpy(entry.name, name, sizeof(entry.name));

  if (!saveFloodChannelBlocks()) {
    strcpy(reply, "Err - save failed");
    return;
  }
  formatFloodChannelBlockDetail(reply, slot);
}

void MyMesh::formatFloodChannelBlocks(const char* selector, char* reply) {
  if (!selectorIsEmpty(selector)) {
    int idx = findFloodChannelBlockBySelector(selector);
    if (idx < 0) {
      strcpy(reply, "Err - not found");
    } else {
      formatFloodChannelBlockDetail(reply, idx);
    }
    return;
  }

  char* out = reply;
  const size_t reply_limit = 150;
  size_t remaining = reply_limit;
  char hops[8];
  formatFloodChannelBlockHops(hops, _prefs.flood_channel_block_max_hops);
  size_t full_len = 2 + strlen(hops);
  for (int i = 0; i < FLOOD_CHANNEL_BLOCK_SLOTS; i++) {
    const auto& entry = flood_channel_blocks[i];
    size_t display_len = entry.active ? strlen(entry.name) : 1;
    full_len += 1 + (i + 1 >= 10 ? 2 : 1) + 1 + display_len;
    if (entry.active && entry.max_hops != FLOOD_CHANNEL_BLOCK_HOPS_INHERIT) {
      char row_hops[8];
      formatFloodChannelBlockHops(row_hops, entry.max_hops);
      full_len += 1 + strlen(row_hops);
    }
  }
  bool trim_names = full_len >= reply_limit;

  int written = snprintf(out, remaining, "> %s", hops);
  if (written < 0 || (size_t)written >= remaining) {
    reply[0] = 0;
    return;
  }
  out += written;
  remaining -= written;

  for (int i = 0; i < FLOOD_CHANNEL_BLOCK_SLOTS && remaining > 1; i++) {
    const char* display = "-";
    char short_display[6];
    const auto& entry = flood_channel_blocks[i];
    if (entry.active) {
      char row_hops[8];
      row_hops[0] = 0;
      if (entry.max_hops != FLOOD_CHANNEL_BLOCK_HOPS_INHERIT) {
        formatFloodChannelBlockHops(row_hops, entry.max_hops);
      }
      if (trim_names) {
        StrHelper::strncpy(short_display, entry.name, sizeof(short_display));
        if (strlen(entry.name) >= sizeof(short_display)) {
          short_display[sizeof(short_display) - 2] = '~';
          short_display[sizeof(short_display) - 1] = 0;
        }
        display = short_display;
      } else {
        display = entry.name;
      }
      written = snprintf(out, remaining, " %d:%s%s%s", i + 1, display,
                         row_hops[0] ? "/" : "", row_hops);
    } else {
      written = snprintf(out, remaining, " %d:%s", i + 1, display);
    }
    if (written < 0 || (size_t)written >= remaining) {
      out[remaining - 1] = 0;
      break;
    }
    out += written;
    remaining -= written;
  }
}

void MyMesh::deleteFloodChannelBlock(const char* selector, char* reply) {
  int idx = findFloodChannelBlockBySelector(selector);
  if (idx < 0 || idx >= FLOOD_CHANNEL_BLOCK_SLOTS || !flood_channel_blocks[idx].active) {
    strcpy(reply, "Err - not found");
    return;
  }

  clearFloodChannelBlockEntry(flood_channel_blocks[idx]);
  if (!saveFloodChannelBlocks()) {
    strcpy(reply, "Err - save failed");
    return;
  }
  strcpy(reply, "OK");
}

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(), 
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

static char* trimSpaces(char* s) {
  while (*s == ' ') s++;
  char* end = s + strlen(s);
  while (end > s && end[-1] == ' ') end--;
  *end = 0;
  return s;
}

static bool parsePathCommand(char* raw, uint8_t* out_path, uint8_t& out_path_len, const char*& err) {
  if (raw == NULL || out_path == NULL) {
    err = "Err - bad params";
    return false;
  }

  char* spec = trimSpaces(raw);
  if (*spec == 0) {
    err = "Err - missing path";
    return false;
  }
  if (strcmp(spec, "clear") == 0 || strcmp(spec, "-") == 0 || strcmp(spec, "none") == 0) {
    out_path_len = OUT_PATH_UNKNOWN;
    return true;
  }
  if (strcmp(spec, "flood") == 0) {
    out_path_len = OUT_PATH_FORCE_FLOOD;
    return true;
  }
  if (strcmp(spec, "direct") == 0) {
    out_path_len = 0;
    return true;
  }

  uint8_t hash_size = 0;
  uint8_t hop_count = 0;
  char* token = spec;
  while (token && *token) {
    char* comma = strchr(token, ',');
    if (comma) *comma = 0;
    token = trimSpaces(token);

    int hex_len = strlen(token);
    if (!(hex_len == 2 || hex_len == 4 || hex_len == 6)) {
      err = "Err - bad params";
      return false;
    }

    uint8_t hop_hash_size = (uint8_t)(hex_len / 2);
    if (hash_size == 0) {
      hash_size = hop_hash_size;
    } else if (hash_size != hop_hash_size) {
      err = "Err - bad params";
      return false;
    }

    if (hop_count >= 63 || (hop_count + 1) * hash_size > MAX_PATH_SIZE) {
      err = "Err - bad params";
      return false;
    }
    if (!mesh::Utils::fromHex(&out_path[hop_count * hash_size], hash_size, token)) {
      err = "Err - bad hex";
      return false;
    }

    hop_count++;
    token = comma ? comma + 1 : NULL;
  }

  if (hash_size == 0 || hop_count == 0) {
    err = "Err - missing path";
    return false;
  }
  out_path_len = ((hash_size - 1) << 6) | (hop_count & 63);
  return true;
}

static void formatPathReply(const uint8_t* path, uint8_t path_len, char* out, size_t out_len) {
  if (path_len == OUT_PATH_FORCE_FLOOD) {
    snprintf(out, out_len, "> flood");
    return;
  }
  if (path_len == OUT_PATH_UNKNOWN) {
    snprintf(out, out_len, "> unknown");
    return;
  }
  if (!mesh::Packet::isValidPathLen(path_len)) {
    snprintf(out, out_len, "> invalid");
    return;
  }
  if ((path_len & 63) == 0) {
    snprintf(out, out_len, "> direct");
    return;
  }

  uint8_t hash_size = (path_len >> 6) + 1;
  uint8_t hop_count = path_len & 63;
  uint8_t byte_len = hop_count * hash_size;
  char hex[(MAX_PATH_SIZE * 2) + 1];
  mesh::Utils::toHex(hex, path, byte_len);
  snprintf(out, out_len, "> hs=%u hops=%u hex=%s", (uint32_t)hash_size, (uint32_t)hop_count, hex);
}

// Whitelist helper for region manager command perms
static bool isRegionMgrAllowed(const char* cmd) {
  while (*cmd == ' ') cmd++; // skip leading spaces
  // region commands (read + write region map)
  if (memcmp(cmd, "region", 6) == 0) return true;
  // read-only getters / status
  if (memcmp(cmd, "get ", 4) == 0) return true;
  if (memcmp(cmd, "ver", 3) == 0) return true;
  if (memcmp(cmd, "board", 5) == 0) return true;
  // "neighbors" (plural) is read-only; reject "neighbor.remove" by checking next char
  if (memcmp(cmd, "neighbors", 9) == 0) return true;
  // bare "clock" is read-only; "clock sync" must be denied
  if (memcmp(cmd, "clock", 5) == 0 && memcmp(cmd, "clock sync", 10) != 0) return true;
  // sensor reads only
  if (memcmp(cmd, "sensor get ", 11) == 0) return true;
  if (memcmp(cmd, "sensor list", 11) == 0) return true;
  return false;
}

void MyMesh::handleCommand(uint32_t sender_timestamp, ClientInfo* sender, char *command, char *reply) {
  char* reply_start = reply;
  int recent_page = 1;
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // Region managers are limited to read-only queries and region commands
  // Admins are unrestricted
  if (sender && !sender->isAdmin() && sender->isRegionMgr()) {
    if (!isRegionMgrAllowed(command)) {
      strcpy(reply, "Err - not permitted");
      return;
    }
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && sender == NULL && parseRecentRepeatersPageCommand(command, recent_page)) {
    formatRecentRepeatersReply(reply, recent_page);
  } else if (sender_timestamp == 0 && sender == NULL
      && (strcmp(command, "get recent.repeater") == 0 || strcmp(command, "get recent.repeaters") == 0)) {
    printRecentRepeatersSerial();
    reply_start[0] = 0;
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (strcmp(command, "get outpath") == 0
          || strcmp(command, "set outpath") == 0
          || strncmp(command, "set outpath ", 12) == 0
          || strcmp(command, "get altpath") == 0
          || strcmp(command, "set altpath") == 0
          || strncmp(command, "set altpath ", 12) == 0) {
    bool is_get = strncmp(command, "get ", 4) == 0;
    bool is_alt = strncmp(command + 4, "altpath", 7) == 0;
    if (sender == NULL) {
      strcpy(reply, "Err - command needs remote client context");
    } else {
      uint8_t* stored_path = is_alt ? sender->alt_path : sender->out_path;
      uint8_t* stored_path_len = is_alt ? &sender->alt_path_len : &sender->out_path_len;
      if (is_get) {
        formatPathReply(stored_path, *stored_path_len, reply, 160);
        return;
      }

      char* spec = command + 11;  // length of "set outpath" or "set altpath"
      if (*spec == ' ') spec++;

      uint8_t path[MAX_PATH_SIZE];
      uint8_t path_len = OUT_PATH_UNKNOWN;
      const char* err = NULL;
      if (!parsePathCommand(spec, path, path_len, err)) {
        strcpy(reply, err ? err : "Err - invalid path");
      } else {
        if (path_len == OUT_PATH_UNKNOWN || path_len == OUT_PATH_FORCE_FLOOD) {
          memset(stored_path, 0, MAX_PATH_SIZE);
          *stored_path_len = path_len;
        } else {
          *stored_path_len = mesh::Packet::copyPath(stored_path, path, path_len);
        }
        dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
        formatPathReply(stored_path, *stored_path_len, reply, 160);
      }
    }
  } else if (strncmp(command, "send text.flood ", 16) == 0) {
    char* text = trimSpaces(command + 16);
    if (*text == 0) {
      strcpy(reply, "Err - usage: send text.flood <message>");
    } else if (sendRepeatersFloodText(text)) {
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - unable to create packet");
    }
  } else if (strcmp(command, "get battery.alert") == 0) {
    sprintf(reply, "> %s", _prefs.battery_alert_enabled ? "on" : "off");
  } else if (strcmp(command, "get battery.alert.low") == 0) {
    sprintf(reply, "> %u", (uint32_t)_prefs.battery_alert_low_percent);
  } else if (strcmp(command, "get battery.alert.critical") == 0) {
    sprintf(reply, "> %u", (uint32_t)_prefs.battery_alert_critical_percent);
  } else if (strncmp(command, "set battery.alert ", 18) == 0) {
    const char* value = command + 18;
    if (strcmp(value, "on") == 0) {
      _prefs.battery_alert_enabled = 1;
      next_battery_alert_check = 0;
      savePrefs();
      strcpy(reply, "OK");
    } else if (strcmp(value, "off") == 0) {
      _prefs.battery_alert_enabled = 0;
      battery_alert_sent = false;
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - usage: set battery.alert <on|off>");
    }
  } else if (strncmp(command, "set battery.alert.low ", 22) == 0) {
    uint8_t percent;
    if (!parseBatteryAlertPercent(command + 22, 1, 100, percent)) {
      strcpy(reply, "Err - usage: set battery.alert.low <1-100>");
    } else if (percent <= _prefs.battery_alert_critical_percent) {
      strcpy(reply, "Err - low must be greater than critical");
    } else {
      _prefs.battery_alert_low_percent = percent;
      next_battery_alert_check = 0;
      savePrefs();
      strcpy(reply, "OK");
    }
  } else if (strncmp(command, "set battery.alert.critical ", 27) == 0) {
    uint8_t percent;
    if (!parseBatteryAlertPercent(command + 27, 0, 99, percent)) {
      strcpy(reply, "Err - usage: set battery.alert.critical <0-99>");
    } else if (percent >= _prefs.battery_alert_low_percent) {
      strcpy(reply, "Err - critical must be less than low");
    } else {
      _prefs.battery_alert_critical_percent = percent;
      next_battery_alert_check = 0;
      savePrefs();
      strcpy(reply, "OK");
    }
  } else if (memcmp(command, "discover.neighbors", 18) == 0) {
    const char* sub = command + 18;
    while (*sub == ' ') sub++;
    if (*sub != 0) {
      strcpy(reply, "Err - discover.neighbors has no options");
    } else {
      sendNodeDiscoverReq();
      strcpy(reply, "OK - Discover sent");
    }
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  mesh::Mesh::loop();
  checkBatteryAlert();

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    uint32_t delay_millis = 0;
    if (pkt) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  processScheduledRadioSettings();

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
  if (_mgr->getOutboundTotal() > 0) return true;
  if (isMillisTimerDue(next_flood_advert) || isMillisTimerDue(next_local_advert)) return true;
  if (isMillisTimerDue(dirty_contacts_expiry)) return true;
  if (_prefs.battery_alert_enabled && isMillisTimerDue(next_battery_alert_check)) return true;
  return hasScheduledRadioWorkDue();
}
