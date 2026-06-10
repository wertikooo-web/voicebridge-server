#!/usr/bin/env python3
"""
VoiceBridge — Generate TTS WAV files for ESP32 SPIFFS.

Requirements:
    pip install gTTS pydub
    (ffmpeg must be installed for pydub MP3 decoding)

Usage:
    cd <project_root>
    python tools/gen_phrases.py

Output:
    data/tts/*.wav   (16 kHz, 16-bit, mono PCM)

Upload to ESP32 (ESP-IDF):
    idf.py spiffs-flash
    # or manually:
    # mkspiffs -c data -b 4096 -p 256 -s 0x4F0000 spiffs.bin
    # esptool.py write_flash 0x310000 spiffs.bin
"""

import io, os, sys

OUTPUT_DIR = os.path.join(os.path.dirname(__file__), '..', 'data', 'tts')
os.makedirs(OUTPUT_DIR, exist_ok=True)

TARGET_RATE = 16000

PHRASES = {
    'govoryite':  'Говорите.',
    'zaversheno': 'Связь завершена.',
    'zapis':      'Запись.',
    'otpravleno': 'Отправлено.',
    'vyzyvaju':   'Вызываю помощь. Отпустите кнопку, чтобы отменить.',
    'svyazhetsa': 'Алексей ответит через несколько минут. Он видел ваше сообщение.',
    'wifi_reset': 'Настройки WiFi сброшены.',
}

def convert_to_wav16k(mp3_bytes):
    try:
        from pydub import AudioSegment
    except ImportError:
        print("ERROR: pip install pydub")
        sys.exit(1)
    seg = AudioSegment.from_mp3(io.BytesIO(mp3_bytes))
    seg = seg.set_frame_rate(TARGET_RATE).set_channels(1).set_sample_width(2)
    out = io.BytesIO()
    seg.export(out, format='wav')
    return out.getvalue()

def generate():
    try:
        from gtts import gTTS
    except ImportError:
        print("ERROR: pip install gTTS")
        sys.exit(1)

    print(f"\nVoiceBridge TTS generator → {os.path.abspath(OUTPUT_DIR)}\n")
    for name, text in PHRASES.items():
        out_path = os.path.join(OUTPUT_DIR, f'{name}.wav')
        print(f"  {name}: \"{text}\"")
        try:
            tts = gTTS(text=text, lang='ru', slow=False)
            buf = io.BytesIO()
            tts.write_to_fp(buf)
            wav = convert_to_wav16k(buf.getvalue())
            with open(out_path, 'wb') as f:
                f.write(wav)
            print(f"    → {out_path}  ({len(wav)//1024} KB)")
        except Exception as e:
            print(f"    ERROR: {e}")

    print("\nDone.")
    print("Upload: idf.py spiffs-flash")

if __name__ == '__main__':
    generate()
