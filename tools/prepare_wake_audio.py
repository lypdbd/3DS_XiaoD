import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import time

import requests


ACK_TEXTS = ("\u600e\u4e48\u4e86\uff1f", "\u55ef\uff1f", "\u5728\u5462\u3002")
ENDPOINT = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"


def read_settings(project: Path) -> dict[str, str]:
    required = ("TTS_API_URL", "TTS_API_KEY", "TTS_RESOURCE_ID", "TTS_SPEAKER")
    settings = {}
    for raw in (project / ".env").read_text(encoding="utf-8-sig").splitlines():
        key, separator, value = raw.strip().partition("=")
        if not separator or key.strip() not in required:
            continue
        key, value = key.strip(), value.strip()
        if key in settings:
            raise ValueError(f"Duplicate {key}")
        settings[key] = json.loads(value) if value.startswith('"') else value
    for key in required:
        value = os.environ.get(key, settings.get(key))
        if not isinstance(value, str) or not value:
            raise ValueError(f"Missing {key}")
        if len(value) > 512 or any(ord(char) < 32 or ord(char) > 126 for char in value):
            raise ValueError(f"Invalid {key}")
        settings[key] = value
    if settings["TTS_API_URL"] != ENDPOINT:
        raise ValueError("TTS_API_URL must be the official HTTPS endpoint")
    return settings


class TtsStream:
    line_limit = 128 * 1024
    audio_limit = 24000 * 2 * 30

    def __init__(self, started: float):
        self.started = started
        self.pending = bytearray()
        self.audio = bytearray()
        self.ended = False
        self.body_bytes = 0
        self.chunks = 0
        self.first_body_ms = 0.0
        self.first_pcm_ms = 0.0
        self.last_pcm_ms = 0.0
        self.max_pcm_gap_ms = 0.0
        self.decode_us = 0.0

    def feed(self, data: bytes) -> None:
        if time.perf_counter() - self.started > 45:
            raise ValueError("TTS response exceeded 45 seconds")
        if data and not self.body_bytes:
            self.first_body_ms = (time.perf_counter() - self.started) * 1000
        self.body_bytes += len(data)
        self.pending.extend(data)
        while b"\n" in self.pending:
            index = self.pending.index(b"\n")
            if index >= self.line_limit:
                raise ValueError("TTS JSON line exceeds 128 KiB")
            line = bytes(self.pending[:index])
            del self.pending[:index + 1]
            self.consume(line)
        if len(self.pending) >= self.line_limit:
            raise ValueError("TTS JSON line exceeds 128 KiB")

    def consume(self, line: bytes) -> None:
        if line in (b"", b"\r"):
            return
        if self.ended:
            raise ValueError("TTS data after end marker")
        started = time.perf_counter()
        message = json.loads(line)
        if not isinstance(message, dict):
            raise ValueError("TTS response must be an object")
        code = message.get("code")
        if isinstance(code, bool) or not isinstance(code, (int, float)):
            raise ValueError("TTS status must be numeric")
        if code not in (0, 20000000):
            raise ValueError(f"TTS business error code={code}")
        data = message.get("data")
        if data is not None and not isinstance(data, str):
            raise ValueError("TTS data must be Base64 text")
        if data:
            audio = base64.b64decode(data, validate=True)
            if not audio or len(self.audio) + len(audio) > self.audio_limit:
                raise ValueError("Empty or oversized TTS audio chunk")
            self.audio.extend(audio)
            now = (time.perf_counter() - self.started) * 1000
            if not self.chunks:
                self.first_pcm_ms = now
            else:
                self.max_pcm_gap_ms = max(self.max_pcm_gap_ms, now - self.last_pcm_ms)
            self.last_pcm_ms = now
            self.chunks += 1
        self.ended = code == 20000000
        self.decode_us += (time.perf_counter() - started) * 1_000_000

    def finish(self) -> bytes:
        if self.pending:
            self.consume(bytes(self.pending))
            self.pending.clear()
        if not self.ended or not self.audio or len(self.audio) % 2:
            raise ValueError("TTS missing successful end or complete PCM16 audio")
        return bytes(self.audio)


def synthesize(settings: dict[str, str], text: str) -> tuple[bytes, dict]:
    body = {"req_params": {
        "text": text,
        "speaker": settings["TTS_SPEAKER"],
        "audio_params": {"format": "pcm", "sample_rate": 24000},
    }}
    headers = {
        "Content-Type": "application/json",
        "Connection": "close",
        "X-Api-Key": settings["TTS_API_KEY"],
        "X-Api-Resource-Id": settings["TTS_RESOURCE_ID"],
    }
    with requests.Session() as session:
        session.trust_env = False
        started = time.perf_counter()
        sink = TtsStream(started)
        with session.post(ENDPOINT, headers=headers,
                          data=json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
                          stream=True, timeout=(12, 15), allow_redirects=False) as response:
            headers_ms = (time.perf_counter() - started) * 1000
            if response.status_code != 200:
                raise ValueError(f"TTS HTTP {response.status_code}")
            for data in response.iter_content(chunk_size=1024):
                if data:
                    sink.feed(data)
            audio = sink.finish()
        metrics = {
            "client": "PC_direct", "voice": settings["TTS_SPEAKER"], "http": 200,
            "text": text, "headers_ms": round(headers_ms, 3),
            "first_body_ms": round(sink.first_body_ms, 3),
            "first_pcm_ms": round(sink.first_pcm_ms, 3),
            "last_pcm_ms": round(sink.last_pcm_ms, 3),
            "total_ms": round((time.perf_counter() - started) * 1000, 3),
            "decode_us": round(sink.decode_us, 1),
            "max_pcm_gap_ms": round(sink.max_pcm_gap_ms, 3),
            "body_bytes": sink.body_bytes, "pcm_bytes": len(audio),
            "chunks": sink.chunks, "audio_ms": len(audio) / 48,
        }
    return audio, metrics


def prepare_ack(project: Path, settings: dict[str, str]) -> None:
    clips = []
    results = []
    for index, text in enumerate(ACK_TEXTS):
        audio, metrics = synthesize(settings, text)
        if len(audio) > 24000 * 2 * 3 or not any(audio):
            raise ValueError("Wake acknowledgement is silent or longer than 3 seconds")
        name = f"{index}.pcm"
        clips.append({"file": name, "text": text, "bytes": len(audio),
                      "sha256": hashlib.sha256(audio).hexdigest()})
        results.append((name, audio))
        print(json.dumps(metrics, ensure_ascii=True))
    manifest = {"schema": 1, "sample_rate": 24000, "format": "pcm_s16le",
                "speaker": settings["TTS_SPEAKER"], "resource": settings["TTS_RESOURCE_ID"],
                "clips": clips}
    output = project / "assets" / "wake_ack"
    output.mkdir(parents=True, exist_ok=True)
    for name, audio in results:
        (output / name).write_bytes(audio)
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=True, indent=2) + "\n",
                                         encoding="ascii")
    print("WAKE_ACK_ASSETS_READY count=3 sample_rate=24000")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate three local wake cues using billable TTS requests")
    parser.add_argument("--generate", action="store_true", required=True,
                        help="Explicitly authorize three TTS requests and replace local wake cues")
    parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    settings = read_settings(project)
    prepare_ack(project, settings)


if __name__ == "__main__":
    main()
