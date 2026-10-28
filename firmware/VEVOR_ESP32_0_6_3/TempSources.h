#pragma once
#include <math.h>
#include <string.h>
// Czysty C++, niezalezny od Arduino: mozna przetestowac na komputerze.
enum class TempSourceChoice : unsigned char { AUTO=0, WEB=1, DS=2, AVG=3 };
struct TempSelection { float temp; const char* source; };
static inline const char* sourceName(TempSourceChoice s){
  switch(s){case TempSourceChoice::WEB:return "WEB";
    case TempSourceChoice::DS:return "DS";
    case TempSourceChoice::AVG:return "AVG";
    default:return "AUTO";}
}
static inline TempSourceChoice sourceFromName(const char* x){
  if(strcmp(x,"WEB")==0)return TempSourceChoice::WEB;
  if(strcmp(x,"DS")==0)return TempSourceChoice::DS;
  if(strcmp(x,"AVG")==0)return TempSourceChoice::AVG;
  return TempSourceChoice::AUTO;
}
static inline bool validCabin(float x){return isfinite(x)&&x>-40.f&&x<60.f&&x!=85.f;}
static inline TempSelection chooseCabinTemperature(TempSourceChoice choice,float web,float ds){
  const bool w=validCabin(web), d=validCabin(ds);
  if(choice==TempSourceChoice::WEB)return w?TempSelection{web,"WEB"}:TempSelection{NAN,"WEB_BRAK"};
  if(choice==TempSourceChoice::DS)return d?TempSelection{ds,"DS"}:TempSelection{NAN,"DS_BRAK"};
  if(choice==TempSourceChoice::AVG && !(w&&d))return {NAN,"AVG_BRAK"};
  // AUTO i AVG lacza tylko dwa wiarygodne pomiary, nigdy zewnetrzna i korpus.
  if((choice==TempSourceChoice::AUTO || choice==TempSourceChoice::AVG) && w && d)
     return {(web+ds)/2.0f,"AVG"};
  // Wybierana sonda niedostepna: informujemy o rzeczywistym zrodle rezerwowym.
  if(w)return {web,"WEB_FALLBACK"};
  if(d)return {ds,"DS_FALLBACK"};
  return {NAN,"BRAK"};
}
