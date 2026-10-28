#pragma once
#include <cmath>
#include "ThermostatCore.h"
#include "AutoStartSafety.h"
// Safe revalidation right before sending automatic START, not a stale cached demand.
static inline bool revalidateQueuedAutoStart(ThermostatCore &t,bool thermoOn,bool armed,
                                             bool freshState,float cabin){
  const bool good=std::isfinite(cabin) && cabin>-40.f && cabin<60.f && cabin!=85.f;
  if(!good || !thermoOn || !armed || !freshState){
    if(!good) t.reset();
    return false;
  }
  t.evaluate((int)std::lround(cabin*10.0f), true, true);
  return allowQueuedAutoStart(thermoOn,armed,freshState,good,t.callForHeat());
}
// In-flight automatic STOP is never retracted; an UNSENT stop can be cancelled
// when the user disables autonomous control.
static inline bool cancelUnsentAutoStop(char &pending,bool transmitted,bool autoStop){
  if(autoStop && pending=='X' && !transmitted){pending=0;return true;}
  return false;
}
