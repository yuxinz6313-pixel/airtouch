@'
# AirTouch 智训板

**AirTouch 智训板** 是一套基于 **ESP32-P4** 的空中无接触交互儿童训练系统，面向儿童反应训练、手眼协调训练、注意力评估与互动教育场景。系统通过摄像头识别空中指针位置，结合本地训练逻辑、成长档案记录、云端数据同步与距离保护机制，实现“无接触交互 — 行为采集 — 表现评估 — 自适应反馈”的完整训练闭环。

本项目为 **COSMIC WIND** 团队参赛作品。

---

## 项目特点

- **空中无接触交互**  
  使用摄像头识别笔尖 / 空中指针位置，无需触摸屏幕即可完成目标选择、悬停确认和训练交互。

- **儿童训练任务设计**  
  内置 Star Catcher 与 Color-Go 两类训练任务，覆盖反应速度、目标捕捉、选择性注意和抑制控制等训练维度。

- **本地成长档案**  
  ESP32-P4 将训练记录保存到 SD 卡，形成儿童本地成长档案，可查看历史表现、趋势图和热力图结果。

- **云边协同闭环**  
  ESP32-P4 负责训练与显示，ESP8266 负责 Wi-Fi 网关、云端上传和参数下发，Cloudflare Worker + D1 负责数据存储、Dashboard 展示和配置管理。

- **距离保护机制**  
  ESP8266 连接 VL53L0X 距离传感器，当儿童距离屏幕过近时，向 ESP32-P4 下发保护事件，P4 显示护眼提示界面。

- **中文图形界面与音效反馈**  
  基于 LVGL 构建中文 UI，包含主菜单、训练页、结果页、成长档案页、形界面与音效反馈**  
  基于 LVGL 构建中文 UI，包含主菜单云端参数同步提示和距离保护弹窗，并支持 WM8960 音频反馈。

---

## 系统架构

```text
ESP32-P4
├─ 摄像头采集
├─ 空中指针识别
├─ LVGL 中文 UI
├─ Star Catcher 训练
├─ Color-Go 训练
├─ SD 本地成长档案
├─ 云端配置读取
├─ UART 与 ESP8266 通信
└─ 距离保护 UI / 音效反馈

ESP8266
├─ Wi-Fi 网络连接
├─ Cloudflare Worker HTTP 通信
├─ P4 UART 数据收发
├─ 训练记录上传
├─ 云端配置拉取
├─ 配置 ACK 上报
└─ VL53L0X 距离保护采集

Cloudflare Worker + D1
├─ 训练记录接收与去重保存
├─ Dashboard 数据展示
├─ Star / Color-Go 参数配置
├─ 配置版本管理
└─ P4 应用状态 ACK 记录
目录结构
airtouch
├─ airtouch_01_p4
│  ├─ main
│  │  ├─ air_aim_trainer_ui.c        # 训练 UI、结果页、成长档案
│  │  ├─ air_pointer.c               # 空中指针融合逻辑
│  │  ├─ air_espdet_probe.cpp        # AI / 摄像头检测链路
│  │  ├─ airtouch_cloud_uart.c       # P4 与 ESP8266 UART 通信
│  │  ├─ airtouch_storage.c          # SD 本地成长档案
│  │  ├─ air_distance_guard_ui.c     # 距离保护 UI
│  │  ├─ app_audio.c                 # 音效控制
│  │  └─ wm8960_codec.c              # WM8960 音频驱动
│  ├─ components
│  ├─ managed_components
│  └─ CMakeLists.txt
│
├─ airtouch_02_esp8266_cloud_tof_guard
│  └─ airtouch_02_esp8266_cloud_tof_guard.ino
│
├─ airtouch_03_cloudflare_worker
│  ├─ src/index.js                   # Worker API 与 Dashboard
│  ├─ schema.sql                     # D1 数据库表结构
│  ├─ schema_config_v2e.sql          # 云端配置表结构
│  ├─ package.json
│  └─ wrangler.toml
│
├─ README.md
├─ FINAL_README.txt
├─ LICENSE
└─ .gitignore
硬件连接
ESP32-P4 与 ESP8266 UART
ESP32-P4 GPIO4  → ESP8266 D5 / GPIO14
ESP32-P4 GPIO5  ← ESP8266 D6 / GPIO12
ESP32-P4 GND    ↔ ESP8266 GND
ESP8266 与 VL53L0X
VL53L0X VIN / VCC → ESP8266 3V3
VL53L0X GND       → ESP8266 GND
VL53L0X SCL       → ESP8266 D1 / GPIO5
VL53L0X SDA       → ESP8266 D2 / GPIO4
VL53L0X XSHUT     → ESP8266 3V3
VL53L0X INT       → 不接
ESP32-P4 与 WM8960
WM8960 VCC    → ESP32-P4 3V3
WM8960 GND    → ESP32-P4 GND
WM8960 SDA    → ESP32-P4 IO46
WM8960 SCL    → ESP32-P4 IO48
WM8960 CLK    → ESP32-P4 IO32
WM8960 WS     → ESP32-P4 IO33
WM8960 RXSDA  → ESP32-P4 IO36
WM8960 TXMCLK → 不接
WM8960 RXMCLK → 不接
编译与烧录
1. ESP32-P4 主工程
cd "C:\Users\zyx\esp\airtouch\airtouch_01_p4"

idf.py build
idf.py -p COM9 -b 460800 app-flash
idf.py -p COM9 monitor
2. ESP8266 云网关与距离保护工程
cd "C:\Users\zyx\esp\airtouch\airtouch_02_esp8266_cloud_tof_guard"

arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 .
arduino-cli upload -p COM6 --fqbn esp8266:esp8266:nodemcuv2 .
arduino-cli monitor -p COM6 --config baudrate=115200
3. Cloudflare Worker
cd "C:\Users\zyx\esp\airtouch\airtouch_03_cloudflare_worker"

npm install
node --check .\src\index.js
npx wrangler deploy
云端接口与数据流

系统通过 ESP8266 将训练记录上传至 Cloudflare Worker，并由 Worker 写入 D1 数据库。Dashboard 可查看训练记录、成长趋势和当前配置状态。

数据流如下：

P4 训练记录
→ UART ATQ
→ ESP8266 HTTP POST
→ Cloudflare Worker
→ D1 数据库
→ Dashboard 展示

Dashboard 参数设置
→ Worker 保存配置版本
→ ESP8266 拉取配置
→ UART CFG 下发 P4
→ P4 写入 CONFIG.TXT
→ CFGACK 回传
→ Worker 更新 applied_version
训练任务
Star Catcher

Star Catcher 用于训练儿童的目标捕捉能力、反应速度和视觉注意稳定性。系统记录命中次数、平均反应时间、最快反应时间、热力图分布和自适应状态。

Color-Go

Color-Go 用于训练儿童的选择性注意和抑制控制能力。儿童需要收集蓝色 Go 目标，并避开红色 No-Go 目标。系统记录正确次数、误触次数、No-Go 抑制表现、准确率和平均反应时间。

本地成长档案

P4 会将训练记录保存到 SD 卡中，并在成长档案页面展示：

1/4 总览
2/4 趋势
3/4 热力图
4/4 自适应反馈

成长档案用于观察儿童训练过程中的反应速度、命中表现、注意分布和阶段性变化。

距离保护

系统使用 VL53L0X 进行距离检测。当检测到儿童距离屏幕过近时，ESP8266 通过 UART 向 P4 发送保护事件：

GUARD,<seq>,<guard_on>,<distance_mm>,<reason>

P4 接收后显示距离保护提示界面，引导儿童保持合适观看距离。

开源说明

本项目代码用于学习、竞赛展示与非商业研究用途。项目中使用到的第三方组件、模型、库文件和示例代码遵循其原始许可证。

如需复现项目，请根据实际硬件连接、串口号、Wi-Fi 环境、Cloudflare Worker 配置和 D1 数据库配置进行调整。

演示视频

演示视频后续补充。

团队

COSMIC WIND

项目名称：

AirTouch 智训板——基于 ESP32-P4 的空中无接触交互的儿童自适应训练板
License

This project is released under the MIT License. See LICENSE for details.
'@ | Set-Content .\README.md -Encoding UTF8

