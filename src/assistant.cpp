#include <Arduino.h>
#include <audio_output.h>
#include <cloud_chat.h>
#include <cloud_connection.h>
#include <assistant_dialogue.h>
#include <microphone.h>
#include <sense_input.h>
#include <speech_recorder.h>
#include <tts_client.h>
#include <wake_engine.h>
#include <wake_ack.h>
#include <algorithm>
#include <utility>

namespace {
enum class State { Wake, Waiting, Recording, Stt, Camera, Llm, Tts, Playing, Error, Paused };
State state = State::Paused;
sense::Microphone microphone;
sense::WakeEngine wake;
assistant::SpeechRecorder recorder;
assistant::History history;
int16_t samples[2048] = {};
bool ready = false;
uint32_t stateAt = 0;
uint32_t windowAt = 0;
uint32_t lastStatus = 0;
uint32_t wakeCount = 0;
uint32_t questionCount = 0;
uint32_t completedCount = 0;
uint32_t wakeFrames = 0;
uint32_t slowFrames = 0;
uint32_t maxInferUs = 0;
uint32_t lastVadLog = 0;
unsigned nextAck = 0;
char command[2049] = {};
size_t commandSize = 0;
bool commandInvalid = false;

const char* name(State value) {
    switch (value) {
        case State::Wake: return "WAKE_LISTEN";
        case State::Waiting: return "WAIT_SPEECH";
        case State::Recording: return "RECORDING";
        case State::Stt: return "STT";
        case State::Camera: return "CAMERA";
        case State::Llm: return "LLM";
        case State::Tts: return "TTS_STREAM";
        case State::Playing: return "PLAYING";
        case State::Error: return "ERROR";
        case State::Paused: return "PAUSED";
    }
    return "INVALID";
}

void phase(State next) {
    Serial.printf("STATE %s -> %s ms=%lu previous_ms=%lu\n",
                  name(state), name(next), millis(), millis() - stateAt);
    state = next;
    stateAt = millis();
    digitalWrite(5, next == State::Error || next == State::Waiting ||
                    next == State::Stt || next == State::Camera || next == State::Llm || next == State::Tts);
    digitalWrite(6, next == State::Waiting || next == State::Recording || next == State::Playing);
    digitalWrite(43, next == State::Wake || next == State::Stt || next == State::Camera ||
                     next == State::Llm || next == State::Tts || next == State::Playing);
}

void status() {
    Serial.printf("STATUS state=%s ready=%d mic=%d wifi=%d AMP=%d live_wakes=%u "
                  "questions=%u completed=%u history_rounds=%u history_bytes=%u "
                  "wake_frames=%u max_infer_us=%u slow_frames=%u free_psram=%u free_heap=%u\n",
                  name(state), ready, microphone.active(), WiFi.status() == WL_CONNECTED,
                  digitalRead(44), wakeCount, questionCount, completedCount,
                  static_cast<unsigned>(history.rounds()), static_cast<unsigned>(history.bytes()),
                  wakeFrames, maxInferUs, slowFrames, ESP.getFreePsram(), ESP.getFreeHeap());
}

void fail(const char* reason, bool fatal = false) {
    microphone.stop();
    recorder.stop();
    digitalWrite(44, LOW);
    Serial.printf("ERROR %s; no automatic API retry; failed turn NOT added to history\n", reason);
    if (fatal) {
        ready = false;
    }
    phase(State::Error);
}

bool enterWake() {
    microphone.stop();
    recorder.stop();
    digitalWrite(44, LOW);
    if (!wake.reset() || !microphone.begin()) {
        fail("cannot restore wake microphone/model", true);
        return false;
    }
    phase(State::Wake);
    Serial.printf("READY say=%s; s=status p=pause r=resume c=clear_history; ENTER required\n",
                  wake.word());
    return true;
}

bool openWindow(const char* reason, uint32_t openedAt) {
    if ((!microphone.active() && !microphone.begin()) || !recorder.begin(openedAt)) {
        fail("cannot start voice activity detection", true);
        return false;
    }
    windowAt = openedAt;
    phase(State::Waiting);
    Serial.printf("WAIT_OPEN reason=%s start_ms=%u deadline_ms=%u wait_ms=5000 "
                  "VAD=mode3 frame_ms=30 attack_ms=90 silence_ms=900 max_utterance_ms=20000\n",
                  reason, openedAt, openedAt + assistant::SpeechGate::kWaitMs);
    return true;
}

void onWake(const char* source) {
    Serial.printf("WAKE source=%s word=%s ms=%lu\n", source, wake.word(), millis());
    if (!strcmp(source, "microphone")) {
        ++wakeCount;
    }
    microphone.stop();
    recorder.stop();
    const auto& clip = wake_ack::kClips[nextAck];
    Serial.printf("WAKE_ACK index=%u source=FLASH network=0 voice=%s\n", nextAck, wake_ack::kSpeaker);
    cloud::printSafe("WAKE_ACK_TEXT", clip.text);
    phase(State::Playing);
    if (!speaker::playPcm(clip.audio, clip.bytes)) {
        fail("local wake acknowledgement playback failed");
        return;
    }
    const uint32_t ended = millis();
    nextAck = wake_ack::next(nextAck);
    Serial.printf("WAKE_ACK_DONE next=%u playback_end_ms=%u history_unchanged=1\n", nextAck, ended);
    openWindow("wake_ack_complete", ended);
}

bool answerQuestion(cloud::Text question) {
    microphone.stop();
    recorder.stop();
    digitalWrite(44, LOW);
    if (!question || !*question) {
        fail("empty question");
        return false;
    }
    const uint32_t started = millis();
    ++questionCount;
    cloud::printSafe("USER", question.get());
    if (!cloud::connectWifi() || !cloud::syncTime()) {
        fail("network/time unavailable");
        return false;
    }
    cloud::Text reply;
    {
        phase(State::Camera);
        cloud::Text image;
        if (!sense::captureJpeg([&image](const uint8_t* data, size_t bytes) {
                image = cloud::dataUri("data:image/jpeg;base64,", data, bytes);
                if (!image) {
                    Serial.println("ERROR image encoding/allocation failed");
                }
                return image != nullptr;
            })) {
            fail("fresh image unavailable; refusing to substitute an old picture");
            return false;
        }
        Serial.printf("IMAGE turn=%u fresh=1 stored_in_history=0 relevance=LLM_FROM_USER_INTENT\n",
                      questionCount);
        auto request = assistant::dialogueRequest(history, question.get(), image.get());
        image.reset();
        if (!request) {
            fail("dialogue request allocation failed");
            return false;
        }
        phase(State::Llm);
        Serial.printf("CONTEXT previous_rounds=%u bytes=%u current_image_only=1\n",
                      static_cast<unsigned>(history.rounds()), static_cast<unsigned>(history.bytes()));
        if (!cloud::postChat("LLM", kLLM_API_URL, kLLM_API_KEY, kTtsRootCa, request.get(), reply)) {
            fail("LLM request failed");
            return false;
        }
    }
    if (!history.accepts(question.get(), reply.get())) {
        fail("turn exceeds history byte budget; refusing inconsistent spoken history");
        return false;
    }
    uint32_t playbackEnded = 0;
    {
        phase(State::Tts);
        cloud::printSafe("ASSISTANT", reply.get());
        if (!speakTts(reply.get())) {
            fail("TTS/playback failed; partial audio possible, history unchanged");
            return false;
        }
        playbackEnded = millis();
    }
    if (!history.commit(std::move(question), std::move(reply))) {
        fail("history commit failed");
        return false;
    }
    ++completedCount;
    Serial.printf("TURN_DONE turn=%u duration_ms=%lu playback_end_ms=%u history_rounds=%u\n",
                  questionCount, millis() - started, playbackEnded, static_cast<unsigned>(history.rounds()));
    return openWindow("playback_complete", playbackEnded);
}

bool processRecording(cloud::Bytes pcm, size_t bytes, const char* source) {
    microphone.stop();
    recorder.stop();
    phase(State::Stt);
    Serial.printf("AUDIO source=%s bytes=%u duration_ms=%u upload=STT_ONLY\n",
                  source, static_cast<unsigned>(bytes), static_cast<unsigned>(bytes / 32));
    if (!cloud::connectWifi() || !cloud::syncTime()) {
        fail("network/time unavailable before STT");
        return false;
    }
    cloud::Text question;
    const bool transcribed = cloud::transcribe(pcm.get(), bytes, question);
    pcm.reset();
    if (!transcribed) {
        fail("STT failed or empty; no image/LLM/TTS request");
        return false;
    }
    if (assistant::ignorableTranscript(question.get())) {
        fail("STT returned only a silence/noise annotation; no image/LLM/TTS request");
        return false;
    }
    return answerQuestion(std::move(question));
}

void dispatch(const char* line) {
    if (!strcmp(line, "s")) {
        status();
    } else if (!strcmp(line, "p")) {
        microphone.stop();
        recorder.stop();
        digitalWrite(44, LOW);
        phase(State::Paused);
    } else if (!strcmp(line, "c")) {
        history.clear();
        Serial.println("HISTORY cleared");
    } else if (!ready) {
        Serial.println("REJECT hardware initialization failed; fix error then reset board");
    } else if (!strcmp(line, "r")) {
        enterWake();
    } else if (*line) {
        Serial.println("REJECT unknown command; use s/p/r/c");
    }
}

void pollSerial() {
    while (Serial.available()) {
        const char ch = static_cast<char>(Serial.read());
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            command[commandSize] = 0;
            if (commandInvalid) {
                Serial.println("REJECT serial line contains control bytes or exceeds 2048 bytes");
            } else {
                dispatch(command);
            }
            commandSize = 0;
            commandInvalid = false;
        } else if (static_cast<unsigned char>(ch) < 32 || commandSize >= sizeof(command) - 1) {
            commandInvalid = true;
        } else if (!commandInvalid) {
            command[commandSize++] = ch;
        }
    }
}
}  // namespace

void setup() {
    digitalWrite(44, LOW);
    pinMode(44, OUTPUT);
    for (uint8_t pin : {5, 6, 43}) {
        digitalWrite(pin, LOW);
        pinMode(pin, OUTPUT);
    }
    Serial.begin(115200);
    const uint32_t started = millis();
    while (!Serial && millis() - started < 3000) {
        delay(10);
    }
    Serial.println("Xiao D teaching assistant: local WakeNet/VAD; questions -> STT; "
                   "each valid question + NEW photo -> Kimi; reply -> TTS");
    if (!psramFound()) {
        fail("PSRAM required", true);
        return;
    }
    cJSON_Hooks hooks = {ps_malloc, free};
    cJSON_InitHooks(&hooks);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(kWIFI_SSID, kWIFI_PASSWORD);
    if (!wake.begin()) {
        fail("WakeNet initialization failed", true);
        return;
    }
    ready = true;
    Serial.printf("MODEL name=%s word=%s rate=16000 samples=%d threshold=%.3f\n",
                  kWakeModelName, wake.word(), wake.frameSamples(), wake.threshold());
    enterWake();
    status();
}

void loop() {
    pollSerial();
    if (state == State::Error) {
        if (ready && millis() - stateAt >= 1000) {
            enterWake();
        } else {
            delay(10);
        }
        return;
    }
    if (!ready || state == State::Paused) {
        delay(10);
        return;
    }
    if (state == State::Wake) {
        if (!microphone.read(samples, wake.frameSamples())) {
            fail("wake microphone read failed");
            return;
        }
        const uint32_t started = micros();
        const int detected = wake.detect(samples);
        const uint32_t elapsed = micros() - started;
        maxInferUs = std::max(maxInferUs, elapsed);
        slowFrames += elapsed > static_cast<uint32_t>(wake.frameSamples()) * 1000000 / 16000;
        ++wakeFrames;
        if (detected > 0) {
            onWake("microphone");
        }
    } else if (state == State::Waiting || state == State::Recording) {
        const uint32_t frameAt = millis();
        if (!microphone.read(samples, assistant::SpeechRecorder::kSamples)) {
            fail("VAD microphone read failed");
            return;
        }
        using E = assistant::SpeechGate::Event;
        const auto event = recorder.feed(samples, frameAt);
        if (event == E::Started) {
            phase(State::Recording);
            Serial.printf("SPEECH_START elapsed_from_window_ms=%lu preroll_ms=300\n", millis() - windowAt);
        } else if (event == E::Complete) {
            const size_t bytes = recorder.size();
            Serial.printf("SPEECH_END reason=silence900ms bytes=%u speech_ms=%u\n",
                          static_cast<unsigned>(bytes), recorder.speechMs());
            auto audio = recorder.take();
            processRecording(std::move(audio), bytes, "microphone");
        } else if (event == E::Timeout) {
            Serial.printf("WAIT_TIMEOUT elapsed_ms=%lu; no audio/photo/API upload\n", millis() - windowAt);
            enterWake();
        } else if (event == E::TooShort) {
            Serial.println("VAD_REJECT brief sound; no upload; retaining original 5s deadline");
            if (millis() - windowAt < assistant::SpeechGate::kWaitMs) {
                openWindow("brief_sound_retry", windowAt);
            } else {
                enterWake();
            }
        } else if (event == E::TooLong) {
            fail("utterance exceeded 20 seconds; discarded instead of silently truncated");
        }
        if ((state == State::Waiting || state == State::Recording) && millis() - lastVadLog >= 1000) {
            lastVadLog = millis();
            unsigned peak = 0;
            for (size_t i = 0; i < assistant::SpeechRecorder::kSamples; ++i) {
                peak = std::max(peak, static_cast<unsigned>(abs(static_cast<int>(samples[i]))));
            }
            Serial.printf("VAD frames=%u voice_frames=%u peak=%u raw_dc=%.1f raw_ac_rms=%.1f\n",
                          recorder.frames(), recorder.voiceFrames(), peak, recorder.mean(), recorder.acRms());
        }
    }
    if (millis() - lastStatus >= 15000) {
        lastStatus = millis();
        status();
    }
    delay(1);
}
