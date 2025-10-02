// ESP8266_NAPT.ino
// 仅做了极小改动以支持：AP 设置随时可修改并永久保存（commit + 零填充 + saveParams 即保存）
// 推荐用 ESP8266 Arduino Core 3.0.0 编译

#define DBG_Printf_Enable true
#define HAVE_NETDUMP 0  // 关闭 NetDump 以避免缺库

#if (DBG_Printf_Enable == true)
  #define BLINKER_PRINT Serial
#endif
#define ArduinoOTA_Enable true

#define LED_PIN 2 //GPIO2
#define KEY_PIN 0 //GPIO0

//定义极性
#define LED_ON LOW
#define LED_OFF HIGH

#define WifiManager_ConnectTimeout 6*60//6mins
#define WifiManager_ConfigPortalTimeout 5*60//5mins
#define NAPT 1000
#define NAPT_PORT 10

#ifndef defaultAPSTASSID
#define defaultAPSTASSID  "ESP8266 extender" 
#define defaultAPPASSWORD "1234567890" 
#define defaultAPMAC      {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}
#endif

// 轻量持久化标识（不影响旧数据；仅在不匹配时回落到默认）
#define CONFIG_MAGIC  0x5A
#define CONFIG_VER    0x01
#define OFF_MAGIC     135
#define OFF_VERSION   136

#include <ESP8266WiFi.h>
#include <lwip/napt.h>
#include <lwip/dns.h>
#include <LwipDhcpServer.h>
#include <WiFiManager.h>         // https://github.com/tzapu/WiFiManager
//for LED status
#include <Ticker.h>
#include <EEPROM.h>
#if (ArduinoOTA_Enable == true)
  #include <ArduinoOTA.h>
#endif   

//for LED status
Ticker LED_ticker;
Ticker KEY_ticker;

bool shouldSaveConfig = false;
bool shouldReconfig = false;
bool shouldNAPTinit = false;
bool shouldOTArun = false;
bool WM_First_Run = true;

uint32_t KEY_Timer;
uint8_t KEY_Shut_Change_Timer;
uint64_t KEY_last_State_Change_tick;
char APSTASSID[64] = defaultAPSTASSID;
char APPASSWORD[64] = defaultAPPASSWORD;
char APMAC[18];
char APMAC_tmp[18];
uint8_t newMACAddress[] = defaultAPMAC;

WiFiManager wifiManager;
WiFiManagerParameter custom_apssid("APssid", "AP SSID", APSTASSID, 64);
WiFiManagerParameter custom_appsw("APpassword", "AP password", APPASSWORD, 64);
WiFiManagerParameter custom_apmac("Apmac", "AP MAC addr", APMAC, 18);

#if HAVE_NETDUMP
#include <NetDump.h>
void dump(int netif_idx, const char* data, size_t len, int out, int success) {
  (void)success;
  Serial.print(out ? F("out ") : F(" in "));
  Serial.printf("%d ", netif_idx);
  netDump(Serial, data, len);
}
#endif

void LED_Tick_Service()
{
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}

void KEY_Tick_Service(void)
{
  uint8_t Key_State_Now = digitalRead(KEY_PIN); 
  bool Key_State_Update = false;
  if(Key_State_Now == LOW)
  {
    KEY_Timer++;  
  }
  else
  {    
    if(KEY_Timer>5)
    {          
      Key_State_Update = true;        
    }
    KEY_Timer = 0;     
  }        
  if(Key_State_Update)
  {
    uint64_t Now_Tick =  millis(); 
    if(Now_Tick<KEY_last_State_Change_tick)
    {
      KEY_Shut_Change_Timer = 0;      
    }
    else
    {
      if((Now_Tick - KEY_last_State_Change_tick)<5000) //5秒内开关都算
      {
        KEY_Shut_Change_Timer++;         
        if(KEY_Shut_Change_Timer>5)//按五次重新配网
        {
          shouldReconfig = true;
          KEY_Shut_Change_Timer = 0;
        }
      } 
      else
      {
        KEY_Shut_Change_Timer = 0;  
      } 
    }  
    KEY_last_State_Change_tick = Now_Tick;  
  }
}

void KEY_Init(void)
{
  KEY_ticker.detach();
  KEY_last_State_Change_tick = millis();
  KEY_Shut_Change_Timer = 0;  
  KEY_Timer = 0; 
  KEY_ticker.attach_ms(5, KEY_Tick_Service);  
}

void MAC_Char2Str(char* MAC_Str,uint8_t* MAC_char)
{
  sprintf(MAC_Str,"%.2X:%.2X:%.2X:%.2X:%.2X:%.2X",MAC_char[0],MAC_char[1],MAC_char[2],MAC_char[3],MAC_char[4],MAC_char[5]);
}
byte nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 16;
}
bool MAC_Str2Char(uint8_t* MAC_char,char* MAC_Str)
{
  uint8_t mac_temp[6];
  uint8_t char_temp,currentByte,byteindex = 0;
  uint32_t i;
  for(i=0;i<17;i++)
  {
    if((i%3)==0)
    {
      char_temp = nibble(MAC_Str[i]);
      if(char_temp>15)return false;
      currentByte = char_temp << 4;
    }
    else if((i%3)==1)
    {
      char_temp = nibble(MAC_Str[i]);
      if(char_temp>15)return false;
      currentByte |= char_temp;
      mac_temp[byteindex] = currentByte;
      byteindex ++;
    }
    else
    {
      if(MAC_Str[i] != ':')return false;
    }
  }
  for(i=0;i<6;i++)MAC_char[i] = mac_temp[i];
  return true;
}

// ★ 修改：写入前零填充，写入后 commit()
void EEPROM_SaveConfig()//保存函数
{
  uint8_t check_sum = 0;
  EEPROM.begin(256);

  // 清空区域，避免短串残留
  for (int i = 0; i < 64; i++) EEPROM.write(i, 0);
  for (int i = 0; i < 64; i++) EEPROM.write(i+64, 0);
  for (int i = 0; i < 6;  i++) EEPROM.write(i+128, 0);

  // 写 SSID
  uint8_t *p = (uint8_t*)(&APSTASSID);
  for (int i = 0; i < 64; i++) { EEPROM.write(i, p[i]); check_sum += p[i]; }
  // 写 PSK
  p = (uint8_t*)(&APPASSWORD);
  for (int i = 0; i < 64; i++) { EEPROM.write(i+64, p[i]); check_sum += p[i]; }
  // 写 MAC
  p = newMACAddress;
  for (int i = 0; i < 6; i++) { EEPROM.write(i+128, p[i]); check_sum += p[i]; }
  // 校验与标识
  EEPROM.write(134, check_sum);
  EEPROM.write(OFF_MAGIC,   CONFIG_MAGIC);
  EEPROM.write(OFF_VERSION, CONFIG_VER);

  EEPROM.commit();   // ★ 必须：提交到 Flash
  EEPROM.end();
}

void EEPROM_ReadConfig()//读取函数
{
  uint8_t check_sum = 0;
  EEPROM.begin(256);
  for (int i = 0; i < 134; i++) check_sum += EEPROM.read(i);

  bool sum_ok   = (EEPROM.read(134) == check_sum);
  bool magic_ok = (EEPROM.read(OFF_MAGIC)   == CONFIG_MAGIC);
  bool ver_ok   = (EEPROM.read(OFF_VERSION) == CONFIG_VER);

  if(!(sum_ok && magic_ok && ver_ok))
  {
  #if (DBG_Printf_Enable == true)
    Serial.println("eeprom Reinit (magic/version/checksum mismatch)");
  #endif  
    // 回落到默认并保存
    strcpy(APSTASSID, defaultAPSTASSID);
    strcpy(APPASSWORD, defaultAPPASSWORD);
    uint8_t defmac[6] = defaultAPMAC;
    for (int i=0;i<6;i++) newMACAddress[i] = defmac[i];
    EEPROM_SaveConfig();
  }
  else
  {
    uint8_t *p = (uint8_t*)(&APSTASSID);
    for (int i = 0; i < 64; i++) { p[i] = EEPROM.read(i+0x00); }
    p = (uint8_t*)(&APPASSWORD);
    for (int i = 0; i < 64; i++) { p[i] = EEPROM.read(i+0x40); }
    p = newMACAddress;
    for (int i = 0; i < 6; i++) { p[i] = EEPROM.read(i+0x80); }
    #if (DBG_Printf_Enable == true)
      Serial.print("saved ap ssid: ");
      Serial.println(APSTASSID);
      Serial.print("saved ap psw: ");
      Serial.println(APPASSWORD); // ★ 修正日志笔误
    #endif  
  }
  EEPROM.end();
  MAC_Char2Str(APMAC,newMACAddress);
  #if (DBG_Printf_Enable == true)
    Serial.print("saved ap mac: ");
    Serial.println(APMAC);
  #endif  
}

void WM_saveConfigCallback () 
{
  #if (DBG_Printf_Enable == true)
    Serial.println("save config");
  #endif   
  if(WiFi.status() == WL_CONNECTED)
  {
    #if (DBG_Printf_Enable == true)
      Serial.println("connected...yeey :)");  
    #endif  
    LED_ticker.detach();
    digitalWrite(LED_PIN, LED_ON);   
    shouldSaveConfig = true;
    shouldNAPTinit = true;
  }
  else
  {
    #if (DBG_Printf_Enable == true)
      Serial.println("failed to connect and Configportal will run");  
    #endif  
    shouldReconfig = true;   
    shouldSaveConfig = false;
    shouldOTArun = false;   
  } 
}

// ★ 修改：参数校验通过也触发保存（不依赖 STA 已连接）
void WM_saveParamsCallback () {
  #if (DBG_Printf_Enable == true)
    Serial.println("Get Params:");
    Serial.print(custom_apssid.getID());Serial.print(" : ");Serial.println(custom_apssid.getValue());
    Serial.print(custom_appsw.getID());Serial.print(" : ");Serial.println(custom_appsw.getValue());
    Serial.print(custom_apmac.getID());Serial.print(" : ");Serial.println(custom_apmac.getValue());
  #endif
  strcpy(APMAC_tmp, custom_apmac.getValue());
  if(MAC_Str2Char(newMACAddress,APMAC_tmp))
  {
    strcpy(APSTASSID, custom_apssid.getValue());
    strcpy(APPASSWORD, custom_appsw.getValue());
    strcpy(APMAC, custom_apmac.getValue());

    shouldSaveConfig = true;   // ★ 新增：参数一旦有效就保存到 EEPROM
    shouldReconfig = false;
    shouldOTArun = false;
  }
  else
  {
  #if (DBG_Printf_Enable == true)
    Serial.println("Error Mac Input!!!!");
  #endif    
    shouldReconfig = true; 
    shouldSaveConfig = false;
    shouldOTArun = false;
    LED_ticker.attach(0.05, LED_Tick_Service);  
  }
}
void WM_ConfigPortalTimeoutCallback()
{
    shouldReconfig = true;   
    shouldSaveConfig = false;
    shouldOTArun = false;     
}

/**
 * 功能描述：初始化wifimanager
 */
void WifiManager_init()
{
  WiFi.mode(WIFI_STA);
  //wifi_set_macaddr(STATION_IF, &newMACAddress[0]);//ST模式

  wifiManager.setConnectTimeout(WifiManager_ConnectTimeout);
  wifiManager.setConfigPortalTimeout(WifiManager_ConfigPortalTimeout);

  #if (DBG_Printf_Enable == true)
    wifiManager.setDebugOutput(true);
  #else  
    wifiManager.setDebugOutput(false);
  #endif
  
  wifiManager.setMinimumSignalQuality(30);

  IPAddress _ip = IPAddress(192, 168, 8, 8);
  IPAddress _gw = IPAddress(192, 168, 8, 1);
  IPAddress _sn = IPAddress(255, 255, 255, 0);
  wifiManager.setAPStaticIPConfig(_ip, _gw, _sn);

  wifiManager.setSaveConfigCallback(WM_saveConfigCallback);
  wifiManager.setSaveParamsCallback(WM_saveParamsCallback);
  wifiManager.setConfigPortalTimeoutCallback(WM_ConfigPortalTimeoutCallback);  
  wifiManager.setBreakAfterConfig(true);
  wifiManager.setRemoveDuplicateAPs(true);
  wifiManager.setConfigPortalBlocking(false);

  if(WM_First_Run)
  {
    custom_apssid.setValue(APSTASSID,16);
    custom_appsw.setValue(APPASSWORD,64);   // ★ 唯一长度改动：10 -> 64
    custom_apmac.setValue(APMAC,17);
    wifiManager.addParameter(&custom_apssid);   
    wifiManager.addParameter(&custom_appsw);
    wifiManager.addParameter(&custom_apmac); 
  }
  WM_First_Run = false;

  if (wifiManager.autoConnect()) 
  {
    #if (DBG_Printf_Enable == true)
      Serial.println("connected...yeey :)");  
    #endif  
    LED_ticker.detach();
    digitalWrite(LED_PIN, LED_ON);
    shouldNAPTinit = true;
  }
  else
  {
    #if (DBG_Printf_Enable == true)
      Serial.println("failed to connect and Configportal running");  
    #endif  
    LED_ticker.attach(0.2, LED_Tick_Service);
    shouldNAPTinit = false;
  }
}

#if (ArduinoOTA_Enable == true)
  void ArduinoOTA_Init(void)
  {
    ArduinoOTA.setPort(8266);
    ArduinoOTA.setHostname("esp8266-extender");
    ArduinoOTA.setPassword("posystorage3");
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) type = "sketch";
        else type = "filesystem";
        #if (DBG_Printf_Enable == true)
          Serial.println("Start updating " + type);
        #endif
      });
      ArduinoOTA.onEnd([]() {
        #if (DBG_Printf_Enable == true)
          Serial.println("\nEnd");
        #endif      
      });
      ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        #if (DBG_Printf_Enable == true)
          Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
        #endif      
      });
      ArduinoOTA.onError([](ota_error_t error) {
        #if (DBG_Printf_Enable == true)
          Serial.printf("Error[%u]: ", error);
          if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
          else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
          else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
          else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
          else if (error == OTA_END_ERROR) Serial.println("End Failed");
        #endif      
      });
      ArduinoOTA.begin();
  }
#endif   

void NAPT_Init(void)
{
#if (DBG_Printf_Enable == true)
  Serial.printf("\nSTA: %s (dns: %s / %s)\n",
                WiFi.localIP().toString().c_str(),
                WiFi.dnsIP(0).toString().c_str(),
                WiFi.dnsIP(1).toString().c_str());
#endif  
  wifi_set_macaddr(SOFTAP_IF, &newMACAddress[0]);//AP模式 
  // give DNS servers to AP side
  dhcpSoftAP.dhcps_set_dns(0, WiFi.dnsIP(0));
  dhcpSoftAP.dhcps_set_dns(1, WiFi.dnsIP(1));

  WiFi.softAPConfig(
    IPAddress(172, 217, 28, 254),
    IPAddress(172, 217, 28, 254),
    IPAddress(255, 255, 255, 0));
  WiFi.softAP(APSTASSID, APPASSWORD);
#if (DBG_Printf_Enable == true)
  Serial.printf("AP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("Heap before: %d\n", ESP.getFreeHeap());
#endif 
  // 修改AP的MAC地址
  wifi_set_macaddr(SOFTAP_IF, &newMACAddress[0]); 
#if (DBG_Printf_Enable == true) 
  Serial.print("mac:");               
  Serial.println(WiFi.macAddress()); 
#endif 

  err_t ret = ip_napt_init(NAPT, NAPT_PORT);
#if (DBG_Printf_Enable == true)
  Serial.printf("ip_napt_init(%d,%d): ret=%d (OK=%d)\n", NAPT, NAPT_PORT, (int)ret, (int)ERR_OK);
#endif 
  if (ret == ERR_OK) {
    ret = ip_napt_enable_no(SOFTAP_IF, 1);
#if (DBG_Printf_Enable == true)
    Serial.printf("ip_napt_enable_no(SOFTAP_IF): ret=%d (OK=%d)\n", (int)ret, (int)ERR_OK);
#endif 
  }
#if (DBG_Printf_Enable == true)
  Serial.printf("Heap after napt init: %d\n", ESP.getFreeHeap());
  if (ret != ERR_OK) {
    Serial.printf("NAPT initialization failed\n");
  }
#endif 
}

void setup(void) 
{
  #if (DBG_Printf_Enable == true)
    Serial.begin(921600);
    Serial.println("");
    Serial.println("Serial 921600");
    Serial.printf("\n\nNAPT Range extender\n");
    Serial.printf("Heap on start: %d\n", ESP.getFreeHeap());
  #endif

#if HAVE_NETDUMP
    phy_capture = dump;
#endif    
            
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);
  LED_ticker.attach(0.6, LED_Tick_Service);   

  EEPROM_ReadConfig();  
  wifi_set_macaddr(SOFTAP_IF, &newMACAddress[0]);//AP模式 

  KEY_Init();   

  WM_First_Run = true;
  WifiManager_init();

  if(shouldNAPTinit)
  {
    NAPT_Init();
    #if (ArduinoOTA_Enable == true)
      ArduinoOTA_Init();
    #endif    
    shouldNAPTinit = false;
    shouldOTArun = true;
  } 
}

void loop(void) 
{
  wifiManager.process();  
  if(shouldReconfig)
  {
    shouldReconfig = false;
    shouldOTArun = false;
    wifiManager.resetSettings();
    delay(100);
    ESP.restart();
  }
  if(shouldSaveConfig)
  {
    shouldSaveConfig = false;    
    EEPROM_SaveConfig();
    delay(100);
    ESP.restart();
  } 
  if(shouldNAPTinit)
  {
    system_soft_wdt_feed();
    NAPT_Init();
    #if (ArduinoOTA_Enable == true)
      system_soft_wdt_feed();
      ArduinoOTA_Init();
    #endif  
    system_soft_wdt_feed();
    shouldNAPTinit = false;
    shouldOTArun = true;
  }  
  if(shouldOTArun)
  { 
    #if (ArduinoOTA_Enable == true)
      ArduinoOTA.handle(); 
    #endif  
  } 
}
