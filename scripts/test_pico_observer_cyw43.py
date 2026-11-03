"""Run the generated CYW43 association method against a stalled handshake."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?", default=Path(
        ".pio/build/PicoW_repeater_observer_mqtt_experimental/pico-patches/CYW43shim.cpp"))
    args = parser.parse_args()
    source = args.source.read_text()
    start = source.index("bool CYW43::begin(")
    end = source.index("\nvoid CYW43::end()", start)
    method = source[start:end]
    harness = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <cstdio>
struct netif { uint8_t hwaddr[6]{}; };
struct cyw43_t { uint32_t wifi_join_state = 0; } cyw43_state;
constexpr int CYW43_AUTH_OPEN = 0, CYW43_AUTH_WPA2_AES_PSK = 1;
constexpr int CYW43_IOCTL_SET_VAR = 2, CYW43_ITF_STA = 0, CYW43_CHANNEL_NONE = 0;
int calls = 0, result = 0, expected_auth = CYW43_AUTH_WPA2_AES_PSK;
void cyw43_arch_enable_sta_mode() {}
void cyw43_arch_enable_ap_mode(const char*, const char*, int) {}
void cyw43_wifi_get_mac(cyw43_t*, int, uint8_t*) {}
int cyw43_ioctl(cyw43_t*, int, size_t, uint8_t*, int) { return 0; }
void cyw43_wifi_update_multicast_filter(cyw43_t*, uint8_t*, bool) {}
int cyw43_wifi_join(cyw43_t* self, size_t size, const uint8_t* ssid,
                   size_t key_size, const uint8_t* key, int auth, const uint8_t*, int) {
    ++calls;
    assert(size == 4 && !memcmp(ssid, "test", 4));
    assert(auth == expected_auth);
    assert(key_size == (auth == CYW43_AUTH_OPEN ? 0 : 6));
    assert(auth == CYW43_AUTH_OPEN ? key == nullptr : !memcmp(key, "secret", 6));
    self->wifi_join_state = 0x201; // Handshake never completes in this double.
    return result;
}
int cyw43_wifi_link_status(cyw43_t*, int) { return 1; }
uint32_t millis() { return 0; }
void delay(unsigned) { throw std::runtime_error("association blocked the lwIP task"); }
class CYW43 {
public:
    netif* _netif = nullptr;
    cyw43_t* _self = nullptr;
    bool _ap = false;
    int _itf = 0, _timeout = 5000;
    const char* _ssid = "test";
    const char* _password = "secret";
    uint8_t _bssid[6]{};
    bool begin(const uint8_t*, netif*);
};
""" + method + r"""
int main() {
    CYW43 driver;
    netif interface;
    assert(driver.begin(nullptr, &interface));
    assert(calls == 1 && cyw43_state.wifi_join_state == 0x201);
    driver._password = nullptr; expected_auth = CYW43_AUTH_OPEN; calls = 0;
    assert(driver.begin(nullptr, &interface));
    assert(calls == 1);
    result = -1; calls = 0;
    assert(!driver.begin(nullptr, &interface));
    assert(calls == 1);
    puts("3 generated CYW43 association checks passed");
}
"""
    with tempfile.TemporaryDirectory(prefix="meshcore-cyw43-") as directory:
        path = Path(directory)
        (path / "check.cpp").write_text(harness)
        subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            str(path / "check.cpp"), "-o", str(path / "check")], check=True)
        subprocess.run([str(path / "check")], check=True, timeout=10)


if __name__ == "__main__":
    main()
