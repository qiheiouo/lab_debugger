# JSON 协议描述格式

Phase 3 的协议核心使用严格 JSON。加载时会先做语法检查，再做字段重名、越界、重叠、长度模式和校验范围检查；错误定义不会进入运行态。

应用内可在“协议解析”页直接加载 JSON，并可先选择默认解析器或具体串口/网络 `sourceId`。不同来源可以同时使用不同 JSON 协议，也可以一部分使用 CSV、一部分使用二进制协议；详细语义见 [逐来源解析配置](SOURCE_PARSERS.md)。仓库中的
`examples/protocols/stm32_status.json` 可以作为固定长度 STM32 状态帧模板；加载后，
数值字段会自动接入实时曲线，非数值字段仍会出现在逐帧检查器中。

## 固定长度示例

```json
{
  "name": "stm32_status_v1",
  "maximum_frame_length": 4096,
  "frame": {
    "header": ["0xAA", "0x55"],
    "length": 12
  },
  "fields": [
    {
      "name": "speed",
      "type": "float32",
      "endian": "little",
      "unit": "m/s"
    },
    {
      "name": "voltage",
      "type": "uint16",
      "endian": "little",
      "scale": 0.01,
      "unit": "V"
    },
    {
      "name": "mode",
      "type": "uint8",
      "enum": {
        "0": "idle",
        "2": "run"
      }
    },
    {
      "name": "ready",
      "type": "bool"
    }
  ],
  "checksum": {
    "type": "crc16_modbus",
    "offset": 10,
    "range_start": 0,
    "range_length": 10,
    "endian": "little"
  }
}
```

没有设置 `byte_offset` 时，字段从帧头后依次排列。显式布局可为每个字段设置 `byte_offset`。

## 动态长度示例

```json
{
  "name": "dynamic_message",
  "maximum_frame_length": 65536,
  "frame": {
    "header": [171, 205],
    "length_field": {
      "offset": 2,
      "type": "uint16",
      "endian": "little",
      "adjustment": 0
    }
  },
  "fields": [
    {
      "name": "message_type",
      "type": "uint8",
      "byte_offset": 4
    }
  ],
  "checksum": {
    "type": "crc8_atm"
  }
}
```

长度字段经过 `adjustment` 后必须表示完整帧长度，包括帧头和校验字节。解析器拒绝小于必需字段范围或大于 `maximum_frame_length` 的长度。

## 支持项

- 类型：`uint8`、`int8`、`uint16`、`int16`、`uint32`、`int32`、`float32`、`float64`、`bool`、`byte_array`；
- 字节序：`little`、`big`；
- 变换：`scale`、`value_offset`；
- 元数据：`unit`、`enum`；
- 校验：`sum8`、`crc8_atm`、`crc16_modbus`、`crc16_ccitt_false`；
- 帧：固定帧头、固定长度、`uint8/16/32` 动态长度字段；
- 流处理：半帧、粘包、垃圾字节、CRC 错误与重新同步。

校验未给出 `offset` 时默认位于帧尾；未给出 `range_length` 时默认覆盖从 `range_start` 到校验字段之前的字节。
