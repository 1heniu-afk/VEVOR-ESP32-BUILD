#pragma once
#include <stdint.h>
#include <stddef.h>
// Ramki ABBA: AirHeaterCC / Heater5439. Rozkaz dotyczy fabrycznego ECU.
struct AbbaStatus {
  bool valid=false;
  uint8_t state=255, mode=255, level=0, error=0, alpine=0;
  int16_t caseTemp=0, ambient=0;
  uint8_t voltage=0, tempUnit=255;
};
static inline uint8_t abbaChecksum(const uint8_t *p, size_t n){
  if(!p)return 0;
  unsigned v=0; for(size_t i=0;i<n;i++)v+=p[i];return uint8_t(v);
}
static inline bool abbaParse(const uint8_t *p,size_t n,AbbaStatus &out){
  if(!p || n!=21 || p[0]!=0xAB || p[1]!=0xBA || p[2]!=0x11 || p[3]!=0xCC) return false;
  if(abbaChecksum(p,20)!=p[20]) return false;
  if(p[15]>1)return false; // poziom wysokosciowy tylko 0/1
  if(!(p[4]==0 || p[4]==1 || p[4]==2 || p[4]==4 || p[4]==6)) return false;
  if(!(p[5]==0 || p[5]==1 || p[5]==0xFF)) return false;
  AbbaStatus q;
  q.valid=true;q.state=p[4];q.mode=p[5];q.error=q.mode==0xFF?p[6]:0;
  q.level=p[6];q.voltage=p[9];q.tempUnit=p[10];
  // ABBA: pole jednostki 0=C, 1=F. Termostat zawsze pracuje w Celsiuszach.
  if(q.tempUnit>1)return false;
  const int rawAmbient=int(p[11])-(q.tempUnit==0?30:22);
  q.ambient = q.tempUnit==0 ? rawAmbient : (rawAmbient-32)*5/9;
  q.caseTemp=(int(p[12])<<8)|p[13];q.alpine=p[15];
  if(q.mode==0 && (q.level<1 || q.level>6) && q.state==1)return false;
  out=q;return true;
}
// Komendy BB/CC opracowane z dokumentacji protokolu; bez ingerencji w rozruch ECU.
static inline void abbaPacket(uint8_t opcode, uint8_t action,uint8_t out[8]){
  out[0]=0xBA;out[1]=0xAB;out[2]=4;out[3]=opcode;
  out[4]=action;out[5]=0;out[6]=0;out[7]=abbaChecksum(out,7);
}
