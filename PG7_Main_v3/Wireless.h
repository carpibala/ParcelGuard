#pragma once
#include "Radio.h"

uint8_t cameraMac[6]={0xC4,0x9E,0x7E,0x00,0x51,0x54};
bool cameraKnown=false,cameraAvailable=false;
uint32_t sessionToken=0,discoveryAt=0,lastCameraSeen=0,nextPhotoId=0,lastPhotoTransmission=0;

// Each stable increase owns its request until the camera confirms that JPG.
// A later increase never waits for an earlier request's lost acknowledgement.
struct PhotoJob {
 uint32_t id, createdAt, sentAt, fallbackAttemptAt;
 const char* kind;
 uint8_t event;
 uint16_t attempts;
 bool pending, fallbackSent;
};
constexpr uint8_t PHOTO_JOB_CAPACITY=8;
PhotoJob photoJobs[PHOTO_JOB_CAPACITY]{};
PhotoJob alarmJob{}; // Reserved so a full ordinary queue cannot delay theft evidence.
constexpr uint32_t PHOTO_RETRY_MS=1200;
constexpr uint32_t PHOTO_EXPIRE_MS=90000;
constexpr uint32_t PHOTO_FALLBACK_RETRY_MS=10000;

uint8_t photoEventCode(const char* kind){
 if(strcmp(kind,"FIRST_ARRIVAL")==0)return PHOTO_FIRST_ARRIVAL;
 if(strcmp(kind,"PACKAGE_ADDED")==0)return PHOTO_PACKAGE_ADDED;
 if(strcmp(kind,"ABNORMAL_LOSS")==0)return PHOTO_ABNORMAL_LOSS;
 if(strcmp(kind,"OWNER_PICKUP")==0)return PHOTO_OWNER_PICKUP;
 return PHOTO_MANUAL;
}

const char* photoNotificationText(uint8_t event){
 if(event==PHOTO_FIRST_ARRIVAL || event==PHOTO_PACKAGE_ADDED)return "📦 新增包裹";
 if(event==PHOTO_ABNORMAL_LOSS)return "🚨 包裹狀態異常（未授權取出）";
 return nullptr; // Authorized pickup and manual captures remain SD-only.
}

void ensurePhotoSession(){
 if(!sessionToken){sessionToken=esp_random();if(!sessionToken)sessionToken=1;}
}

uint8_t pendingPhotoCount(){
 uint8_t count=alarmJob.pending?1:0;
 for(const auto &job:photoJobs)if(job.pending)++count;
 return count;
}

void sendPhotoJob(PhotoJob &job){
 if(!radioSend(SHOT,cameraMac,sessionToken,job.id,false,job.event)){
  // A local send error is not a delivery attempt; retry on the next pass.
  job.sentAt=millis();
  lastPhotoTransmission=job.sentAt;
  Serial0.printf("PHOTO_SEND_ERROR id=%lu type=%s\n",job.id,job.kind);
  return;
 }
 job.sentAt=millis();
 ++job.attempts;
 lastPhotoTransmission=job.sentAt;
 Serial0.printf("PHOTO_REQUEST id=%lu type=%s attempt=%u\n",job.id,job.kind,job.attempts);
}

bool requestWirelessPhoto(const char* kind="MANUAL"){
 if(!radioReady){Serial0.printf("PHOTO_RADIO_FAILED type=%s\n",kind);return false;}
 ensurePhotoSession();
 const uint8_t event=photoEventCode(kind);
 if(strcmp(kind,"ABNORMAL_LOSS")==0){
  if(alarmJob.pending)return true;
  alarmJob={++nextPhotoId,millis(),0,0,kind,event,0,true,false};
  Serial0.printf("PHOTO_URGENT id=%lu type=%s\n",alarmJob.id,kind);
  sendPhotoJob(alarmJob); // Alarm is dispatched before the Telegram call.
  return true;
 }
 for(auto &job:photoJobs){
  if(job.pending)continue;
  job={++nextPhotoId,millis(),0,0,kind,event,0,true,false};
  Serial0.printf("PHOTO_QUEUED id=%lu type=%s waiting=%u\n",job.id,kind,pendingPhotoCount());
  return true;
 }
 Serial0.printf("PHOTO_QUEUE_FULL type=%s NOT_QUEUED\n",kind);
 return false;
}

void showWireless(){
 Serial0.printf("LINK online=%u ready=%u pending=%u alarm_pending=%u\n",
               cameraKnown,cameraAvailable,pendingPhotoCount(),alarmJob.pending);
 for(const auto &job:photoJobs){
  if(job.pending)Serial0.printf("PHOTO_PENDING id=%lu type=%s attempts=%u\n",job.id,job.kind,job.attempts);
 }
}

void finishPhotoJob(PhotoJob &job,bool ok){
 if(ok){
  Serial0.printf("PHOTO_CONFIRMED id=%lu type=%s\n",job.id,job.kind);
  job.pending=false;
 }else{
  // Camera-side errors are retried with the same id. The camera caches only
  // successful captures, so a failed capture can be tried again.
  Serial0.printf("PHOTO_RETRY id=%lu type=%s camera_failed\n",job.id,job.kind);
  job.sentAt=millis()-PHOTO_RETRY_MS;
 }
}

void updatePhotoFallback(PhotoJob &job,uint32_t now){
 const char* msg=photoNotificationText(job.event);
 if(!job.pending || !msg || job.fallbackSent)return;
 const uint32_t waitMs=job.event==PHOTO_ABNORMAL_LOSS?12000:30000;
 if(now-job.createdAt<waitMs)return;
 if(job.fallbackAttemptAt && now-job.fallbackAttemptAt<PHOTO_FALLBACK_RETRY_MS)return;
 job.fallbackAttemptAt=now;
 if(sendTelegramMessage(msg)){
  job.fallbackSent=true;
  Serial0.printf("PHOTO_FALLBACK_TEXT id=%lu type=%s\n",job.id,job.kind);
 }
}

void expirePhotoJob(PhotoJob &job,uint32_t now){
 if(job.pending && now-job.createdAt>=PHOTO_EXPIRE_MS){
  Serial0.printf("PHOTO_FAILED id=%lu type=%s attempts=%u\n",job.id,job.kind,job.attempts);
  job.pending=false;
 }
}

void updateWireless(){
 if(!radioReady)return;
 ensurePhotoSession();
 uint32_t now=millis();
 const uint32_t discoveryInterval=cameraKnown?1000:250;
 if(now-discoveryAt>=discoveryInterval){
  discoveryAt=now;
  radioSend(DISCOVER,cameraMac,sessionToken,0);
 }
 Envelope e;
 for(int n=0;n<12&&xQueueReceive(inbox,&e,0)==pdTRUE;++n){
  if(e.p.session!=sessionToken||memcmp(cameraMac,e.src,6))continue;
  if(e.p.type==OFFER){
   lastCameraSeen=millis();
   if(!cameraKnown)Serial0.println("CAMERA_ONLINE");
   cameraKnown=true;cameraAvailable=e.p.ok;
  }else if(e.p.type==RESULT){
   if(alarmJob.pending&&e.p.id==alarmJob.id){finishPhotoJob(alarmJob,e.p.ok);continue;}
   for(auto &job:photoJobs){
    if(job.pending&&e.p.id==job.id){finishPhotoJob(job,e.p.ok);break;}
   }
  }
 }
 now=millis();
 if(cameraKnown&&now-lastCameraSeen>=8000){
  cameraKnown=false;cameraAvailable=false;Serial0.println("CAMERA_OFFLINE");
 }
 updatePhotoFallback(alarmJob,now);
 for(auto &job:photoJobs)updatePhotoFallback(job,now);
 expirePhotoJob(alarmJob,now);
 for(auto &job:photoJobs)expirePhotoJob(job,now);

 // At most one new transmission per pass; alarm always goes first.
 if(now-lastPhotoTransmission<80)return;
 if(alarmJob.pending && (alarmJob.attempts==0||now-alarmJob.sentAt>=PHOTO_RETRY_MS)){
  sendPhotoJob(alarmJob);return;
 }
 for(auto &job:photoJobs){
  if(job.pending && (job.attempts==0||now-job.sentAt>=PHOTO_RETRY_MS)){
   sendPhotoJob(job);return;
  }
 }
}
