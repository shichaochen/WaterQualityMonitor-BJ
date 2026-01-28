# ESP32 水质监测系统

基于 ESP32 的多传感器水质监测系统，可实时监测多项水质参数并上传至 ThingSpeak 平台。

## 功能特性

- **水质参数监测**：
  - NH4+（铵离子）浓度（mg/L）
  - 浊度（NTU）
  - 水温（DS18B20）
  - TDS（总溶解固体）浓度（ppm，0-900）

- **环境参数监测**：
  - 空气温度（DHT11）
  - 空气湿度（DHT11）
  - 光照强度（BH1750）

- **数据传输**：
  - WiFi 连接（支持多组配置，自动选择最优网络）
  - ThingSpeak 云端数据上传

- **WiFi 智能管理**：
  - 多组 WiFi 配置（最多 5 组）
  - 按信号强度自动选择最优 WiFi
  - 自动重连机制（路由恢复后自动重连）
  - Web 配置界面（无需修改代码）
  - 连接质量监控

## 硬件配置

### 传感器连接

#### RS485 Modbus 传感器
- **NH4+ 传感器**：
  - Slave ID: 1
  - RX: GPIO 16
  - TX: GPIO 17
  - RE/DE: GPIO 4
  - 使用 Serial2

- **浊度传感器**：
  - Slave ID: 3
  - RX: GPIO 12
  - TX: GPIO 13
  - RE/DE: GPIO 2
  - 使用 SoftwareSerial

#### I2C 传感器
- **BH1750 光照传感器**：I2C 总线（默认 SDA/SCL）

#### 单总线传感器
- **DS18B20 水温传感器**：GPIO 18

#### 数字传感器
- **DHT11 温湿度传感器**：GPIO 5

#### 模拟量传感器
- **TDS 水质传感器**：GPIO 34（ADC1_CH6）
  - 测量范围：0-900 ppm
  - 使用 ESP32 内置 ADC（12位分辨率）

## 软件依赖

### Arduino 库
- `WiFi` (ESP32 内置)
- `WebServer` (ESP32 内置) - WiFi 配置 Web 服务器
- `Preferences` (ESP32 内置) - 保存 WiFi 配置
- `ThingSpeak` - ThingSpeak 通信库
- `ModbusMaster` - Modbus RTU 主站库
- `DHT sensor library` - DHT11/DHT22 传感器库
- `BH1750` - BH1750 光照传感器库
- `OneWire` - 单总线通信库
- `DallasTemperature` - DS18B20 温度传感器库
- `SoftwareSerial` - 软件串口库

### 安装方法
在 Arduino IDE 中通过库管理器安装以下库：
- ThingSpeak
- ModbusMaster
- DHT sensor library
- BH1750
- OneWire
- DallasTemperature

## 配置说明

### WiFi 配置（智能自动配置功能）

系统支持 **多组 WiFi 配置**和**智能自动选择**功能，无需修改代码即可配置 WiFi。

#### 核心功能：
- **多组配置**：最多保存 5 组 WiFi 配置
- **智能选择**：自动按信号强度选择最优 WiFi 连接
- **自动重连**：路由故障恢复后自动重连，无需重启
- **连接监控**：实时监控连接质量和信号强度
- **Web 配置**：通过 Web 界面管理所有 WiFi 配置

#### 自动配置流程：
1. **首次启动**：
   - 系统会尝试连接代码中设置的默认 WiFi（如果存在）
   - 如果连接成功，配置会自动保存
   - 如果连接失败，系统会启动配置模式

2. **配置模式**：
   - 系统会创建一个名为 `ESP32-Config` 的 WiFi 热点（无密码）
   - 使用手机或电脑连接到该热点
   - 在浏览器中访问 `http://192.168.4.1`
   - 在配置页面中：
     - 查看已保存的 WiFi 配置列表
     - 添加新的 WiFi 配置（最多 5 组）
     - 删除不需要的配置
     - 立即尝试连接（按信号强度自动选择最优）
   - 配置会自动保存，下次启动时自动连接

3. **智能连接**：
   - 系统会扫描周围 WiFi 网络
   - 匹配已保存的配置
   - 按信号强度排序（从强到弱）
   - 自动连接信号最强的网络

4. **自动重连**：
   - 路由故障时自动检测并开始重连
   - 路由恢复后立即自动重连
   - 无需重启系统
   - 自动选择信号最强的可用网络

#### 默认 WiFi 配置（可选）：
如果需要设置默认 WiFi（作为备选），可以在代码中修改：
```cpp
const char* default_ssid     = "room-8816-941";
const char* default_password = "04851601";
```

#### Web 配置界面功能：
- `/` - WiFi 配置主页面（添加、查看、删除配置）
- `/status` - 查看系统状态（WiFi 状态、IP、信号强度、已保存配置等）
- `/connect` - 立即尝试连接（按信号强度自动选择）
- `/rescan` - 重新扫描 WiFi 网络
- `/clear` - 清除所有配置

### ThingSpeak 配置
在代码中修改以下参数：
```cpp
unsigned long channelNumber = 1880892UL;
const char* writeAPIKey     = "0UWC02XHIMUUKHGK";
```

### ThingSpeak 字段映射
- Field 1: 空气温度 (°C)
- Field 2: 空气湿度 (%)
- Field 3: 光照强度 (lx)
- Field 4: 水温 (°C)
- Field 5: 浊度 (NTU)
- Field 6: NH4+ 浓度 (mg/L)
- Field 8: TDS 浓度 (ppm)

## 工作流程

1. **初始化阶段**：
   - 加载保存的 WiFi 配置
   - 按信号强度自动选择最优 WiFi 连接
   - 初始化所有传感器
   - 配置看门狗定时器（30秒）

2. **主循环**：
   - WiFi 连接状态监控和自动重连
   - 连接质量监控（每30秒）
   - 读取所有传感器数据
   - 处理异常情况（返回 0 值）
   - 上传数据至 ThingSpeak
   - 等待 5 分钟（300秒）后重复

## 注意事项

- 所有 Modbus 读取操作都有超时保护（默认 800ms）
- 系统配置了看门狗定时器，防止程序卡死
- 传感器读取失败时会输出错误代码并返回 0 值
- WiFi 配置会自动保存到 ESP32 的 NVS（非易失性存储）
- 系统会自动按信号强度选择最优 WiFi，无需手动选择
- 路由故障恢复后，系统会自动重连，无需重启

## 串口监视器

系统通过串口（115200 baud）输出详细的运行信息，包括：
- WiFi 连接状态
- 传感器初始化状态
- 实时传感器读数
- ThingSpeak 上传状态

## 故障排除

- **WiFi 连接失败**：检查 SSID 和密码是否正确
- **传感器读取失败**：检查接线和传感器供电
- **ThingSpeak 上传失败**：检查 API Key 和网络连接
- **看门狗复位**：检查是否有传感器长时间无响应

