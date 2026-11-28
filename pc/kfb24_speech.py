"""Windows text-to-speech for board playback.

The bridge speaks through the board speaker, not the PC: replies are rendered
with the built-in SAPI voices into a 16 kHz mono 16-bit WAV and streamed to the
firmware as IMA ADPCM blocks. System.Speech is part of Windows, so this needs
no extra Python packages.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import wave
from pathlib import Path
from typing import Optional

SAMPLE_RATE = 16000


def _ps_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def synthesize(text: str, voice: str = "", rate: int = 0,
               timeout: float = 30.0) -> Optional[bytes]:
    """Render ``text`` to raw 16 kHz mono s16le PCM.

    Returns None when PowerShell or the voice is unavailable. ``rate`` is the
    SAPI rate (-10..10); 0 is the voice default.
    """
    text = (text or "").strip()
    if not text or os.name != "nt":
        return None
    rate = max(-10, min(10, int(rate)))
    with tempfile.TemporaryDirectory(prefix="kfb24-tts-") as temp:
        work = Path(temp)
        text_path = work / "text.txt"
        wav_path = work / "reply.wav"
        script_path = work / "speak.ps1"
        text_path.write_text(text, encoding="utf-8")
        script = f"""
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.Rate = {rate}
$voice = {_ps_quote(voice)}
if ($voice) {{ $synth.SelectVoice($voice) }}
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
    {SAMPLE_RATE},
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono)
$synth.SetOutputToWaveFile({_ps_quote(str(wav_path))}, $format)
$body = [System.IO.File]::ReadAllText({_ps_quote(str(text_path))},
                                      [System.Text.Encoding]::UTF8)
$synth.Speak($body)
$synth.Dispose()
"""
        script_path.write_text(script, encoding="utf-8-sig")
        try:
            result = subprocess.run(
                ["powershell.exe", "-NoProfile", "-NonInteractive",
                 "-ExecutionPolicy", "Bypass", "-File", str(script_path)],
                capture_output=True, timeout=timeout,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
        if result.returncode != 0 or not wav_path.is_file():
            return None
        try:
            with wave.open(str(wav_path), "rb") as wav:
                if (wav.getnchannels() != 1 or wav.getsampwidth() != 2 or
                        wav.getframerate() != SAMPLE_RATE):
                    return None
                return wav.readframes(wav.getnframes())
        except (OSError, wave.Error):
            return None
