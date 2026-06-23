#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <NCMEthernetlwIP.h>
#include <lua/lua.hpp>
#include <lwip/netif.h>
#include <dhcpserver/dhcpserver.h>
#include <WebSocketsServer.h>
#include <Adafruit_Protomatter.h>

// --- HUB75 Configuration ---
struct Hub75Pins {
    int* rgb = nullptr;
    int* addr = nullptr;
    int clk = -1;
    int lat = -1;
    int oe = -1;
    int rgb_count = 0;
    int addr_count = 0;
} hub75_pins;

Adafruit_Protomatter* matrix = nullptr;

// --- Configuration ---
const uint8_t mac[] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};
IPAddress ip(192, 168, 7, 1);
IPAddress gateway(192, 168, 7, 1);
IPAddress subnet(255, 255, 255, 0);

NCMEthernetlwIP ethusb;
WebServer server(80);
WebSocketsServer webSocket(81);
dhcp_server_t my_dhcp_server;

// --- State Management ---
String current_file = "main.lua";
String lua_code_pending = "";
bool run_requested = false;
bool stop_requested = false;
bool is_running = false;
unsigned long last_execution_time = 0;
unsigned long last_yield_time = 0;
String web_serial_buffer = "";
int used_pins[40];
int used_pins_count = 0;

// --- Helpers ---
String jsonEscape(String s) {
    String res = "";
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '\"') res += "\\\"";
        else if (c == '\\') res += "\\\\";
        else if (c == '\n') res += "\\n";
        else if (c == '\r') res += "\\r";
        else if (c == '\t') res += "\\t";
        else if (c < 32) {} // Ignore other control chars
        else res += c;
    }
    return res;
}

void log_to_web(String msg) {
    web_serial_buffer += msg;
    if (web_serial_buffer.length() > 2000) {
        web_serial_buffer = web_serial_buffer.substring(web_serial_buffer.length() - 2000);
    }
}

// --- WebSocket Event Handler ---
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    (void)num; (void)type; (void)payload; (void)length;
}

// --- Lua Bindings ---
int lua_print(lua_State *L) {
    int n = lua_gettop(L);
    String out = "";
    for (int i = 1; i <= n; i++) {
        const char *s = lua_tostring(L, i);
        if (s) out += s;
        if (i < n) out += "\t";
    }
    Serial.println(out);
    log_to_web(out + "\n");
    webSocket.broadcastTXT(out + "\n");
    return 0;
}

int lua_digitalWrite(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    int val = luaL_checkinteger(L, 2);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, val);

    // Track used pins for cleanup
    bool already_tracked = false;
    for (int i = 0; i < used_pins_count; i++) {
        if (used_pins[i] == pin) {
            already_tracked = true;
            break;
        }
    }
    if (!already_tracked && used_pins_count < 40) {
        used_pins[used_pins_count++] = pin;
    }

    return 0;
}

int lua_delay(lua_State *L) {
    int ms = luaL_checkinteger(L, 1);
    unsigned long start = millis();
    while (millis() - start < (unsigned long)ms) {
        if (stop_requested) break;
        server.handleClient();
        webSocket.loop();
        delay(1);
    }
    return 0;
}

void lua_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    if (stop_requested) luaL_error(L, "Stopped by user");
    unsigned long now = millis();
    if (now - last_yield_time >= 10) {
        last_yield_time = now;
        server.handleClient();
        webSocket.loop();
        yield();
    }
}

// --- HUB75 Lua Bindings ---

int lua_hub75_setPins(lua_State *L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) {
        return luaL_error(L, "rgbPins and addrPins must be tables");
    }
    int clk = luaL_checkinteger(L, 3);
    int lat = luaL_checkinteger(L, 4);
    int oe = luaL_checkinteger(L, 5);

    if (hub75_pins.rgb) {
        delete[] hub75_pins.rgb;
        hub75_pins.rgb = nullptr;
    }
    if (hub75_pins.addr) {
        delete[] hub75_pins.addr;
        hub75_pins.addr = nullptr;
    }

    int rgb_len = lua_rawlen(L, 1);
    hub75_pins.rgb_count = rgb_len;
    hub75_pins.rgb = new int[rgb_len];
    for (int i = 1; i <= rgb_len; i++) {
        lua_rawgeti(L, 1, i);
        hub75_pins.rgb[i - 1] = luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }

    int addr_len = lua_rawlen(L, 2);
    hub75_pins.addr_count = addr_len;
    hub75_pins.addr = new int[addr_len];
    for (int i = 1; i <= addr_len; i++) {
        lua_rawgeti(L, 2, i);
        hub75_pins.addr[i - 1] = luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }

    hub75_pins.clk = clk;
    hub75_pins.lat = lat;
    hub75_pins.oe = oe;

    return 0;
}

int lua_hub75_begin(lua_State *L) {
    int width = luaL_checkinteger(L, 1);
    int height = luaL_checkinteger(L, 2);

    if (hub75_pins.rgb_count == 0 || hub75_pins.rgb == nullptr ||
        hub75_pins.addr_count == 0 || hub75_pins.addr == nullptr ||
        hub75_pins.clk == -1) {
        return luaL_error(L, "HUB75 pins not configured. Call setPins first.");
    }

    if (matrix) {
        delete matrix;
        matrix = nullptr;
    }

    uint8_t* rgb_uint8 = new uint8_t[hub75_pins.rgb_count];
    for (int i = 0; i < hub75_pins.rgb_count; i++) {
        rgb_uint8[i] = (uint8_t)hub75_pins.rgb[i];
    }
    uint8_t* addr_uint8 = new uint8_t[hub75_pins.addr_count];
    for (int i = 0; i < hub75_pins.addr_count; i++) {
        addr_uint8[i] = (uint8_t)hub75_pins.addr[i];
    }

    matrix = new Adafruit_Protomatter(
        width, 
        4, 
        hub75_pins.rgb_count / 6, 
        rgb_uint8,
        hub75_pins.addr_count,
        addr_uint8,
        (uint8_t)hub75_pins.clk,
        (uint8_t)hub75_pins.lat,
        (uint8_t)hub75_pins.oe,
        true, 
        height
    );

    ProtomatterStatus status = matrix->begin();
    delete[] rgb_uint8;
    delete[] addr_uint8;

    if (status != PROTOMATTER_OK) {
        delete matrix;
        matrix = nullptr;
        return luaL_error(L, "Failed to initialize HUB75 matrix");
    }

    return 0;
}



int lua_hub75_show(lua_State *L) {
    (void)L;
    if (matrix) {
        matrix->show();
    }
    return 0;
}

int lua_hub75_fillScreen(lua_State *L) {
    int r = luaL_checkinteger(L, 1);
    int g = luaL_checkinteger(L, 2);
    int b = luaL_checkinteger(L, 3);
    if (matrix) {
        matrix->fillScreen(matrix->color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawPixel(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int r = luaL_checkinteger(L, 3);
    int g = luaL_checkinteger(L, 4);
    int b = luaL_checkinteger(L, 5);
    if (matrix) {
        matrix->drawPixel(x, y, matrix->color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawLine(lua_State *L) {
    int x0 = luaL_checkinteger(L, 1);
    int y0 = luaL_checkinteger(L, 2);
    int x1 = luaL_checkinteger(L, 3);
    int y1 = luaL_checkinteger(L, 4);
    int r = luaL_checkinteger(L, 5);
    int g = luaL_checkinteger(L, 6);
    int b = luaL_checkinteger(L, 7);
    if (matrix) {
        matrix->drawLine(x0, y0, x1, y1, matrix->color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawRect(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int r = luaL_checkinteger(L, 5);
    int g = luaL_checkinteger(L, 6);
    int b = luaL_checkinteger(L, 7);
    if (matrix) {
        matrix->drawRect(x, y, w, h, matrix->color565(r, g, b));
    }
    return 0;
}

int lua_hub75_fillRect(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int r = luaL_checkinteger(L, 5);
    int g = luaL_checkinteger(L, 6);
    int b = luaL_checkinteger(L, 7);
    if (matrix) {
        matrix->fillRect(x, y, w, h, matrix->color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawCircle(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int radius = luaL_checkinteger(L, 3);
    int r = luaL_checkinteger(L, 4);
    int g = luaL_checkinteger(L, 5);
    int b = luaL_checkinteger(L, 6);
    if (matrix) {
        matrix->drawCircle(x, y, radius, matrix->color565(r, g, b));
    }
    return 0;
}

int lua_hub75_fillCircle(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int radius = luaL_checkinteger(L, 3);
    int r = luaL_checkinteger(L, 4);
    int g = luaL_checkinteger(L, 5);
    int b = luaL_checkinteger(L, 6);
    if (matrix) {
        matrix->fillCircle(x, y, radius, matrix->color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawString(lua_State *L) {
    const char* text = luaL_checkstring(L, 1);
    int x = luaL_checkinteger(L, 2);
    int y = luaL_checkinteger(L, 3);
    int r = luaL_checkinteger(L, 4);
    int g = luaL_checkinteger(L, 5);
    int b = luaL_checkinteger(L, 6);
    if (matrix) {
        matrix->setTextColor(matrix->color565(r, g, b));
        matrix->setCursor(x, y);
        matrix->print(text);
    }
    return 0;
}

void run_lua(String code) {
    is_running = true;
    stop_requested = false;
    lua_State *L = luaL_newstate();
    if (!L) {
        log_to_web("Error: Lua State Fail\n");
        is_running = false;
        return;
    }
    luaL_openlibs(L);
    lua_register(L, "print", lua_print);
    lua_register(L, "digitalWrite", lua_digitalWrite);
    lua_register(L, "delay", lua_delay);
    lua_pushinteger(L, HIGH); lua_setglobal(L, "HIGH");
    lua_pushinteger(L, LOW); lua_setglobal(L, "LOW");
    lua_pushinteger(L, LED_BUILTIN); lua_setglobal(L, "LED_BUILTIN");
    
    // Register hub75 module
    lua_newtable(L);
    lua_pushcfunction(L, lua_hub75_setPins); lua_setfield(L, -2, "setPins");
    lua_pushcfunction(L, lua_hub75_begin); lua_setfield(L, -2, "begin");
    lua_pushcfunction(L, lua_hub75_show); lua_setfield(L, -2, "show");
    lua_pushcfunction(L, lua_hub75_fillScreen); lua_setfield(L, -2, "fillScreen");
    lua_pushcfunction(L, lua_hub75_drawPixel); lua_setfield(L, -2, "drawPixel");
    lua_pushcfunction(L, lua_hub75_drawLine); lua_setfield(L, -2, "drawLine");
    lua_pushcfunction(L, lua_hub75_drawRect); lua_setfield(L, -2, "drawRect");
    lua_pushcfunction(L, lua_hub75_fillRect); lua_setfield(L, -2, "fillRect");
    lua_pushcfunction(L, lua_hub75_drawCircle); lua_setfield(L, -2, "drawCircle");
    lua_pushcfunction(L, lua_hub75_fillCircle); lua_setfield(L, -2, "fillCircle");
    lua_pushcfunction(L, lua_hub75_drawString); lua_setfield(L, -2, "drawString");
    lua_setglobal(L, "hub75");

    lua_sethook(L, lua_hook, LUA_MASKCOUNT, 100);

    log_to_web("--- [" + current_file + "] Start ---\n");
    unsigned long start_time = millis();
    if (luaL_dostring(L, code.c_str())) {
        log_to_web("Lua Error: " + String(lua_tostring(L, -1)) + "\n");
    }
    last_execution_time = millis() - start_time;
    log_to_web("--- [" + current_file + "] End ---\n");
    
    // Cleanup: Reset used pins
    for (int i = 0; i < used_pins_count; i++) {
        digitalWrite(used_pins[i], LOW);
        pinMode(used_pins[i], INPUT);
    }
    used_pins_count = 0;

    // Cleanup HUB75 matrix
    if (matrix) {
        delete matrix;
        matrix = nullptr;
    }
    if (hub75_pins.rgb) {
        delete[] hub75_pins.rgb;
        hub75_pins.rgb = nullptr;
    }
    if (hub75_pins.addr) {
        delete[] hub75_pins.addr;
        hub75_pins.addr = nullptr;
    }
    hub75_pins.rgb_count = 0;
    hub75_pins.addr_count = 0;
    hub75_pins.clk = -1;
    hub75_pins.lat = -1;
    hub75_pins.oe = -1;

    lua_close(L);
    is_running = false;
    stop_requested = false;
}

// --- HTML GUI ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Pico Lua Playground</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: system-ui, sans-serif; margin: 0; background: #fff5f7; color: #4b5563; display: flex; height: 100vh; overflow: hidden; }
        .sidebar { width: 240px; background: rgba(255,255,255,0.7); backdrop-filter: blur(10px); border-right: 2px dashed #f0b6d2; display: flex; flex-direction: column; padding: 10px; box-sizing: border-box; }
        .sidebar-header { padding: 15px 10px; font-weight: 700; color: #db2777; border-bottom: 2px solid #fee2e9; margin-bottom: 12px; display: flex; justify-content: space-between; align-items: center; }
        .file-list { flex: 1; overflow-y: auto; }
        .file-item { padding: 10px 15px; cursor: pointer; border-radius: 12px; margin-bottom: 8px; display: flex; justify-content: space-between; align-items: center; font-size: 14px; background: #fff; transition: 0.2s; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.05); }
        .file-item:hover { background: #fff1f2; color: #db2777; transform: translateY(-2px); }
        .file-item.active { background: linear-gradient(135deg, #fecdd3 0%, #ffe4e6 100%); color: #e11d48; border: 1px solid #fda4af; }
        .delete-btn { color: #f43f5e; opacity: 0.3; transition: 0.2s; font-size: 18px; }
        .file-item:hover .delete-btn { opacity: 1; }
        .main { flex: 1; display: flex; flex-direction: column; background: #fff; margin: 15px; border-radius: 24px; border: 2px solid #fbcfe8; overflow: hidden; box-shadow: -10px 0 30px rgba(0,0,0,0.02); }
        .header { padding: 12px 25px; background: linear-gradient(90deg, #fff0f6 0%, #f3f0ff 100%); border-bottom: 2px solid #f3e8ff; display: flex; justify-content: space-between; align-items: center; }
        .editor-container { display: flex; flex: 1; overflow: hidden; background: #fafaf9; position: relative; }
        .line-numbers { width: 45px; padding: 25px 5px 25px 0; font-family: monospace; font-size: 14px; line-height: 1.6; text-align: right; color: #db2777; background: #fff0f6; border-right: 2px dashed #fbcfe8; user-select: none; overflow-y: hidden; box-sizing: border-box; }
        textarea { flex: 1; border: none; padding: 25px 25px 25px 15px; font-family: monospace; font-size: 14px; outline: none; background: transparent; color: #4c1d95; line-height: 1.6; resize: none; box-sizing: border-box; overflow-y: auto; }
        .terminal { height: 180px; background: #2d2d2d; color: #4ec9b0; padding: 15px; font-family: monospace; font-size: 12px; overflow-y: auto; border-top: 2px solid #fbcfe8; white-space: pre-wrap; line-height: 1.4; }
        .btn { padding: 8px 18px; border-radius: 18px; border: none; cursor: pointer; font-weight: 700; font-size: 13px; transition: all 0.2s; box-shadow: 0 4px 6px rgba(0,0,0,0.05); }
        .btn-run { background: linear-gradient(135deg, #a7f3d0 0%, #34d399 100%); color: white; margin-left: 8px; }
        .btn-stop { background: linear-gradient(135deg, #fca5a5 0%, #ef4444 100%); color: white; margin-left: 8px; }
        .btn-save { background: linear-gradient(135deg, #a5f3fc 0%, #818cf8 100%); color: white; }
        .btn:disabled { background: #e5e7eb; color: #9ca3af; cursor: not-allowed; box-shadow: none; transform: none !important; }
        #status-light { width: 10px; height: 10px; border-radius: 50%; background: #ccc; display: inline-block; margin-right: 8px; }
        .running #status-light { background: #34d399; box-shadow: 0 0 8px #34d399; }
        .modal-overlay { position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(255, 245, 247, 0.7); backdrop-filter: blur(4px); display: flex; align-items: center; justify-content: center; opacity: 0; pointer-events: none; transition: all 0.3s ease; z-index: 1000; }
        .modal-overlay.show { opacity: 1; pointer-events: auto; }
        .modal { background: #ffffff; border-radius: 24px; padding: 30px; width: 320px; border: 2px solid #fbcfe8; box-shadow: 0 10px 25px rgba(0,0,0,0.05); }
        .modal-title { font-weight: 700; font-size: 18px; color: #db2777; margin-bottom: 15px; }
        .modal-input { width: 100%; padding: 10px 15px; border-radius: 14px; border: 2px solid #fee2e9; box-sizing: border-box; outline: none; font-size: 14px; margin-bottom: 20px; }
        .modal-actions { display: flex; justify-content: flex-end; gap: 10px; }
        .btn-cancel { background: #f3f4f6; color: #6b7280; }
        .btn-docs { background: linear-gradient(135deg, #c084fc 0%, #a855f7 100%); color: white; margin-right: 8px; }
        .btn:hover:not(:disabled) { transform: translateY(-2px); box-shadow: 0 6px 12px rgba(0,0,0,0.1); }
        .modal-large { width: 700px; max-width: 95%; max-height: 85vh; display: flex; flex-direction: column; box-sizing: border-box; }
        .modal-body { overflow-y: auto; flex: 1; margin-top: 15px; padding-right: 5px; text-align: left; }
        .api-section { margin-bottom: 24px; }
        .api-section-title { font-weight: 700; font-size: 16px; color: #db2777; border-bottom: 2px solid #fee2e9; padding-bottom: 6px; margin-bottom: 12px; }
        .api-item { background: #fff8f9; border-left: 4px solid #db2777; padding: 12px 16px; border-radius: 4px 12px 12px 4px; margin-bottom: 12px; box-shadow: 0 2px 4px rgba(219,39,119,0.03); }
        .api-signature { font-family: monospace; font-weight: 700; color: #9d174d; font-size: 14px; margin-bottom: 6px; }
        .api-desc { font-size: 13px; color: #4b5563; line-height: 1.5; margin-bottom: 6px; }
        .api-params { font-size: 12px; color: #6b7280; border-top: 1px dashed #fbcfe8; padding-top: 6px; margin-top: 6px; }
        .api-param-item { display: inline-block; margin-right: 15px; }
        .api-param-name { font-weight: 600; color: #b45309; font-family: monospace; }
        .api-param-type { color: #059669; font-family: monospace; font-size: 11px; }
    </style>
</head>
<body>
    <div class="sidebar">
        <div class="sidebar-header"><span>EXPLORER</span><button onclick="newFile()" style="background:#fff0f6; border:2px solid #fbcfe8; color:#db2777; width:30px; height:30px; border-radius:50%; cursor:pointer; font-weight:700;">+</button></div>
        <div id="fileList" class="file-list"></div>
    </div>
    <div class="main" id="mainContainer">
        <div class="header">
            <div id="fileName" style="font-weight:700; color:#7c3aed; font-size:16px;">main.lua</div>
            <div style="display: flex; align-items: center;">
                <span id="ramText" style="font-size:11px; margin-right:15px; color:#6b7280; font-family:monospace;">RAM: -- KB</span>
                <span id="statusText" style="font-size:12px; margin-right:12px; font-weight:700; color:#9333ea;">Idle</span>
                <div id="status-light"></div>
                <button class="btn btn-docs" onclick="openDocsModal()">API Docs</button>
                <button class="btn btn-save" onclick="saveCode()">Save</button>
                <button class="btn btn-run" id="runBtn" onclick="runCode()">Run</button>
                <button class="btn btn-stop" id="stopBtn" onclick="stopCode()" disabled>Stop</button>
            </div>
        </div>
        <div class="editor-container">
            <div class="line-numbers" id="lineNumbers">1</div>
            <textarea id="editor" spellcheck="false" placeholder="-- Write your Lua code here..." onscroll="syncScroll()" oninput="updateLineNumbers()"></textarea>
        </div>
        <div class="terminal" id="terminal">--- Web Terminal ---</div>
    </div>

    <div id="newFileModal" class="modal-overlay"><div class="modal"><div class="modal-title">Create New File</div><input type="text" id="newFileNameInput" class="modal-input" placeholder="e.g. blink.lua"><div class="modal-actions"><button class="btn btn-cancel" onclick="closeModal()">Cancel</button><button class="btn btn-save" onclick="submitNewFile()">Create</button></div></div></div>
    <div id="deleteFileModal" class="modal-overlay"><div class="modal"><div class="modal-title">Delete File</div><p id="deleteMessage" style="font-size:14px; margin-bottom:20px; color:#6b7280; font-weight:600;"></p><div class="modal-actions"><button class="btn btn-cancel" onclick="closeDeleteModal()">Cancel</button><button class="btn btn-stop" onclick="submitDeleteFile()">Delete</button></div></div></div>

    <div id="docsModal" class="modal-overlay">
        <div class="modal modal-large">
            <div class="modal-title" style="display:flex; justify-content:space-between; align-items:center; margin-bottom: 10px;">
                <span>Lua API Documentation</span>
                <span onclick="closeDocsModal()" style="cursor:pointer; font-size:24px; color:#db2777; font-weight:700;">&times;</span>
            </div>
            <div class="modal-body">
                <div class="api-section">
                    <div class="api-section-title">Global Functions & Constants</div>
                    
                    <div class="api-item">
                        <div class="api-signature">print(...)</div>
                        <div class="api-desc">Outputs values to the Web Terminal for debugging. Accepts multiple arguments of any type.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">...</span> <span class="api-param-type">any</span> - Values to print</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">digitalWrite(pin, state)</div>
                        <div class="api-desc">Sets the state of a GPIO pin to HIGH or LOW. Useful for controlling onboard or external LEDs.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">pin</span> <span class="api-param-type">number</span> - GPIO pin number (e.g. 0-29)</span>
                            <span class="api-param-item"><span class="api-param-name">state</span> <span class="api-param-type">number</span> - Pin state (HIGH or LOW)</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">delay(ms)</div>
                        <div class="api-desc">Pauses the execution of the script for a specified duration of milliseconds.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">ms</span> <span class="api-param-type">number</span> - Delay duration in milliseconds</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">Constants</div>
                        <div class="api-desc">Pre-defined global variables:</div>
                        <div class="api-params" style="border:none; padding:0; margin:0;">
                            <span class="api-param-item"><span class="api-param-name">HIGH</span> <span class="api-param-type">1</span></span>
                            <span class="api-param-item"><span class="api-param-name">LOW</span> <span class="api-param-type">0</span></span>
                            <span class="api-param-item"><span class="api-param-name">LED_BUILTIN</span> <span class="api-param-type">25</span> - Built-in LED pin</span>
                        </div>
                    </div>
                </div>

                <div class="api-section">
                    <div class="api-section-title">hub75 Module (HUB75 RGB LED Matrix 64x64)</div>

                    <div class="api-item">
                        <div class="api-signature">hub75.setPins(rgbTable, addrTable, clk, lat, oe)</div>
                        <div class="api-desc">Configures the pins dynamically. Must be called first before calling begin().</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">rgbTable</span> <span class="api-param-type">table</span> - Array of 6 numbers for {R1, G1, B1, R2, G2, B2}</span>
                            <span class="api-param-item"><span class="api-param-name">addrTable</span> <span class="api-param-type">table</span> - Array of 5 numbers for {A, B, C, D, E}</span>
                            <span class="api-param-item"><span class="api-param-name">clk</span> <span class="api-param-type">number</span> - Clock pin</span>
                            <span class="api-param-item"><span class="api-param-name">lat</span> <span class="api-param-type">number</span> - Latch pin</span>
                            <span class="api-param-item"><span class="api-param-name">oe</span> <span class="api-param-type">number</span> - Output enable pin</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.begin(width, height)</div>
                        <div class="api-desc">Initializes the matrix driver. Creates the frame buffer and starts the PIO driving signals.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">width</span> <span class="api-param-type">number</span> - Matrix width (e.g. 64)</span>
                            <span class="api-param-item"><span class="api-param-name">height</span> <span class="api-param-type">number</span> - Matrix height (e.g. 64)</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.show()</div>
                        <div class="api-desc">Pushes the back buffer to the LED matrix panel to display the changes. Call this to refresh the screen.</div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.fillScreen(r, g, b)</div>
                        <div class="api-desc">Fills the frame buffer with a single solid RGB color.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">r</span>, <span class="api-param-name">g</span>, <span class="api-param-name">b</span> <span class="api-param-type">number</span> - RGB components (0-255)</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.drawPixel(x, y, r, g, b)</div>
                        <div class="api-desc">Draws a pixel at the given coordinate with specified color.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">x</span>, <span class="api-param-name">y</span> <span class="api-param-type">number</span> - Coordinates (0 to 63)</span>
                            <span class="api-param-item"><span class="api-param-name">r</span>, <span class="api-param-name">g</span>, <span class="api-param-name">b</span> <span class="api-param-type">number</span> - RGB components (0-255)</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.drawLine(x0, y0, x1, y1, r, g, b)</div>
                        <div class="api-desc">Draws a straight line between two points.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">x0</span>, <span class="api-param-name">y0</span> <span class="api-param-type">number</span> - Start point coordinates</span>
                            <span class="api-param-item"><span class="api-param-name">x1</span>, <span class="api-param-name">y1</span> <span class="api-param-type">number</span> - End point coordinates</span>
                            <span class="api-param-item"><span class="api-param-name">r</span>, <span class="api-param-name">g</span>, <span class="api-param-name">b</span> <span class="api-param-type">number</span> - RGB components (0-255)</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.drawRect(x, y, w, h, r, g, b)</div>
                        <div class="api-desc">Draws an unfilled rectangle outline.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">x</span>, <span class="api-param-name">y</span> <span class="api-param-type">number</span> - Top-left coordinates</span>
                            <span class="api-param-item"><span class="api-param-name">w</span>, <span class="api-param-name">h</span> <span class="api-param-type">number</span> - Width and height</span>
                            <span class="api-param-item"><span class="api-param-name">r</span>, <span class="api-param-name">g</span>, <span class="api-param-name">b</span> <span class="api-param-type">number</span> - RGB components (0-255)</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.fillRect(x, y, w, h, r, g, b)</div>
                        <div class="api-desc">Draws a filled rectangle.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">x</span>, <span class="api-param-name">y</span> <span class="api-param-type">number</span> - Top-left coordinates</span>
                            <span class="api-param-item"><span class="api-param-name">w</span>, <span class="api-param-name">h</span> <span class="api-param-type">number</span> - Width and height</span>
                            <span class="api-param-item"><span class="api-param-name">r</span>, <span class="api-param-name">g</span>, <span class="api-param-name">b</span> <span class="api-param-type">number</span> - RGB components (0-255)</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.drawCircle(x, y, radius, r, g, b)</div>
                        <div class="api-desc">Draws a circle outline with specified radius.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">x</span>, <span class="api-param-name">y</span> <span class="api-param-type">number</span> - Center coordinates</span>
                            <span class="api-param-item"><span class="api-param-name">radius</span> <span class="api-param-type">number</span> - Circle radius</span>
                            <span class="api-param-item"><span class="api-param-name">r</span>, <span class="api-param-name">g</span>, <span class="api-param-name">b</span> <span class="api-param-type">number</span> - RGB components (0-255)</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.fillCircle(x, y, radius, r, g, b)</div>
                        <div class="api-desc">Draws a filled circle with specified radius.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">x</span>, <span class="api-param-name">y</span> <span class="api-param-type">number</span> - Center coordinates</span>
                            <span class="api-param-item"><span class="api-param-name">radius</span> <span class="api-param-type">number</span> - Circle radius</span>
                            <span class="api-param-item"><span class="api-param-name">r</span>, <span class="api-param-name">g</span>, <span class="api-param-name">b</span> <span class="api-param-type">number</span> - RGB components (0-255)</span>
                        </div>
                    </div>

                    <div class="api-item">
                        <div class="api-signature">hub75.drawString(text, x, y, r, g, b)</div>
                        <div class="api-desc">Draws text starting at the given coordinates.</div>
                        <div class="api-params">
                            <span class="api-param-item"><span class="api-param-name">text</span> <span class="api-param-type">string</span> - String message to draw</span>
                            <span class="api-param-item"><span class="api-param-name">x</span>, <span class="api-param-name">y</span> <span class="api-param-type">number</span> - Text starting baseline coordinates</span>
                            <span class="api-param-item"><span class="api-param-name">r</span>, <span class="api-param-name">g</span>, <span class="api-param-name">b</span> <span class="api-param-type">number</span> - RGB components (0-255)</span>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <script>
    let currentPath = "/main.lua";
    let fileToDelete = "";
    let isWsConnected = false;
    let ws;

    const editor = document.getElementById("editor");
    const lineNumbers = document.getElementById("lineNumbers");

    function updateLineNumbers() {
        const lines = editor.value.split('\n');
        const count = lines.length;
        let numbers = "";
        for (let i = 1; i <= count; i++) {
            numbers += i + '<br>';
        }
        lineNumbers.innerHTML = numbers;
    }

    function syncScroll() {
        lineNumbers.scrollTop = editor.scrollTop;
    }

    function connectWS() {
        ws = new WebSocket('ws://' + window.location.hostname + ':81');
        ws.onopen = () => { isWsConnected = true; console.log("WS Connected"); };
        ws.onclose = () => { isWsConnected = false; console.log("WS Disconnected. Reconnecting..."); setTimeout(connectWS, 2000); };
        ws.onmessage = (e) => {
            const term = document.getElementById("terminal");
            term.innerText += e.data;
            term.scrollTop = term.scrollHeight;
        };
        ws.onerror = (err) => { ws.close(); };
    }

    function listFiles() { fetch('/list_files').then(r => r.json()).then(files => {
        const list = document.getElementById("fileList"); list.innerHTML = "";
        files.forEach(f => {
            const div = document.createElement("div");
            div.className = "file-item" + (("/"+f.name) === currentPath ? " active" : "");
            div.innerHTML = `<span>${f.name}</span><span class="delete-btn" onclick="deleteFile('${f.name}', event)">&times;</span>`;
            div.onclick = () => loadFile("/" + f.name); list.appendChild(div);
        });
    }).catch(()=>{});}

    function loadFile(path) { currentPath = path; document.getElementById("fileName").innerText = path.substring(1); fetch('/read?path=' + path).then(r => r.text()).then(t => { editor.value = t; updateLineNumbers(); listFiles(); }).catch(()=>{}); }
    function saveCode() { fetch('/upload?path=' + currentPath, { method: 'POST', body: editor.value }).then(() => listFiles()).catch(()=>{}); }
    function runCode() { fetch('/run', { method: 'POST', body: editor.value }).catch(()=>{}); }
    function stopCode() { fetch('/stop', { method: 'POST' }).catch(()=>{}); }
    function newFile() { document.getElementById("newFileModal").classList.add("show"); document.getElementById("newFileNameInput").focus(); }
    function closeModal() { document.getElementById("newFileModal").classList.remove("show"); }
    function submitNewFile() {
        const name = document.getElementById("newFileNameInput").value.trim();
        if(name) {
            const filename = name.endsWith('.lua') ? name : name + '.lua';
            const path = filename.startsWith("/") ? filename : "/" + filename;
            fetch('/create?path=' + path, { method: 'POST' }).then(() => { loadFile(path); closeModal(); });
        }
    }
    function deleteFile(name, event) { event.stopPropagation(); if (name === "main.lua") return; fileToDelete = name; document.getElementById("deleteMessage").innerText = `Delete ${name}?`; document.getElementById("deleteFileModal").classList.add("show"); }
    function closeDeleteModal() { document.getElementById("deleteFileModal").classList.remove("show"); }
    function submitDeleteFile() { fetch('/delete?path=/' + fileToDelete, { method: 'POST' }).then(() => { if (currentPath === "/" + fileToDelete) loadFile("/main.lua"); else listFiles(); closeDeleteModal(); }); }
    function openDocsModal() { document.getElementById("docsModal").classList.add("show"); }
    function closeDocsModal() { document.getElementById("docsModal").classList.remove("show"); }

    setInterval(() => {
        fetch('/poll').then(r => r.json()).then(data => {
            const container = document.getElementById("mainContainer");
            const stText = document.getElementById("statusText");
            const ramText = document.getElementById("ramText");
            const runBtn = document.getElementById("runBtn");
            const stopBtn = document.getElementById("stopBtn");
            const term = document.getElementById("terminal");
            
            if (data.free_heap) {
                ramText.innerText = "RAM: " + Math.round(data.free_heap / 1024) + " KB";
            }

            if(data.running) {
                container.classList.add("running");
                stText.innerText = "Running...";
                runBtn.disabled = true; stopBtn.disabled = false;
            } else {
                container.classList.remove("running");
                stText.innerText = "Idle";
                runBtn.disabled = false; stopBtn.disabled = true;
            }
            if(data.logs && !isWsConnected) {
                term.innerText += data.logs;
                term.scrollTop = term.scrollHeight;
            }
        }).catch(()=>{});
    }, 2000);

    window.onload = () => { loadFile("/main.lua"); connectWS(); };
    </script>
</body>
</html>
)rawliteral";

// --- Web Handlers ---
void handleRoot() { server.send(200, "text/html", index_html); }
void handleList() {
    String json = "["; Dir root = LittleFS.openDir("/"); bool first = true;
    while (root.next()) { if (!first) json += ","; json += "{\"name\":\"" + root.fileName() + "\"}"; first = false; }
    json += "]"; server.send(200, "application/json", json);
}
void handleRead() {
    String path = server.arg("path");
    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        if (f) {
            String content = f.readString();
            f.close();
            server.send(200, "text/plain", content);
        } else {
            server.send(500, "text/plain", "Error opening file");
        }
    } else {
        server.send(200, "text/plain", "");
    }
}
void handleUpload() {
    String path = server.arg("path"); if (path == "") path = "/main.lua";
    File f = LittleFS.open(path, "w");
    if (f) { f.print(server.arg("plain")); f.close(); server.send(200, "text/plain", "Saved"); }
    else server.send(500, "text/plain", "Error");
}
void handleCreate() { String path = server.arg("path"); File f = LittleFS.open(path, "w"); if (f) { f.close(); server.send(200); } else server.send(500); }
void handleDelete() { String path = server.arg("path"); if (LittleFS.remove(path)) server.send(200); else server.send(500); }

void handleRun() {
    lua_code_pending = server.arg("plain");
    run_requested = true;
    server.send(200, "text/plain", "OK");
}
void handleStop() { stop_requested = true; server.send(200, "text/plain", "Stop Issued"); }

void handlePoll() {
    String json;
    json.reserve(512);
    json = "{";
    json += "\"running\":" + String(is_running ? "true" : "false") + ",";
    json += "\"free_heap\":" + String(rp2040.getFreeHeap()) + ",";
    json += "\"exec_time\":" + String(last_execution_time) + ",";
    json += "\"logs\":\"" + jsonEscape(web_serial_buffer) + "\"";
    json += "}";
    server.send(200, "application/json", json);
    web_serial_buffer = "";
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    LittleFS.begin();
    ethusb.config(ip, gateway, subnet);
    ethusb.begin(mac);

    // Initialize DHCP Server
    ip_addr_t lwip_ip, lwip_mask;
    IP_ADDR4(&lwip_ip, ip[0], ip[1], ip[2], ip[3]);
    IP_ADDR4(&lwip_mask, subnet[0], subnet[1], subnet[2], subnet[3]);
    dhcp_server_init(&my_dhcp_server, &lwip_ip, &lwip_mask, netif_default);

    server.on("/", handleRoot);
    server.on("/list_files", handleList);
    server.on("/read", handleRead);
    server.on("/upload", HTTP_POST, handleUpload);
    server.on("/create", HTTP_POST, handleCreate);
    server.on("/delete", HTTP_POST, handleDelete);
    server.on("/run", HTTP_POST, handleRun);
    server.on("/stop", HTTP_POST, handleStop);
    server.on("/poll", handlePoll);
    server.begin();
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
}

void loop() {
    server.handleClient();
    webSocket.loop();
    if (run_requested) {
        run_requested = false;
        delay(50);
        run_lua(lua_code_pending);
    }
}