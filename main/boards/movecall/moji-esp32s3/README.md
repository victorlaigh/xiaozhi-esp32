# 编译配置命令

**配置编译目标为 ESP32S3：**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig：**

```bash
idf.py menuconfig
```

**选择板子：**

```
Xiaozhi Assistant -> Board Type -> Movecall Moji 小智AI衍生版
```


**编译：**

```bash
idf.py build
```

## 待机时钟与天气

待机画面显示大号本地时间、IP 粗略定位得到的城市、日期/星期和天气图标/当日高低温。时钟直接使用设备已同步的系统时间，避免重复叠加时区偏移。设备通过 HTTPS 每 60 分钟查询一次 Open-Meteo，并每 12 小时刷新一次 IP 定位；首次定位失败时每 5 分钟重试，已有定位后刷新失败则保留上次城市并在 12 小时后再试。天气请求失败时保留当前运行期间最近一次成功的数据；重启后需联网重新获取。

IP 定位使用 [ipwho.is](https://ipwho.is/)，该服务会看到设备的公网 IP，定位通常只有城市级精度，可能受 VPN、运营商网络和出口位置影响。天气使用无需 API Key 的 [Open-Meteo](https://open-meteo.com/)；屏幕会标注数据来源。Open-Meteo 免费 API 仅限非商业用途，数据按 [CC BY 4.0](https://open-meteo.com/en/licence) 授权；商业用途需使用其付费服务。设备离线或服务不可用时，时间仍可显示，天气保持当前运行期间的上次成功数据。

### 待机样式

在 `idf.py menuconfig` 的 **Xiaozhi Assistant → Movecall Moji Standby Style** 中选择烧录默认样式：`01 - Dark Minimal`、`02 - Weather Atmosphere` 或 `03 - Precision Instrument`。首次启动使用此默认值；运行时设置会保存在 NVS 并覆盖编译默认值。

连接助手后，也可以让它调用屏幕主题 MCP 工具切换样式：

- `moji-minimal`
- `moji-weather`
- `moji-instrument`

设备每小时在独立后台任务中查询天气，不依赖助手调用 MCP；这里的 MCP 主题切换只负责即时更换屏幕样式。

说“试一下自定义工具”可以触发 `self.screen.show_white_square`，在屏幕中央显示白色方块 5 秒后自动关闭。