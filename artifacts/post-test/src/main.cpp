// Locket POST probe - serial only, no display, no M5 libs.
// Verifies silicon, I2C devices, and both RF Pack radios on the exact
// StickS3 + RF Pack S3 wiring, then takes a few live passive samples.
// Throwaway diagnostic. Overwrites whatever firmware was on the device.

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

// ---- RF Pack S3 shared SPI bus (from Bruce m5stack-sticks3 board cfg) ----
static const int PIN_SCK = 5, PIN_MISO = 4, PIN_MOSI = 6;
static const int CC_CS = 2, CC_GDO0 = 3;   // CC1101
static const int NRF_CSN = 8, NRF_CE = 1;  // nRF24L01+

// ---- I2C buses ----
static const int SYS_SDA = 47, SYS_SCL = 48; // BMI270 0x68, ES8311 0x18, PMIC 0x6e
static const int GRV_SDA = 9,  GRV_SCL = 10; // Grove PORT.A

SPIClass rfspi(HSPI);

static void csHigh(int pin){ digitalWrite(pin, HIGH); }
static void csLow(int pin){ digitalWrite(pin, LOW); }

// ---------- CC1101 ----------
static uint8_t ccStrobe(uint8_t cmd){
  csLow(CC_CS);
  // wait for MISO (chip ready) to go low, with timeout
  uint32_t t0 = micros();
  while(digitalRead(PIN_MISO) && (micros()-t0) < 5000){}
  uint8_t s = rfspi.transfer(cmd);
  csHigh(CC_CS);
  return s;
}
static uint8_t ccReadStatus(uint8_t addr){
  // status regs (0x30-0x3D) require burst read bit: 0xC0 | addr
  csLow(CC_CS);
  uint32_t t0 = micros();
  while(digitalRead(PIN_MISO) && (micros()-t0) < 5000){}
  rfspi.transfer(0xC0 | addr);
  uint8_t v = rfspi.transfer(0x00);
  csHigh(CC_CS);
  return v;
}
static void ccProbe(){
  Serial.println(F("\n[CC1101] CS=2 GDO0=3 bus SCK5/MISO4/MOSI6"));
  rfspi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  // reset: toggle CS then SRES strobe
  csHigh(CC_CS); delayMicroseconds(30);
  csLow(CC_CS);  delayMicroseconds(30);
  csHigh(CC_CS); delayMicroseconds(45);
  ccStrobe(0x30); // SRES
  delay(1);
  uint8_t partnum = ccReadStatus(0x30); // PARTNUM  (expect 0x00)
  uint8_t version = ccReadStatus(0x31); // VERSION  (expect ~0x14)
  rfspi.endTransaction();
  Serial.printf("  PARTNUM=0x%02X  VERSION=0x%02X  -> %s\n",
    partnum, version,
    (version!=0x00 && version!=0xFF) ? "PRESENT/answers" : "NO ANSWER");
}
// read CC1101 RSSI status reg as a rough noise-floor sample (needs RX on).
static void ccRssiSample(){
  rfspi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  ccStrobe(0x34); // SRX -> enter RX
  delay(3);
  uint8_t rssiRaw = ccReadStatus(0x34); // RSSI
  ccStrobe(0x36); // SIDLE
  rfspi.endTransaction();
  int rssi_dbm = (rssiRaw >= 128 ? (rssiRaw-256) : rssiRaw)/2 - 74;
  Serial.printf("  CC1101 RSSI raw=0x%02X ~= %d dBm (default 433 cfg, rough)\n",
    rssiRaw, rssi_dbm);
}

// ---------- nRF24 ----------
static uint8_t nrfReadReg(uint8_t reg){
  csLow(NRF_CSN);
  rfspi.transfer(0x00 | (reg & 0x1F)); // R_REGISTER
  uint8_t v = rfspi.transfer(0xFF);
  csHigh(NRF_CSN);
  return v;
}
static void nrfWriteReg(uint8_t reg, uint8_t val){
  csLow(NRF_CSN);
  rfspi.transfer(0x20 | (reg & 0x1F)); // W_REGISTER
  rfspi.transfer(val);
  csHigh(NRF_CSN);
}
static void nrfProbe(){
  Serial.println(F("\n[nRF24] CSN=8 CE=1 bus SCK5/MISO4/MOSI6"));
  digitalWrite(NRF_CE, LOW);
  rfspi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  uint8_t before = nrfReadReg(0x05);        // RF_CH
  nrfWriteReg(0x05, 0x4C);                   // write 76
  uint8_t after = nrfReadReg(0x05);          // read back
  uint8_t config = nrfReadReg(0x00);         // CONFIG
  rfspi.endTransaction();
  Serial.printf("  CONFIG=0x%02X  RF_CH write0x4C read0x%02X -> %s\n",
    config, after, (after==0x4C) ? "PRESENT/answers" : "NO ANSWER");
  (void)before;
}
// Passive 2.4GHz energy sweep using nRF24 carrier-detect (RPD) per channel.
static void nrfEnergySweep(){
  rfspi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  nrfWriteReg(0x00, 0x0B); // PWR_UP, PRX
  int hits = 0; int peakCh = -1;
  Serial.print(F("  2.4GHz RPD sweep (ch with carrier): "));
  for(int ch=0; ch<=125; ch+=1){
    nrfWriteReg(0x05, ch);
    csHigh(NRF_CE); // CE high momentarily wrong pin state handled below
    digitalWrite(NRF_CE, HIGH);
    delayMicroseconds(300); // settle + listen
    digitalWrite(NRF_CE, LOW);
    uint8_t rpd = nrfReadReg(0x09) & 0x01; // RPD/CD
    if(rpd){ hits++; if(peakCh<0) peakCh=ch; Serial.printf("%d ", ch); }
  }
  nrfWriteReg(0x00, 0x09); // power down-ish (keep PWR_UP off)
  rfspi.endTransaction();
  Serial.printf("\n  RPD hits=%d firstCh=%d (>-64dBm carriers seen)\n", hits, peakCh);
}

// ---------- I2C ----------
static void i2cScan(const char* name, TwoWire &bus){
  Serial.printf("\n[I2C %s] scanning...\n", name);
  int found=0;
  for(uint8_t a=1; a<127; a++){
    bus.beginTransmission(a);
    if(bus.endTransmission()==0){
      const char* who = (a==0x18)?"ES8311 codec":(a==0x68)?"BMI270 IMU":
                        (a==0x6e)?"M5PM1 PMIC":"?";
      Serial.printf("  0x%02X  %s\n", a, who);
      found++;
    }
  }
  if(!found) Serial.println("  (none)");
}

static void wifiSample(){
  Serial.println(F("\n[WiFi] passive AP scan (2-3s)..."));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  int n = WiFi.scanNetworks(false, true);
  Serial.printf("  APs seen: %d\n", n);
  for(int i=0;i<n && i<8;i++){
    Serial.printf("    %-24s ch%2d %4d dBm %s\n",
      WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
      WiFi.BSSIDstr(i).c_str());
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
}

void setup(){
  Serial.begin(115200);
  delay(1500);
  Serial.println(F("\n=================================================="));
  Serial.println(F(" LOCKET POST  -  StickS3 + RF Pack S3 probe"));
  Serial.println(F("=================================================="));

  Serial.printf("Chip: %s rev %d, %d cores @ %d MHz\n",
    ESP.getChipModel(), ESP.getChipRevision(),
    ESP.getChipCores(), getCpuFrequencyMhz());
  Serial.printf("Flash: %u MB\n", ESP.getFlashChipSize()/(1024*1024));
  size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  Serial.printf("PSRAM total: %u bytes (%u MB)\n",
    (unsigned)psram, (unsigned)(psram/(1024*1024)));
  Serial.printf("Free heap: %u  Free PSRAM: %u\n",
    (unsigned)ESP.getFreeHeap(),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  uint8_t mac[6]; WiFi.macAddress(mac);
  Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

  // CS lines idle high, CE idle low
  pinMode(CC_CS, OUTPUT);  csHigh(CC_CS);
  pinMode(NRF_CSN, OUTPUT); csHigh(NRF_CSN);
  pinMode(NRF_CE, OUTPUT);  digitalWrite(NRF_CE, LOW);
  pinMode(CC_GDO0, INPUT);
  rfspi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  // I2C buses
  Wire.begin(SYS_SDA, SYS_SCL, 100000);
  i2cScan("SYS 47/48", Wire);
  Wire1.begin(GRV_SDA, GRV_SCL, 100000);
  i2cScan("GROVE 9/10", Wire1);

  // Radios
  ccProbe();
  ccRssiSample();
  nrfProbe();
  nrfEnergySweep();

  // Live passive sample
  wifiSample();

  Serial.println(F("\n[POST DONE] looping heap watch every 5s."));
}

void loop(){
  static uint32_t last=0;
  if(millis()-last > 5000){
    last=millis();
    Serial.printf("[alive] free heap=%u  psram=%u\n",
      (unsigned)ESP.getFreeHeap(),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
}
