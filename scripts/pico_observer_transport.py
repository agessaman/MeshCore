"""Build environment-local patches without modifying shared packages."""
from pathlib import Path

Import("env")

def patch_source(build_env, node):
    source = Path(node.srcnode().get_abspath())
    text = source.read_text()
    if source.name == "WebSocketsClient.cpp":
        constructor = "_client.num          = 0;"
        if text.count(constructor) != 1:
            raise RuntimeError("Pinned WebSockets constructor changed")
        text = text.replace(constructor, "_client = WSclient_t{};\n    " + constructor)
        needle = "_client.ssl = new WEBSOCKETS_NETWORK_SSL_CLASS();"
        patch = (
            "\n#if defined(PICO_MQTT_OBSERVER) && defined(ARDUINO_ARCH_RP2040)\n"
            "            // MeshCore Pico TLS minimum\n"
            "            if (_client.ssl) _client.ssl->setSSLVersion(BR_TLS12, BR_TLS12);\n"
            "#endif\n"
        )
        text = text.replace(needle + patch, needle)
        if text.count(needle) != 1:
            raise RuntimeError("Pinned WebSockets TLS allocation changed")
        text = text.replace(needle, needle + patch)
        needle = "write(client, (uint8_t *)handshake.c_str(), handshake.length());"
        if text.count(needle) != 1:
            raise RuntimeError("Pinned WebSockets upgrade request changed")
        text = text.replace(needle, needle + '\n    client->cProtocol = "";')
        needle = "String headerLine = _client.tcp->readStringUntil('\\n');"
        if text.count(needle) != 1:
            raise RuntimeError("Pinned WebSockets HTTP parser changed")
        text = text.replace(needle, """String headerLine;
                bool complete = false;
                while (millis() - _lastHeaderSent < WEBSOCKETS_TCP_TIMEOUT) {
                    int value = _client.tcp->read();
                    if (value < 0) {
                        if (!_client.tcp->connected()) break;
                        delay(1);
                        continue;
                    }
                    if (value == '\\n') { complete = true; break; }
                    if (headerLine.length() >= 1024) break;
                    headerLine += (char)value;
                }
                if (!complete) {
                    clientDisconnect(&_client, "HTTP header timeout or too long");
                    return;
                }""")
        text = text.replace(
            "_lastHeaderSent + WEBSOCKETS_TCP_TIMEOUT < millis()",
            "millis() - _lastHeaderSent >= WEBSOCKETS_TCP_TIMEOUT",
        )
    elif source.name == "WebSockets.cpp":
        needle = "len = client->tcp->read((uint8_t *)out, n);\n        if(len > 0) {\n            t = millis();"
        if text.count(needle) != 1:
            raise RuntimeError("Pinned WebSockets frame read changed")
        text = text.replace(needle, "len = client->tcp->read((uint8_t *)out, n);\n        if(len > 0) {")
    elif source.name == "freertos-main.cpp":
        needle = 'xTaskCreate(__core0, "CORE0", 1024,'
        if text.count(needle) != 1:
            raise RuntimeError("Pinned Arduino-Pico application task creation changed")
        text = text.replace(needle, 'xTaskCreate(__core0, "CORE0", 4096,')
    elif source.name == "CYW43shim.cpp":
        start = text.index("        cyw43_wifi_join(")
        end_marker = "        return false; // We ended above with a failure"
        if text.count(end_marker) != 1:
            raise RuntimeError("Pinned CYW43 association loop changed")
        end = text.index(end_marker, start) + len(end_marker)
        # The observer calls WiFi.beginNoBlock on the lwIP task. Waiting here
        # would prevent that same task from servicing association and DHCP.
        text = text[:start] + """        return cyw43_wifi_join(&cyw43_state, strlen(_ssid), (const uint8_t *)_ssid,
            _password ? strlen(_password) : 0, (const uint8_t *)_password, authmode,
            (_bssid[0] | _bssid[1] | _bssid[2] | _bssid[3] | _bssid[4] | _bssid[5]) ? _bssid : nullptr,
            CYW43_CHANNEL_NONE) == 0;""" + text[end:]
    else:
        return node
    target = Path(build_env.subst("$BUILD_DIR/pico-patches")) / source.name
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text() != text:
        target.write_text(text)
    build_env.AppendUnique(CPPPATH=[str(source.parent)])
    return build_env.Object(str(target))


env.AddBuildMiddleware(patch_source, "*WebSocketsClient.cpp")
env.AddBuildMiddleware(patch_source, "*WebSockets.cpp")
env.AddBuildMiddleware(patch_source, "*freertos-main.cpp")
env.AddBuildMiddleware(patch_source, "*CYW43shim.cpp")
