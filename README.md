# 借助第三方大模型翻译MTool导出的未翻译文本(OpenAI兼容）
### 有些API有每分钟请求限制，所以并发量最好不要太高(轮询也是一种好的解决方案)
------
**创建默认配置文件**

```bash
./translation_worker --create-config
```
### 配置参数说明

- `base_url`: 默认deepseek
- `api_key`: 密钥（必填）
- `workers`: 并发工作线程数（默认：20）
- `model`: 模型名称
- `timeout_seconds`: 请求超时时间（默认：60秒）
- `max_retries`:失败重试次数(默认3)
- `max_delay_ms`:最大延迟（默认10000ms)
- `base_delay_ms`:初始延迟(默认1000ms)
