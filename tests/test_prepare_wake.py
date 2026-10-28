import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "prepare_wake.py"
spec = importlib.util.spec_from_file_location("prepare_wake", MODULE_PATH)
prepare_wake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_wake)


class WakeAssetsTests(unittest.TestCase):
    def test_git_blob_hash(self):
        data = b"sample"
        self.assertEqual(prepare_wake.blob_hash(data),
                         hashlib.sha1(b"blob 6\0sample").hexdigest())

    def test_verified_download_and_cache(self):
        data = b"sample"
        lock = {"repository": "espressif/esp-sr", "ref": "v1.7.1",
                "files": {"dir/file": prepare_wake.blob_hash(data)}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(prepare_wake, "urlopen", return_value=io.BytesIO(data)) as get:
                prepare_wake.acquire_assets(lock, root)
                self.assertEqual((root / "dir" / "file").read_bytes(), data)
                prepare_wake.acquire_assets(lock, root)
                self.assertEqual(get.call_count, 1)

    def test_corrupt_cache_rejected(self):
        lock = {"files": {"file": prepare_wake.blob_hash(b"correct")}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "file").write_bytes(b"wrong")
            with self.assertRaisesRegex(ValueError, "checksum"):
                prepare_wake.acquire_assets(lock, root)

    def test_bad_download_not_saved(self):
        lock = {"repository": "espressif/esp-sr", "ref": "v1.7.1",
                "files": {"file": prepare_wake.blob_hash(b"expected")}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(prepare_wake, "urlopen", return_value=io.BytesIO(b"wrong")):
                with self.assertRaisesRegex(ValueError, "checksum"):
                    prepare_wake.acquire_assets(lock, root)
            self.assertFalse((root / "file").exists())

    def test_unsafe_paths(self):
        for name in ("../escape", "/absolute", "folder\\escape", "C:/escape", ""):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                with self.assertRaisesRegex(ValueError, "Unsafe"):
                    prepare_wake.acquire_assets({"files": {name: "x"}}, Path(directory))

    def test_packaging_isolated_and_metadata_matches(self):
        self.check_packaging(b"packed model")

    def test_model_size_limits(self):
        for data in (b"", b"x" * (0x80000 + 1)):
            with self.subTest(size=len(data)), self.assertRaisesRegex(ValueError, "fit"):
                self.check_packaging(data)

    def check_packaging(self, data):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            (project / "tools").mkdir()
            lock = {"repository": "espressif/esp-sr", "ref": "v1.7.1",
                    "model": "wn9_hilexin", "files": {}}
            (project / "tools" / "wake_assets.json").write_text(json.dumps(lock))
            source = project / ".pio" / "wake" / "esp-sr-v1.7.1" / "model"
            source = source / "wakenet_model" / "wn9_hilexin"
            source.mkdir(parents=True)
            names = {"_MODEL_INFO_", "wn9_data", "wn9_index"}
            for name in names:
                (source / name).write_bytes(b"fixture")
            build = project / "build"
            stale = build / "wake_models" / "old_model"
            stale.mkdir(parents=True)
            (stale / "untrusted").write_bytes(b"not part of the model")

            def pack(models):
                root = Path(models)
                self.assertEqual({path.name for path in root.iterdir()}, {"wn9_hilexin"})
                self.assertEqual({path.name for path in (root / "wn9_hilexin").iterdir()}, names)
                (root / "srmodels.bin").write_bytes(data)

            with patch.object(prepare_wake.runpy, "run_path", return_value={"pack_models": pack}):
                _, image, generated = prepare_wake.prepare(project, build)
            self.assertEqual(image.read_bytes(), data)
            header = (generated / "wake_model_metadata.h").read_text()
            self.assertIn(f"kWakeModelSize = {len(data)};", header)
            self.assertIn(",".join(str(value) for value in hashlib.sha256(data).digest()), header)
            self.assertEqual(list(build.glob("wake-model-*")), [])

    def test_partition_layout(self):
        text = (MODULE_PATH.parents[1] / "partitions_wake.csv").read_text()
        end = 0x9000
        for line in text.splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            name, _, _, offset, size, _ = (field.strip() for field in line.split(","))
            offset, size = int(offset, 0), int(size, 0)
            self.assertGreaterEqual(offset, end)
            self.assertEqual(offset % 0x1000, 0)
            self.assertEqual(size % 0x1000, 0)
            end = offset + size
            self.assertLessEqual(end, 8 * 1024 * 1024)
            if name == "model":
                self.assertEqual((offset, size), (0x770000, 0x80000))


if __name__ == "__main__":
    unittest.main()
