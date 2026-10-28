import hashlib
import json
from pathlib import Path, PurePosixPath
import runpy
import shutil
import tempfile
from urllib.request import urlopen


def blob_hash(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def acquire_assets(lock, cache):
    for relative, digest in lock["files"].items():
        parts = PurePosixPath(relative).parts
        if not parts or relative.startswith("/") or "\\" in relative or ":" in relative or ".." in parts:
            raise ValueError("Unsafe dependency path")
        path = cache.joinpath(*parts)
        if path.exists():
            if blob_hash(path.read_bytes()) != digest:
                raise ValueError(f"Wake dependency checksum mismatch: {relative}")
            continue
        url = f"https://raw.githubusercontent.com/{lock['repository']}/{lock['ref']}/{relative}"
        print(f"Downloading ESP-SR {relative}")
        with urlopen(url, timeout=30) as response:
            data = response.read(10 * 1024 * 1024 + 1)
        if len(data) > 10 * 1024 * 1024 or blob_hash(data) != digest:
            raise ValueError(f"Wake dependency size/checksum mismatch: {relative}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def prepare(project, build):
    lock = json.loads((project / "tools" / "wake_assets.json").read_text(encoding="utf-8"))
    cache = project / ".pio" / "wake" / ("esp-sr-" + lock["ref"])
    acquire_assets(lock, cache)
    build.mkdir(parents=True, exist_ok=True)
    model = lock["model"]
    if not model or any(char not in "abcdefghijklmnopqrstuvwxyz0123456789_" for char in model):
        raise ValueError("Invalid wake model name")
    with tempfile.TemporaryDirectory(prefix="wake-model-", dir=build) as temporary:
        models = Path(temporary)
        destination = models / model
        destination.mkdir()
        for name in ("_MODEL_INFO_", "wn9_data", "wn9_index"):
            shutil.copyfile(cache / "model" / "wakenet_model" / model / name, destination / name)
        packer = runpy.run_path(str(cache / "model" / "pack_model.py"))
        packer["pack_models"](str(models))
        data = (models / "srmodels.bin").read_bytes()
    if not data or len(data) > 0x80000:
        raise ValueError("Wake model does not fit its 512 KiB partition")
    image = build / "wake_models" / "srmodels.bin"
    image.parent.mkdir(parents=True, exist_ok=True)
    if not image.exists() or image.read_bytes() != data:
        image.write_bytes(data)
    digest = hashlib.sha256(data).digest()
    generated = build / "wake_generated"
    generated.mkdir(parents=True, exist_ok=True)
    header = (
        "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
        f'static constexpr char kWakeModelName[] = "{model}";\n'
        f"static constexpr size_t kWakeModelSize = {len(data)};\n"
        "static constexpr uint8_t kWakeModelSha256[] = {"
        + ",".join(str(value) for value in digest) + "};\n"
    )
    target = generated / "wake_model_metadata.h"
    if not target.exists() or target.read_text(encoding="ascii") != header:
        target.write_text(header, encoding="ascii")
    return cache, image, generated


if "Import" in globals():
    Import("env")
    project = Path(env.subst("$PROJECT_DIR"))
    build = Path(env.subst("$BUILD_DIR"))
    cache, image, generated = prepare(project, build)
    env.Append(
        CPPPATH=[
            str(cache / "include" / "esp32s3"),
            str(cache / "src" / "include"),
            str(cache / "test_apps" / "esp-sr" / "main" / "samples"),
            str(generated),
        ],
        LIBPATH=[str(cache / "lib" / "esp32s3")],
        LIBS=["wakenet", "dl_lib", "c_speech_features", "hufzip",
              "esp_audio_processor", "espressif__esp-dsp"],
        FLASH_EXTRA_IMAGES=[("0x770000", str(image))],
    )
    env.BuildSources(
        str(build / "wake_model_loader"), str(cache / "src"),
        src_filter=["+<model_path.c>"],
    )
