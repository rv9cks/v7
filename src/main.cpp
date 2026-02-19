#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

#ifdef OTA
#include <ArduinoOTA.h>
#endif

// Конфигурация
#define LEDCONFIRM
#define BEEP_DC

#ifdef BEEP_DC
#define BEEP_ON HIGH
#define BEEP_OFF LOW
#endif

#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <Ticker.h>

#ifndef STASSID
#define STASSID "dummynet1"
#define STAPSK "11223344"
#endif

// Конфигурационные константы
struct Config {
    static constexpr byte LED = D6;
    static constexpr byte INTERRUPT_PIN = D7;
    static constexpr byte TONE_PIN = D5;
    static constexpr int MAX_RECORDS = 512;
    static constexpr uint32_t SPEED_FADE = 5;
    static constexpr int MAXPW = 255;
    static constexpr unsigned long DEBOUNCE_INTERVAL = 20;
};

const char *ssid = STASSID;
const char *password = STAPSK;
const char *fname = "/0.txt";
const char *backup_fn = "/1.txt";
const String REDIR = "<html><BODY><head> <meta http-equiv=\"refresh\" content=\"2;URL=/\" /></head></BODY></HTML>";

// Структуры данных оптимизированные для ESP8266
struct tim {
    uint8_t H = 0;
    uint8_t M = 0;
    uint8_t S = 0;
    uint8_t SS = 0;
};

struct rec {
    uint16_t num = 999;  // Компактнее чем char[8]
    tim shot;
    
    void setNum(const String& numStr) {
        num = numStr.toInt();
    }
    
    String getNumStr() const {
        return String(num);
    }
};

// Оптимизированный кэш файлов
struct FileCache {
    String paths[6];
    String contents[6];
    uint8_t count = 0;
    
    String get(const String& path) {
        for(uint8_t i = 0; i < count; i++) {
            if(paths[i] == path) return contents[i];
        }
        return "";
    }
    
    void add(const String& path, const String& content) {
        if(count < 6) {
            paths[count] = path;
            contents[count] = content;
            count++;
        }
    }
};

// Кольцевой буфер для записей
class CircularBuffer {
private:
    rec buffer[Config::MAX_RECORDS];
    uint16_t head = 0;
    uint16_t tail = 0;
    uint16_t count = 0;
    
public:
    void push(const rec& record) {
        buffer[head] = record;
        head = (head + 1) % Config::MAX_RECORDS;
        if (count < Config::MAX_RECORDS) {
            count++;
        } else {
            tail = (tail + 1) % Config::MAX_RECORDS;
        }
    }
    
    rec& operator[](uint16_t index) {
        return buffer[(tail + index) % Config::MAX_RECORDS];
    }
    
    const rec& operator[](uint16_t index) const {
        return buffer[(tail + index) % Config::MAX_RECORDS];
    }
    
    uint16_t size() const { return count; }
    
    void clear() {
        head = tail = count = 0;
    }
};

// Глобальные переменные
CircularBuffer mainArray;
volatile bool rebootsys = false;
volatile bool need_tick_start = false;
volatile uint8_t key_register = 0xff;
volatile uint8_t prv_key_register = 0xff;
volatile bool key_pressed = false;
volatile int bri = Config::MAXPW;
volatile uint16_t main_index = 0;
volatile bool need_update = true;
volatile bool need_refresh = false;
uint32_t long_pressed = 0;
uint32_t lastFileUpdate = 0;
String writeBuffer;

Ticker keypolling;
Ticker tick;
Ticker ledblinker;
Ticker Beep_DC;

ESP8266WebServer server(80);
tim t;

// Оптимизированный кэш для статических файлов
FileCache fileCache;

// Прототипы функций
void flip();
void key_poll();
uint8_t debounce(uint8_t sample);
String getContentCached(const String& pth);
void create_backup();
void addRecord(const tim& shot, uint16_t num = 999);
void updateTextFileAsync();
void batchWriteToFile();
void printMemoryStats();
#ifdef BEEP_DC
void routinebeeper();
void startbeepdc();
void startbeepdc_inf();
void stopbeepdc_inf();
#endif

// ==================== ОПТИМИЗИРОВАННЫЕ ФУНКЦИИ ====================

// Простая и надёжная функция отсчёта времени
void flip() {
    t.SS++;
    if (t.SS > 99) {
        t.SS = 0;
        t.S++;
    }
    if (t.S > 59) {
        t.S = 0;
        t.M++;
    }
    if (t.M > 59) {
        t.M = 0;
        t.H++;
    }
}

#ifdef BEEP_DC
void routinebeeper() {
    digitalWrite(Config::TONE_PIN, BEEP_OFF);
    Beep_DC.detach();
}

void startbeepdc() {
    if (Beep_DC.active()) return;
    digitalWrite(Config::TONE_PIN, BEEP_ON);
    Beep_DC.once_ms(250, routinebeeper);
}

void startbeepdc_inf() {
    digitalWrite(Config::TONE_PIN, BEEP_ON);
}

void stopbeepdc_inf() {
    digitalWrite(Config::TONE_PIN, BEEP_OFF);
}
#endif

uint8_t debounce(uint8_t sample) {
    static uint8_t state, cnt0, cnt1;
    uint8_t delta, toggle;

    delta = sample ^ state;
    cnt1 = cnt1 ^ cnt0;
    cnt0 = ~cnt0;
    cnt0 &= delta;
    cnt1 &= delta;
    toggle = cnt0 & cnt1;
    state ^= toggle;

    return state;
}

void key_poll() {
    key_register = debounce(digitalRead(Config::INTERRUPT_PIN));

    if (prv_key_register != key_register) {
        prv_key_register = key_register;
        if (key_register == 0) {
            if (rebootsys) {
                rebootsys = false;
                #ifdef BEEP_DC
                stopbeepdc_inf();
                #endif
                need_tick_start = true;
            } else {
                key_pressed = true;
                #ifdef BEEP_DC
                startbeepdc();
                #endif
                
                // Добавляем запись с защитой от переполнения
                if (mainArray.size() < Config::MAX_RECORDS) {
                    addRecord(t, 999);
                } else {
                    Serial.println(F("ERROR: Record buffer full!"));
                }
            }
        }
    }
}

String getContentCached(const String& pth) {
    String cached = fileCache.get(pth);
    if (cached.length() > 0) {
        return cached;
    }
    
    File file = LittleFS.open(pth, "r");
    if (file) {
        String content = file.readString();
        file.close();
        fileCache.add(pth, content);
        Serial.println(F("Cached: ") + pth);
        return content;
    } else {
        Serial.println(F("ERROR: Failed to cache ") + pth);
        return "";
    }
}

void create_backup() {
    Serial.println(F("Create backup file"));
    LittleFS.remove("/backup.txt");
    if (!LittleFS.rename(fname, "/backup.txt")) {
        Serial.println(F("*** Error rename main file to backup "));
    }
}

void addRecord(const tim& shot, uint16_t num) {
    rec newRec;
    newRec.shot = shot;
    newRec.num = num;
    
    mainArray.push(newRec);
    main_index = mainArray.size();
    need_update = true;
}

// Асинхронное обновление файла с батчингом
void updateTextFileAsync() {
    const uint32_t UPDATE_INTERVAL = 5000; // 5 секунд
    
    if (need_update && (millis() - lastFileUpdate > UPDATE_INTERVAL)) {
        batchWriteToFile();
        lastFileUpdate = millis();
    }
}

void batchWriteToFile() {
    writeBuffer.reserve(1024);
    writeBuffer = "";
    
    for (uint16_t i = 0; i < mainArray.size(); i++) {
        const rec& record = mainArray[i];
        writeBuffer += String(record.num) + "\t";
        writeBuffer += String(record.shot.H) + ":" + String(record.shot.M) + ":" + 
                      String(record.shot.S) + "," + String(record.shot.SS) + "\r\n";
    }
    
    File file = LittleFS.open(fname, "w");
    if (file) {
        file.print(writeBuffer);
        file.close();
        Serial.println(F("Text file updated"));
        need_update = false;
    } else {
        Serial.println(F("ERROR: Failed to update text file"));
    }
}

// Мониторинг памяти
void printMemoryStats() {
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 30000) { // Каждые 30 секунд
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t maxBlock = ESP.getMaxFreeBlockSize();
        Serial.printf("Free heap: %u, Max block: %u, Fragmentation: %u%%\n", 
                     freeHeap, maxBlock,
                     100 - (maxBlock * 100 / freeHeap));
        lastPrint = millis();
    }
}

// ==================== HTML HANDLERS ====================

// ==================== ОПТИМИЗИРОВАННЫЕ HTML HANDLERS ====================

void handleRoot() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Отправляем заголовок
    server.sendContent(getContentCached("/root1.html"));
    
    // Отправляем записи порциями по 10 для экономии памяти
    const uint8_t CHUNK_SIZE = 10;
    char buffer[256];
    
    for (uint16_t i = 0; i < mainArray.size(); i += CHUNK_SIZE) {
        String chunk = "";
        chunk.reserve(512);
        
        for (uint8_t j = 0; j < CHUNK_SIZE && (i + j) < mainArray.size(); j++) {
            const rec& record = mainArray[i + j];
            
            snprintf(buffer, sizeof(buffer),
                "<tr><td class=\"td1\">%u</td><td class=\"td2\">"
                "<a href=/edit?rec=%u>%u %02u:%02u:%02u,%02u</a></td></tr>",
                i + j, i + j, record.num,
                record.shot.H, record.shot.M, record.shot.S, record.shot.SS);
            chunk += buffer;
        }
        
        server.sendContent(chunk);
        yield(); // Даем время другим задачам
    }
    
    server.sendContent("</table>");
    
    server.sendContent("<br><br><a href=\"/utils.html\">Utils</a> | ");
    server.sendContent("<a href=\"/fsedit\">Full Edit</a> | ");
    server.sendContent("<a href=\"/downfile\" download>Download</a>");
    
    // Информация о системе
    snprintf(buffer, sizeof(buffer),
        "<br><br>Current time: %02u:%02u:%02u,%02u<br>"
        "Records: %u | Free heap: %u",
        t.H, t.M, t.S, t.SS,
        mainArray.size(), ESP.getFreeHeap());
    server.sendContent(buffer);
    
    // Закрывающие теги
    server.sendContent(getContentCached("/root2.html"));
    server.sendContent(""); // Завершаем chunked transfer
}

void handleFSEdit() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    server.sendContent(getContentCached("/root1fs.html"));
    
    char buffer[256];
    const uint8_t CHUNK_SIZE = 8;
    
    for (uint16_t i = 0; i < mainArray.size(); i += CHUNK_SIZE) {
        String chunk = "";
        chunk.reserve(512);
        
        for (uint8_t j = 0; j < CHUNK_SIZE && (i + j) < mainArray.size(); j++) {
            const rec& record = mainArray[i + j];
            const char* tdClass = (record.num == 999) ? "td3" : "td2";
            
            snprintf(buffer, sizeof(buffer),
                "<tr><td class=\"td1\">%u</td><td class=\"%s\">"
                "<input type=\"tel\" maxlength=\"5\" name=\"nm%u\" value=\"%u\" onfocus=\"this.value='';\">"
                "%02u:%02u:%02u,%02u</td></tr>",
                i + j, tdClass, i + j, record.num,
                record.shot.H, record.shot.M, record.shot.S, record.shot.SS);
            chunk += buffer;
        }
        
        server.sendContent(chunk);
        yield();
    }
    
    server.sendContent("</table><br>");
    server.sendContent("<button type=\"submit\" name=\"submit\" value=\"1\" class=\"button-on\">SUBMIT</button>");
    server.sendContent("<button type=\"submit\" name=\"reset\" value=\"1\" class=\"button-off\">CANCEL</button>");
    server.sendContent("</form></div></body></html>");
    server.sendContent("");
}

void handleEdit() {
    String s;
    if (server.arg("rec") != "") {
        s = server.arg("rec");
        Serial.printf("Rec = %s", s.c_str());
    }
    server.send(200, "text/html", getContentCached("/edit1.html") + s + getContentCached("/edit2.html"));
}

void handleReplace() {
    if (server.arg("reset") == "1") {
        Serial.println(F("replace: Cancel update line"));
        server.send(200, "text/html", REDIR);
        return;
    }

    String rec_param = server.arg("rec");
    Serial.println(F("replace: received rec = ") + rec_param);

    String new_number = server.arg("number");
    if (new_number == "") new_number = "999";
    Serial.println(F("replace: new_number ") + new_number);

    if (rec_param != "") {
        int line = rec_param.toInt();
        if (line >= 0 && line < (int)mainArray.size()) {
            mainArray[line].num = new_number.toInt();
            need_update = true;
            need_refresh = true;
        }
    }
    server.send(200, "text/html", REDIR);
}

void handleRestart() {
    if (server.arg("reset") == "1") {
        Serial.println(F("confirm-reply: Cancel confirm"));
        server.send(200, "text/html", REDIR);
        return;
    }

    tick.detach();
    t.H = 0; t.M = 0; t.S = 0; t.SS = 0;
    rebootsys = true;
    need_update = true;
    need_refresh = true;
    #ifdef BEEP_DC
    startbeepdc_inf();
    #endif
    server.send(200, "text/html", REDIR);
}

void handleFSEditProcess() {
    if (server.hasArg("reset")) {
        Serial.println(F("fsedit cancelled"));
        need_refresh = true;
        return;
    }

    bool changed = false;
    for (int i = 0; i < server.args(); i++) {
        String ar = server.argName(i);
        if (ar.indexOf("nm") != -1) {
            int k = ar.substring(2).toInt();
            if (k >= 0 && k < (int)mainArray.size() && !server.arg(ar).isEmpty()) {
                uint16_t new_value = server.arg(ar).toInt();
                if (mainArray[k].num != new_value) {
                    mainArray[k].num = new_value;
                    changed = true;
                }
            }
        }
    }
    
    if (changed) {
        need_update = true;
    }
    need_refresh = true;
}

void handleConfirmTr() {
    if (server.arg("reset") == "1") {
        Serial.println(F("confirm-reply: Cancel confirm"));
        server.send(200, "text/html", REDIR);
        return;
    }
    create_backup();
    mainArray.clear();
    main_index = 0;
    need_update = true;
    need_refresh = true;
    server.send(200, "text/html", REDIR);
}

// ==================== SETUP ====================

void setup() {
    Serial.begin(115200);
    delay(3000);
    Serial.println(F("Booting"));
    Serial.print(F("Version at  /home/vs/wrk/from_probook/wrk-1/esp-altCross-www-nodemcu2-vs-v7-deepseek "));
    Serial.print(__TIME__);
    Serial.print(F(" "));
    Serial.println(__DATE__);
    
    WiFi.softAP(ssid, password);
    Serial.print(F("Access Point \""));
    Serial.print(ssid);
    Serial.println(F("\" started"));
    Serial.print(F("IP address:\t"));
    Serial.println(WiFi.softAPIP());
    delay(3000);

#ifdef OTA
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else {
            type = "filesystem";
        }
        Serial.println("Start updating " + type);
    });

    ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
#endif

    server.on("/", handleRoot);
    server.on("/fsedit", handleFSEdit);
    server.on("/edit", handleEdit);
    server.on("/replace", handleReplace);
    server.on("/restart", handleRestart);
    server.on("/recreate", []() { need_refresh = true; });
    server.on("/fseditprocess", handleFSEditProcess);
    server.on("/confirm-tr", handleConfirmTr);

    // Статические файлы (только для утилит и загрузки)
    server.serveStatic("/downfile", LittleFS, fname);
    server.serveStatic("/backup.txt", LittleFS, "/backup.txt");
    server.serveStatic("/utils.html", LittleFS, "/utils.html");
    server.serveStatic("/confirmtr.html", LittleFS, "/confirmtr.html");
    server.serveStatic("/confirmrestart.html", LittleFS, "/confirmrestart.html");
    server.serveStatic("/quedit", LittleFS, "/qu1.html");
    server.serveStatic("/style.css", LittleFS, "/style.css");

    server.begin();
    Serial.println(F("HTTP server started"));

    if (!LittleFS.begin()) {
        Serial.println(F("An Error has occurred while mounting LittleFS"));
    }

    create_backup();
    tick.attach(0.01, flip); // 10ms интервал для точного времени
    
    pinMode(Config::INTERRUPT_PIN, INPUT_PULLUP);
    pinMode(Config::LED, OUTPUT);
    pinMode(Config::TONE_PIN, OUTPUT);
    digitalWrite(Config::LED, LOW);
    delay(1000);
    
    keypolling.attach_ms(Config::DEBOUNCE_INTERVAL, key_poll);

    // Предзагружаем статические шаблоны в кэш
    getContentCached("/style.css");
    getContentCached("/root1.html");
    getContentCached("/root2.html");
    getContentCached("/root1fs.html");
    getContentCached("/edit1.html");
    getContentCached("/edit2.html");
    
    Serial.println(F("System ready"));
}

// ==================== ОПТИМИЗИРОВАННЫЙ LOOP ====================

void loop() {
#ifdef OTA
    ArduinoOTA.handle();
#endif

    server.handleClient();
    
    if (need_tick_start) {
        tick.attach(0.01, flip);
        need_tick_start = false;
    }

    // Асинхронное обновление текстового файла
    updateTextFileAsync();
    
    // Обрабатываем необходимость обновления страницы
    if (need_refresh) {
        server.send(200, "text/html", REDIR);
        need_refresh = false;
    }
    
    // Мониторинг памяти
    printMemoryStats();
    
    yield(); // Даем время системе
}