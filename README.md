# FoloToy AI Passport Claude Buddy

这是面向 ESP32-C3 FoloToy AI Passport 的 Claude Hardware Buddy 固件。设备使用现有
BSP、LVGL 和 ESP-IDF 原生 API，通过加密 BLE Nordic UART Service 与 Claude
Desktop 通信，在屏幕上显示会话状态、近期消息和工具审批请求。

Hardware Buddy API 仍是实验性接口，不是 Claude Desktop 的正式支持功能。使用前
必须在 Claude Desktop 中开启开发者模式（Help → Troubleshooting → Enable
Developer Mode）。

## 硬件与功能

- ESP32-C3（无 PSRAM）
- ST7789P3 LCD，240 × 320，竖屏
- 三个 ADC 按键：`UP`、`DOWN`、`OK`
- CW2017 电量计（若硬件不可用，状态页显示 `--`）
- 加密 BLE LE Secure Connections、绑定和设备端取消配对
- 原版 18 种像素 Buddy，可在 Settings → Buddy 切换；每种均支持 sleep、idle、
  busy、attention、celebrate、heart 和 pairing/confirmation 动画
- 针对 240 × 320 竖屏重排的原版风格像素 HUD、Pet、Info、菜单与审批面板

设备默认以 `Claude-<MAC 后缀>` 广播。协议使用公开 Hardware Buddy wire protocol：

| 项目 | UUID |
| --- | --- |
| Nordic UART Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| RX（桌面 → 设备） | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| TX（设备 → 桌面） | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

RX、TX 和 TX CCCD 需要加密访问。收发数据是以换行分隔的 UTF-8 JSON；30 秒没有
有效心跳时，设备清除当前审批并回到离线状态。

## 配对与自动重连

1. 在 Claude Desktop 打开 **Help → Troubleshooting → Enable Developer Mode**。
2. 打开 **Developer → Open Hardware Buddy…**，点击 **Connect**。
3. 从设备列表选择 `Claude-*`。首次连接若系统请求蓝牙权限，请允许。
4. 设备会显示六位 Secure Connections passkey；在操作系统的蓝牙提示框中输入设备
   显示的六位数字。
5. 加密完成后设备自动回到 Buddy 页面。已绑定主机在双方唤醒且 BLE 开关开启时
   会自动重连，不需要再次输入配对码。

如果找不到设备，请先按任意键唤醒，并在设备 **Settings → BLE** 确认蓝牙已开启。

## 按键与页面

### 普通页面

- `UP` 短按：按原版顺序循环 Normal、Pet 和 Info。
- `DOWN` 短按：Normal 中滚动近期消息，Pet 中循环 2 页，Info 中循环 6 页。
- `OK` 长按：打开原版浮层菜单；用 `UP`/`DOWN` 选择、`OK` 执行。
- Normal 显示 Buddy 和近期消息；Pet 显示 token/等级及说明；Info 包含 About、
  Buttons、Claude、Device、Bluetooth 和 Credits 六页。

### 审批页面

收到有效审批心跳后会自动切换到审批页，并显示工具名和参数提示。

- `UP` 短按：向上滚动较长的工具参数。
- `OK` 短按：发送一次性批准（`permission/once`）。
- `DOWN` 短按：发送拒绝（`permission/deny`）。

每个审批 ID 只允许一次决定。发送后页面锁定，并显示 `Sending...`、`Sent` 或
`Send failed`；断开、超时或收到新的审批后才会清除该状态。

### Settings 页面

- `UP` / `DOWN`：选择原版的 Brightness、Sound、Bluetooth、Wi-Fi、LED、
  Transcript、Clock rotation、Buddy、Reset 或 Back。
- `OK`：调整亮度、切换 BLE/Transcript，或进入 Reset 子菜单。
- 本板没有对应硬件或尚未移植的 Sound、Wi-Fi、LED 和 Clock rotation
  显示为 `n/a`，选择时只提示不可用，不会伪造状态。
- `Unpair`：确认页中 `OK` 删除 BLE bonds 并取消配对，`DOWN` 取消。
- `Factory reset`：确认页中 `OK` 清除应用设置和统计，`DOWN` 取消。
- `Buddy`：在 18 种内置 Buddy 之间循环；已上传自定义角色时，第 19 项为该角色。
- `Delete character`：删除已上传的自定义角色，并切回内置 Buddy。

## 构建、烧录与监视

项目固定使用 ESP-IDF 5.5.3 和 ESP32-C3：

```bash
get_idf553
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

也可以先通过 ESP-IDF 5.5.3 的 `export.sh` 初始化环境，再执行上述命令。只烧录
而不打开串口监视器时使用 `idf.py flash`。

主机逻辑测试可这样运行：

```bash
get_idf553
cmake -S tests -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

## 自定义角色上传

设备通过上游的 `char_begin`/`file`/`chunk`/`file_end`/`char_end` 命令接收一个
自定义角色。

**格式**

- 角色在主机端预先转换为 BPK1 格式（见 `main/buddy_pack_format.h`），设备不做
  GIF 解码。
- 帧为 4-bit 调色板索引，使用 UI 的 16 色，索引 0 透明。
- 帧宽、高均不超过 120 像素，在舞台区按 1:1 居中显示。

**存储**

- 角色存放在 `partitions.csv` 中的 1 MB `buddy` 数据分区（子类型 0x40）。
- `nvs`、`phy_init`、`factory` 的偏移与大小和原 single-app-large 分区表相同，
  因此 `idf.py flash` 不会清除设置和 BLE bonds。

**传输规则**

- 只接受名为 `pack.bin` 的单个文件。其他文件（例如 Claude Desktop 拖放的
  GIF）会被拒绝，并提示先在主机端转换。
- `char_begin` 先擦除旧角色的首扇区；之后边写边按扇区擦除。
- `char_end` 校验头部和数据的 CRC-32，通过后才显示新角色。传输中断、断开或
  校验失败时不会留下可加载的半成品。
- 上传成功后自动切换到新角色。
- 静态内存增加约 4.3 KB，主要是 3 KB 的分块缓冲。

**主机工具**

配套的主机工具 claude-buddy-bridge 提供两个命令：

- `pack_builder.py`：把 PNG 精灵表或上游角色包（`manifest.json` + GIF）转换为 BPK1。
- `buddyd.py push`：通过 BLE 上传。

## 当前限制

- 只保存一个自定义角色。
- 设备端不解码 GIF；Claude Desktop 拖放的 GIF 角色包需先在主机端转换。
- 没有 IMU，因此不会触发摇晃或翻面（`dizzy` 状态没有本机触发）。
- 设备不保存近期消息、工具参数、审批 ID 或会话内容到 Flash。

## 板级验收清单

请在真实 FoloToy AI Passport 和开启开发者模式的 Claude Desktop 上逐项记录 `PASS`、
`FAIL` 或 `NOT RUN`，不要把未执行的硬件项目当作通过：

- [ ] ESP-IDF 5.5.3 全新配置、编译成功，且无项目源文件警告。
- [ ] 启动完成 NVS、BSP 和 NimBLE 初始化；设备以 `Claude-*` 广播。
- [ ] 未连接时显示 sleep；连接、空闲、忙碌、审批、庆祝和 heart 状态正确切换。
- [ ] Claude Desktop 开发者模式能发现设备，首次连接显示正确六位配对码。
- [ ] 配对后状态显示加密，重启设备后已绑定主机自动重连。
- [ ] Settings → Unpair 删除绑定；下一次连接需要新的配对码。
- [ ] 状态页的电池、电压、连接安全状态和统计字段正确，或明确显示不可用。
- [ ] 工具名和参数提示可查看；批准一次、拒绝和发送失败均安全且各只发送一次。
- [ ] 审批期间快速重复按键、断开连接和 30 秒心跳超时都不会产生额外决定。
- [ ] Normal、Pet 2 页、Info 6 页、长按菜单、Settings、Reset 和确认流程均可操作。
- [ ] 至少重复连接/断开 20 次，并持续连接 30 分钟；无看门狗、BLE/分配错误或
      持续下降的可用堆。

## 项目结构

```text
components/bsp/     FoloToy AI Passport 板级驱动和公开接口
main/               Claude Buddy 应用、BLE、协议、状态、设置和 LVGL UI
main/buddy_pack*    自定义角色格式、上传状态机和 Flash 存储
tests/              无硬件的协议、状态、BLE、角色上传和应用逻辑测试
partitions.csv      分区表（含 1 MB buddy 角色分区）
sdkconfig.defaults  ESP32-C3、LVGL 和应用默认配置
```

## 归属

本项目与 Anthropic 的 [`claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy)
公开 Hardware Buddy BLE wire protocol 兼容。协议说明、UUID 和 JSON 行为以其公开
参考资料为准；实现使用 ESP-IDF 原生 API 编写。

本仓库没有复制或改编上游源代码、角色画面、GIF 或其他艺术资源，因此没有把上游
代码或艺术资源作为本项目文件重新分发；仓库原有的许可状态保持不变。上游仓库的
许可证和第三方资源说明见其 [`LICENSE`](https://github.com/anthropics/claude-desktop-buddy/blob/main/LICENSE)。
