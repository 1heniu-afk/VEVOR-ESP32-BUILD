#pragma once
#include <stdint.h>
// Czyste C++: gate stanowi JEDNO zrodlo walidacji polecen w programie i testach.
struct CommandGate {
 bool remoteEnabled;
 uint8_t state, mode, level;
 uint32_t uptimeMs, sinceOffMs, bootGraceMs, minRestartMs;
};
static inline bool canQueueCommand(char op, const CommandGate& g){
 if(!g.remoteEnabled || g.mode!=0)return false;
 switch(op){
  case 'S': return (g.state==0 || g.state==6) && g.uptimeMs>=g.bootGraceMs && g.sinceOffMs>=g.minRestartMs;
  case 'X': return g.state==1;
  case 'U': return g.state==1 && g.level>=1 && g.level<6;
  case 'D': return g.state==1 && g.level>1 && g.level<=6;
  case 'A': return g.state==0 || g.state==1 || g.state==6;
  default: return false;
 }
}
