#pragma once
// Zero zaleznosci od Arduino: identyczna logika testowana na komputerze i uruchamiana w ESP32.
// Wszystkie temperatury sa w dziesiatych czesciach stopnia Celsjusza.
#include <stdint.h>

class ThermostatCore {
 public:
  ThermostatCore() : target10_(200), halfBand10_(10), callForHeat_(false) {}
  static bool validBand(int halfBand10) { return halfBand10 >= 10 && halfBand10 <= 50 && (halfBand10 % 5 == 0); }
  static bool validTarget(int target10) { return target10 >= 50 && target10 <= 350 && (target10 % 5 == 0); }
  bool configure(int target10, int halfBand10) {
    if (!validTarget(target10) || !validBand(halfBand10)) return false;
    target10_ = target10; halfBand10_ = halfBand10;
    return true;
  }
  // Demand to sygnal logiczny - NIE rozkaz uruchomienia nagrzewnicy.
  // Nieprawidlowy lub brakujacy odczyt wymusza brak zadania grzania.
  bool evaluate(int temperature10, bool valid, bool enabled) {
    if (!enabled || !valid) { callForHeat_ = false; return callForHeat_; }
    const int low = target10_ - halfBand10_;
    const int high = target10_ + halfBand10_;
    if (temperature10 <= low) callForHeat_ = true;
    if (temperature10 >= high) callForHeat_ = false;
    return callForHeat_;
  }
  void reset() { callForHeat_ = false; }
  bool callForHeat() const { return callForHeat_; }
  int low10() const { return target10_ - halfBand10_; }
  int high10() const { return target10_ + halfBand10_; }
  int target10() const { return target10_; }
  int halfBand10() const { return halfBand10_; }
 private:
  int target10_, halfBand10_;
  bool callForHeat_;
};
