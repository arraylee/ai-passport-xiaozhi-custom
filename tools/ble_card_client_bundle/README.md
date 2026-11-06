# TRAE-CARD BLE 上位机独立运行包

本目录可以整体复制到其他电脑。不要只复制 `ble_card_client.py`，它还会导入
`screen_capture_protocol.py`。

## 文件

- `ble_card_client.py`：主程序。
- `screen_capture_protocol.py`：屏幕截图协议解析，必需。
- `requirements-client.txt`：Python 第三方依赖清单，必需。
- `avatar_catalog.json`：固件内置头像名称列表；缺少时程序仍能运行，但下拉列表只能使用默认值。
- `run.bat` / `run.sh`：启动入口。

## 安装与启动

需要 Python 3.10 或更高版本、系统蓝牙和 Tkinter。

Windows：

```bat
py -m pip install -r requirements-client.txt
run.bat
```

Ubuntu/Debian：

```sh
sudo apt install python3-tk bluetooth bluez
python3 -m pip install -r requirements-client.txt
./run.sh
```

macOS：建议使用 python.org 或 Homebrew 提供的带 Tkinter 的 Python，然后执行：

```sh
python3 -m pip install -r requirements-client.txt
./run.sh
```

Linux 下当前用户还需要具备访问蓝牙服务的权限。程序通过 BLE 连接设备，不需要串口驱动。

头像上传时，透明图片编码为 96×156 PNG；不透明图片编码为 96×160 基线 JPEG，设备会
居中裁掉上下各 2px，屏幕上的实际头像区域仍为 96×156。这样可满足 ESP32 硬件 JPEG
解码器对宽高必须为 8 的倍数的要求。
