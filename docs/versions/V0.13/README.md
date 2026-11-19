# V0.13 显示里程碑

日期：2026-11-19。用户确认当前第一块主板画面正常，顺时针90°横屏方向正确；据此保存显示调通阶段的代码基线，供后续功能验证及回退。固件版本仍为V0.13，本次不改变已交付运行逻辑。

分支：`milestone/v0.13-display`。标签：`v0.13-display-milestone`。父基线为V0.03提交`894ed53`；V0.04～V0.13为期间累积的工作区改动，本次一次入库，不补造历史提交。

## 相对V0.03的主要代码改动

| 文件/模块 | 最终行为 |
|---|---|
| `Core/Src/main.c` LTDC用户区 | DEPolarity缓存设为AL，同时清GCR.DEPOL，输出有效区高电平；启动前采样PF10提供生成/修正逻辑证据。V0.11实板确认修正后主要暗带、文字/几何重复明显改善 |
| `Core/Src/main.c` GPIO用户区 | PD6面板RESET覆盖为开漏/NOPULL，复位释放依靠外部上拉，避免原3.3V推挽；PD7仍为桥芯片RESET，两者分开 |
| `board_display_diag.c` | 同步两层HAL缓存和BFCR/DCCR，防地址更新重放旧混合参数；保留8页各4秒、FB/BG白、棋盘、灰阶、细线、完整四角和故障停机 |
| `ssd2828_video.c`、`display_config.h` | 从上电固定FALLING/B6=2008；同值边沿请求只读，运行中改边沿返回RESTART_REQUIRED，避免无复位热切换破坏视频 |
| `board_ssd2828.c`、`ssd2828_bus.c` | 增加启动期0C/0A/0B回读取证、长度/ACK/ECC/CRC/BTA校验；取证后真实受控关电/复位、重新Probe和厂家初始化，再开始显示。内部BIST和18寄存器快照为保留诊断接口，默认页不自动调用 |
| `display_surface.h`、`display_test.c`、`boot_screen.c`、`board_screen.c` | 逻辑960×400映射到原生400×960，默认顺时针90°，不交换LTDC/桥物理宽高，不缩放 |
| `board_test.c/.h`、`main.c`、`CMakeLists.txt` | 独立五键测试，低有效/30ms消抖、DOWN/UP日志、计数和高亮；有效按下一次排队80ms短鸣，长按不连发。TIM7 4kHz中断翻转PD3生成2kHz，队列32、30ms音间隔、有DROP计数 |
| `board_test.c`、`boot_screen.c` | 默认固定横屏KEY BEEP TEST页，双SDRAM帧VBlank交换；显示运行时间、启动检查、LTDC错误、雷达/流量原始电平及泵阀命令电平；不自动操作泵阀，不把GPIO状态当功能验收 |
| `tests/display`、`tests/board_test`、`tools/test.sh` | 增加真实GPIO/DE用户区提取、HAL地址/缓存回归、双方向全像素旋转、按钮消抖/队列/定时器/帧所有权/故障注入；原SDRAM回归保留 |

保持原厂家39包、32MiB SDRAM/90MHz/refresh683、LTDC27.1MHz/450×1004/RGB565、DSI2lane/400Mbps等配置。IOC、生成代码区、HAL/CMSIS、链接/启动文件相对V0.03保持；手写修正位于USER区或独立文件。用户可后续手动在CubeMX持久化DE Active Low，不能将物理宽高改成960×400。

## 当前硬件条件及验收边界

- 第一块当前可显示组合：原屏、R111=10kΩ、R119=22Ω、PD6开漏，外部10kΩ上拉到R119输出；用户确认画面正常。本轮22Ω实际电压未提供，不把历史约1.44～1.45V当成新读数。
- 同V0.13换R119=0Ω、用户报告1.8V时背光稳定但图像闪动，换回22Ω恢复。22Ω为调试条件；额定供电、RESET/供电瞬态和真实冷启动/30分钟稳定性未验收。
- 顺时针90°横屏方向已由用户确认，后续沿用。
- 五键屏上高亮/计数、每次实际蜂鸣的实板结果尚未回传；主机测试不等于听到了声音。
- 第二块虽同为22Ω及10kΩ上拉，所附日志实际V0.07，桥ID三次FFFF后主动关显示电源；该板无图仍需独立排查，不能记为两块板均通过。
- 泵/阀、温度/压力/流量采样、Flash业务驱动及H863/语音/TDS/LoRa协议仍按后续阶段安排。

## 本次入库核验

1. 提交前63份工程/测试/配置快照与V0.13交付逐字节一致，103份HAL/CMSIS哈希一致；main生成区相对V0.03保持。本次只更新版本说明和IDE忽略规则，固件代码未再改动。
2. 在现有STM32Cube工具链（GNU14.3.1+st.2、CMake4.3.1+st.1、Ninja1.13.2+st.1）重新构建Debug/Release，各45步，零警告、零错误。Release Flash33736字节、静态内部RAM3120字节。
3. 在WSL Ubuntu-22.04运行`tools/test.sh`，显示/横屏、SDRAM和board_test的ASan/UBSan回归全部通过。详细软件边界见测试源码；不代替实板验收。
4. 两种构建的BIN及HEX与10:10/10:12交付文件精确一致。Release HEX SHA256：`a2591eac0aa18c799ad09ef9d759cf37198f6906f0ed9b1e8e80ea14e3eb2d62`。

`source_sha256.json`覆盖固件源代码、测试、构建入口/配置/IOC等；不含Git内部指针、当前文档或该清单自身。`sdk_sha256.json`保留103依赖精确字节哈希；`firmware_sha256.json`记录原交付和本次重建产物，BIN/HEX可精确核对，ELF含构建路径不强求同哈希。

`.clangd`、`.settings/`保留在本机并忽略；编译输出、私有记录和临时日志不纳入源码提交。构建和测试仍使用根目录现有入口，输出路径必须位于仓库外。

恢复此基线可在当前工作区没有未保存改动时执行`git switch --detach v0.13-display-milestone`，或从标签新建工作分支继续开发。烧录仍使用带V0.13版本号的HEX并校验，硬件接线与供电条件需一并核对。
