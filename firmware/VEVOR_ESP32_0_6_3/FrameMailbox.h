#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
// Wywoluj put/take pod ta sama blokada portMUX. Ta struktura NIE wykonuje rozkazow.
struct FrameMailbox {
 uint8_t bytes[32] = {0};
 size_t length=0;
 bool ready=false;
 uint32_t overwritten=0;
 void put(const uint8_t* src,size_t n){
  if(n>0 && !src){ready=false;length=0;return;}
  if(ready)++overwritten;
  length=n;
  memset(bytes,0,sizeof(bytes));
  if(n>0) memcpy(bytes,src,n<sizeof(bytes)?n:sizeof(bytes));
  ready=true;
 }
 bool take(uint8_t* dst,size_t &n){
  if(!ready || !dst)return false;
  n=length;
  memcpy(dst,bytes,n<sizeof(bytes)?n:sizeof(bytes));
  ready=false;
  return true;
 }
};
