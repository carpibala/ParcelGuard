#pragma once
#include "Radio.h"
#include "PhotoTelegram.h"
struct CachedShot {
 bool valid, notified;
 uint8_t mac[6], event;
 uint32_t session,id,uploadAttemptAt;
 char path[48];
};
CachedShot recent[16]{};uint8_t recentNext=0;
bool ownerKnown=true;uint8_t ownerMac[6]={0x7C,0xD5,0x44,0x33,0x8C,0x2C};
// The main board follows its Wi-Fi AP; only the camera searches channels.
uint32_t lastOwnerPacket=0, channelChangedAt=0;
uint32_t wifiConnectingUntil=0;
bool ownerChannelLocked=false;
uint8_t searchChannel=6;
void updateWireless(){
 if(!radioReady)return;
 Envelope e;
 for(int n=0;n<12&&xQueueReceive(inbox,&e,0)==pdTRUE;++n){
  if(memcmp(ownerMac,e.src,6))continue;
  if(e.p.type!=DISCOVER && e.p.type!=SHOT)continue;
  lastOwnerPacket=millis();ownerChannelLocked=true;
  if(e.p.type==DISCOVER){radioSend(OFFER,e.src,e.p.session,0,cameraReady&&sdReady);continue;}
  if(e.p.type!=SHOT||memcmp(e.p.target,localMac,6))continue;
  if(ownerKnown&&memcmp(ownerMac,e.src,6))continue;
  if(!ownerKnown){memcpy(ownerMac,e.src,6);ownerKnown=true;}
  CachedShot* cached=nullptr;
  for(auto &c:recent){if(c.valid&&c.session==e.p.session&&c.id==e.p.id&&!memcmp(c.mac,e.src,6)){cached=&c;break;}}
  if(cached){
   // A retry reuses the saved frame. Never photograph a later state for an old id.
   if(!cached->notified && (!cached->uploadAttemptAt || millis()-cached->uploadAttemptAt>=5000)){
    cached->uploadAttemptAt=millis();
    cached->notified=uploadEventPhoto(cached->path,cached->event);
   }
   lastOwnerPacket=millis();
   radioSend(RESULT,e.src,e.p.session,e.p.id,cached->notified);
   Serial0.printf("DUPLICATE_ACK id=%lu notified=%u\n",e.p.id,cached->notified);
   continue;
  }
  if(photosRemaining){Serial0.println("WIRELESS_BUSY: retry later");continue;}
  Serial0.printf("WIRELESS_TRIGGER id=%lu\n",e.p.id);
  char path[48]{};
  bool ok=saveOnePhoto(path,sizeof(path));
  // Saving and checking the JPG can take seconds. Keep this channel long
  // enough for the asynchronous ESP-NOW result transmission to complete.
  lastOwnerPacket=millis();
  // Keep the exact frame for this event even if the Telegram upload fails.
  if(ok){
   auto &c=recent[recentNext++%16];
   c={};c.valid=true;c.session=e.p.session;c.id=e.p.id;c.event=e.p.event;
   snprintf(c.path,sizeof(c.path),"%s",path);memcpy(c.mac,e.src,6);
   c.uploadAttemptAt=millis();
   c.notified=uploadEventPhoto(c.path,c.event);
   ok=c.notified;
  }
  lastOwnerPacket=millis();
  radioSend(RESULT,e.src,e.p.session,e.p.id,ok);
  Serial0.printf("WIRELESS_RESULT id=%lu complete=%u\n",e.p.id,ok);
 }
 // Stay on a working channel; search again if the main board changes AP/channel.
 if(ownerChannelLocked && millis()-lastOwnerPacket>=2500)ownerChannelLocked=false;
 if(WiFi.status()==WL_CONNECTED || (int32_t)(millis()-wifiConnectingUntil)<0)return;
 if(!ownerChannelLocked && millis()-channelChangedAt>=350){
  channelChangedAt=millis();
  wifi_country_t country{};
  uint8_t first=1,last=11;
  if(esp_wifi_get_country(&country)==ESP_OK && country.nchan){
   first=country.schan;last=first+country.nchan-1;
  }
  searchChannel=(searchChannel<first || searchChannel>=last)?first:searchChannel+1;
  esp_wifi_set_channel(searchChannel,WIFI_SECOND_CHAN_NONE);
 }
}
