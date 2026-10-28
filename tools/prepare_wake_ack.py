import hashlib
import json
from pathlib import Path


def write_changed(path: Path, text: str) -> None:
    if not path.exists() or path.read_text(encoding="ascii") != text:
        path.write_text(text, encoding="ascii")


def prepare(project: Path, build: Path) -> Path:
    source = project / "assets" / "wake_ack"
    manifest_path = source / "manifest.json"
    if not manifest_path.exists():
        raise ValueError("Missing wake cues: run python tools\\prepare_wake_audio.py --generate explicitly")
    manifest = json.loads(manifest_path.read_text(encoding="ascii"))
    if (manifest.get("schema") != 1 or manifest.get("sample_rate") != 24000 or
            manifest.get("format") != "pcm_s16le"):
        raise ValueError("Unsupported wake cue format")
    voice = manifest.get("speaker")
    if not isinstance(voice, str) or not voice or len(voice) > 512 or not voice.isascii():
        raise ValueError("Invalid wake cue speaker")
    clips = manifest.get("clips")
    if not isinstance(clips, list) or len(clips) != 3:
        raise ValueError("Exactly three wake cues are required")
    definitions = []
    entries = []
    for index, clip in enumerate(clips):
        if not isinstance(clip, dict) or clip.get("file") != f"{index}.pcm":
            raise ValueError("Invalid wake cue file name")
        text = clip.get("text")
        if not isinstance(text, str) or not text or len(text) > 64:
            raise ValueError("Invalid wake cue label")
        audio = (source / clip["file"]).read_bytes()
        if (not audio or len(audio) % 2 or len(audio) > 24000 * 2 * 3 or not any(audio) or
                audio[:4] == b"RIFF" or audio[:3] == b"ID3"):
            raise ValueError(f"Invalid wake cue PCM16: {index}")
        if len(audio) != clip.get("bytes") or hashlib.sha256(audio).hexdigest() != clip.get("sha256"):
            raise ValueError(f"Wake cue checksum/length mismatch: {index}")
        rows = [",".join(f"0x{value:02x}" for value in audio[start:start + 24])
                for start in range(0, len(audio), 24)]
        definitions.append(f"static const uint8_t audio{index}[] PROGMEM = {{\n" +
                           ",\n".join(rows) + "\n};\n")
        entries.append(f"{{audio{index}, sizeof(audio{index}), {json.dumps(text, ensure_ascii=True)}}}")
    generated = build / "wake_ack_generated"
    generated.mkdir(parents=True, exist_ok=True)
    metadata = (
        "#pragma once\nnamespace wake_ack {\n"
        f"static constexpr char kSpeaker[] = {json.dumps(voice)};\n"
        "static constexpr unsigned kCount = 3;\n}\n"
    )
    code = '#include <Arduino.h>\n#include <wake_ack.h>\nnamespace wake_ack {\n'
    code += "".join(definitions)
    code += "const Clip kClips[kCount] = {\n" + ",\n".join(entries) + "\n};\n}\n"
    write_changed(generated / "wake_ack_metadata.h", metadata)
    write_changed(generated / "wake_ack_assets.cpp", code)
    return generated


if "Import" in globals():
    Import("env")
    project = Path(env.subst("$PROJECT_DIR"))
    build = Path(env.subst("$BUILD_DIR"))
    generated = prepare(project, build)
    env.Append(CPPPATH=[str(project / "include"), str(generated)])
    env.BuildSources(str(build / "wake_ack_objects"), str(generated),
                     src_filter=["+<wake_ack_assets.cpp>"])
