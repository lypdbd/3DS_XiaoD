import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "prepare_wake_ack.py"
spec = importlib.util.spec_from_file_location("prepare_wake_ack", SCRIPT)
prepare_wake_ack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_wake_ack)


class WakeAckTests(unittest.TestCase):
    def make_assets(self, project):
        source = project / "assets" / "wake_ack"
        source.mkdir(parents=True)
        audio = b"\x01\x00\x02\x00"
        manifest = {"schema": 1, "sample_rate": 24000, "format": "pcm_s16le",
                    "speaker": "test-voice", "clips": []}
        for index in range(3):
            (source / f"{index}.pcm").write_bytes(audio)
            manifest["clips"].append({"file": f"{index}.pcm", "text": f"cue{index}",
                                      "bytes": len(audio), "sha256": hashlib.sha256(audio).hexdigest()})
        return source, manifest

    def write_manifest(self, source, manifest):
        (source / "manifest.json").write_text(json.dumps(manifest), encoding="ascii")

    def test_generation_and_no_unnecessary_rewrite(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            source, manifest = self.make_assets(project)
            self.write_manifest(source, manifest)
            generated = prepare_wake_ack.prepare(project, project / "build")
            header = (generated / "wake_ack_metadata.h").read_text()
            self.assertIn('"test-voice"', header)
            self.assertIn("kCount = 3", header)
            output = generated / "wake_ack_assets.cpp"
            code = output.read_text()
            self.assertEqual(code.count("PROGMEM"), 3)
            self.assertIn("0x01,0x00,0x02,0x00", code)
            original_time = output.stat().st_mtime_ns
            prepare_wake_ack.prepare(project, project / "build")
            self.assertEqual(original_time, output.stat().st_mtime_ns)

    def test_missing_assets_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory, self.assertRaisesRegex(ValueError, "Missing wake cues"):
            prepare_wake_ack.prepare(Path(directory), Path(directory) / "build")

    def test_corrupt_asset_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            source, manifest = self.make_assets(project)
            self.write_manifest(source, manifest)
            (source / "1.pcm").write_bytes(b"\x03\x00\x04\x00")
            with self.assertRaisesRegex(ValueError, "checksum"):
                prepare_wake_ack.prepare(project, project / "build")

    def test_invalid_manifest(self):
        changes = (
            lambda manifest: manifest.update(sample_rate=16000),
            lambda manifest: manifest.update(clips=manifest["clips"][:2]),
            lambda manifest: manifest["clips"][0].update(file="..\\other.pcm"),
            lambda manifest: manifest.update(speaker=""),
        )
        for change in changes:
            with self.subTest(change=change), tempfile.TemporaryDirectory() as directory:
                project = Path(directory)
                source, manifest = self.make_assets(project)
                change(manifest)
                self.write_manifest(source, manifest)
                with self.assertRaises(ValueError):
                    prepare_wake_ack.prepare(project, project / "build")

    def test_pcm_constraints(self):
        for audio in (b"", b"\0\0", b"\1", b"RIFF12", b"ID3abc", b"\1\0" * 72001):
            with self.subTest(size=len(audio)), tempfile.TemporaryDirectory() as directory:
                project = Path(directory)
                source, manifest = self.make_assets(project)
                self.write_manifest(source, manifest)
                (source / "0.pcm").write_bytes(audio)
                with self.assertRaisesRegex(ValueError, "PCM16"):
                    prepare_wake_ack.prepare(project, project / "build")


if __name__ == "__main__":
    unittest.main()
