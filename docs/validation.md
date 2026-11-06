# 验证与复现

环境：ESP-IDF 5.5.3、C11 编译器、CMake、zlib、Python 3 + Pillow、Node.js/npm。先按 README 配置 ESP-IDF 环境并完成 `idf.py build`，恢复 `dependencies.lock` 锁定的组件；`IDF_PATH` 供原 SDK 广播序列化器及 Unity 测试定位源码。

## 2026-09-17 翻页笔退出后循环断连修复（未烧录）

日志中的普通广播 → `active=0 bonded=1` → `reason=534` 循环来自固件主动拒绝普通模式下的已配对设备；
随后 MTU `status=7` 是 NimBLE 断连清理，不是需要降档的 MTU 不兼容。

- 保留普通模式下已有配对的 BLE 连接和加密恢复，但不发送 HID 报告、不自动替换配对记录。
- 显式退出及换设备继续先释放输入，再结束连接；迟到的加密/配对事件不能跳过释放与等待阶段。
- 已在关闭的连接不再启动 MTU、连接参数或业务初始化；重复断开请求去重，失败请求仍允许重试。
- MTU 每次连接主动请求一次 256，协商值取双方最小值；断连、host reset 和句柄复用时作废旧回调。
- Host 全量 85 项通过，包括 Clicker 36 项（其中 30 项平台行为测试启用 ASan/UBSan）、广告 29 项和
  MTU 专项 10 项；MTU 测试抽取本地 NimBLE 的真实交换/一次发送标志/最小值计算，验证 23/185/256、
  断连清理、句柄复用和错误回调。另通过 App 68 项、工具 68 项、UI Python 5 项、玩法核心 11 项及 worker 输入套件。
- ESP-IDF 5.5.3 编译通过：应用 2,870,416 B，3 MiB 分区剩余 275,312 B，发布输入余量校验通过。

本节记录为提交前验证，尚未烧录。实机需验证：翻页笔连接并操作 → 返回主页 → 同一手机/小程序重连与同步资料；
正常切换允许出现一次本地主动断连，但不应持续出现 `active=0 bonded=1` 后立即断开。
另检查再次进入翻页笔、换设备及退出时无按键/触控残留。固件仍仅支持一台中央连接，不能同时占用两台手机。

## 2026-09-17 整体审计修复（未烧录）

- JSON：拒绝非法 UTF-8、非有限数字、超出 RTC 范围的时间；数值先夹紧再转换，整条检查后提交，避免半更新。
- RTTTL：有界数字解析、时值/八度/BPM/音符检查；接收、开机读取、用户内容检测均完整校验，不保存仅头部合法的坏曲谱。
- 身份：BLE 特征升级 v2，Key 槽置零；绑定使用完整 64 字节 Key 的百分号编码 URL。绑定页截图在 LVGL 锁内拒绝，缺身份/开页失败保留可操作主页。
- 休眠：仅本机合法完整业务广播续期；翻页笔取消休眠恢复原用途，不恢复过期或已退出的会话。
- 音频：真实 SDK open 失败后清理部分打开状态，保证重试真正重新启用 codec，保留请求静音状态。
- Web Flasher：换包/取消/加载失败立即作废旧包，过滤旧异步结果；连接/烧录锁输入、防重入，固件、身份及台账使用同一快照。
- 容量：SIZE 优化，不改分区/字库；应用 2,870,064 B，3 MiB 分区余量 275,664 B（约 269 KiB、8.8%）。
  相比审计前的 3,005,776 B 减少 135,712 B；静态 DRAM 从 196,922 B 降至 185,646 B。
  发布脚本新增至少 256 KiB 余量门槛，实际五段输入校验通过。
- 测试：工具/App/UI/Host Python 分别 68/68/5/63 项；全服务 Unity 137 项；输入专项 23 项及 59,861 次变异/截断；
  玩法核心 11 项、worker 输入套件、原生 LVGL 20 项、图片生命周期 13 项和 9 种故障注入、截图 6 项；
  Web Flasher 50 项及 TypeScript/Vite 构建通过。ASan/UBSan/float-cast-overflow 用于输入及相关主机回归；
  沙箱不支持 LeakSanitizer，该专项关闭泄漏检测，不代表已完成泄漏检测。

本节记录为提交前验证，尚未烧录。外部小程序不在仓库中，若依赖 v1 蓝牙 Key 读取，必须先适配二维码绑定。
上线前仍需真机确认冷启动/深睡、翻页笔取消休眠后的重新连接、codec 重试、手机扫码和网页真实刷写。
以下为先前验证记录，不能代替本次实机验收。

## PR #10 同步新版主线后的验证

电量修复分支合入主线 `79d1e17`（包含 PR #11）后，保留新版界面、玩法和非零电量文字分段，融合未知电量 `--%`、真实 `0%` 及 CW2017 原始 SOC 采样。

- 应用层 68 项、UI Python 5 项、Host Python 58 项、Unity 服务 124 项通过。
- CW2017 启动测试 3 组、120 个场景通过；原生 LVGL 20 项通过，包含同档百分比刷新、未知电量、真实零电量、相册和冷启动/唤醒回归。
- ESP-IDF 5.5.3 编译通过，应用为 3,005,776 字节，3 MiB 分区剩余 139,952 字节（约 4%）；未修改主线分区布局、资源包或版本号。
- Linux/glibc 下原生 UI 测试使用下方严格 C11 配置，避免测试回调 `on_exit` 与 GNU 扩展同名；没有修改该非电量测试的源代码。
- 本次合并结果尚未烧录，仍需实机验证冷启动、软件重启和深睡唤醒。下方原验证记录及真机记录来自 PR #11，不能替代此次验证。

## 已执行

| 检查 | 结果 |
| --- | --- |
| 原 demo Unity 服务 | 119 项通过，测试正文与断言未改 |
| 玩法 C 核心 | 11 项通过，启用 UBSan |
| 实际 app worker 输入 | 1 套行为测试通过 |
| 原生 LVGL UI | 20 项通过，包含 144 张资源与 PNG 像素一致性 |
| 图片生命周期 | 13 项通过，9 种回退故障全部被检出 |
| Host Python／App／UI／工具 | 58／54／5／65 项通过 |
| 网页刷机工具 | 23 项通过，TypeScript 与 Vite 构建通过 |
| CW2017／截图 | 1／5 项通过 |
| 字体 | 原生 4bpp、完整历史个人资料字符集及许可内容覆盖通过 |
| 固件／发布包 | ESP-IDF 完整编译；五段地址、擦除边界、图包、字体许可及 SHA-256 校验通过 |

另从 Git 暂存内容导出独立源码副本，重跑工具、Host Python、原 demo 服务和玩法核心测试；副本不含历史证据目录、废弃图片或本地构建。LVGL/ESP-IDF 为已锁定的外部依赖。

提交前完整字库检查发现，文案变化导致“制、黑”同时离开快速子集和备用字库。已重新生成备用字库、消除重复字符，并把真实字库检查加入工具回归测试。修正后应用为 **3,004,352 字节**，3 MiB 分区剩余 **141,376 字节**；静态 DRAM **196,876 字节**，资源包 **511,856 字节**。具体二进制哈希随 Git 构建信息变化，以各发布包 manifest 为准。

## 复现命令

在仓库根目录执行：

```sh
python3 -m unittest discover -s tests/host -p 'test_*.py'
python3 -m unittest discover -s components/app/test -p 'test_*.py'
python3 -m unittest discover -s components/ui/presentation/test -p 'test_*.py'
python3 -m unittest discover -s tools -p 'test_*.py'
python3 tests/host/run_demo_services.py
python3 tests/host/run_input_boundaries.py
python3 tests/host/test_identity_activity_regression.py
python3 tools/gen_font_notices.py --check
python3 tools/gen_ui_fonts.py --check
python3 tools/gen_cn_font.py --check

cmake -S tests/host -B build-host-core
cmake --build build-host-core -j8
ctest --test-dir build-host-core --output-on-failure
cmake -S tests/host/app_plays_input -B build-host-input
cmake --build build-host-input -j8
ctest --test-dir build-host-input --output-on-failure
cmake -S tests/host/ui -B build-host-ui -DCMAKE_C_EXTENSIONS=OFF
cmake --build build-host-ui -j8
ctest --test-dir build-host-ui --output-on-failure
cmake -S tests/host/jpeg_view_lifecycle -B build-host-jpeg
cmake --build build-host-jpeg -j8
ctest --test-dir build-host-jpeg --output-on-failure
python3 tests/host/jpeg_view_lifecycle/check_regressions.py build-host-jpeg

bash components/platform/platform_esp32/test/run_screen_capture_lvgl_test.sh
cc -std=c11 -Icomponents/platform/platform_esp32/include \
  -Icomponents/platform/platform_esp32/src \
  components/platform/platform_esp32/test/test_cw2017_profile.c \
  -o build-host-core/test_cw2017_profile
build-host-core/test_cw2017_profile

npm ci --prefix tools/web_flasher
npm --prefix tools/web_flasher test
npm --prefix tools/web_flasher run build
idf.py build size
python3 scripts/release.py
python3 tools/verify_release.py releases/<本次生成的文件>.zip --build-dir build
```

`build-host-*` 均为本地输出，不提交。字体再生成需按 [字体说明](../assets/fonts/README.md)同时刷新 UI 子集与备用字库。资源再生成使用 `python3 tools/build_pixel_assets.py`，其图片预览输出到本地 `docs/evidence/`，不作为真机截图。

## 真机证据及剩余范围

已刷入的 preview.27-beta.1 完成五段校验、身份与 NVS 前后哈希核对、75 秒启动观察及 INFO/ID/BLE/BATT/I2C 检查。已有用户反馈包括 OPPO Find N5 外屏视频翻动、iPhone 17 设置列表双向拖动，以及首页头像恢复；用户试用后同意提 PR。

**本 PR 提交前重新生成的备用字库经过构建与主机测试，未再次刷入设备。** 当前 PR 不声明量产验收或所有手机兼容。仍需测量电流／续航、连接手机并发时堆峰值、稀有昵称渲染和长期操作，以及逐一验证视频应用。手机输入方式与限制见 [蓝牙翻页笔](bluetooth-clicker.md)。
