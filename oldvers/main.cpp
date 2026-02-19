#include <Arduino.h>
#include <ESP8266WiFi.h>
// #include <ESP8266mDNS.h>
#include <WiFiUdp.h>

#ifdef OTA
#include <ArduinoOTA.h>
#endif

//   FADELED - включение плавного затухания LED после нажатия кнопки
//   несовместимо с LEDCONFIRM
// #define FADELED
//   LEDCONFIRM - включение мигания LED при запросе корневой страницы
//  несовместимо с  FADELED
#define LEDCONFIRM
// #define BEEP_TONE     //  если пищалка на переменном токе
#define BEEP_DC    // если пищалка на постоянном токе

#ifdef BEEP_DC
#define BEEP_ON HIGH
#define BEEP_OFF LOW
#endif

#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <Ticker.h>
#ifndef STASSID
// #define STASSID "dummynet0"
#define STASSID "dummynet1"
#define STAPSK "11223344"
#endif
//
// ! pio device monitor --port /dev/ttyUSB0 --baud 115200
// ! pio project init --ide vim --board nodemcuv2

const byte LED = D6;
const byte interruptPin = D7;
const byte tonePin = D5;
const char *ssid = STASSID;
const char *password = STAPSK;
const int MAXPW = 255;
const uint32_t SPEED_FADE = 5; // ms
const char *fname = "/0.txt";
const char *backup_fn = "/1.txt";
const String REDIR = "<html><BODY><head> <meta http-equiv=\"refresh\" "
                     "content=\"2;URL=/\" /></head></BODY></HTML>";

struct tim
{
  unsigned char H = 0;
  unsigned char M = 0;
  unsigned char S = 0;
  unsigned char SS = 0;
};

tim t;

struct rec
{
  String num = "999";
  int session;
  tim shot;
};

// rec mainArray[256];
rec mainArray[1024];

Ticker keypolling;
Ticker tick;
Ticker ledblinker;
Ticker Beep_DC;

// volatile unsigned int i_real = 0;
volatile boolean rebootsys = false;
volatile boolean need_tick_start = false;
volatile int current_view_session = -1;
volatile int current_session = 0;
volatile unsigned char key_register = 0xff;
volatile unsigned char prv_key_register = 0xff;
volatile boolean key_pressed = false;
volatile int bri = MAXPW;
volatile unsigned int main_index = 0;
volatile boolean need_update = true;
volatile boolean need_refresh = false;
unsigned int long_pressed = 0;

ESP8266WebServer server(80);

void flip()
{
  t.SS++;
  if (t.SS > 99)
  {
    t.SS = 0;
    t.S++;
  }
  if (t.S > 59)
  {
    t.S = 0;
    t.M++;
  }
  if (t.M > 59)
  {
    t.M = 0;
    t.H++;
  }
}

#ifdef FADELED
void routineledfade()
{
  analogWrite(LED, bri);
  if (--bri <= 0)
  {
    bri = MAXPW;
    ledblinker.detach();
    analogWrite(LED, 0);
  }
}
void startfadeled()
{

  if (ledblinker.active())
  {
    bri = MAXPW;
    return;
  }
  ledblinker.attach_ms(SPEED_FADE, routineledfade);
}
#endif

#ifdef BEEP_DC
void routinebeeper()
{
  digitalWrite(tonePin, BEEP_OFF);
  Beep_DC.detach();
}

void startbeepdc()
{
  if (Beep_DC.active())
  {
    return;
  }

  digitalWrite(tonePin, BEEP_ON);
  // Beep_DC.attach_ms(250,routinebeeper);
  Beep_DC.once_ms(250, routinebeeper);
}
void startbeepdc_inf()
{
  digitalWrite(tonePin, BEEP_ON);
}
void stopbeepdc_inf()
{
  digitalWrite(tonePin, BEEP_OFF);
}

#endif

unsigned char debounce(unsigned char sample)
{
  static unsigned char state, cnt0, cnt1;
  unsigned char delta, toggle;

  delta = sample ^ state;

  cnt1 = cnt1 ^ cnt0;
  cnt0 = ~cnt0;

  cnt0 &= delta;
  cnt1 &= delta;

  toggle = cnt0 & cnt1;
  state ^= toggle;

  return state;
}
void key_poll()
{

  key_register = debounce(digitalRead(interruptPin));

  // Serial.printf("\n\r *** %X  %u",key_register,long_pressed);

  if (prv_key_register != key_register)
  {
    // Serial.printf("\n\r *** %X",key_register);
    prv_key_register = key_register;
    if (key_register == 0)
    {
      // Serial.printf("\n\r*** %d:%d:%d %d",t.H,t.M,t.S,t.SS);
      if (rebootsys)
      {
        rebootsys = false; // restart timer
        #ifdef BEEP_TONE
        noTone(tonePin);
        #endif
        #ifdef BEEP_DC
        stopbeepdc_inf();
        #endif
        // tick.attach(0.01, flip);
        need_tick_start = true;
      }
      else
      {
        key_pressed = true;
        // chain[i_real] = t;
#ifdef BEEP_TONE
        tone(tonePin, 1500, 250);
#endif
#ifdef BEEP_DC
        startbeepdc();
#endif
#ifdef FADELED
        startfadeled();
#endif
        // File file = LittleFS.open(fname, "a+");
        // char str[30];
        //  sprintf (str, "%u\t%02u:%02u:%02u,%02u", i_real,
        //  chain[i_real].H,chain[i_real].M,chain[i_real].S,chain[i_real].SS);
        //  sprintf(str, "%u\t%02u:%02u:%02u,%02u", i_real, t.H, t.M, t.S,
        //  t.SS);
        // sprintf(str, "999\t%02u:%02u:%02u,%02u", t.H, t.M, t.S, t.SS);
        // file.println(str);
        // file.close();
        mainArray[main_index].shot = t;
        mainArray[main_index].num = "999";
        mainArray[main_index].session = current_session;
        main_index++;
        need_update = true;
        Serial.print("Current session = ");
        Serial.println(current_session);
        // тут преверим выход за границу массива
      }
    }
  }
}

String getContent(String pth)
{
  File tmp = LittleFS.open(pth, "r");
  String ptr1_ = tmp.readString();
  tmp.close();
  return ptr1_;
}

void create_backup()
{
  Serial.println(F("Create backup file"));
  if (!LittleFS.remove("/backup.txt"))
  {
    Serial.println(F("*** /backup.txt not deleted"));
  }
  if (!LittleFS.rename(fname, "/backup.txt"))
  {
    Serial.println(F("*** Error rename main file to backup "));
  }
}

void setup()
{
  // start v2
  // git remote add origin ssh://vs@10.0.2.2:/volume1/public/git/esp-alt-timer.git
  // git push origin v2
  Serial.begin(115200);
  delay(3000);
  Serial.println("Booting");
  Serial.print("Version at ");
  Serial.print(__TIME__);
  Serial.print(" ");
  Serial.println(__DATE__);
  WiFi.softAP(ssid, password); // Start the access point
  Serial.print("Access Point \"");
  Serial.print(ssid);
  Serial.println("\" started");
  Serial.print("password: ");
  Serial.println(password);
  Serial.print("IP address:\t");
  Serial.println(
      WiFi.softAPIP()); // Send the IP address of the ESP8266 to the computer
  delay(3000);

  current_view_session = current_session;

// ArduinoOTA {{{
#ifdef OTA
  ArduinoOTA.onStart([]()

                     {
                       String type;
                       if (ArduinoOTA.getCommand() == U_FLASH) {
                         type = "sketch";
                       } else { // U_FS
                         type = "filesystem";
                       }
                       // NOTE: if updating FS this would be the place to
                       // unmount FS using FS.end()
                       Serial.println("Start updating " + type); });

  ArduinoOTA.onEnd([]()
                   { Serial.println("\nEnd"); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    } });
  // }}}

  ArduinoOTA.begin();
#endif

  server.on("/edit", []()
            {
    String s;
    if (server.arg("rec") == "") {
    } else {
      s = server.arg("rec");
      Serial.printf("Rec = %s", s.c_str());
    }
    server.send(200, "text/html",
                getContent("/edit1.html") + s + getContent("edit2.html")); });

  // #define DEBUG1

  server.on("/replace", []()
            {

#ifdef DEBUG1
    String message = "args received :";
    message += server.args();
    for (int i = 0; i < server.args(); i++) {

      message += "Arg " + (String)i + " –> ";
      message += server.argName(i) + ": ";
      message += server.arg(i) + "\n";
    }
    Serial.println(message);
#endif

    if (server.arg("reset") == "1") {
      // нажали кнопку отказа от сохранения
      Serial.println("replace: Cancel update line");
      server.send(200, "text/html", REDIR);
      return;
    }

    String rec = server.arg("rec");
    Serial.println("replace: received rec = " + rec);

    String new_number = server.arg("number");
    if (new_number == "")
      new_number = "999";
    Serial.println("replace: new_number " + new_number);

    if (rec != "") {
      int line = atoi(rec.c_str());
      mainArray[line].num = new_number;
      need_update = true;
    }
    server.send(200, "text/html", REDIR); });

  server.on("/restart", []()
            {
              Serial.printf("arg reset= %s\n", server.arg("reset").c_str());
              Serial.printf("arg password= %s\n", server.arg("password").c_str());

              if (server.arg("reset") == "1")
              {
                // нажали кнопку отказа от сохранения
                Serial.println("confirm-reply: Cancel confirm");
                server.send(200, "text/html", REDIR);
                return;
              }

              // const tim tm = {99,99,99,99};
              tick.detach(); // RESTART timer
              t.H = 0;
              t.M = 0;
              t.S = 0;
              t.SS = 0;
              rebootsys = true;
              need_update = true;
              need_refresh = true;
              current_session++;
              current_view_session = current_session;
              #ifdef BEEP_TONE
              tone(tonePin, 1000);
              #endif
              #ifdef BEEP_DC
              startbeepdc_inf();
              #endif
              // server.send(200, "text/html", REDIR);
            });

  server.on("/recreate", []()
            {
    need_update = true;
    // server.send(200, "text/html", REDIR);
    need_refresh = true; });

  server.on("/inc_session_number", []()
            {
    current_view_session++;
    if (current_view_session > current_session)  
            current_view_session = current_session;
    need_update = true;
    // server.send(200, "text/html", REDIR);
    need_refresh = true; });

  server.on("/dec_session_number", []()
            {
    current_view_session--;
    if (current_view_session <= 0 )  
            current_view_session = 0;
    need_update = true;
    // server.send(200, "text/html", REDIR);
    need_refresh = true; });

  server.on("/reset_session_number", []()
            {
    current_view_session=-1;
    need_update = true;
    // server.send(200, "text/html", REDIR);
    need_refresh = true; });

  server.on("/fseditprocess", []()
            {
    //String message = "args received :";
    //message += server.args();
    if (server.hasArg("reset")){
      //Serial.println(F("submit is true"));
      Serial.println(F("fsedit cancelled"));
      need_update = true;
      need_refresh = true;
      return;
    }

//         mainArray[line].num = new_number;
//    String new_number = server.arg("number");


    for (int i = 0; i < server.args(); i++) {

      //message += "Arg " + (String)i + " –> ";
      String ar = server.argName(i);

      if (ar.indexOf("nm") != -1 ) {
        //String a = ar.substring(0+2,-1);
        //Serial.println(a);
        int k = ar.substring(0+2,-1).toInt();  // индекс в массиве данных
        // String new_number = server.arg(ar);
        //mainArray[k].num = server.arg(ar);
        if (server.arg(ar).isEmpty()) {
          //Serial.printf("String is empty,index,skip replace = %u\n",k);
        } else {
          //Serial.printf("new value at index = %u\n",k);
          mainArray[k].num = server.arg(ar);
        }

        //Serial.printf("int = %u\n",k);
      }
      //message += server.argName(i) + ": ";
      //message += server.arg(i) + "\n";
      //String new_number = server.arg(i);
    }
    //Serial.println(message);
    need_update = true;
    need_refresh = true; });

  //  server.on("/reboot", []() {
  //    const String ptr = "<html><BODY><head> <meta http-equiv=\"refresh\" "
  //                       "content=\"2;URL=/\" /></head></BODY></HTML>";
  //    server.send(200, "text/html", ptr);
  //    delay(10000);
  //    ESP.restart();
  //  });

  //  server.on("/truncate", []() {
  //    create_backup();
  //    main_index = 0;
  //    need_update = true;
  //    need_refresh = true;
  //    // server.send(200, "text/html", REDIR);
  //  });

  server.on("/confirm-tr", []()
            {
              Serial.printf("arg reset= %s\n", server.arg("reset").c_str());
              Serial.printf("arg password= %s\n", server.arg("password").c_str());

              if (server.arg("reset") == "1")
              {
                // нажали кнопку отказа от сохранения
                Serial.println("confirm-reply: Cancel confirm");
                server.send(200, "text/html", REDIR);
                return;
              }
              create_backup();
              main_index = 0;
              need_update = true;
              need_refresh = true;
              // server.send(200, "text/html", REDIR);
            });

  server.serveStatic("/downfile", LittleFS, fname);
  server.serveStatic("/backup.txt", LittleFS, "/backup.txt");
  server.serveStatic("/utils.html", LittleFS, "/utils.html");
  server.serveStatic("/fsedit.html", LittleFS, "/fsedit.html");
  server.serveStatic("/confirmtr.html", LittleFS, "/confirmtr.html");
  server.serveStatic("/confirmrestart.html", LittleFS, "/confirmrestart.html");
  server.serveStatic("/quedit", LittleFS, "/qu1.html");
  server.serveStatic("/style.css", LittleFS, "/style.css");
  server.serveStatic("/", LittleFS,
                     "/root.html"); //  ставим последним, иначе не работает

  // ---------------------------------------------

  server.begin(); // Start server
  Serial.println(F("HTTP server started"));
  // FS init
  if (!LittleFS.begin())
  {
    Serial.println(F("An Error has occurred while mounting LittleFS"));
  }
  create_backup();
  tick.attach(0.01, flip);
  // keypolling.attach(0.2, key_poll);
  pinMode(interruptPin, INPUT_PULLUP); // key pin D2
  pinMode(LED, OUTPUT);
  pinMode(tonePin, OUTPUT);
  digitalWrite(LED, LOW);
  delay(1000);
  keypolling.attach(0.02, key_poll);
}

void loop()
{

#ifdef OTA
  ArduinoOTA.handle();
#endif

  server.handleClient(); // Handle client requests
  if (need_tick_start)
  {
    tick.attach(0.01, flip);
    need_tick_start = false;
  }

  if (need_update)
  {
    // out_file - форма показанная по умолчанию
    // fsedit_file - форма при полноэкранном редмктировании
    // file  - выгрузка данных в альткросс
    File out_file = LittleFS.open("/root.html", "w");
    File fsedit_file = LittleFS.open("/fsedit.html", "w");
    File file = LittleFS.open(fname, "w");

    // Serial.print(F("Need update true, main_index = "));
    // Serial.println(main_index);

    out_file.print(getContent("/root1.html"));
    fsedit_file.print(getContent("/root1fs.html"));

    // out_file.print(getContent("/filter_table1.html"));
    // out_file.print("<td><button class=\"button-on\" type=\"button\" onClick=\"Refresh()\"><label>-1</label></button></td>");
    // out_file.print(getContent("/filter_table2.html"));

    for (unsigned int i = 0; i < main_index; i++)
    {
      // вывод в HTML файл. его  фильтруем по номеру забега
      if (current_view_session == mainArray[i].session || current_view_session == -1)
      {

        out_file.printf("<tr><td class=\"td1\">%u/%u</td><td class=\"td2\"><a "
                        "href=/edit?rec=%u>%s %02u:%02u:%02u,%02u</a></td></tr>",
                        i, mainArray[i].session, i, mainArray[i].num.c_str(), mainArray[i].shot.H,
                        mainArray[i].shot.M, mainArray[i].shot.S,
                        mainArray[i].shot.SS);
        if (mainArray[i].num.equals("999"))
        {
          fsedit_file.printf("<tr><td class=\"td1\">%u/%u</td><td class=\"td3\">"
                             "<input type=\"tel\" maxlength=\"5\" name=\"nm%u\" value=\"%s\" onfocus=\"this.value='';\">"
                             "%02u:%02u:%02u,%02u</td></tr>",
                             i, mainArray[i].session, i, mainArray[i].num.c_str(), mainArray[i].shot.H,
                             mainArray[i].shot.M, mainArray[i].shot.S,
                             mainArray[i].shot.SS);
        }
        else
        {
          fsedit_file.printf("<tr><td class=\"td1\">%u/%u</td><td class=\"td2\">"
                             "<input type=\"tel\" maxlength=\"5\" name=\"nm%u\" value=\"%s\" onfocus=\"this.value='';\">"
                             "%02u:%02u:%02u,%02u</td></tr>",
                             i, mainArray[i].session, i, mainArray[i].num.c_str(), mainArray[i].shot.H,
                             mainArray[i].shot.M, mainArray[i].shot.S,
                             mainArray[i].shot.SS);
        }
      }
      // вывод в текстовый файл. его не фильтруем по номеру забега
      file.printf("%s\t%02u:%02u:%02u,%02u\r\n", mainArray[i].num.c_str(),
                  mainArray[i].shot.H, mainArray[i].shot.M, mainArray[i].shot.S,
                  mainArray[i].shot.SS);
    }
    out_file.print("</table>");
    out_file.print("<br><br><br><a  href=\"/utils.html\">restart</a>");
    out_file.printf("<br>Ct=%02u:%02u:%02u,%02u", t.H, t.M, t.S, t.SS);
    out_file.printf("<br>ESP.getFreeHeap()= %u",
                    ESP.getFreeHeap()); // returns the free heap size.
    out_file.print(getContent("/root2.html"));
    fsedit_file.print(getContent("/root2fs.html"));

    out_file.flush();
    fsedit_file.flush();
    file.flush();

    out_file.close();
    fsedit_file.close();
    file.close();

    need_update = false;
    if (need_refresh)
    {
      server.send(200, "text/html", REDIR);
      need_refresh = false;
    }
  }
}
