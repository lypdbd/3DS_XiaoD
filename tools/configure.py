import json
import os
from pathlib import Path
from urllib.parse import urlsplit
import re

Import("env")

project = Path(env.subst("$PROJECT_DIR"))
values = {}
config_path = project / ".env"
if config_path.exists():
    for line_number, raw in enumerate(
        config_path.read_text(encoding="utf-8-sig").splitlines(), 1
    ):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if not separator or not key.isidentifier():
            raise ValueError(f".env line {line_number}: expected KEY=value")
        if key in values:
            raise ValueError(f".env line {line_number}: duplicate key")
        if value.startswith('"'):
            value = json.loads(value)
            if not isinstance(value, str):
                raise ValueError(f".env line {line_number}: expected a string")
        values[key] = value

wifi = {}
for key in ("WIFI_SSID", "WIFI_PASSWORD"):
    value = os.environ.get(key, values.get(key))
    if value is None:
        raise ValueError(f"Missing {key}: copy .env.example to .env or set environment variables")
    if "\0" in value or "\n" in value or "\r" in value:
        raise ValueError(f"Invalid control character in {key}")
    wifi[key] = value
if not 1 <= len(wifi["WIFI_SSID"].encode("utf-8")) <= 32:
    raise ValueError("WIFI_SSID must contain 1 to 32 UTF-8 bytes")
if len(wifi["WIFI_PASSWORD"].encode("utf-8")) > 63:
    raise ValueError("WIFI_PASSWORD must contain at most 63 UTF-8 bytes")

settings = dict(wifi)
api_keys = ("TTS_API_URL", "TTS_API_KEY", "TTS_RESOURCE_ID", "TTS_SPEAKER",
            "STT_API_URL", "STT_API_KEY", "STT_MODEL",
            "LLM_API_URL", "LLM_API_KEY", "LLM_MODEL")
for key in api_keys:
    value = os.environ.get(key, values.get(key))
    if not value:
        raise ValueError(f"Missing {key}: configure it in .env")
    if any(ord(char) < 32 or ord(char) > 126 for char in value):
        raise ValueError(f"{key} must contain printable ASCII only")
    if len(value) > 512:
        raise ValueError(f"{key} exceeds 512 characters")
    settings[key] = value
if settings["TTS_API_URL"] != "https://openspeech.bytedance.com/api/v3/tts/unidirectional":
    raise ValueError("TTS_API_URL must be the official HTTPS unidirectional endpoint")
stt = urlsplit(settings["STT_API_URL"])
hosts = ("dashscope.aliyuncs.com", "dashscope-intl.aliyuncs.com")
workspace_host = re.fullmatch(
    r"ws-[a-z0-9-]+\.(cn-beijing|ap-southeast-1)\.maas\.aliyuncs\.com",
    stt.hostname or "",
)
if (stt.scheme != "https" or stt.username is not None or stt.password is not None
        or stt.port not in (None, 443) or stt.query or stt.fragment
        or stt.path != "/compatible-mode/v1/chat/completions"
        or not (stt.hostname in hosts or workspace_host)):
    raise ValueError("STT_API_URL must be an official Aliyun HTTPS chat endpoint")
if settings["LLM_API_URL"] != "https://api.moonshot.cn/v1/chat/completions":
    raise ValueError("LLM_API_URL must be the official Kimi HTTPS chat endpoint")
if settings["STT_MODEL"] != "qwen3-asr-flash":
    raise ValueError("This project currently supports STT_MODEL=qwen3-asr-flash")
if settings["LLM_MODEL"] != "kimi-k2.6":
    raise ValueError("This project currently supports LLM_MODEL=kimi-k2.6")

generated = Path(env.subst("$BUILD_DIR")) / "generated"
generated.mkdir(parents=True, exist_ok=True)
header = "#pragma once\n" + "".join(
    f"static constexpr char k{key}[] = {json.dumps(value, ensure_ascii=False)};\n"
    for key, value in settings.items()
)
target = generated / "local_config.h"
if not target.exists() or target.read_text(encoding="utf-8") != header:
    target.write_text(header, encoding="utf-8")
env.Append(CPPPATH=[str(generated)])
