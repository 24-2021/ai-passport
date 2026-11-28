"""Local voice commands and offline speech recognition for the kfb24 bridge.

The bridge normally lets WeType transcribe the board audio. When a local
SenseVoice model is installed, the bridge instead recognises the utterance
itself, routes a small deterministic command grammar to the existing app
actions, and types everything else into the focused window. Routing stays
rule-based on purpose: no cloud account, no key, and no silent rewrites of
dictated text.
"""

from __future__ import annotations

import array
import os
import re
import threading
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Optional

SAMPLE_RATE = 16000
MODEL_GLOB = "sherpa-onnx-sense-voice-*"

# Spoken target -> bridge action. Keep the English spellings that appear on
# screen; Chinese aliases are what the user actually says.
APP_ALIASES: dict[str, tuple[str, ...]] = {
    "trae": ("trae", "tree", "try"),
    "wechat": ("微信", "wechat", "weixin"),
    "chrome": ("浏览器", "chrome", "谷歌浏览器", "谷歌"),
    "edge": ("edge", "edge浏览器", "必应浏览器"),
    "terminal": ("终端", "命令行", "terminal", "powershell", "cmd"),
    "orca": ("orca", "欧卡"),
}

OPEN_VERBS = ("打开", "启动", "运行", "切换到", "切到", "进入", "回到", "唤醒")
SUBMIT_PHRASES = ("回车", "按回车", "确认", "发送", "提交")
LEADING_POLITE = ("请", "帮我", "麻烦", "给我", "那个")
TRAILING_POLITE = ("吧", "好吗", "谢谢", "一下")

_PUNCTUATION = re.compile(r"[\s，。！？、,.!?;；:：\"'“”‘’（）()【】\[\]<>《》~`!@#$%^&*_+=|\\/-]+")


def normalize(text: str) -> str:
    """Fold width, case, punctuation and whitespace for stable matching."""
    folded = unicodedata.normalize("NFKC", text).lower()
    return _PUNCTUATION.sub("", folded)


def _strip_polite(text: str) -> str:
    changed = True
    while changed:
        changed = False
        for prefix in LEADING_POLITE:
            if text.startswith(prefix) and len(text) > len(prefix):
                text = text[len(prefix):]
                changed = True
        for suffix in TRAILING_POLITE:
            if text.endswith(suffix) and len(text) > len(suffix):
                text = text[: -len(suffix)]
                changed = True
    return text


@dataclass(frozen=True)
class Intent:
    action: str


def _alias_action(target: str) -> Optional[str]:
    for action, aliases in APP_ALIASES.items():
        if target == action or target in aliases:
            return action
    return None


def match_intent(text: str, commands: Optional[Mapping[str, str]] = None) -> Optional[Intent]:
    """Return an app/submit action for a spoken command, else None.

    Only whole utterances match. A sentence that merely contains "发送" is
    dictated, not executed, so dictation cannot trigger an unrelated action.
    """
    body = normalize(text)
    if not body:
        return None
    body = _strip_polite(body)
    if not body:
        return None

    if commands:
        for phrase, action in commands.items():
            if body == normalize(phrase):
                return Intent(action)

    if body in SUBMIT_PHRASES:
        return Intent("enter")

    for verb in OPEN_VERBS:
        if body.startswith(verb) and len(body) > len(verb):
            action = _alias_action(body[len(verb):])
            if action:
                return Intent(action)

    action = _alias_action(body)
    if action:
        return Intent(action)
    return None


def resolve_model_dir(configured: str = "") -> Optional[Path]:
    """Find a SenseVoice model directory with model and tokens files."""
    candidates: list[Path] = []
    if configured:
        candidates.append(Path(os.path.expandvars(configured)).expanduser())
    root = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "kfb24" / "models"
    if root.is_dir():
        candidates.extend(sorted(root.glob(MODEL_GLOB)))
    for path in candidates:
        if not (path / "tokens.txt").is_file():
            continue
        if (path / "model.int8.onnx").is_file() or (path / "model.onnx").is_file():
            return path
    return None


class Recognizer:
    """Offline SenseVoice recogniser, loaded lazily and used serially."""

    def __init__(self, model_dir: Path, threads: int = 2):
        import sherpa_onnx

        model = model_dir / "model.int8.onnx"
        if not model.is_file():
            model = model_dir / "model.onnx"
        self._lock = threading.Lock()
        self._recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=str(model),
            tokens=str(model_dir / "tokens.txt"),
            num_threads=max(1, int(threads)),
            use_itn=True,
            language="zh",
            debug=False,
        )

    def transcribe(self, pcm: bytes) -> str:
        if len(pcm) < 2:
            return ""
        samples = array.array("h")
        samples.frombytes(pcm[: len(pcm) // 2 * 2])
        waveform = [value / 32768.0 for value in samples]
        with self._lock:
            stream = self._recognizer.create_stream()
            stream.accept_waveform(SAMPLE_RATE, waveform)
            self._recognizer.decode_stream(stream)
            return stream.result.text.strip()


def type_text(text: str) -> bool:
    """Insert Unicode text into the focused control without touching the clipboard."""
    if os.name != "nt" or not text:
        return False
    import ctypes
    from ctypes import wintypes

    ulong_ptr = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong

    class KEYBDINPUT(ctypes.Structure):
        _fields_ = (("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                    ("dwExtraInfo", ulong_ptr))

    class MOUSEINPUT(ctypes.Structure):
        _fields_ = (("dx", wintypes.LONG), ("dy", wintypes.LONG),
                    ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                    ("time", wintypes.DWORD), ("dwExtraInfo", ulong_ptr))

    class HARDWAREINPUT(ctypes.Structure):
        _fields_ = (("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD),
                    ("wParamH", wintypes.WORD))

    class InputUnion(ctypes.Union):
        _fields_ = (("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT))

    class INPUT(ctypes.Structure):
        _anonymous_ = ("u",)
        _fields_ = (("type", wintypes.DWORD), ("u", InputUnion))

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    send_input = user32.SendInput
    send_input.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
    send_input.restype = wintypes.UINT

    keyeventf_unicode = 0x0004
    keyeventf_keyup = 0x0002
    events: list[INPUT] = []

    def add(scan: int, flags: int) -> None:
        event = INPUT()
        event.type = 1  # INPUT_KEYBOARD
        event.ki = KEYBDINPUT(0, scan, flags, 0, 0)
        events.append(event)

    for char in text.replace("\r\n", "\n").replace("\r", "\n"):
        if char == "\n":
            add(0x0D, 0)
            add(0x0D, keyeventf_keyup)
            continue
        encoded = char.encode("utf-16-le")
        for index in range(0, len(encoded), 2):
            scan = int.from_bytes(encoded[index:index + 2], "little")
            add(scan, keyeventf_unicode)
            add(scan, keyeventf_unicode | keyeventf_keyup)

    if not events:
        return False
    array_type = INPUT * len(events)
    sent = send_input(len(events), array_type(*events), ctypes.sizeof(INPUT))
    return sent == len(events)
