"""
Fix new _observer_mqtt envs to match the V3/V4 reference pattern.
Run from the repo root or anywhere; uses absolute paths.
"""
import re, os

VARIANTS_DIR = r"C:\MeshCore-MQTT\variants"

VARIANTS = [
    "ebyte_eora_s3",
    "generic-e22",
    "heltec_ct62",
    "heltec_tracker",
    "heltec_tracker_v2",
    "heltec_v2",
    "heltec_wireless_paper",
    "lilygo_t3s3_sx1276",
    "lilygo_tbeam_1w",
    "lilygo_tdeck",
    "lilygo_tlora_c6",
    "lilygo_tlora_v2_1",
    "m5stack_unit_c6l",
    "meshadventurer",
    "nibble_screen_connect",
    "tenstar_c3",
    "thinknode_m2",
    "thinknode_m5",
    "xiao_c3",
    "xiao_c6",
]

WIFI_CRED_BLOCK = (
    "#  -D WIFI_SSID='\"ssid\"'\n"
    "#  -D WIFI_PWD='\"password\"'\n"
    "#  -D MQTT_SERVER='\"your-mqtt-broker.com\"'\n"
    "#  -D MQTT_PORT=1883\n"
    "#  -D MQTT_USERNAME='\"your-username\"'\n"
    "#  -D MQTT_PASSWORD='\"your-password\"'"
)


def process_section(name: str, body: str) -> str:
    """Apply all fixes to a single env section body."""
    if not name.endswith("_observer_mqtt"):
        return body

    is_room = "room_server" in name

    # 1. Fix cert source
    body = body.replace(
        "board_ssl_cert_source = adafruit-full",
        "board_ssl_cert_source = adafruit",
    )

    if not is_room:
        # Repeater: fix debug block (commented MQTT_DEBUG + MESH lines)
        # Pattern A: MQTT_DEBUG + MESH lines already there
        body = re.sub(
            r";  -D MQTT_DEBUG=1\n;  -D MESH_PACKET_LOGGING=1\n;  -D MESH_DEBUG=1",
            (
                "  -D MQTT_DEBUG=1\n"
                "  -D MQTT_MEMORY_DEBUG=1\n"
                "; Keep default observer profile less verbose to reduce runtime contention.\n"
                ";  -D MESH_PACKET_LOGGING=1\n"
                ";  -D MESH_DEBUG=1"
            ),
            body,
        )

        # Insert MQTT_WIFI_TX_POWER + WITH_SNMP + WiFi block
        if "  -D ESP32_CPU_FREQ=160\n" in body:
            body = body.replace(
                "  -D ESP32_CPU_FREQ=160\n",
                (
                    "  -D ESP32_CPU_FREQ=160\n"
                    "  -D MQTT_WIFI_TX_POWER=WIFI_POWER_11dBm\n"
                    "  -D WITH_SNMP=1\n"
                    + WIFI_CRED_BLOCK + "\n"
                ),
                1,
            )
        elif "  -D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y\n" in body:
            body = body.replace(
                "  -D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y\n",
                (
                    "  -D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y\n"
                    "  -D MQTT_WIFI_TX_POWER=WIFI_POWER_11dBm\n"
                    "  -D WITH_SNMP=1\n"
                    + WIFI_CRED_BLOCK + "\n"
                ),
                1,
            )

        # Add SNMPAgent right after JWTHelper in build_src_filter
        body = body.replace(
            "  +<helpers/JWTHelper.cpp>\n",
            "  +<helpers/JWTHelper.cpp>\n  +<helpers/SNMPAgent.cpp>\n",
            1,
        )

        # Add SNMP_Agent lib before paulstoffregen/Time
        body = body.replace(
            "  paulstoffregen/Time@1.6.1",
            "  0neblock/SNMP_Agent\n  paulstoffregen/Time@1.6.1",
            1,
        )

    else:
        # Room server: fix MQTT_DEBUG and optionally add MESH debug lines

        # Case: no MESH debug lines — MQTT_DEBUG immediately before CONFIG_MBEDTLS
        body = re.sub(
            r";  -D MQTT_DEBUG=1\n  -D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y",
            (
                "  -D MQTT_DEBUG=1\n"
                ";  -D MESH_PACKET_LOGGING=1\n"
                ";  -D MESH_DEBUG=1\n"
                "  -D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y"
            ),
            body,
        )

        # Case: MESH debug lines already there but MQTT_DEBUG still commented
        body = re.sub(
            r";  -D MQTT_DEBUG=1\n;  -D MESH_PACKET_LOGGING=1\n;  -D MESH_DEBUG=1",
            (
                "  -D MQTT_DEBUG=1\n"
                ";  -D MESH_PACKET_LOGGING=1\n"
                ";  -D MESH_DEBUG=1"
            ),
            body,
        )

        # Insert MQTT_WIFI_TX_POWER
        if "  -D ESP32_CPU_FREQ=160\n" in body:
            body = body.replace(
                "  -D ESP32_CPU_FREQ=160\n",
                "  -D ESP32_CPU_FREQ=160\n  -D MQTT_WIFI_TX_POWER=WIFI_POWER_11dBm\n",
                1,
            )
        elif "  -D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y\n" in body:
            body = body.replace(
                "  -D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y\n",
                "  -D CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y\n  -D MQTT_WIFI_TX_POWER=WIFI_POWER_11dBm\n",
                1,
            )

    return body


def process_file(filepath: str):
    with open(filepath, "r", newline="") as f:
        original = f.read()

    # Split file into sections by [env:...] boundaries, preserving headers
    # Pattern: split just before [env: keeping the delimiter
    parts = re.split(r"(?=^\[env:)", original, flags=re.MULTILINE)

    result_parts = []
    for part in parts:
        m = re.match(r"^\[env:([^\]]+)\]", part)
        if m:
            env_name = m.group(1)
            new_part = process_section(env_name, part)
            result_parts.append(new_part)
        else:
            result_parts.append(part)

    new_content = "".join(result_parts)

    if new_content != original:
        with open(filepath, "w", newline="") as f:
            f.write(new_content)
        print(f"  UPDATED: {os.path.basename(os.path.dirname(filepath))}")
    else:
        print(f"  no change: {os.path.basename(os.path.dirname(filepath))}")


print("Fixing observer MQTT envs...")
for variant in VARIANTS:
    fp = os.path.join(VARIANTS_DIR, variant, "platformio.ini")
    if os.path.exists(fp):
        process_file(fp)
    else:
        print(f"  NOT FOUND: {fp}")

print("Done.")
