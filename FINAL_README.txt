AirTouch Final Work

项目名称：
AirTouch 智训板——基于 ESP32-P4 的空中无接触交互的儿童自适应训练板

最终目录：
airtouch_01_p4
- ESP32-P4 主控工程
- 包含主菜单、Star Catcher、Color-Go、成长档案、中文 UI、音效、SD 本地档案、云端配置读取、GUARD 距离保护 UI
- 已修复：中文字库、Star 时间显示、Star/Color-Go 训练页与结果页 UI、成长档案趋势图、热力图探索指标

airtouch_02_esp8266_cloud_tof_guard
- ESP8266 云端网关工程
- 功能：Wi-Fi、Cloudflare 固定 IP HTTP、P4 UART 通信、ATQ 上传、CFG 下发、CFGACK 上报、VL53L0X ToF 距离保护事件发送
- 协议：GUARD,<seq>,<guard_on>,<mm>,<reason>

airtouch_03_cloudflare_worker
- Cloudflare Worker + D1 云端工程
- 功能：训练记录接收、去重保存、Dashboard 展示、参数配置下发、配置 ACK 状态更新
- Color-Go 参数页已隐藏“启用自适应难度”，Star 保留自适应难度

常用编译命令：

P4:
cd C:\Users\zyx\esp\airtouch_airtouch\airtouch_01_p4
idf.py build
idf.py -p COM9 -b 460800 app-flash
idf.py -p COM9 monitor

ESP8266:
cd C:\Users\zyx\esp\airtouch_airtouch\airtouch_02_esp8266_cloud_tof_guard
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 .
arduino-cli upload -p COM6 --fqbn esp8266:esp8266:nodemcuv2 .
arduino-cli monitor -p COM6 --config baudrate=115200

Cloudflare Worker:
cd C:\Users\zyx\esp\airtouch_airtouch\airtouch_03_cloudflare_worker
npx.cmd wrangler deploy

最终联调验收：
1. P4 主菜单正常
2. Star 完整训练，时间 UI 跟随 SD CONFIG.TXT
3. Color-Go 完整训练，结果页正常
4. 成长档案 1/4 到 4/4 正常
5. 云端参数保存后 ESP8266/P4 能同步
6. ToF 距离保护弹窗正常
7. 音效正常


