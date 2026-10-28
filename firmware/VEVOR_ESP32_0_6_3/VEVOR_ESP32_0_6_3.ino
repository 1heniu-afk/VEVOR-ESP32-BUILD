/*
 VEVOR ZM2003 / Heater5439 — ESP32 BLE ABBA, bez WiFi i serwera WWW.
 Sterownik ESP32 0.6.3: wspolne BLE MTU64, dwa DS18B20, termostat,
 START/STOP i poziomy H1-H6. Bez WiFi.
 WAZNE: komendy fizycznego sterownika wymagaja weryfikacji pod nadzorem.
*/
#include <Arduino.h>
#include <stdlib.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "BLEOTA.h"
#include "ThermostatCore.h"
#include "ABBAProtocol.h"
#include "AccessKey.h"
#include "TempSources.h"
#include "ControlGate.h"
#include "FrameMailbox.h"
#include "PendingPolicy.h"
#include "AutoStartSafety.h"
#include "QueuedAutoPolicy.h"
#include "CommandMailboxPolicy.h"
#include "StopOutcomePolicy.h"
#include "OtaSafety.h"
#include "OtaPublicKey.h"

static const uint8_t TEMP_PIN=18;
static const uint16_t HEATER_REQUEST_MTU=64; // 21 bajtow statusu ABBA wymaga MTU >= 24
static constexpr bool DIAGNOSTYKA_TYLKO_ODCZYT = false;
static const BLEUUID HEATER_SVC("0000fff0-0000-1000-8000-00805f9b34fb");
static const BLEUUID HEATER_NOTIFY("0000fff1-0000-1000-8000-00805f9b34fb");
static const BLEUUID HEATER_WRITE("0000fff2-0000-1000-8000-00805f9b34fb");
static const char* PHONE_SVC="d7b67a20-9eb5-4f13-9ea5-6bc59a0a9800";
static const char* PHONE_RX ="d7b67a21-9eb5-4f13-9ea5-6bc59a0a9800";
static const char* PHONE_TX ="d7b67a22-9eb5-4f13-9ea5-6bc59a0a9800";
static const uint32_t HEATER_FRESH_MS=9000, SEND_GAP_MS=700, TEMP_PERIOD=6000;
static const uint32_t MIN_RESTART_MS=10UL*60UL*1000UL; // 10 min; dodatkowy limit startow
static const uint32_t CMD_WAIT_MS=14000;
static const uint32_t CONFIRM_WAIT_MS=25000;
static const uint32_t BOOT_GRACE_MS=10UL*60UL*1000UL;

Preferences prefs;
OneWire ow(TEMP_PIN);
DallasTemperature probes(&ow);
ThermostatCore thermostat;
BLEOTAClass bleOta;
BLEServer* localServer=nullptr;
BLECharacteristic* phoneNotify=nullptr;
BLEClient* heaterClient=nullptr;
BLERemoteCharacteristic* heaterRx=nullptr;
BLERemoteCharacteristic* heaterTx=nullptr;
BLEAdvertisedDevice* candidate=nullptr;
volatile bool phoneConnected=false;
volatile bool phoneDisconnectedFlag=false;
volatile bool heaterConnected=false;
volatile bool discovered=false;
volatile bool disconnectFlag=false;
portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
FrameMailbox incomingFrames;
AbbaStatus heaterStatus;
uint32_t heaterTimestamp=0,lastPacketTime=0,lastCmdTime=0,lastTempTime=0,lastScan=0;
uint32_t lastOffTime=0,lastStartTime=0,lastNotificationTime=0;
uint32_t bootAt=0;
// Podstawowe statystyki sesji; licznik poprawnych ramek pozostaje diagnostyczny.
uint32_t heatingSeconds=0, startTransitions=0, lastStatUpdate=0, lastStatPersist=0;
uint32_t statusSamples=0;
float fuelEstimateMl=0;
uint8_t lastObservedState=255;
// Oszacowanie Hz dotyczy stabilnych biegow H1-H6, nie rozruchu / AUTO.
static const float HZ_ALPINE[6]={1.403f,1.80f,2.21f,2.52f,2.87f,3.11f};
static const float HZ_NORMAL[6]={1.93f,2.50f,3.10f,3.72f,4.31f,4.62f};
uint8_t beforeState=255,beforeLevel=0,beforeAlpine=255;
bool awaitingChange=false;
String lastAction="BRAK";
int targetGear=0;
uint32_t pendingSince=0;
char pendingOp=0;  // 'S' start, 'X' stop, 'U' up, 'D' down, 'A' alpine

uint32_t postOpPreviousPacket=0;
bool remoteEnabled=false, thermoEnabled=false, mappingValid=false;
bool pendingThermoStart=false; // START automatyczny: ponownie sprawdz temperature przed wyslaniem
bool pendingThermoStop=false; // STOP automatyczny moze byc anulowany przez THERMO:OFF
bool stopAfterPendingConfirmed=false;
// Nie wolno ukrywac niepotwierdzonego STOP po zniknieciu BLE.
bool otaSessionActive=false;
bool otaRejected=false;
bool stopOutcomeUncertain=false; // Obowiazuje do swiezo potwierdzonego OFF/COOLDOWN/STANDBY
bool selectedSensors[2]={false,false};
bool phoneUnlocked=false;
DeviceAddress addr[2];
String insideROM;
float tInside=NAN,tOutside=NAN;
// Domyslnie AUTO: VEVOR wystarcza bez zadnych DS18B20.
// Jesli sonda kabinowa jest dostepna, AUTO uwzglednia oba pomiary.
TempSourceChoice tempSource=TempSourceChoice::AUTO;
TempSelection currentTemp={NAN, "BRAK"};
char cmdMailbox[64]={0};
uint8_t sensorCount=0;
// Liczniki pozwalaja sprawdzic na POCO, czy dochodza jakiekolwiek bajty z FFF1.
uint32_t rxTotal=0, rxValid=0, pollSent=0, pollFailed=0;
uint8_t rxLen=0;
char rxHex[65]="", rxReject[12]="BRAK";
bool notifyRequested=false, cccdPresent=false;
bool mtuRequestSucceeded=false;

static const char* rejectReason(const uint8_t* p, size_t n){
  if(n!=21)return "LEN";
  if(p[0]!=0xAB || p[1]!=0xBA || p[2]!=0x11 || p[3]!=0xCC)return "HEADER";
  if(abbaChecksum(p,20)!=p[20])return "SUMA";
  if(!(p[4]==0 || p[4]==1 || p[4]==2 || p[4]==4 || p[4]==6))return "STATE";
  if(!(p[5]==0 || p[5]==1 || p[5]==0xFF))return "MODE";
  if(p[10]>1)return "TEMP_UNIT";
  if(p[15]>1)return "ALPINE";
  if(p[5]==0 && p[4]==1 && (p[6]<1 || p[6]>6))return "GEAR";
  return "OK";
}


bool fresh();
bool goodTemp(float t){return isfinite(t) && t>-55 && t<125 && t!=85;}
bool roomTempOK(float t){return isfinite(t) && t>-40 && t<60 && t!=85;}
TempSelection selectCabinTemperature(){
  // Sterownik przekazuje temperature kabiny w statusie ABBA. Temperatura
  // korpusu caseTemp nie jest pomiarem powietrza w kabinie.
  float web= fresh() && roomTempOK((float)heaterStatus.ambient)?(float)heaterStatus.ambient:NAN;
  float ext= mappingValid && roomTempOK(tInside)?tInside:NAN;
  return chooseCabinTemperature(tempSource,web,ext);
}
String romString(const uint8_t* p){String s="";for(int k=0;k<8;k++){if(p[k]<16)s+='0';s+=String(p[k],HEX);}s.toUpperCase();return s;}

class PhoneServerCallbacks:public BLEServerCallbacks {
 void onConnect(BLEServer*) override {phoneUnlocked=false;phoneConnected=true;}
 void onDisconnect(BLEServer*) override {phoneConnected=false;phoneUnlocked=false;phoneDisconnectedFlag=true;}
};
class PhoneRxCallbacks:public BLECharacteristicCallbacks {
 void onWrite(BLECharacteristic* ch) override {
   String v=ch->getValue();v.trim();v.toUpperCase();
   // Polecenia obslugiwane WYLACZNIE w petli, nie w BLE callbacku.
   if(v.length()<=55){
     portENTER_CRITICAL(&lock);
     offerCommand(cmdMailbox,sizeof(cmdMailbox),v.c_str());
     portEXIT_CRITICAL(&lock);
   }
 }
};
class HeaterClientCallbacks:public BLEClientCallbacks {
 void onConnect(BLEClient*) override {heaterConnected=true;}
 void onDisconnect(BLEClient*) override {heaterConnected=false;disconnectFlag=true;}
};
class HeaterDiscovery:public BLEAdvertisedDeviceCallbacks {
 void onResult(BLEAdvertisedDevice d) override {
   if(d.haveName() && d.getName()=="Heater5439"){
     if(!candidate)candidate=new BLEAdvertisedDevice(d);
     discovered=true;
     BLEDevice::getScan()->stop();
   }
 }
};

bool fresh(){return heaterConnected && heaterTimestamp && (millis()-heaterTimestamp<=HEATER_FRESH_MS) && heaterStatus.valid;}
void heaterNotification(BLERemoteCharacteristic*,uint8_t *data,size_t size,bool){
 // Callback BLE zapisuje tylko bajty. Parser i heaterStatus dzialaja w loop(),
 // zeby nie bylo wyscigu miedzy zmiana stanu i decyzja START/STOP.
 portENTER_CRITICAL(&lock);
 incomingFrames.put(data,size);
 portEXIT_CRITICAL(&lock);
}
void receiveHeaterFrame(){
 uint8_t buf[32];size_t size=0;
 portENTER_CRITICAL(&lock);
 const bool hasPacket=incomingFrames.take(buf,size);
 portEXIT_CRITICAL(&lock);
 if(!hasPacket)return;
 const size_t count=(size<32?size:32);
 char tmp[65];
 for(size_t i=0;i<count;i++)snprintf(tmp+i*2,3,"%02X",buf[i]);
 tmp[count*2]=0;
 const char* reason=rejectReason(buf,size);
 AbbaStatus q;
 const bool ok=(strcmp(reason,"OK")==0 && abbaParse(buf,size,q));
 ++rxTotal;rxLen=size>255?255:(uint8_t)size;
 memcpy(rxHex,tmp,count*2+1);
 strncpy(rxReject,reason,sizeof(rxReject)-1);rxReject[sizeof(rxReject)-1]=0;
 if(ok){
   ++rxValid;heaterStatus=q;heaterTimestamp=millis();lastPacketTime++;statusSamples++;
   stopOutcomeUncertain=updatedStopOutcome(stopOutcomeUncertain,true,q.state);
 }
}


// BLEOTA callbacks are invoked via process() from loop (not BLE IRQ).
// An OTA attempt while the heater is heating/ambiguous is aborted.
class HeaterOtaCallbacks: public BLEOTACallbacks {
 public:
  void beforeStartOTA() override { beginSafeOta(); }
  void beforeStartSPIFFS() override { beginSafeOta(); }
  void afterStop() override {
    otaSessionActive=true; // keep interlock until reboot
    lastAction="OTA_ZAKONCZONA_RESTART";
  }
  void afterAbort() override {
    if(otaRejected){
      otaRejected=false;
      lastAction="OTA_ZABLOKOWANA_PIEC_NIE_OFF";
      return; // rejected outsider must not disable an active thermostat
    }
    otaSessionActive=false; remoteEnabled=false; thermoEnabled=false;
    targetGear=0; phoneUnlocked=false;
    lastAction="OTA_PRZERWANA";
  }
 private:
  void beginSafeOta(){
    const OtaGate g={heaterConnected,fresh() && (millis()-heaterTimestamp<=2500),remoteEnabled,thermoEnabled,
                     pendingOp!=0,awaitingChange,heaterStatus.state};
    if(!canStartOta(g)){
      otaRejected=true;
      lastAction="OTA_ZABLOKOWANA_PIEC_NIE_OFF";
      bleOta.abort();
      return;
    }
    // From here no user or autonomous command is allowed until abort/reboot.
    otaRejected=false;
    otaSessionActive=true;remoteEnabled=false;thermoEnabled=false;
    phoneUnlocked=false;targetGear=0;
    cancelUnsentAny(pendingOp,awaitingChange);
    pendingThermoStart=false;pendingThermoStop=false;
    stopAfterPendingConfirmed=false;
    lastAction="OTA_AKTYWNA";
  }
};

bool connectHeater(){
 if(!candidate)return false;
 if(!heaterClient){heaterClient=BLEDevice::createClient();heaterClient->setClientCallbacks(new HeaterClientCallbacks());}
 if(!heaterClient->connect(candidate)){
   // Po nieudanej probie adres/obiekt skanowania moze byc przestarzaly.
   delete candidate;candidate=nullptr;discovered=false;return false;
 }
 // MTU 23 (domyslne) daje tylko 20 bajtow danych powiadomienia.
 // Odpowiedz ABBA 0x11 CC ma 21 bajtow, w tym sume kontrolna.
 // Oficjalne BLEClient::setMTU() w Arduino ESP32 3.3.12 negocjuje MTU z urzadzeniem.
 // Nie zmieniamy parsera: 20-bajtowa odpowiedz nie zawiera calej ramki.
 mtuRequestSucceeded=heaterClient->setMTU(HEATER_REQUEST_MTU);
 Serial.printf("Heater5439 MTU request=%u, wyslanie=%s\n", HEATER_REQUEST_MTU, mtuRequestSucceeded?"OK":"BLAD");
 BLERemoteService* svc=heaterClient->getService(HEATER_SVC);
 if(!svc){heaterClient->disconnect();return false;}
 heaterRx=svc->getCharacteristic(HEATER_NOTIFY);heaterTx=svc->getCharacteristic(HEATER_WRITE);
 if(!heaterRx || !heaterTx || !heaterTx->canWrite() || !heaterRx->canNotify()){
   heaterClient->disconnect();return false;
 }
 // Arduino ESP32 3.3.12: registerForNotify() zwraca void, nie bool.
 // Poprawność subskrypcji potwierdza dopiero odebrana ramka ABBA.
 cccdPresent=(heaterRx->getDescriptor(BLEUUID((uint16_t)0x2902))!=nullptr);
 heaterRx->registerForNotify(heaterNotification);
 notifyRequested=true;
 heaterConnected=true;
 Serial.println("Heater5439 GATT podpiety. Oczekuje na prawidlowy odczyt ABBA.");
 return true;
}
bool rawWrite(uint8_t type,uint8_t param){
 // Status CC albo pojedyncze polecenie BB za zgodą operatora.
 if(DIAGNOSTYKA_TYLKO_ODCZYT && type!=0xCC)return false;
 if(!heaterConnected || !heaterTx || millis()-lastCmdTime<SEND_GAP_MS)return false;
 uint8_t p[8];abbaPacket(type,param,p);
 const bool sent=heaterTx->writeValue(p,8,true);
 if(type==0xCC){if(sent)++pollSent;else ++pollFailed;}
 if(sent)lastCmdTime=millis();
 return sent;
}

void statusPoll(){rawWrite(0xCC,0);}
void discoverProbes(){
  probes.begin();
  sensorCount=0;
  for(uint8_t i=0;i<2;i++){
    selectedSensors[i]=probes.getAddress(addr[i],i);
    if(selectedSensors[i])sensorCount++;
  }
  mappingValid=false;
  for(uint8_t i=0;i<2;i++)if(selectedSensors[i] && romString(addr[i])==insideROM)mappingValid=true;
}
void probeTemps(){
 if(!sensorCount){tInside=NAN;tOutside=NAN;return;}
 probes.requestTemperatures();
 tInside=NAN;tOutside=NAN;
 if(!mappingValid)return;
 for(int i=0;i<2;i++){
   if(!selectedSensors[i])continue;
   float t=probes.getTempC(addr[i]);
   if(romString(addr[i])==insideROM)tInside=roomTempOK(t)?t:NAN;
   else tOutside=goodTemp(t)?t:NAN;
 }
}
void recordPowerChange(){
 static int oldState=255;
 if(!fresh())return;
 int s=heaterStatus.state;
 if(s!=oldState){
  Serial.printf("VEVOR state %d -> %d; %lu ms\n",oldState,s,millis());
  if((s==0 || s==6) && (oldState==1 || oldState==2))lastOffTime=millis();
  oldState=s;
 }
}

// Maks. 512 bajtow wg ATT. Status mieści sie w jednej charakterystyce BLE,
// a komunikaty są pobierane przez READ (powiadomienie moze byc skrocone na MTU 23).
String stateMessage(){
  uint32_t valid; uint8_t length; char why[12];
  portENTER_CRITICAL(&lock);
  valid=rxValid; length=rxLen; memcpy(why,rxReject,sizeof(why));
  portEXIT_CRITICAL(&lock);
  TempSelection cabin=selectCabinTemperature();
  String v="{\"fw\":\"0.6.3\",\"heaterBLE\":";
  v+=(heaterConnected?"true":"false");
  v+=",\"heaterFresh\":";v+=(fresh()?"true":"false");
  v+=",\"heaterState\":"+String(fresh()?heaterStatus.state:255);
  v+=",\"mode\":"+String(fresh()?heaterStatus.mode:255);
  v+=",\"gear\":"+String(fresh()?heaterStatus.level:0);
  v+=",\"alpine\":"+String(fresh()?heaterStatus.alpine:255);
  v+=",\"caseTemp\":"+String(fresh()?heaterStatus.caseTemp:0);
  v+=",\"vevorRoom\":";
  v+=(fresh() && roomTempOK((float)heaterStatus.ambient)?String(heaterStatus.ambient):"null");
  v+=",\"regulationTemp\":";
  v+=(roomTempOK(cabin.temp)?String(cabin.temp,1):"null");
  v+=",\"sourceSelected\":\""+String(sourceName(tempSource))+"\"";
  v+=",\"sourceUsed\":\""+String(cabin.source)+"\"";
  v+=",\"tInside\":";v+=(roomTempOK(tInside)?String(tInside,1):"null");
  v+=",\"tOutside\":";v+=(goodTemp(tOutside)?String(tOutside,1):"null");
  v+=",\"tSensors\":"+String(sensorCount);
  v+=",\"armed\":";v+=(remoteEnabled?"true":"false");
  v+=",\"stopUncertain\":";v+=(stopOutcomeUncertain?"true":"false");
  v+=",\"otaActive\":";v+=(otaBlocksCommands(otaSessionActive,bleOta.isRunning())?"true":"false");
  v+=",\"thermostat\":";v+=(thermoEnabled?"true":"false");
  v+=",\"set10\":"+String(thermostat.target10());
  v+=",\"band10\":"+String(thermostat.halfBand10());
  v+=",\"pending\":\""+String(pendingOp?pendingOp:'-')+"\"";
  // result jest ograniczony w raportach do przewidywalnych statusow.
  v+=",\"result\":\""+lastAction.substring(0,35)+"\"";
  v+=",\"rxValid\":"+String(valid);
  v+=",\"rxLen\":"+String(length);
  v+=",\"rxWhy\":\""+String(why)+"\"";
  v+=",\"heatS\":"+String(heatingSeconds);
  v+=",\"starts\":"+String(startTransitions);
  v+=",\"fuelMl\":"+String(fuelEstimateMl,1);
  v+="}";
  if(v.length()>512){
    // A GATT attribute cannot exceed 512 bytes. Avoid silently truncating JSON.
    Serial.printf("BLE status too long: %u bytes\n",(unsigned)v.length());
    return String("{\"fw\":\"0.6.3\",\"error\":\"STATUS_TOO_LONG\"}");
  }
  return v;
}

void emitState(){
  String x=stateMessage();Serial.println(x);
  if(phoneConnected && phoneNotify){
    // JSON 300-500 B nie miesci sie w pojedynczym NOTIFY MTU64.
    // Pilot POCO pobiera kompletny raport przez GATT READ; bez ucinania JSON.
    phoneNotify->setValue(x.c_str());
  }
}

// Komendy stanu nie sa ponawiane automatycznie. Po wyslaniu czekamy na
// potwierdzenie nowa ramka (zmiana stanu lub biegu). W czasie potwierdzania
// blokujemy inne komendy, zeby nie dwukrotnie przełączyć A1.
bool requestAction(char op){
 if(!fresh() || pendingOp || awaitingChange)return false;
 pendingThermoStart=false;
 // Jeden wspolny, testowany algorytm dopuszczania komend.
 CommandGate g={remoteEnabled, heaterStatus.state, heaterStatus.mode, heaterStatus.level,
   millis()-bootAt, lastOffTime ? millis()-lastOffTime : UINT32_MAX,
   BOOT_GRACE_MS, MIN_RESTART_MS};
 if(!canQueueCommand(op,g))return false;
 const uint8_t s=heaterStatus.state;
 pendingOp=op;pendingSince=millis();postOpPreviousPacket=lastPacketTime;
 beforeState=s;beforeLevel=heaterStatus.level;beforeAlpine=heaterStatus.alpine;
 lastAction=String("OCZEKUJE_")+op;
 return true;
}
void runAction(){
 // Po utracie kabinowego pomiaru lub wylaczeniu automatyki nie wysylaj
 // juz przygotowanego automatycznego START, nawet gdy status pieca jest swiezy.
 if(pendingOp=='S' && pendingThermoStart && !awaitingChange){
   TempSelection t=selectCabinTemperature();
   // Uwaga: demand MUSI byc przeliczony z NOWEJ temperatury (nie z cache).
   // Rozkaz jest dopuszczony tylko przy wciaz poprawnym pomiarze i swiezym stanie ECU.
   if(!revalidateQueuedAutoStart(thermostat,thermoEnabled,remoteEnabled,
                                fresh(),t.temp)){
     pendingOp=0;pendingThermoStart=false;
     lastAction="AUTO_START_ANULOWANY";return;
   }
 }
 if(pendingOp=='X' && pendingThermoStop && !awaitingChange){
   TempSelection t=selectCabinTemperature();
   if(!thermoEnabled || !roomTempOK(t.temp) || !fresh()){
     pendingOp=0;pendingThermoStop=false;
     lastAction="AUTO_STOP_ANULOWANY";return;
   }
   thermostat.evaluate(lroundf(t.temp*10),true,true);
   if(thermostat.callForHeat()){
     pendingOp=0;pendingThermoStop=false;
     lastAction="AUTO_STOP_JUZ_NIEPOTRZEBNY";return;
   }
 }
 if(awaitingChange){
   // Check that reply arrived AFTER sending, not the cached pre-command status.
   if(fresh() && lastPacketTime!=postOpPreviousPacket){
      bool confirmed=false;
      if(pendingOp=='S') confirmed=heaterStatus.state==1;
      if(pendingOp=='X') confirmed=heaterStatus.state==2 || heaterStatus.state==0 || heaterStatus.state==6;
      if(pendingOp=='U') confirmed=heaterStatus.mode==0 && heaterStatus.level==beforeLevel+1;
      if(pendingOp=='D') confirmed=heaterStatus.mode==0 && heaterStatus.level+1==beforeLevel;
      if(pendingOp=='A') confirmed=heaterStatus.alpine<=1 && heaterStatus.alpine!=beforeAlpine;
      if(confirmed){
        const char finished=pendingOp;
        lastAction=String("POTWIERDZONO_")+finished;
        if(finished=='X'){lastOffTime=millis();stopOutcomeUncertain=false;}
        awaitingChange=false;pendingOp=0;pendingThermoStart=false;pendingThermoStop=false;
        // STOP was requested after sending START/UP/DOWN. Send a new STOP
        // only once, and only if the *new* status confirms real heating.
        if(stopAfterPendingConfirmed){
          stopAfterPendingConfirmed=false;
          if(heaterStatus.state==1 && remoteEnabled && heaterStatus.mode==0){
            if(requestAction('X'))lastAction="STOP_PO_POTWIERDZENIU";
            else lastAction="STOP_PO_POTWIERDZENIU_ODRZUCONY";
          }else lastAction="STOP_NIE_WYMAGANY_LUB_BRAK_STANU";
        }
        emitState();return;
      }
   }
   if(!heaterConnected || millis()-pendingSince>CONFIRM_WAIT_MS){
      // Nie mozna deklarowac STOP, gdy polecenie nie zostalo potwierdzone.
      // Gdy STOP zgloszono podczas START, komunikat wymaga kontroli fabrycznego ECU.
      if(pendingOp=='X' || stopAfterPendingConfirmed)stopOutcomeUncertain=true;
      lastAction=stopAfterPendingConfirmed || pendingOp=='X' ? "STOP_NIEPOTWIERDZONY_SPRAWDZ_PIEC" : String("BRAK_POTWIERDZENIA_")+pendingOp;
      awaitingChange=false;pendingOp=0;pendingThermoStart=false;pendingThermoStop=false;stopAfterPendingConfirmed=false;remoteEnabled=false;thermoEnabled=false;
      targetGear=0;emitState();
   }
   return;
 }
 if(!pendingOp)return;
 // Rozbrojenie w trakcie oczekiwania na nowszy status anuluje NIEWYSLANA komende.
 if(!remoteEnabled){lastAction="ANULOWANO_ROZBROJENIE";pendingOp=0;pendingThermoStart=false;pendingThermoStop=false;return;}
 if(!heaterConnected || millis()-pendingSince>CMD_WAIT_MS){lastAction="BRAK_POLACZENIA";pendingOp=0;pendingThermoStart=false;pendingThermoStop=false;remoteEnabled=false;thermoEnabled=false;return;}
 if(lastPacketTime==postOpPreviousPacket){statusPoll();return;}
 if(!fresh() || millis()-heaterTimestamp>2500){lastAction="STATUS_NIEAKTUALNY";pendingOp=0;pendingThermoStart=false;pendingThermoStop=false;remoteEnabled=false;thermoEnabled=false;return;}
 // Dodatkowa weryfikacja wszystkich warunkow także tuz przed wysłaniem.
 CommandGate sendGate={remoteEnabled, heaterStatus.state, heaterStatus.mode, heaterStatus.level,
   millis()-bootAt, lastOffTime ? millis()-lastOffTime : UINT32_MAX,
   BOOT_GRACE_MS, MIN_RESTART_MS};
 if(!canQueueCommand(pendingOp,sendGate)){
    lastAction="ZMIANA_STANU_ANULOWANA";pendingOp=0;pendingThermoStart=false;pendingThermoStop=false;return;
 }
 if(millis()-lastCmdTime<SEND_GAP_MS)return;
 uint8_t bb=(pendingOp=='U'?0xA2:pendingOp=='D'?0xA3:pendingOp=='A'?0xA5:0xA1);
 if(rawWrite(0xBB,bb)){
    if(pendingOp=='S')lastStartTime=millis();
    awaitingChange=true;pendingSince=millis();postOpPreviousPacket=lastPacketTime;
    lastAction=String("WYSLANO_")+pendingOp;
    emitState();
 }else{lastAction="BLAD_WYSYLANIA";pendingOp=0;pendingThermoStart=false;pendingThermoStop=false;}
}
static bool readStrictNumber(const String &text,int &result){
  const char* raw=text.c_str();
  if(!raw || !*raw)return false;
  char* end=nullptr;
  long val=strtol(raw,&end,10);
  if(end==raw || *end!='\0' || val<-10000L || val>10000L)return false;
  result=(int)val;
  return true;
}
void handlePhoneCommand(String command){
 if(command.startsWith("KEY:")){
   if(command.substring(4)==DEVICE_ACCESS_KEY){
     phoneUnlocked=true;lastAction="DOSTEP_OK";
   }else{
     // Bledny kod NIE MOZE wylaczyc autonomicznego termostatu.
     // Zasady dostepu do BLE OTA wymagaja osobnego hardeningu.
     lastAction="ZLY_KOD";
   }
   emitState();return;
 }
 if(command!="STATUS" && !phoneUnlocked){
   lastAction="WYMAGANY_KOD";emitState();return;
 }
 if(DIAGNOSTYKA_TYLKO_ODCZYT && command!="STATUS" && !command.startsWith("IN:")){
   Serial.println("DIAG: polecenie zablokowane, tylko STATUS i przypisanie sondy IN:0 / IN:1");
   emitState();return;
 }
 if(command=="STATUS"){
   if(heaterConnected) statusPoll();
   emitState();return;
 }
 if(command=="ARM"){
   // Operator wyraznie zezwala na sterowanie; po restarcie zawsze OFF.
   if(fresh() && heaterStatus.mode==0){remoteEnabled=true;lastAction="ARM_OK";}
   else lastAction="ARM_ODRZUCONE";
   emitState();return;
 }
 if(command=="DISARM"){
   remoteEnabled=false;thermoEnabled=false;thermostat.reset();targetGear=0;
   stopAfterPendingConfirmed=false;
   cancelUnsentAny(pendingOp,awaitingChange);
   pendingThermoStart=false;pendingThermoStop=false; // juz wyslane: czekaj na status
   lastAction=awaitingChange?"DISARM_PO_WYSLANIU":"DISARM";
   emitState();return;
 }
 if(command=="START" || command=="STOP" || command=="UP" || command=="DOWN"){
   // STOP otrzymany zanim START zostal wyslany: anuluj zamiast uruchamiac piec.
   // Gdy START juz wyslany, tylko oczekiwanie na swiezy odczyt moze rozstrzygnac wynik.
   if(command=="STOP" && pendingOp){
     thermoEnabled=false;thermostat.reset();targetGear=0;
     if(pendingOp=='X'){
       lastAction="STOP_JUZ_ZLECONY";emitState();return;
     }
     if(awaitingChange){
       // Do not send a second toggle until the latest readback confirms heat.
       stopAfterPendingConfirmed=true;stopOutcomeUncertain=true;
       lastAction="STOP_CZEKA_NA_POTWIERDZENIE";
     }else if(pendingOp=='S'){
       cancelUnsentStart(pendingOp,awaitingChange);
       pendingThermoStart=false;pendingThermoStop=false;
       lastAction="START_ANULOWANY_PRZEZ_STOP";
     }else{
       cancelUnsentAny(pendingOp,awaitingChange);
       if(requestAction('X'))lastAction="STOP_ZAMIAST_BIEGU";
       else lastAction="STOP_ODRZUCONY_NOWY_STAN";
     }
     emitState();return;
   }
   if(thermoEnabled && command!="STOP"){
     lastAction="WYLACZ_TERMOSTAT_PRZED_RECZNYM";emitState();return;
   }
   char c=command=="START"?'S':command=="STOP"?'X':command=="UP"?'U':'D';
   bool accepted=requestAction(c);
   if(command=="STOP"){
     stopOutcomeUncertain=stopRequestedOutcome(stopOutcomeUncertain,fresh(),heaterStatus.state);
     // Wyłączenie automatyki jest bezpieczne nawet przy nieudanym STOP.
     targetGear=0;thermoEnabled=false;thermostat.reset();
   }
   if(!accepted)lastAction="ODRZUCONE_"+command;
   Serial.printf("Zlecenie %s: %s\n",command.c_str(),accepted?"przyjete":"ODRZUCONE (status/bezpieczenstwo)");
   emitState();return;
 }
 if(command.startsWith("H") && command.length()==2 && command.charAt(1)>='1' && command.charAt(1)<='6'){
   if(remoteEnabled && !thermoEnabled && fresh() && !pendingOp && !awaitingChange && heaterStatus.state==1 && heaterStatus.mode==0){
     targetGear=command.charAt(1)-'0';lastAction="CEL_H"+String(targetGear);
   }else lastAction="H_ODRZUCONE";
   emitState();return;
 }
 if(command=="ALPINE"){
   // 0xA5 confirmed by examining original VEVOR 2kW 1.5 native Android APK:
   // string "baab04bba50000". Original GUI only allows the toggle in OFF,
   // HEATING or STANDBY; never in COOLDOWN.
   if(thermoEnabled){lastAction="ALPINE_WYLACZ_TERMOSTAT";emitState();return;}
   if(requestAction('A'))lastAction="ALPEJSKI_CZEKA_NA_POTWIERDZENIE";
   else lastAction="ALPEJSKI_ODRZUCONY";
   emitState();return;
 }
 if(command.startsWith("IN:")){
   if(command.length()!=4 || (command[3]!='0' && command[3]!='1')){
     lastAction="ZLY_INDEKS_SONDY";emitState();return;
   }
   uint8_t idx=command[3]-'0';
   if(idx<2 && selectedSensors[idx]){
    insideROM=romString(addr[idx]);prefs.putString("insideROM",insideROM);mappingValid=true;
    thermoEnabled=false;thermostat.reset();
    probeTemps();emitState();
   }return;
 }
 if(command.startsWith("SOURCE:")){
   String chosen=command.substring(7);
   if(chosen=="AUTO" || chosen=="WEB" || chosen=="DS" || chosen=="AVG"){
     tempSource=sourceFromName(chosen.c_str());
     prefs.putString("source",chosen);
     lastAction="SOURCE_"+chosen;
   }else lastAction="SOURCE_BLEDNE";
   emitState();return;
 }
 if(command.startsWith("TEMP:")){
   int v=0;
   if(readStrictNumber(command.substring(5),v) && thermostat.configure(v,thermostat.halfBand10())){
     prefs.putInt("set10",v);lastAction="TEMP_OK";
   }else lastAction="TEMP_ZAKRES";
   emitState();return;
 }
 if(command.startsWith("BAND:")){
   int v=0;
   if(readStrictNumber(command.substring(5),v) && thermostat.configure(thermostat.target10(),v)){
     prefs.putInt("band10",v);lastAction="BAND_OK";
   }else lastAction="BAND_ZAKRES";
   emitState();return;
 }
 if(command=="THERMO:ON"){
   targetGear=0;
   // Bez DS18B20 dziala na temperaturze kabiny z VEVOR-a.
   // Nie uzywamy temperatury korpusu, sondy zewnetrznej ani starej ramki.
   TempSelection cabin=selectCabinTemperature();
   if(remoteEnabled && roomTempOK(cabin.temp) && fresh() && !pendingOp && !awaitingChange){
     if(heaterStatus.mode==0){targetGear=0;thermoEnabled=true;thermostat.reset();lastAction="THERMO_ON";}
     else lastAction="WYMAGANY_TRYB_RECZNY";
   }else lastAction="BRAK_TEMP_LUB_BLE";
   emitState();return;
 }
 if(command=="THERMO:OFF"){
   thermoEnabled=false;thermostat.reset();targetGear=0;
   // Nie pozwalaj, by START przygotowany przez termostat wyszedl po jego wylaczeniu.
   // Nie kasuj polecen juz wyslanych: wymagaja potwierdzenia ze swiezego statusu.
   cancelUnsentStart(pendingOp,awaitingChange);
   cancelUnsentAutoStop(pendingOp,awaitingChange,pendingThermoStop);
   pendingThermoStart=false;pendingThermoStop=false;
   lastAction="THERMO_OFF";emitState();return;
 }
 Serial.printf("Nieznane polecenie: %s\n",command.c_str());
}
void handleThermostat(){
 if(DIAGNOSTYKA_TYLKO_ODCZYT)return;
 if(!thermoEnabled || !remoteEnabled)return;
 TempSelection cabin=selectCabinTemperature();
 if(!roomTempOK(cabin.temp) || !fresh() || heaterStatus.mode!=0){
  thermoEnabled=false;thermostat.reset();targetGear=0;
  lastAction="TEMP_KABINY_LUB_BLE_BRAK";
  cancelUnsentStart(pendingOp,awaitingChange);
  cancelUnsentAutoStop(pendingOp,awaitingChange,pendingThermoStop);
  pendingThermoStart=false;pendingThermoStop=false;
  // Nie przyjmujemy nowych START bez danych. Nie zmieniamy na slepo
  // stanu palnika. Fabryczny ECU zachowuje sterowanie i wychladzanie.
  Serial.println("Termostat wstrzymany: brak swiezej temperatury kabiny / statusu");emitState();return;
 }
 thermostat.evaluate(lroundf(cabin.temp*10),true,true);
 if(pendingOp || awaitingChange || targetGear)return;
 if(thermostat.callForHeat() && (heaterStatus.state==0 || heaterStatus.state==6))pendingThermoStart=requestAction('S');
 if(!thermostat.callForHeat() && heaterStatus.state==1){
   if(requestAction('X'))pendingThermoStop=true;
 }
}
void updateStats(){
  const uint32_t now=millis();
  if(!lastStatUpdate){lastStatUpdate=now;return;}
  const uint32_t delta=now-lastStatUpdate;
  if(delta<1000)return;
  lastStatUpdate=now;
  if(!fresh()) {lastObservedState=255;return;}
  const uint8_t st=heaterStatus.state;
  if(st==1 && lastObservedState!=1 && lastObservedState!=255){startTransitions++;}
  lastObservedState=st;
  // Zliczamy maks. 5s/iteracje, aby przerwy w komunikacji nie udawaly pracy.
  if(st!=1 || delta>5000)return;
  heatingSeconds+=delta/1000;
  if(heaterStatus.mode==0 && heaterStatus.level>=1 && heaterStatus.level<=6 && (heaterStatus.alpine==0 || heaterStatus.alpine==1)){
    const float hz=(heaterStatus.alpine?HZ_ALPINE:HZ_NORMAL)[heaterStatus.level-1];
    fuelEstimateMl += hz*18.5f*float(delta)/1000000.0f; // 18.5 ml / 1000 impulsow
  }
}
void setup(){
 Serial.begin(115200);delay(400);bootAt=millis();
 prefs.begin("vevor-esp32",false);
 // Kod indywidualny dla urzadzenia. Nie zapisuj PIN-u w publicznym repo.
 // Nowy PIN widoczny w monitorze USB-Serial 115200 przy pierwszym uruchomieniu.
 DEVICE_ACCESS_KEY=prefs.getString("controlPin","");
 if(DEVICE_ACCESS_KEY.length()!=8 ||
    DEVICE_ACCESS_KEY.toInt()==0 ||
    DEVICE_ACCESS_KEY.indexOf(' ')>=0){
   char generated[9];
   snprintf(generated,sizeof(generated),"%08lu",(unsigned long)(esp_random()%100000000UL));
   DEVICE_ACCESS_KEY=generated;
   prefs.putString("controlPin",DEVICE_ACCESS_KEY);
 }
 Serial.print("POUFNY PIN STEROWANIA (zachowaj): ");
 Serial.println(DEVICE_ACCESS_KEY);
 int t=prefs.getInt("set10",220),b=prefs.getInt("band10",15);
 if(!thermostat.configure(t,b))thermostat.configure(220,15);
 insideROM=prefs.getString("insideROM","");
 tempSource=sourceFromName(prefs.getString("source","AUTO").c_str());
 heatingSeconds=prefs.getULong("heatS",0);
 startTransitions=prefs.getULong("starts",0);
 fuelEstimateMl=prefs.getFloat("fuelMl",0.0f);
 thermoEnabled=false;remoteEnabled=false; // zamierzenie: nigdy samoczynnie po restarcie bez testu
 probes.begin();probes.setResolution(12);
 discoverProbes();
 for(uint8_t i=0;i<2;i++)if(selectedSensors[i])Serial.printf("DS18B20 #%d ROM %s\n",i,romString(addr[i]).c_str());
 // Sondy sa opcjonalne. Ich brak nie uniemozliwia uzycia Webasto.
 probeTemps();lastTempTime=millis();
 BLEDevice::init("VEVOR-ESP32-STEROWNIK");
 localServer=BLEDevice::createServer();localServer->setCallbacks(new PhoneServerCallbacks());
 BLEService* control=localServer->createService(PHONE_SVC);
 BLECharacteristic* rx=control->createCharacteristic(PHONE_RX,BLECharacteristic::PROPERTY_WRITE);
 rx->setCallbacks(new PhoneRxCallbacks());
 phoneNotify=control->createCharacteristic(PHONE_TX,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY);
 phoneNotify->addDescriptor(new BLE2902());control->start();
 // OTA is signed (SHA256/RSA2048) to prevent nearby BLE clients flashing foreign code.
 bleOta.begin(localServer, true);
 if(bleOta.setKey(OTA_RSA_PUBLIC_KEY,strlen(OTA_RSA_PUBLIC_KEY))){
   Serial.println("ERROR: OTA public key parsing failed; halting to avoid insecure OTA");
   while(true)delay(1000);
 }
 bleOta.setCallbacks(new HeaterOtaCallbacks());
 bleOta.setModel("ESP32-WROOM-32-CH340");bleOta.setFWVersion("STEROWNIK-0.6.3");
 bleOta.setManufactuer("VEVOR ESP32 projekt");bleOta.init();
 BLEAdvertising* ads=BLEDevice::getAdvertising();
 ads->addServiceUUID(PHONE_SVC);ads->addServiceUUID(bleOta.getBLEOTAuuid());
 BLEDevice::startAdvertising();
 BLEScan* scan=BLEDevice::getScan();scan->setAdvertisedDeviceCallbacks(new HeaterDiscovery());
 scan->setActiveScan(true);scan->setInterval(130);scan->setWindow(90);
 Serial.println("STEROWNIK 0.6.3: czujniki + BLE sterowanie ARM/DISARM.");
}
void loop(){
 // Deliver OTA start/abort callbacks BEFORE allowing any heater command.
 bleOta.process();
 uint32_t now=millis();
 receiveHeaterFrame();
 // KRYTYCZNE: komendy z telefonu (zwlaszcza DISARM/THERMO:OFF) musza
 // byc przetworzone PRZED wyslaniem jakiejkolwiek zaplanowanej akcji.
 char mailboxCopy[64];
 portENTER_CRITICAL(&lock);
 memcpy(mailboxCopy,cmdMailbox,sizeof(mailboxCopy));cmdMailbox[0]=0;
 portEXIT_CRITICAL(&lock);
 String command=mailboxCopy;
 if(otaBlocksCommands(otaSessionActive,bleOta.isRunning())){
   // Podczas transferu OTA odrzucamy nowe polecenia z telefonu i UART.
   command="";
   remoteEnabled=false;thermoEnabled=false;targetGear=0;
   cancelUnsentAny(pendingOp,awaitingChange);
   pendingThermoStart=false;pendingThermoStop=false;stopAfterPendingConfirmed=false;
 }
 if(command.length())handlePhoneCommand(command);
 if(!otaBlocksCommands(otaSessionActive,bleOta.isRunning()) && Serial.available()){
   String line=Serial.readStringUntil('\n');line.trim();line.toUpperCase();
   if(line.length())handlePhoneCommand(line);
 }
 if(phoneDisconnectedFlag){
   phoneDisconnectedFlag=false;
   // Bez aktywnej automatyki zamknij reczne sterowanie i anuluj niewyslane rozkazy.
   if(!thermoEnabled){remoteEnabled=false;targetGear=0;cancelUnsentAny(pendingOp,awaitingChange);pendingThermoStart=false;pendingThermoStop=false;}
 }
 // OTA aktualizuje flash; w tym czasie zadne nowe rozkazy START ani przelaczania.
 if(otaBlocksCommands(otaSessionActive,bleOta.isRunning())){
   remoteEnabled=false;thermoEnabled=false;targetGear=0;
   cancelUnsentAny(pendingOp,awaitingChange);pendingThermoStart=false;pendingThermoStop=false;stopAfterPendingConfirmed=false;
   lastAction="OTA_AKTYWNA_STEROWANIE_STOP";
   bleOta.process();delay(40);return;
 }
 if(!heaterConnected && now-lastScan>12000){
   lastScan=now;
   if(!candidate){BLEDevice::getScan()->start(3,false);BLEDevice::getScan()->clearResults();}
   if(discovered && candidate){connectHeater();}
 }
 if(disconnectFlag){
   stopOutcomeUncertain=stopDisconnectedOutcome(stopOutcomeUncertain,pendingOp=='X',stopAfterPendingConfirmed);
   if(stopOutcomeUncertain)lastAction="STOP_NIEPOTWIERDZONY_SPRAWDZ_PIEC";
   portENTER_CRITICAL(&lock);incomingFrames.ready=false;portEXIT_CRITICAL(&lock);
   notifyRequested=false;cccdPresent=false;disconnectFlag=false;pendingOp=0;pendingThermoStart=false;pendingThermoStop=false;thermoEnabled=false;heaterStatus.valid=false;
   heaterRx=nullptr;heaterTx=nullptr;stopAfterPendingConfirmed=false;
   if(candidate){delete candidate;candidate=nullptr;}discovered=false;
   heaterTimestamp=0;
   awaitingChange=false;targetGear=0;remoteEnabled=false;
   // Po rozlaczeniu wymagaj od nowa rzeczywistego odczytu przed komenda.
 }
 if(now-lastTempTime>=TEMP_PERIOD){
   lastTempTime=now;
   static uint8_t rescanCounter=0;
   if(++rescanCounter>=10){rescanCounter=0;discoverProbes();}
   probeTemps();handleThermostat();emitState();
 }
 // Zmiana H1-H6 przez pojedynczy krok i weryfikację kolejnego odczytu.
 if(targetGear && !pendingOp && !awaitingChange && remoteEnabled && fresh()){
    if(heaterStatus.state==1 && heaterStatus.mode==0){
      if(heaterStatus.level==targetGear){targetGear=0;lastAction="H_OSIAGNIETE";}
      else if(heaterStatus.level>=1 && heaterStatus.level<=6){
        if(!requestAction(heaterStatus.level<targetGear?'U':'D')){
          lastAction="H_ZABLOKOWANE";targetGear=0;
        }
      }
    }else {lastAction="H_PRZERWANE";targetGear=0;}
 }
 if(heaterConnected && now-lastCmdTime>=3000 && now-lastNotificationTime>=3000){
   if(rawWrite(0xCC,0))lastNotificationTime=now;
 }
 runAction();recordPowerChange();updateStats();
 if(now-lastStatPersist>=15UL*60UL*1000UL){
   lastStatPersist=now;
   prefs.putULong("heatS",heatingSeconds);
   prefs.putULong("starts",startTransitions);
   prefs.putFloat("fuelMl",fuelEstimateMl);
 }
 static bool lastPhone=false;
 if(lastPhone && !phoneConnected){delay(50);localServer->startAdvertising();}
 lastPhone=phoneConnected;
 bleOta.process();delay(20);
}
