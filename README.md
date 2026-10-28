# 小D：XIAO ESP32-S3 Sense 教学语音助手

用于讲解嵌入式软硬件开发的半双工视觉语音助手，使用 PlatformIO + Arduino。
工程只保留最终 `assistant` 环境，不再包含独立灯效、录音、云服务或唤醒调试固件。

交互流程：

**“嗨，乐鑫”唤醒 → 本地短句应答 → 5秒等待开口 → VAD录音断句 → 百炼STT →
新拍一张照片 → Kimi上下文问答 → 火山TTS → 扬声器播放 → 再等5秒开口。**

项目仓库：[lypdbd/3DS_XiaoD](https://github.com/lypdbd/3DS_XiaoD)。
第一次使用请先核对第1节硬件，再按**第3节安装/申请API → 第4节烧录/首次对话**操作。
第5节换音色和第8节自动化测试不是首次使用的必做步骤。

## 1. 硬件

- XIAO ESP32-S3 **Sense**，安装自带摄像头/麦克风扩展板。
- 本项目实机摄像头为 OV5640，采集 VGA JPEG；使用板载 PSRAM。
- 自制载板提供 MAX98357A I2S 功放和共阴 RGB LED，功放使用3.3V供电。
- 仅连接 XIAO 两侧14脚；Sense自带摄像头和PDM麦克风使用内部连接。

| 外设信号 | ESP32-S3 GPIO |
|---|---:|
| MAX98357A DIN / XIAO D0 | 1 |
| MAX98357A BCLK / D1 | 2 |
| MAX98357A LRCLK / D3 | 4 |
| MAX98357A SD_MODE / D7 | 44 |
| RGB 红 / 绿 / 蓝 | 5 / 6 / 43 |
| Sense PDM CLK / DATA | 42 / 41 |

MAX98357A SD_MODE高电平使能并选左槽；扬声器接功放的差分输出，不将任一输出接地。
当前播放为24kHz PCM16，单声道复制到I2S左右槽，**数字音量80%**。
音量在 [lib/speaker/audio_output.cpp](lib/speaker/audio_output.cpp) 中设置，对回答和唤醒短句均生效。

摄像头连接：XCLK10、SDA40、SCL39、D0～D7依次为15/17/18/16/14/12/11/48、
VSYNC38、HREF47、PCLK13。
Arduino 2.x PDM配置使用 `I2S.setAllPins(-1, 42, 41, -1, -1)`，CLK42放在WS参数位置。

[hardware](hardware) 保存XIAO的KiCad符号及DIP/SMD封装参考。
它们不是完整的自制载板原理图或PCB工程；使用前仍需核对引脚、尺寸和底部避让。

## 2. 工程结构

| 路径 | 用途 |
|---|---|
| [src/assistant.cpp](src/assistant.cpp) | 唯一应用入口、交互状态机和串口控制 |
| [include](include) | 唤醒、VAD、相机、云协议、提示词、上下文和证书 |
| [lib/speaker](lib/speaker) | 应用与设备测试共用的I2S播放组件 |
| [assets/wake_ack](assets/wake_ack) | 三段本地应答PCM和完整性清单 |
| [hardware](hardware) | 硬件符号和封装参考 |
| [tools](tools) | 配置生成、依赖/模型打包、本地应答音频准备 |
| [tests](tests) | 主机测试和标准PlatformIO设备回归测试 |
| [third_party](third_party) | 第三方许可 |
| [platformio.ini](platformio.ini) | 唯一 `assistant` 环境及测试配置 |
| [partitions_wake.csv](partitions_wake.csv) | 8MiB Flash分区 |

构建缓存、采集数据、日志和压缩包不是工程源文件，不提交到Git。

## 3. 软件安装与API配置

以下操作以Windows为例。先准备开发板、**能传数据的USB-C线**、2.4GHz Wi-Fi，
以及下述三家平台的API访问权限。STT负责“听懂”，LLM负责“思考和看图”，TTS负责“说话”，
三者是不同服务，密钥不能互相替代。

### 3.1 安装VS Code与插件

1. 从 [VS Code官网](https://code.visualstudio.com/) 下载并安装VS Code。
2. 打开左侧“扩展”，或按 `Ctrl+Shift+X`，搜索并安装
   [PlatformIO IDE](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)。
   确认扩展ID为 `platformio.platformio-ide`，不要装成名称相似的其他扩展。
3. 等待PlatformIO初始化完成，按提示重载VS Code。插件自带PlatformIO Core，
   **不需要另装一份PlatformIO CLI，也不需要用Arduino IDE新建或烧录这个工程**。
4. C/C++编辑、跳转可使用微软的
   [C/C++扩展](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools)；
   若已经安装，无需重复安装。Python扩展不是编译固件的必需项。
5. 使用 `git clone` 下载工程时，先安装 [Git](https://git-scm.com/downloads)，
   并确认终端能运行 `git --version`。不使用Git时，可直接从仓库网页下载ZIP。

插件安装与界面说明见 [PlatformIO官方安装指南](https://docs.platformio.org/en/latest/integration/ide/vscode.html)。
首次初始化、构建会下载工具链和依赖，需要能访问相应下载源；网络慢时查看终端进度和具体错误，
不要通过关闭TLS证书校验解决下载问题。

### 3.2 下载并打开工程

任选一种方式：

- 仓库网页点击 **Code → Download ZIP**，先解压再使用；
- 或在准备存放项目的目录执行：

```powershell
git clone https://github.com/lypdbd/3DS_XiaoD.git
```

在VS Code选择 **文件 → 打开文件夹**，打开能直接看到 `platformio.ini` 的那一层目录。
**不要点击New Project再创建一套空工程，也不要只打开单个源文件。**
等待PlatformIO识别，左侧Project Tasks中应能找到唯一环境 `assistant`。

本工程已经固定板型 `seeed_xiao_esp32s3` 和 `platformio/espressif32@6.13.0`，
对应Arduino ESP32 2.0.17 / ESP-IDF 4.4，ESP-SR固定为v1.7.1。
即使硬件叫Sense，PlatformIO这里的板型名称也仍是 `seeed_xiao_esp32s3`，无需自行换成其他板型或最新版SDK。

### 3.3 创建本机配置文件

在工程根目录打开PowerShell终端，首次执行：

```powershell
Copy-Item .env.example .env
```

**已经填写过 `.env` 时不要再执行复制，以免覆盖真实配置。**
也可以在VS Code文件列表中复制 [.env.example](.env.example)，将副本重命名为 `.env`。
确认文件名不是 `.env.txt`。**真实密钥只填在 `.env`，不要填进要上传Git的 `.env.example`。**

| 配置项 | 填写方式 |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | 自己的2.4GHz Wi-Fi名称和密码 |
| `STT_API_URL` | 下节百炼业务空间对应的完整请求地址 |
| `STT_API_KEY` | 百炼API Key |
| `STT_MODEL` | 保持 `qwen3-asr-flash` |
| `LLM_API_URL` | 保持 `https://api.moonshot.cn/v1/chat/completions` |
| `LLM_API_KEY` | Kimi开放平台API Key |
| `LLM_MODEL` | 保持 `kimi-k2.6` |
| `TTS_API_URL` | 保持 `https://openspeech.bytedance.com/api/v3/tts/unidirectional` |
| `TTS_API_KEY` | 火山语音服务API Key |
| `TTS_RESOURCE_ID` | 保持 `seed-tts-2.0`，并在控制台开通对应服务 |
| `TTS_SPEAKER` | 首次使用保持 `zh_female_qingchezizi_uranus_bigtts`，与本地短音一致 |

格式注意：

- 每行 `KEY=value`，密钥只粘贴值本身，不加 `Bearer `、`X-Api-Key:` 等前缀。
- 支持双引号JSON字符串，例如 `WIFI_SSID="My WiFi"`；不要使用中文引号。
- 不要把说明文字或行尾注释粘到值后面；注释请另起一行，以 `#` 开头。
- 不要重复定义同名项；进程环境变量会覆盖文件中的同名值。
- 修改 `.env` 后需要**重新编译并烧录**，仅保存文件不会更新已经运行的设备。

### 3.4 三家的API去哪里申请

按平台提示完成账号认证、服务开通和计费设置。是否提供免费额度、额度有效期及实际价格以控制台为准，
本工程不保证免费调用。建议为教学项目单独创建密钥，设置可用的限额或费用告警，先少量测试。

#### A. 百炼：语音识别STT

1. 登录 [阿里云百炼](https://bailian.console.aliyun.com/)，首次建议选择**华北2（北京）**，
   与配置模板的地域保持一致。
2. 打开 [百炼API Key管理](https://bailian.console.aliyun.com/cn-beijing/model/settings/api-key)，
   创建或复制API Key。确认其归属业务空间及权限允许调用 `qwen3-asr-flash`。
   若无创建权限，联系账号或业务空间管理员。
3. 在控制台查看该业务空间的ID和接口调用示例。将Key填入 `STT_API_KEY`，
   将完整HTTP请求地址填入 `STT_API_URL`。北京地域模板为：

```dotenv
STT_API_URL=https://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/chat/completions
```

把 `{WorkspaceId}` 整段替换成完整业务空间ID（包含 `ws-` 前缀），不是API Key，也不是空间的显示名称。
**地域、业务空间、密钥要匹配，不能把不同地域的Key和地址混用。**
本项目直接发HTTP请求，需要保留 `/chat/completions`，不要只填SDK的 `base_url`。
`STT_MODEL` 保持 `qwen3-asr-flash`。

官方依据：[获取API Key](https://help.aliyun.com/zh/model-studio/get-api-key)、
[Qwen-ASR接口](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference)。

#### B. Kimi：视觉与上下文问答LLM

1. 登录 [Kimi API开放平台](https://platform.kimi.com/)，不是普通聊天页面。
   旧的Moonshot开放平台链接可能会跳转至此。
2. 进入 [API Keys](https://platform.kimi.com/console/api-keys)，创建并复制密钥，
   填入 `LLM_API_KEY`。
3. 查看控制台的可用额度、模型权限，确认能调用本工程使用的 `kimi-k2.6`。
   API服务的计费与开通状态以开放平台控制台为准。
4. 保持以下两项，不要把控制台网页地址填进API地址：

```dotenv
LLM_API_URL=https://api.moonshot.cn/v1/chat/completions
LLM_MODEL=kimi-k2.6
```

[官方快速入门](https://platform.kimi.com/docs/get-api-key)可能展示更新的模型；
本工程的协议、参数和配置校验针对已验证的 `kimi-k2.6`，不要只照抄新模型名称。
若账号无法使用该模型，应先确认服务可用性；切换其他模型需要同步调整代码并验证。

#### C. 火山引擎：语音合成TTS

1. 登录 [火山语音新版控制台](https://console.volcengine.com/speech/new)，
   找到语音合成服务，按控制台提示开通**豆包语音合成模型2.0**。
2. 在语音服务的API Key管理中创建或取得Key，填入 `TTS_API_KEY`。
   本工程采用请求头 **`X-Api-Key`** 鉴权，不是旧版的AppID/Access Token组合，
   也不要把其他产品的密钥直接当作语音服务Key。
3. 核对该账号/Key拥有 `seed-tts-2.0` 对应的资源权限，确认计费或额度可用。
   **有Key不等于已经开通对应语音合成资源**。
4. 首次使用保持模板中的接口、资源和音色不变：

```dotenv
TTS_API_URL=https://openspeech.bytedance.com/api/v3/tts/unidirectional
TTS_RESOURCE_ID=seed-tts-2.0
TTS_SPEAKER=zh_female_qingchezizi_uranus_bigtts
```

`seed-tts-2.0` 必须搭配2.0支持的音色。想换音色时先在控制台确认支持，再按第5节重新生成本地短音。
如果报权限错误，应同时检查Key、服务资源、音色和控制台开通状态，不能只反复换Key。

官方依据：[新版控制台指引](https://www.volcengine.com/docs/6561/2119699?lang=zh)、
[HTTP单向流式接口与鉴权](https://www.volcengine.com/docs/6561/1598757)。
控制台可能需要登录，页面名称变化时以上述官方说明为准。

### 3.5 烧录前检查

- 三家Key均已填到 `.env`，没有把真实Key填进 `.env.example`。
- Wi-Fi是2.4GHz；`{WorkspaceId}` 已替换，STT地址保留完整路径。
- 首次使用没有随意修改模型、音色、板型或SDK版本。
- 三家服务权限/额度已确认；普通编译和本地唤醒不调用云API，实际云对话会按平台规则计费。

构建脚本将配置生成到被忽略的 `.pio` 目录，不把真实密钥写进源文件。
重复字段、缺少配置、非法端点或模型会明确报错。

可选的 `platformio.local.ini` 仅保存本机工具链路径，不提交到Git。
原开发机器使用 `D:\PlatformIO`；**其他电脑不需要创建这个盘符/目录，也不需要复制这份本机配置**。

## 4. 编译与烧录

### 4.1 连接设备、找串口

1. 用数据线将XIAO的USB口连接电脑，确认Sense扩展板、功放和扬声器已经按第1节连接。
2. 在VS Code底部PlatformIO工具栏打开 **PlatformIO Core CLI** 终端；
   鼠标悬停在图标上可以看到名称。后续 `pio` 命令在这个已配置环境的终端中执行。
3. 在工程目录运行：

```powershell
pio device list
```

找到插入XIAO后新增的串口，也可在Windows“设备管理器 → 端口”中查看。
**下面的 `COM8` 只是开发机器示例，必须换成自己电脑上的实际端口**，不要误选蓝牙串口。

### 4.2 编译

```powershell
pio run -e assistant
```

或者在PlatformIO工具栏选择 **Build**，确保当前环境是 `assistant`。
首次会下载工具链、锁定版本的ESP-SR库和模型，并校验哈希；根据网络状况可能需要较长时间。
等待终端出现 `[SUCCESS]` 后再继续。报错时先看第一条实际错误，而不是只看最后的“编译失败”。

无需自己下载WakeNet模型，也不用单独在Arduino IDE中安装ESP32开发板包。
工程随附默认音色的三段短音，未换音色时不必先运行音频生成脚本。

### 4.3 烧录

先关闭占用串口的监视器、其他串口工具，然后执行：

```powershell
pio run -e assistant -t upload --upload-port COM8
```

也可点击PlatformIO工具栏 **Upload**；连接多个串口设备时，更推荐上面的显式端口命令。
烧录结束应看到写入校验通过以及 `[SUCCESS]`，设备通常会自动复位。

普通Upload会一起写入主程序、分区表和唤醒模型；**不要只烧录 `firmware.bin`**。
不要为了“省步骤”改用拖拽单个BIN/UF2文件的方式。固件启动时还会验证模型分区SHA-256。

若无法识别端口或无法进入下载模式，按 [Seeed官方BootLoader说明](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/#bootloader-mode)：

1. 断开USB，按住板上的 **BOOT** 按钮。
2. 保持按住BOOT接入USB，连接后松开。
3. 重新运行 `pio device list`，用当前端口再执行本工程Upload。
4. 烧录完成若仍未运行，可按一次 **RESET**。不要在正常启动时一直按着BOOT。

这只是恢复下载模式，不需要为了恢复端口再新建、烧录一个无关的Blink工程。

### 4.4 看日志并完成第一次对话

```powershell
pio device monitor --port COM8 --baud 115200
```

也可点击PlatformIO工具栏 **Serial Port Monitor**。重启后端口号若变化，先重新查看端口列表。
如果打开监视器时错过启动日志，输入 `s` 并按回车，可查看当前状态。

正常待机应看到 `state=WAKE_LISTEN`、`ready=1`、`mic=1`、`AMP=0`；
Wi-Fi连接成功后为 `wifi=1`。蓝灯只代表本地唤醒正在工作，不能单凭蓝灯判断云服务已经连通。

1. 音量默认80%，扬声器先不要贴近耳边。确认允许对现场录音、拍照，再开始测试。
2. 说 **“嗨，乐鑫”**。
3. 等它说完“怎么了？”等本地应答，黄灯亮起后，在5秒内说：
   **“你好，简单介绍一下你自己。”**
4. 应依次看到录音、STT、拍照、LLM、TTS和播放相关日志，随后听到回答。
   云请求要等待网络和服务响应，不是每一步都能立即完成。
5. 回答播完后可以继续提问，不必再次唤醒；保持安静约5秒，会回到蓝灯待机。

即使普通聊天，每个有效问题也会新拍一张照片；模型按问题判断是否参考图片。
本地唤醒应答不产生云请求，后续真实问题会调用三家服务。
退出串口监视器可按 `Ctrl+C`；下一次烧录前先退出监视器。

### 4.5 常见问题

| 现象 | 先检查什么 |
|---|---|
| `pio` 不是可识别命令 | 使用PlatformIO自带CLI终端，确认插件初始化完成；不要假设普通系统终端已配置PATH |
| 找不到 `platformio.ini` 或环境 | VS Code和终端应位于工程根目录，而不是上一级目录或 `src` 内 |
| `Missing ...`、配置格式错误 | 检查 `.env` 是否存在、是否误名为 `.env.txt`、Key是否为空；不要填写到模板里 |
| STT地址被拒绝 | 替换 `{WorkspaceId}`，核对地域、完整业务空间ID及 `/chat/completions` 后缀 |
| `Wake cue voice mismatch` | 默认音色先别改；确需换音色，按第5节显式重新生成并烧录，不要绕过检查 |
| 没有串口、一直等待连接或上传失败 | 换数据线/USB口、关闭其他串口程序、确认实际COM号；必要时按4.3节进BOOT模式 |
| `wifi=0` | 检查2.4GHz网络、SSID/密码、信号和网络是否需要网页登录；先解决Wi-Fi再查API |
| HTTP 401/403或权限错误 | 先看是STT、LLM还是TTS；核对对应Key、地域/业务空间、模型/资源权限和账号状态 |
| TTS有Key仍报资源权限错误 | 确认开通 `seed-tts-2.0` 对应服务且音色匹配；有Key或有余额都不单独保证资源授权 |
| HTTP 200但仍报TTS错误 | HTTP成功不等于业务成功；查看业务码及 `ERROR TTS`，不要忽略结束标记或返回空音频 |
| 额度、限流或模型不可用 | 去对应控制台检查余额/配额/模型授权；不要反复重置或盲改模型名，调用可能计费 |
| TLS、时间同步或连接超时 | 检查网络、DNS和时间同步是否可用；不要关闭证书验证或换成HTTP |
| 拍照失败或无声音 | 检查Sense扩展板、相机连接、功放供电与I2S接线；结合第1节引脚及串口错误定位 |
| 回答等待较长、播放时说话没反应 | 这是整段接收后播放的半双工实现；查看 `TTS_TIMING`，等黄灯亮起再说话 |
| 修改Key/密码后设备仍用旧配置 | 重新编译并烧录；只编辑电脑上的 `.env` 不会改变设备Flash |

分区保持NVS及两个应用槽的起始地址不变；`model` 在 `0x770000`，大小512KiB，
SPIFFS在 `0x670000`，大小1MiB。若设备以前使用过不同文件系统布局，应先备份其数据。
本项目不使用SPIFFS存放录音、图片或上下文。

## 5. 本地唤醒应答

每次新唤醒依次播放：

**“怎么了？” → “嗯？” → “在呢。” → 循环**

三段原始音频约0.780、0.603、0.775秒，共103592字节，直接嵌入应用Flash。
唤醒时不发网络请求；连续追问不重复应答，短句不进入LLM历史，重启从第一句开始。
播放成功才前进下标；失败明确报错。

改变 `.env` 中的音色后，在工程目录显式生成：

```powershell
python tools\prepare_wake_audio.py --generate
pio run -e assistant -t upload --upload-port COM8
```

本机也可使用 `D:\PlatformIO\penv\Scripts\python.exe`。准备工具需要Python 3.10+和 `requests`；
本机PlatformIO Python已安装。若其他电脑缺少此包，使用所选Python执行 `python -m pip install requests`。
如果系统没有 `python` 命令，可以使用PlatformIO安装环境里的Python；Windows默认位置通常是
`%USERPROFILE%\.platformio\penv\Scripts\python.exe`，自定义安装以自己的实际路径为准。
这一步只在需要重新生成短音时使用，不是普通编译的前置操作。

`--generate` 会进行**三次计费TTS请求**，全部合成成功后才替换音频和清单。
普通构建不会自动调用计费API。构建会检查格式、长度、非静音、SHA-256及音色一致性；
不一致时请明确重新生成，不要绕过检查或只修改清单里的音色名称。

## 6. 交互与边界

| 灯光 | 状态 |
|---|---|
| 蓝 | 离线等待“嗨，乐鑫” |
| 黄 | 等待用户开口 |
| 绿 | 录音 |
| 紫 | STT、相机、LLM或TTS |
| 青 | 播放唤醒短句或回答 |
| 红 | 错误 |

- 唤醒短句播完后再开始5秒开口窗口。90ms连续VAD确认开口，保留约300ms前滚音频。
- 开口后不受最初5秒截断，连续约900ms静音结束一句；单句最长20秒，超限报错并丢弃。
- 累计VAD人声不足240ms的极短声音会被拒绝；仍有时间时继续等待，不无限延长原窗口。
- 回答实际播放完、DMA排空、功放关闭后，再开5秒窗口，无需再次唤醒；不说话则回到蓝灯。
  麦克风恢复约0.13秒也计入5秒。
- **半双工，不支持打断。** 录音、拍照、云请求和播放互斥，黄灯亮后再说话。
- VAD和STT使用相同的约13Hz高通去直流音频，避免麦克风直流偏置误触发；
  待唤醒WakeNet仍使用原始输入。VAD不能保证排除所有噪声、电视人声或误唤醒。
- 空识别和明确的静音/噪声标注不继续请求LLM；咳嗽可以得到自然关心，但不会据此诊断疾病。

### 上下文与视觉

保留最近4轮完整文字问答，合计最多8192字节UTF-8；超限按整轮淘汰旧记录。
只有TTS实际成功播放后的问答才进入历史。历史跨再次唤醒保留，但重启、断电或串口 `c` 会清空。

每个有效问题重新拍照，仅把当前图片发送给Kimi，历史中只有文字。
相机失败则中止本轮，不使用旧图冒充新图。模型在同一次请求中按用户问题判断图片是否相关，
普通聊天或知识问题应忽略照片；看不清、过暗、被挡住或没拍到时明确说明，不猜丝印或连线。
图中文字不能改写系统指令，旧视觉描述不能当作当前图像证据。

提示词见 [assistant_dialogue.h](include/assistant_dialogue.h)：助手自称“小D”，通常简短中文回答，
关闭thinking，不输出Markdown或思考过程。提示词不能保证零幻觉，教学中应人工核对关键视觉结论。

### 音频、延迟和错误

TTS仍是**完整接收并确认业务结束后再播放**，不是边下载边播放。
保留批量NDJSON处理、严格Base64解码、容量上限，以及正常TLS证书/域名/时钟校验。
首块音频到达不等于开始播放，响应头时间也不等于纯合成时间。

运行日志中的 `TTS_TIMING` 给出首正文、首/末PCM、块间最大间隔与接收处理时间。
`receive_cpu_us` 是累计墙钟时间（可能含调度抢占），`decode_cpu_us` / `base64_cpu_us`
是嵌套子阶段，不能相加。云端持续生成与传输交叠，不能仅凭这些指标精确分离纯网络耗时。

STT/LLM读取超时30秒、接收预算60秒；TTS读取超时15秒、接收预算45秒。
底层阻塞读可能额外等待一次读取超时，这些不是严格端到端上限。
失败明确记录原因，不自动重试计费请求；通常约1秒后回到待唤醒。
硬件或模型无法恢复时停止运行，修复后重启。

## 7. 串口控制

115200波特率，命令以回车/换行提交：

| 命令 | 功能 |
|---|---|
| `s` | 状态、Wi-Fi、功放、内存、唤醒/问答计数及历史大小 |
| `p` | 暂停采集，丢弃尚未上传的录音 |
| `r` | 恢复离线唤醒监听 |
| `c` | 清除文字上下文 |

保留状态、VAD、识别/回答文字、HTTP状态与耗时、新照片、播放和上下文日志。
固件不再提供手动问题、合成测速、模拟唤醒或端到端fixture入口。
网络与播放为同步操作，期间收到的命令会排队，`p` 不是实时紧急打断。

## 8. 回归测试

主机测试不调用云API：

```powershell
python -m unittest discover -s tests -p "test_*.py"
```

设备测试使用同一个 `assistant` 配置下的标准PlatformIO测试，不编入运行固件：

```powershell
pio test -e assistant --upload-port COM8 --test-port COM8 -v
```

首次运行会安装声明的Unity测试依赖。测试不调用云API，不保存录音或照片，
但会在80%音量播放三段本地短音，并采集一次相机画面检查采集返回值。
覆盖原有43项核心检查（含37项TTS解析子检查）、官方唤醒正负样本、实际播放、
播放后麦克风恢复及摄像头采集。官方PCM测试不等于真人唤醒识别率测试。

**设备测试会临时替换应用，测试结束必须重新烧录正式固件：**

```powershell
pio run -e assistant -t upload --upload-port COM8
```

整理后的版本已通过31项主机测试、5项设备测试分组（0失败），
三段实际播放日志均为 `volume=80%`、`AMP=0`、`I2S_RELEASED=1`。
相机释放仍可能出现框架已有的 `gdma_disconnect` 提示；释放及后续采集验证成功，
本项目没有改动框架底层或宣称消除此提示。

## 9. Git与隐私

- 提交源码、配置模板、三段固定短音、模型依赖清单和必要第三方许可。
- **不提交 `.env`、`.pio`、`platformio.local.ini`、录音、照片、日志或生成固件。**
  [.gitignore](.gitignore) 已覆盖这些路径及常见构建产物。
- Git忽略不是加密：编译后的固件及设备Flash仍可能包含Wi-Fi密码和API Key，不发布固件二进制。
- 串口问题/回答、照片、音频本身也可能涉及隐私，演示前获得参与者同意。
- 提交前检查 `git status --short` 和 `git diff --cached --stat`；
  若密钥曾被提交，删除文件并不能清除历史，应立即吊销或轮换密钥。
- 本项目面向教学，不是经过量产认证的商业语音产品。
  ESP-SR及模型须遵守上游授权，保留 [ESP-SR-LICENSE.txt](third_party/ESP-SR-LICENSE.txt)。

## 参考

- [Seeed XIAO ESP32-S3 Sense](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- [PlatformIO VS Code安装与操作](https://docs.platformio.org/en/latest/integration/ide/vscode.html)
- [百炼API Key申请](https://help.aliyun.com/zh/model-studio/get-api-key)
- [Qwen-ASR接口](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference)
- [Kimi API快速入门](https://platform.kimi.com/docs/get-api-key)
- [ESP-SR v1.7.1](https://github.com/espressif/esp-sr/tree/v1.7.1)
- [WakeNet模型](https://github.com/espressif/esp-sr/tree/v1.7.1/model/wakenet_model/wn9_hilexin)
- [火山TTS HTTP单向流式接口](https://www.volcengine.com/docs/6561/1598757)
- [PlatformIO Unity设备测试](https://docs.platformio.org/en/latest/advanced/unit-testing/frameworks/unity.html)
