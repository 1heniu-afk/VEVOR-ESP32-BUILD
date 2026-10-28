#pragma once
// To samo zachowanie w firmware i testach. Po rozbrojeniu/wyłączeniu termostatu
// można anulować tylko polecenie, które NIE zostało wysłane do ECU.
static inline bool cancelUnsentStart(char &pending,bool transmitted){
 if(pending=='S' && !transmitted){pending=0;return true;}
 return false;
}
static inline bool cancelUnsentAny(char &pending,bool transmitted){
 if(pending && !transmitted){pending=0;return true;}
 return false;
}
