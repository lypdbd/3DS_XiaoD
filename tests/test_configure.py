import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "configure.py"
WIFI = "WIFI_SSID=test-network\nWIFI_PASSWORD=test-password\n"
TTS = (
    "TTS_API_URL=https://openspeech.bytedance.com/api/v3/tts/unidirectional\n"
    "TTS_API_KEY=test-not-a-real-key\n"
    "TTS_RESOURCE_ID=seed-tts-2.0\n"
    "TTS_SPEAKER=test-speaker\n"
)
CLOUD = (
    "STT_API_URL=https://ws-test.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/chat/completions\n"
    "STT_API_KEY=fake-stt-key\n"
    "STT_MODEL=qwen3-asr-flash\n"
    "LLM_API_URL=https://api.moonshot.cn/v1/chat/completions\n"
    "LLM_API_KEY=fake-llm-key\n"
    "LLM_MODEL=kimi-k2.6\n"
)


class BuildEnv:
    def __init__(self, root):
        self.root = root
        self.includes = []

    def subst(self, value):
        return {"$PROJECT_DIR": str(self.root), "$BUILD_DIR": str(self.root / "build")}[value]

    def Append(self, **kwargs):
        self.includes.extend(kwargs["CPPPATH"])


class ConfigureTests(unittest.TestCase):
    def generate(self, text=WIFI + TTS + CLOUD, overrides=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".env").write_text(text, encoding="utf-8")
            env = BuildEnv(root)
            namespace = {"env": env, "Import": lambda _: None}
            with patch.dict(os.environ, overrides or {}, clear=True):
                exec(compile(SCRIPT.read_text(encoding="utf-8"), str(SCRIPT), "exec"), namespace)
            self.assertEqual(env.includes, [str(root / "build" / "generated")])
            return (root / "build" / "generated" / "local_config.h").read_text(encoding="utf-8")

    def test_all_settings(self):
        header = self.generate()
        for line in (WIFI + TTS + CLOUD).splitlines():
            self.assertIn(f"k{line.split('=')[0]}[]", header)

    def test_each_api_key_required(self):
        lines = (TTS + CLOUD).splitlines(keepends=True)
        for index in range(len(lines)):
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, "Missing"):
                self.generate(WIFI + "".join(lines[:index] + lines[index + 1:]))

    def test_each_wifi_key_required(self):
        for line in WIFI.splitlines(keepends=True):
            with self.subTest(line=line), self.assertRaisesRegex(ValueError, "Missing WIFI_"):
                self.generate(line + TTS + CLOUD)

    def test_process_override(self):
        header = self.generate(overrides={"TTS_SPEAKER": "override-speaker"})
        self.assertIn('"override-speaker"', header)
        self.assertNotIn('"test-speaker"', header)

    def test_json_escaping(self):
        header = self.generate((WIFI + TTS + CLOUD).replace("test-not-a-real-key", '"a\\\\b\\"c"'))
        self.assertIn('"a\\\\b\\"c"', header)

    def test_wrong_tts_endpoint(self):
        with self.assertRaisesRegex(ValueError, "official HTTPS"):
            self.generate(WIFI + TTS.replace("https://", "http://") + CLOUD)

    def test_header_newline_rejected_without_secret(self):
        with self.assertRaisesRegex(ValueError, "printable ASCII") as error:
            self.generate((WIFI + TTS + CLOUD).replace("test-not-a-real-key", '"hidden\\nheader"'))
        self.assertNotIn("hidden", str(error.exception))

    def test_duplicate_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate key"):
            self.generate(WIFI + TTS + CLOUD + "TTS_API_KEY=duplicate\n")

    def test_oversized_header_rejected(self):
        with self.assertRaisesRegex(ValueError, "512"):
            self.generate((WIFI + TTS + CLOUD).replace("test-speaker", "x" * 513))

    def test_wifi_lengths(self):
        for overrides in ({"WIFI_SSID": ""}, {"WIFI_SSID": "x" * 33},
                          {"WIFI_PASSWORD": "x" * 64}):
            with self.subTest(overrides=overrides), self.assertRaisesRegex(ValueError, "WIFI_"):
                self.generate(overrides=overrides)

    def test_cloud_endpoint_rejections(self):
        replacements = (
            ("https://ws-test.", "http://ws-test."),
            ("https://ws-test.", "https://secret@ws-test."),
            ("cn-beijing.maas.aliyuncs.com", "cn-beijing.maas.aliyuncs.com.evil.test"),
            ("cn-beijing.maas.aliyuncs.com", "cn-beijing.maas.aliyuncs.com:444"),
            ("/compatible-mode/v1/chat/completions", "/compatible-mode/v1"),
            ("/compatible-mode/v1/chat/completions", "/compatible-mode/v1/chat/completions?key=x"),
            ("api.moonshot.cn", "api.moonshot.cn.evil.test"),
        )
        for old, new in replacements:
            with self.subTest(new=new), self.assertRaisesRegex(ValueError, "official"):
                self.generate(WIFI + TTS + CLOUD.replace(old, new))

    def test_cloud_model_rejections(self):
        for model in ("qwen3-asr-flash", "kimi-k2.6"):
            with self.subTest(model=model), self.assertRaisesRegex(ValueError, "currently supports"):
                self.generate(WIFI + TTS + CLOUD.replace(model, "unsupported"))

    def test_cloud_official_legacy_endpoint(self):
        self.generate(WIFI + TTS + CLOUD.replace(
            "ws-test.cn-beijing.maas.aliyuncs.com", "dashscope.aliyuncs.com"))


if __name__ == "__main__":
    unittest.main()
