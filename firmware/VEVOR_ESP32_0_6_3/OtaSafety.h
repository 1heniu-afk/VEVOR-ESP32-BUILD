#pragma once
#include <stdint.h>
// OTA is only permitted with reliable confirmation that the burner is idle
// and all user/thermostat commands are disarmed. Ambiguous states are denied.
struct OtaGate {
  bool heaterConnected, fresh, armed, thermo, pending, awaiting;
  uint8_t state;
};
static inline bool canStartOta(const OtaGate& g){
  return g.heaterConnected && g.fresh && !g.armed && !g.thermo &&
         !g.pending && !g.awaiting && (g.state==0 || g.state==6);
}
// This flag is latched from beforeStartOTA until afterAbort or device reboot.
// BLEOTA.isRunning() alone is not a transfer-level lock.
static inline bool otaBlocksCommands(bool sessionActive,bool writingSector){
  return sessionActive || writingSector;
}
