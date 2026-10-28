#pragma once
// Wspolna logika (firmware + testy) czy dopuszczalne jest wyslanie
// WCZESNIEJ zaplanowanego automatycznego START. Nie odwraca wydanej komendy.
static inline bool allowQueuedAutoStart(bool thermoOn, bool armed,
                                        bool freshState, bool freshCabin,
                                        bool demandForHeat) {
  return thermoOn && armed && freshState && freshCabin && demandForHeat;
}
