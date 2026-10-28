#pragma once
#include <stdint.h>
// Policy: STOP is a request, not evidence of a stopped burner.
// A confirmed fresh OFF, COOLDOWN or STANDBY frame resolves uncertainty.
static inline bool heaterIsStoppedOrCooling(uint8_t state){
 return state==0 || state==2 || state==6;
}
static inline bool stopRequestedOutcome(bool wasUncertain,bool fresh,uint8_t state){
 return wasUncertain || !fresh || !heaterIsStoppedOrCooling(state);
}
static inline bool updatedStopOutcome(bool wasUncertain,bool fresh,uint8_t state){
 if(!fresh)return wasUncertain;
 return heaterIsStoppedOrCooling(state)?false:wasUncertain;
}
static inline bool stopDisconnectedOutcome(bool wasUncertain,bool stopPending,bool stopAfterPending){
 return wasUncertain || stopPending || stopAfterPending;
}
