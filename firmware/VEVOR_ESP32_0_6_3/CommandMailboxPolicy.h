#pragma once
#include <stddef.h>
#include <string.h>
// Wysoki priorytet DISARM > STOP > THERMO:OFF > komendy zwykle.
// Jedna skrzynka zabezpiecza przed nadpisaniem awaryjnej blokady przez STATUS.
static inline unsigned commandPriority(const char* cmd){
  if(strcmp(cmd,"DISARM")==0)return 3;
  if(strcmp(cmd,"STOP")==0)return 2;
  if(strcmp(cmd,"THERMO:OFF")==0)return 1;
  return 0;
}
static inline bool offerCommand(char* mailbox,size_t capacity,const char* incoming){
  if(!mailbox || !incoming || !capacity)return false;
  const size_t n=strlen(incoming);
  if(n==0 || n>=capacity)return false;
  const unsigned current=commandPriority(mailbox), next=commandPriority(incoming);
  if(mailbox[0] && (next<=current))return false;
  memcpy(mailbox,incoming,n+1);
  return true;
}
