import base64
import importlib.util
import json
from pathlib import Path
import time
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "prepare_wake_audio.py"
spec = importlib.util.spec_from_file_location("prepare_wake_audio", SCRIPT)
audio = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audio)


class WakeAudioTests(unittest.TestCase):
    def sink(self):
        return audio.TtsStream(time.perf_counter())

    def test_split_stream_and_metrics(self):
        sink = self.sink()
        data = b'{"code":0,"data":"AQACAA=="}\r\n{"code":20000000}'
        for byte in data:
            sink.feed(bytes([byte]))
        self.assertEqual(sink.finish(), b"\x01\x00\x02\x00")
        self.assertEqual(sink.body_bytes, len(data))
        self.assertEqual(sink.chunks, 1)
        self.assertLessEqual(sink.first_body_ms, sink.first_pcm_ms)
        self.assertLessEqual(sink.first_pcm_ms, sink.last_pcm_ms)

    def test_invalid_responses(self):
        for message in (
            b'{"code":0,"data":"AQACAA=="}',
            b'{"code":20000000}',
            b'{"code":45000030}',
            b'{"code":false,"data":"AQACAA=="}',
            b'{"code":20000000,"data":"AQ=="}',
            b'{"code":20000000,"data":123}',
            b'{"code":20000000,"data":"???"}',
            b'[]',
            b'{broken}',
            b'{"code":20000000,"data":"AQACAA=="}\n{"code":0}',
        ):
            with self.subTest(message=message), self.assertRaises((ValueError, TypeError)):
                sink = self.sink()
                sink.feed(message)
                sink.finish()

    def test_odd_chunk_boundaries(self):
        sink = self.sink()
        sink.feed(b'{"code":0,"data":"AQ=="}\n{"code":20000000,"data":"AA=="}')
        self.assertEqual(sink.finish(), b"\x01\x00")

    def test_line_limit(self):
        with self.assertRaisesRegex(ValueError, "128 KiB"):
            self.sink().feed(b"x" * audio.TtsStream.line_limit)

    def test_audio_limit(self):
        sink = self.sink()
        sink.audio_limit = 2
        with self.assertRaisesRegex(ValueError, "oversized"):
            sink.feed(json.dumps({"code":20000000,
                                  "data":base64.b64encode(b"\x01\x00\x02\x00").decode()}).encode() + b"\n")


if __name__ == "__main__":
    unittest.main()
