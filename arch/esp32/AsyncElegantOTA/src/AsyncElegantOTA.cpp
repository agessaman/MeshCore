#include <AsyncElegantOTA.h>

AsyncElegantOtaClass AsyncElegantOTA;

void AsyncElegantOtaClass::setID(const char* id){
    _id = id;
}

void AsyncElegantOtaClass::begin(AsyncWebServer *server, const char* username, const char* password){
    _server = server;

    if(strlen(username) > 0){
        _authRequired = true;
        _username = username;
        _password = password;
    }else{
        _authRequired = false;
        _username = "";
        _password = "";
    }

    _server->on("/update/identity", HTTP_GET, [&](AsyncWebServerRequest *request){
        if(_authRequired){
            if(!request->authenticate(_username.c_str(), _password.c_str())){
                return request->requestAuthentication();
            }
        }
        #if defined(ESP8266)
            request->send(200, "application/json", "{\"id\": \""+_id+"\", \"hardware\": \"ESP8266\"}");
        #elif defined(ESP32)
            request->send(200, "application/json", "{\"id\": \""+_id+"\", \"hardware\": \"ESP32\"}");
        #endif
    });

    _server->on("/update", HTTP_GET, [&](AsyncWebServerRequest *request){
        if(_authRequired){
            if(!request->authenticate(_username.c_str(), _password.c_str())){
                return request->requestAuthentication();
            }
        }
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", ELEGANT_HTML, ELEGANT_HTML_SIZE);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    _server->on("/update", HTTP_POST, [&](AsyncWebServerRequest *request) {
        if(_authRequired){
            if(!request->authenticate(_username.c_str(), _password.c_str())){
                return request->requestAuthentication();
            }
        }
        if (_uploadRequest != request) {
            if (_uploadRequest != nullptr || Update.isRunning() ||
                _restartPending.load(std::memory_order_acquire)) {
                return request->send(409, "text/plain", "OTA already running");
            }
            return request->send(400, "text/plain", "OTA upload not accepted");
        }
        _uploadRequest = nullptr;
        // the request handler is triggered after the upload has finished... 
        // create the response, add header, and send response
        AsyncWebServerResponse *response = request->beginResponse((Update.hasError())?500:200, "text/plain", (Update.hasError())?"FAIL":"OK");
        response->addHeader("Connection", "close");
        response->addHeader("Access-Control-Allow-Origin", "*");
        armDeferredRestart();
        // Finished request records delivery and leaves the update alone.
        request->onDisconnect([this]() {
            _restartClientClosed.store(true, std::memory_order_release);
        });
        request->send(response);
        Serial.println("OTA: reply queued, waiting to restart");
    }, [&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        //Upload handler chunks in data
        if(_authRequired){
            if(!request->authenticate(_username.c_str(), _password.c_str())){
                return request->requestAuthentication();
            }
        }

        if (!index) {
            if (_uploadRequest || Update.isRunning() ||
                _restartPending.load(std::memory_order_acquire)) {
                return request->send(409, "text/plain", "OTA already running");
            }
            if(!request->hasParam("MD5", true)) {
                return request->send(400, "text/plain", "MD5 parameter missing");
            }

            if(!Update.setMD5(request->getParam("MD5", true)->value().c_str())) {
                return request->send(400, "text/plain", "MD5 parameter invalid");
            }

            #if defined(ESP8266)
                int cmd = (filename == "filesystem") ? U_FS : U_FLASH;
                Update.runAsync(true);
                size_t fsSize = ((size_t) &_FS_end - (size_t) &_FS_start);
                uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
                if (!Update.begin((cmd == U_FS)?fsSize:maxSketchSpace, cmd)){ // Start with max available size
            #elif defined(ESP32)
                int cmd = (filename == "filesystem") ? U_SPIFFS : U_FLASH;
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) { // Start with max available size
            #endif
                Update.printError(Serial);
                return request->send(400, "text/plain", "OTA could not begin");
            }
            _uploadRequest = request;
            request->client()->setRxTimeout(UPLOAD_IDLE_TIMEOUT_SECONDS);
            request->onDisconnect([this, request]() {
                if (_uploadRequest != request) return;
                _uploadRequest = nullptr;
                if (Update.isRunning()) {
                    Update.abort();
                    Serial.println("OTA: abandoned upload aborted");
                }
            });
        }

        if (_uploadRequest != request) return;

        // Write chunked data to the free sketch space
        if(len){
            if (Update.write(data, len) != len) {
                return request->send(400, "text/plain", "OTA could not begin");
            }
        }
            
        if (final) { // if the final flag is set then this is the last frame of data
            if (!Update.end(true)) { //true to set the size to the current progress
                Update.printError(Serial);
                return request->send(400, "text/plain", "Could not end OTA");
            }
        }else{
            return;
        }
    });
}

// deprecated, keeping for backward compatibility
void AsyncElegantOtaClass::loop() {
}

void AsyncElegantOtaClass::armDeferredRestart() {
    _restartClientClosed.store(false, std::memory_order_relaxed);
    _restartArmedMs.store(millis(), std::memory_order_relaxed);
    _restartPending.store(true, std::memory_order_release);
}

bool AsyncElegantOtaClass::restartArmed(uint32_t& armed_ms, bool& client_closed) const {
    if (!_restartPending.load(std::memory_order_acquire)) return false;
    armed_ms = _restartArmedMs.load(std::memory_order_relaxed);
    client_closed = _restartClientClosed.load(std::memory_order_acquire);
    return true;
}

// Blocking reboot for existing callers. The upload reply defers instead.
void AsyncElegantOtaClass::restart() {
    yield();
    delay(1000);
    yield();
    ESP.restart();
}

String AsyncElegantOtaClass::getID(){
    String id = "";
    #if defined(ESP8266)
        id = String(ESP.getChipId());
    #elif defined(ESP32)
        id = String((uint32_t)ESP.getEfuseMac(), HEX);
    #endif
    id.toUpperCase();
    return id;
}
