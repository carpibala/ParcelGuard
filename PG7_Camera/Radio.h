#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
constexpr uint32_t PG_MAGIC=0x50473731;
constexpr uint8_t DISCOVER=1,OFFER=2,SHOT=3,RESULT=4;
constexpr uint8_t PHOTO_MANUAL=0,PHOTO_FIRST_ARRIVAL=1,PHOTO_PACKAGE_ADDED=2,
                  PHOTO_ABNORMAL_LOSS=3,PHOTO_OWNER_PICKUP=4;
struct __attribute__((packed)) Packet { uint32_t magic,session,id; uint8_t type,ok,event,target[6]; };
struct Envelope {Packet p; uint8_t src[6];};
QueueHandle_t inbox=nullptr;
uint8_t localMac[6], broadcastMac[6]={255,255,255,255,255,255};
bool radioReady=false;
void radioReceive(const esp_now_recv_info_t* info,const uint8_t* data,int len) {
 if(!info || len!=sizeof(Packet) || !inbox)return;
 Envelope e{}; memcpy(&e.p,data,sizeof(Packet)); memcpy(e.src,info->src_addr,6);
 if(e.p.magic!=PG_MAGIC)return;
 if(memcmp(e.p.target,localMac,6)&&memcmp(e.p.target,broadcastMac,6))return;
 xQueueSend(inbox,&e,0);
}
bool radioSend(uint8_t type,const uint8_t* target,uint32_t session,uint32_t id,bool ok=false,uint8_t event=PHOTO_MANUAL) {
 if(!radioReady)return false;
 Packet p{};p.magic=PG_MAGIC;p.type=type;p.session=session;p.id=id;p.ok=ok;p.event=event;memcpy(p.target,target,6);
 esp_err_t err=esp_now_send(broadcastMac,(uint8_t*)&p,sizeof(p));
 if(err!=ESP_OK)Serial0.printf("RADIO_SEND_ERROR=%d\n",err);
 return err==ESP_OK;
}
void initRadio() {
 WiFi.mode(WIFI_STA); WiFi.setSleep(false);
 if(esp_wifi_set_channel(6,WIFI_SECOND_CHAN_NONE)!=ESP_OK){Serial0.println("RADIO_CHANNEL_FAIL");return;}
 esp_wifi_get_mac(WIFI_IF_STA,localMac);
 // SD write/readback blocks the loop briefly; leave room for photo requests.
 inbox=xQueueCreate(32,sizeof(Envelope));
 if(!inbox || esp_now_init()!=ESP_OK){Serial0.println("RADIO_INIT_FAIL");return;}
 esp_now_peer_info_t peer{};memcpy(peer.peer_addr,broadcastMac,6);peer.channel=0; // Follow the current Wi-Fi channel, including the notification AP.
peer.ifidx=WIFI_IF_STA;
 if(esp_now_add_peer(&peer)!=ESP_OK || esp_now_register_recv_cb(radioReceive)!=ESP_OK){Serial0.println("RADIO_PEER_FAIL");return;}
 radioReady=true;
 Serial0.printf("RADIO_OK MAC=%02X:%02X:%02X:%02X:%02X:%02X CHANNEL=6\n",localMac[0],localMac[1],localMac[2],localMac[3],localMac[4],localMac[5]);
}
