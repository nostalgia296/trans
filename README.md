# trans
### 还需要一些完善,比如退出后接续任务等
**创建默认配置文件**

```bash
./translation_worker --create-config
```
### 配置参数说明

- `base_url`: 默认deepseek
- `api_key`: 密钥（必填）
- `workers`: 并发工作线程数（默认：20）
- `model`: 模型名称
- `timeout_seconds`: 请求超时时间（默认：60秒
