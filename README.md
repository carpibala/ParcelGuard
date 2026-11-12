## ParcelGuard Photo Notification Board
For the main board, select ESP32 DevKit V1. For the camera, select ESP32-S3 Dev Module (16 MB Flash, OPI PSRAM). Both boards must use the matching versions provided in this folder, because the ESP-NOW packets include photo event types.

## RGB Wiring
Common-cathode RGB module: R to D14, G to D26, B to D27, and the common cathode to GND. The code uses active-high to turn on each color: red for alarm, green for protection, and blue for waiting. Each color channel needs a current-limiting resistor; if the module does not have built-in resistors, connect a resistor in series with R, G, and B respectively. The buzzer is still connected to D25.

## Behavior
Starting with an empty platform does not take a photo.

Place phone A. After the weight stabilizes, take one photo, and Telegram sends a “📦 新增包裹” notification with that photo.

Then place phone B. After the weight stabilizes, take another photo and send the corresponding photo notification. The same applies when continuing with C.

When the weight drops abnormally without a valid RFID card being swiped, take one photo and send a “🚨 包裹狀態異常（未授權取出）” notification with the photo.

After an alarm, swipe the registered owner card. The red LED and buzzer stop immediately, and the blue LED stays on. After about 30 seconds, if the platform is empty, the state returns to WAITING, and the blue LED remains on. Swiping an unregistered card will not clear the alarm.

The existing text notifications for valid RFID authorization and invalid cards remain unchanged. When an authorized removal occurs, take a photo and save it to the camera SD card, following the original setting of not sending it to Telegram.

Photos are first saved to the camera SD card, then uploaded by the camera. If the upload is not completed, the main board attempts to resend the original text notification 12 seconds after an alarm and 30 seconds after placement; the camera continues retrying the upload of the original photo and will not retake a photo for the same event. If the photo upload succeeds after the text fallback, you may receive two notifications: a text notification and a photo notification.

## Testing
Power both boards and insert the microSD card into the camera. Ensure the platform is empty, then start the main board; the main board performs empty-platform zeroing on every reboot.

Place A and wait for the weight to stabilize, then place B and wait for the weight to stabilize, and finally remove A and B without RFID authorization.

Under normal conditions, Telegram should receive three notifications with photos. The three photos should correspond to A, A+B, and the empty platform respectively; three JPGs should also be saved under /photos/ on the camera SD card.

During an alarm, swipe the registered owner card. The main board serial output should show RFID UID = ..., STATE -> AUTHORIZED, and OWNER AUTHORIZED via RFID alarm clear. The red LED and buzzer should stop immediately, and the blue LED should stay on. If it shows NO OWNER CARD or INVALID CARD, first register the owner card with u in the empty-platform WAITING state.

The main board serial output can show PHOTO_REQUEST, PHOTO_CONFIRMED, or PHOTO_FALLBACK_TEXT. Photo upload results are output on the camera serial as PHOTO_TG_SENT or PHOTO_TG_FAILED.

The camera’s PhotoTelegramConfig.h contains the Wi-Fi and Telegram configuration copied from the current main board source code. Keep this project and archive only on your own devices.

# ParcelGuard 照片通知版

主板请选择 ESP32 DevKit V1，相机请选择 ESP32-S3 Dev Module（16 MB Flash、OPI PSRAM）。两块板必须使用本文件夹中的配套版本，因为 ESP-NOW 数据包包含照片事件类型。

## RGB 接线

共阴极 RGB 模块：R 接 D14、G 接 D26、B 接 D27、公共负极接 GND。代码按高电平点亮，对应红色报警、绿色保护、蓝色等待。每个颜色通道需有限流电阻；若模块没有内置电阻，请在 R、G、B 各串接电阻。蜂鸣器仍接 D25。

## 行为

- 空平台启动不会拍照。
- 放手机 A，重量稳定后拍一张，Telegram 发送一条带该照片的“📦 新增包裹”通知。
- 再放手机 B，重量稳定后再拍一张，并发送对应照片通知。继续放 C 也一样。
- 未刷有效 RFID 卡、重量异常下降时，拍一张并发送带照片的“🚨 包裹狀態異常（未授權取出）”通知。
- 报警后刷已登记的主人卡，红灯和蜂鸣器立即停止，蓝灯常亮。约 30 秒后，若平台为空，状态回到 WAITING，蓝灯继续常亮。刷未登记的卡不会解除报警。
- 有效 RFID 授权与无效卡片的原有文字通知照旧。授权取走时拍照并保存到相机 SD 卡，沿用原来的不发送 Telegram 设定。
- 照片先保存到相机 SD 卡，再由相机上传。若上传未完成，主板在报警 12 秒后、放置 30 秒后尝试补发原文字通知；相机继续重试上传原照片，不会为同一事件重新拍照。若补发后照片又成功上传，可能收到文字和照片两条通知。

## 测试

1. 两块板接电、相机插好 microSD。确保平台为空，再让主板启动；主板每次重启都会进行空平台归零。
2. 放 A 并等重量稳定，再放 B 并等重量稳定，最后未经 RFID 授权拿走 A、B。
3. 正常情况下 Telegram 应收到三条**带照片**的通知，三张照片分别对应 A、A+B、空平台；相机 SD 卡 `/photos/` 内也应保存三张 JPG。
4. 报警时刷已登记的主人卡。主板串口应显示 `RFID UID = ...`、`STATE -> AUTHORIZED`、`OWNER AUTHORIZED via RFID alarm clear`，红灯和蜂鸣器应立即停止，蓝灯常亮。如果显示 `NO OWNER CARD` 或 `INVALID CARD`，需先在空平台 WAITING 状态用 `u` 登记主人卡。
5. 主板串口可看 `PHOTO_REQUEST`、`PHOTO_CONFIRMED` 或 `PHOTO_FALLBACK_TEXT`。照片上传结果在相机串口输出 `PHOTO_TG_SENT` 或 `PHOTO_TG_FAILED`。

相机的 `PhotoTelegramConfig.h` 包含从当前主板源码复制的 Wi-Fi 和 Telegram 配置。此工程及压缩包请仅在自己的设备上保存。
