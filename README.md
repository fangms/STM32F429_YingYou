# STM32F429 YingYou / H863

STM32F429IGT6 板级验证工程，屏幕为 GC9503V＋BOE 3.99寸、400×960，经SSD2828桥接。当前版本 **V0.03**，用于排查屏幕竖向色带、偏色和文字失真。

当前分支：`bringup/v0.03-display-diagnostics`。这是首个入库的现有工程基线，保留CubeMX生成配置、HAL/CMSIS依赖及厂家初始化表；没有虚构更早版本的Git提交历史。

## 当前状态

- 软件：显示与SDRAM的ASan/UBSan主机测试通过；Debug/Release均44步，0警告0错误。交付Flash分别43300/30112字节，静态RAM均2968字节。
- 实板：用户已将R111换成10kΩ，V0.02的32MiB自检和桥控制通信通过，但屏幕图像失真。V0.03正在烧录测试，正常画面与长期稳定性尚未验收。
- 范围：点屏和UART5调试日志；屏幕无触摸，外接模块协议暂缓，泵/传感器第二版。

## 工程内容

| 目录/文件 | 用途 |
|---|---|
| `STM32F429IGT6.ioc`、`.mxproject` | CubeMX配置及生成元数据 |
| `Core` | 生成代码、SDRAM/SSD2828/GC9503驱动、显示诊断与日志 |
| `Drivers` | 工程所需HAL/CMSIS，附原许可文件 |
| `cmake`、`CMakePresets.json`、链接脚本/启动文件 | CMake/Ninja/GNU ARM构建配置 |
| `tests/display`、`tests/sdram` | 现有主机测试、HAL模型、厂家命令对照 |
| `tools/build.ps1`、`tools/test.sh` | 构建导出与主机测试入口，输出目录需指定在仓库外 |
| `docs/versions/V0.03` | 初始交付源码/SDK/固件哈希及版本说明 |

后续修改以当前工程为工作区。CubeMX界面由用户手动修改，手写逻辑放在USER CODE块或独立文件；重新生成后检查差异，避免覆盖用户代码。

## Windows构建

沿用已安装STM32Cube环境的GNU14.3.1+st.2、CMake4.3.1+st.1、Ninja1.13.2+st.1。脚本在`%LOCALAPPDATA%/stm32cube/bundles`定位工具，并根据自身位置确定源码目录。

在工程根目录的PowerShell运行，输出路径按你的实际位置调整：

```powershell
./tools/build.ps1 -Configuration Debug -OutputRoot 'F:/AI Works/Projects/03-H863-STM32审查/构建输出_仓库'
./tools/build.ps1 -Configuration Release -OutputRoot 'F:/AI Works/Projects/03-H863-STM32审查/构建输出_仓库'
```

输出包含构建日志、ELF/MAP和带版本号的BIN/HEX及SHA256。版本取`Core/Inc/firmware_version.h`，文件名格式为`H863_V0.03_Release_YYYYMMDD_HHMM.hex`（北京时间）。烧录优先使用HEX。

若在其它环境构建，将`arm-none-eabi-gcc`、CMake、Ninja加入PATH，然后用`cmake --preset Release -B <仓库外构建目录>`及`cmake --build <同一目录>`；工具链差异可能影响产物哈希。

## 主机测试

Linux/WSL需要C编译器及ASan/UBSan运行库。运行以下命令，输出路径放在仓库外：

```sh
sh tools/test.sh '/mnt/f/AI Works/Projects/03-H863-STM32审查/测试输出_仓库'
```

入口使用`-Wall -Wextra -Werror -fsanitize=address,undefined`，依次回归显示和SDRAM测试；成功末尾分别出现`ALL DISPLAY TESTS PASSED`及`ALL TESTS PASSED`。

## V0.03上板测试

复位后先等待完整SDRAM自检与面板初始化，然后自动循环两种边沿，每组约80秒：

1. LTDC背景黑/白/红/绿/蓝，各5秒；两图层关闭，生成图像不取SDRAM帧。
2. 固定单帧黑/白/红/绿/蓝、竖条、横条、棋盘、几何，各5秒。
3. 静态文字页10秒，显示版本及`EDGE RISING`或`EDGE FALLING`。

先RISING（桥B6=0008），后FALLING（B6=2008），随后循环；其余CubeMX时序、DSI速率和面板命令保持。每张FB先禁图层并确认VBlank提交，再写帧并提交启用。切边沿先清B7 bit3（VEN），回读B6后恢复视频。

从复位开始录约3分30秒，比较BG/FB同色及两边沿的清晰度，并保存全新串口日志。UART5：适配器RX接PC12、GND共地、115200/8N1/无流控。黑色图案5秒、文字页静止10秒和切换时短暂变黑属于预定行为。

串口每秒ALIVE，新增STEP/EDGE、视频运行期C6/C7等16寄存器、LTDC图层/时序和帧哈希。检测到扫描、切换/回读或帧变化错误时停止显示、关闭背光/电源，串口保留失败phase。`INIT-SENT`、零错误或帧哈希不变均不代表实际画面验收通过。

## 后续提交

修改后先查看`git diff`，完成相关测试/编译，再提交到当前调试分支。新固件递增版本，并使头文件、日志、屏幕版本与导出文件名一致；确认正常点屏后再安排稳定性验收和后续功能。

本项目应用代码尚未单独声明开源许可。`Drivers`内第三方代码沿用各自`LICENSE.txt`及源文件声明。
