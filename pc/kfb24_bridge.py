#!/usr/bin/env python3
"""kfb24 PC bridge: USB serial + Wi-Fi TCP control for the kfb24 board.

The board sends one-line commands such as ``KFB24 CMD voice 7``.  This bridge
executes them on Windows and acknowledges on the same transport.  It also sends
heartbeats so the board can show whether the bridge is online, and accepts
``--configure`` to provision Wi-Fi credentials over USB.

Examples:
    python pc/kfb24_bridge.py --list-ports
    python pc/kfb24_bridge.py --port COM6 --tcp-port 8765
    python pc/kfb24_bridge.py --configure --ssid MyWifi --password secret
    python pc/kfb24_bridge.py --test-action voice --dry-run
"""

from __future__ import annotations

import argparse
import asyncio
import atexit
import base64
import binascii
import ctypes
import json
import os
import queue
import shutil
import socket
import subprocess
import sys
import threading
import time
import uuid
import zlib
from ctypes import wintypes
from pathlib import Path
from typing import Callable, Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - reported by main
    serial = None
    list_ports = None

try:
    from bleak import BleakClient, BleakScanner
except ImportError:  # pragma: no cover - reported by main when --ble is used
    BleakClient = None
    BleakScanner = None

VERSION = "1.0.0"
PREFIX = "KFB24 "
DEFAULT_TCP_PORT = 8765
DEFAULT_CONFIG = Path(os.environ.get("APPDATA", str(Path.home()))) / "kfb24" / "bridge.json"
ESP_VID = 0x303A
ESP_PID = 0x1001

# Nordic UART Service, matching main/kfb24_ble.c.
BLE_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
BLE_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
BLE_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


def log(message: str) -> None:
    print(message, flush=True)


_bridge_mutex = None


def acquire_single_instance() -> None:
    """Prevent a second bridge from double-executing board commands."""
    global _bridge_mutex
    if os.name != "nt":
        return
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL,
                                      wintypes.LPCWSTR]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel32.CreateMutexW(None, False, "Local\\kfb24_bridge")
    if not handle:
        raise RuntimeError("cannot create the bridge mutex")
    if ctypes.get_last_error() == 183:  # ERROR_ALREADY_EXISTS
        kernel32.CloseHandle(handle)
        raise RuntimeError("another kfb24 bridge is already running")
    _bridge_mutex = handle


def release_single_instance() -> None:
    global _bridge_mutex
    if os.name != "nt" or not _bridge_mutex:
        return
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle(_bridge_mutex)
    _bridge_mutex = None


def parse_frame(line: str) -> Optional[dict]:
    if not line.startswith(PREFIX):
        return None
    parts = line.strip().split()
    if len(parts) < 2 or parts[0] != "KFB24":
        return None
    kind = parts[1]
    try:
        if kind == "CMD" and len(parts) == 4:
            return {"kind": kind, "name": parts[2], "id": int(parts[3], 10)}
        if kind == "HELLO" and len(parts) in (2, 3):
            return {"kind": kind, "version": parts[2] if len(parts) == 3 else ""}
        if kind == "ACK" and len(parts) == 4:
            return {"kind": kind, "id": int(parts[2], 10),
                    "ok": parts[3] == "OK"}
        if kind == "STATE" and len(parts) >= 4:
            return {"kind": kind, "transport": parts[2],
                    "up": parts[3] == "1",
                    "detail": " ".join(parts[4:])}
    except ValueError:
        return None
    return None


def find_serial_port(explicit: Optional[str]) -> Optional[str]:
    if explicit:
        return explicit
    if list_ports is None:
        return None
    candidates = []
    for port in list_ports.comports():
        if port.vid == ESP_VID and port.pid == ESP_PID:
            candidates.append(port.device)
    if candidates:
        return sorted(candidates)[0]
    for port in list_ports.comports():
        text = f"{port.description or ''} {port.hwid or ''}".lower()
        if "303a" in text or "usb serial" in text or "usb jtag" in text:
            return port.device
    return None


def _candidate_ipv4s() -> list[str]:
    """All local IPv4 addresses, including virtual adapters."""
    values: list[str] = []
    try:
        for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            address = item[4][0]
            if address not in values:
                values.append(address)
    except OSError:
        pass
    return values


def _is_private(address: str) -> bool:
    octets = [int(part) for part in address.split(".")]
    return (octets[0] == 10 or
            (octets[0] == 172 and 16 <= octets[1] <= 31) or
            (octets[0] == 192 and octets[1] == 168))


def _windows_default_ipv4() -> str | None:
    """The IPv4 address of the adapter that owns the default route."""
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-NetIPConfiguration | Where-Object "
             "{$_.IPv4DefaultGateway -ne $null} | Select-Object -First 1)"
             ".IPv4Address.IPAddress"],
            capture_output=True, text=True, timeout=8,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    address = result.stdout.strip().splitlines()[0].strip() if result.stdout.strip() else ""
    return address or None


def local_ipv4() -> str:
    """Pick a LAN address the board can reach, not a VPN/WSL adapter."""
    if IS_WINDOWS:
        address = _windows_default_ipv4()
        if address and _is_private(address):
            return address
    candidates = [a for a in _candidate_ipv4s()
                  if not a.startswith(("127.", "169.254."))
                  and not a.startswith("198.18.")]
    candidates.sort(key=lambda a: (not a.startswith("192.168."),
                                   not a.startswith("10."),
                                   a.endswith(".1")))
    if candidates:
        return candidates[0]
    return "127.0.0.1"


# ---------------------------------------------------------------------------
# Windows input and app launching
# ---------------------------------------------------------------------------

IS_WINDOWS = sys.platform == "win32"
ULONG_PTR = ctypes.c_ulonglong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_ulong


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


def make_guid(value: str) -> GUID:
    parsed = uuid.UUID(value.strip().strip("{}"))
    return GUID(parsed.fields[0], parsed.fields[1], parsed.fields[2],
                (ctypes.c_ubyte * 8)(*parsed.bytes[8:]))


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [
        ("uMsg", wintypes.DWORD),
        ("wParamL", wintypes.WORD),
        ("wParamH", wintypes.WORD),
    ]


class INPUT_UNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", INPUT_UNION)]


INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_EXTENDEDKEY = 0x0001

VK_MODS = {"ctrl": 0x11, "control": 0x11, "alt": 0x12, "shift": 0x10,
           "win": 0x5B, "windows": 0x5B}
VK_KEYS = {"space": 0x20, "tab": 0x09, "enter": 0x0D, "return": 0x0D,
           "esc": 0x1B, "escape": 0x1B,
           "win": 0x5B, "lwin": 0x5B, "rwin": 0x5C,
           "right-alt": 0xA5, "right_alt": 0xA5, "ralt": 0xA5}
VK_RMENU = 0xA5
EXTENDED_VKS = {0x5B, 0x5C, VK_RMENU}


def _key_event(vk: int, up: bool, extended: bool = False) -> INPUT:
    event = INPUT()
    event.type = INPUT_KEYBOARD
    event.ki.wVk = vk
    event.ki.wScan = 0
    event.ki.dwFlags = (KEYEVENTF_KEYUP if up else 0) | (
        KEYEVENTF_EXTENDEDKEY if extended else 0)
    event.ki.time = 0
    event.ki.dwExtraInfo = 0
    return event


def _send_inputs(events: list[INPUT]) -> bool:
    if not IS_WINDOWS:
        log("input: Windows only")
        return False
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
    user32.SendInput.restype = wintypes.UINT
    array = (INPUT * len(events))(*events)
    sent = user32.SendInput(len(events), array, ctypes.sizeof(INPUT))
    if sent != len(events):
        log(f"input: SendInput sent {sent}/{len(events)} "
            f"(error {ctypes.get_last_error()})")
        return False
    return True


def hotkey_keys(spec: str) -> Optional[list[tuple[int, bool]]]:
    """Return (virtual-key, extended) pairs, modifiers first."""
    parts = [part.strip().lower() for part in spec.split("+") if part.strip()]
    if not parts:
        return None
    if len(parts) == 1 and parts[0] in ("right-alt", "right_alt", "ralt"):
        return [(VK_RMENU, True)]
    key = parts[-1]
    vk = VK_KEYS.get(key)
    if vk is None:
        if len(key) == 1 and key.isalnum():
            vk = ord(key.upper())
        else:
            log(f"input: unknown hotkey key {key!r}")
            return None
    keys: list[tuple[int, bool]] = []
    for mod in parts[:-1]:
        mod_vk = VK_MODS.get(mod)
        if mod_vk is None:
            log(f"input: unknown hotkey modifier {mod!r}")
            return None
        keys.append((mod_vk, mod_vk in EXTENDED_VKS))
    keys.append((vk, vk in EXTENDED_VKS))
    return keys


def press_keys(keys: list[tuple[int, bool]]) -> bool:
    return _send_inputs([_key_event(vk, False, extended)
                         for vk, extended in keys])


def release_keys(keys: list[tuple[int, bool]]) -> bool:
    return _send_inputs([_key_event(vk, True, extended)
                         for vk, extended in reversed(keys)])


def send_hotkey(spec: str) -> bool:
    keys = hotkey_keys(spec)
    if not keys:
        return False
    events = [_key_event(vk, False, extended) for vk, extended in keys]
    events.extend(_key_event(vk, True, extended)
                  for vk, extended in reversed(keys))
    return _send_inputs(events)


def send_right_alt_hold(duration_ms: int = 1200) -> bool:
    """Hold Right Alt, the common push-to-talk key for Chinese IMEs."""
    keys = [(VK_RMENU, True)]
    if not press_keys(keys):
        return False
    time.sleep(max(1, duration_ms) / 1000.0)
    return release_keys(keys)


# Visible top-level process names, matched against the executable basename.
WINDOW_PROCESSES = {
    "wechat": ("weixin.exe", "wechat.exe"),
    "orca": ("orca.exe",),
    "terminal": ("windowsterminal.exe", "wt.exe"),
    "trae": ("trae.exe",),
    "chrome": ("chrome.exe",),
    "edge": ("msedge.exe",),
}
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SW_RESTORE = 9
SW_MINIMIZE = 6


def _process_image_name(pid: int) -> str:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(len(buf))
        if not kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return ""
        return Path(buf.value).name.lower()
    finally:
        kernel32.CloseHandle(handle)


def find_window_for_processes(names: tuple[str, ...]) -> Optional[int]:
    """Return a visible top-level window owned by one of ``names``."""
    if not IS_WINDOWS or not names:
        return None
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.EnumWindows.argtypes = [
        ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM),
        wintypes.LPARAM,
    ]
    user32.EnumWindows.restype = wintypes.BOOL
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.IsWindowVisible.restype = wintypes.BOOL
    user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
    user32.GetWindowTextLengthW.restype = ctypes.c_int
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.GetWindowTextW.restype = ctypes.c_int
    user32.GetWindowThreadProcessId.argtypes = [
        wintypes.HWND, ctypes.POINTER(wintypes.DWORD),
    ]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.GetWindowRect.restype = wintypes.BOOL
    user32.IsIconic.argtypes = [wintypes.HWND]
    user32.IsIconic.restype = wintypes.BOOL
    user32.GetForegroundWindow.restype = wintypes.HWND

    targets = {name.lower() for name in names}
    foreground = int(user32.GetForegroundWindow() or 0)
    found: list[int] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def callback(hwnd, _lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        if user32.GetWindowTextLengthW(hwnd) <= 0:
            return True
        minimized = bool(user32.IsIconic(hwnd))
        rect = wintypes.RECT()
        if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
            return True
        if not minimized and (rect.right <= rect.left or rect.bottom <= rect.top):
            return True
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if not pid.value:
            return True
        if _process_image_name(pid.value) in targets:
            if int(hwnd) == foreground:
                found.insert(0, int(hwnd))
                return False
            found.append(int(hwnd))
        return True

    user32.EnumWindows(callback, 0)
    return found[0] if found else None


def is_minimized(hwnd: int) -> bool:
    if not IS_WINDOWS:
        return False
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.IsIconic.argtypes = [wintypes.HWND]
    user32.IsIconic.restype = wintypes.BOOL
    return bool(user32.IsIconic(hwnd))


def is_foreground(hwnd: int) -> bool:
    if not IS_WINDOWS:
        return False
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.GetForegroundWindow.restype = wintypes.HWND
    return int(user32.GetForegroundWindow() or 0) == hwnd


def minimize_window(hwnd: int) -> bool:
    if not IS_WINDOWS:
        return False
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.ShowWindow.restype = wintypes.BOOL
    return bool(user32.ShowWindow(hwnd, SW_MINIMIZE))


def focus_window(hwnd: int) -> bool:
    if not IS_WINDOWS:
        return False
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.ShowWindow.restype = wintypes.BOOL
    user32.BringWindowToTop.argtypes = [wintypes.HWND]
    user32.BringWindowToTop.restype = wintypes.BOOL
    user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    user32.SetForegroundWindow.restype = wintypes.BOOL
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.GetWindowThreadProcessId.argtypes = [
        wintypes.HWND, ctypes.POINTER(wintypes.DWORD),
    ]
    user32.GetWindowThreadProcessId.restype = wintypes.DWORD
    user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
    user32.AttachThreadInput.restype = wintypes.BOOL
    kernel32.GetCurrentThreadId.restype = wintypes.DWORD

    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.BringWindowToTop(hwnd)
    if user32.SetForegroundWindow(hwnd):
        return True

    foreground = user32.GetForegroundWindow()
    if not foreground:
        return False
    foreground_tid = user32.GetWindowThreadProcessId(foreground, None)
    current_tid = kernel32.GetCurrentThreadId()
    if not foreground_tid or foreground_tid == current_tid:
        return False
    attached = user32.AttachThreadInput(current_tid, foreground_tid, True)
    try:
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
    finally:
        if attached:
            user32.AttachThreadInput(current_tid, foreground_tid, False)
    return bool(user32.GetForegroundWindow() == hwnd)


def activate_input_profile(clsid: str, profile: str, langid: int) -> bool:
    """Activate a TSF language profile (WeType has no legacy HKL)."""
    if not IS_WINDOWS:
        log("ime: Windows only")
        return False
    try:
        ole32 = ctypes.WinDLL("ole32")
        ole32.CoInitializeEx.argtypes = [ctypes.c_void_p, wintypes.DWORD]
        ole32.CoInitializeEx.restype = ctypes.c_long
        init_hr = ole32.CoInitializeEx(None, 0x2)  # COINIT_APARTMENTTHREADED
        if init_hr < 0:
            log(f"ime: CoInitializeEx failed 0x{init_hr & 0xFFFFFFFF:08x}")
            return False
        try:
            msctf = ctypes.WinDLL("msctf")
            create = msctf.TF_CreateInputProcessorProfiles
            create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
            create.restype = ctypes.c_long
            profiles = ctypes.c_void_p()
            hr = create(ctypes.byref(profiles))
            if hr != 0 or not profiles:
                log(f"ime: TF_CreateInputProcessorProfiles failed "
                    f"0x{hr & 0xFFFFFFFF:08x}")
                return False
            try:
                vtable = ctypes.cast(
                    profiles, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))
                ).contents
                activate = ctypes.WINFUNCTYPE(
                    ctypes.c_long, ctypes.c_void_p,
                    ctypes.POINTER(GUID), wintypes.WORD, ctypes.POINTER(GUID)
                )(vtable[10])
                clsid_guid = make_guid(clsid)
                profile_guid = make_guid(profile)
                # Signature is (clsid, langid, guidProfile); the langid comes
                # before the profile GUID in msctf.h.
                hr = activate(profiles, ctypes.byref(clsid_guid), langid,
                              ctypes.byref(profile_guid))
                if hr != 0:
                    log(f"ime: ActivateLanguageProfile failed "
                        f"0x{hr & 0xFFFFFFFF:08x}")
                    return False
                log(f"ime: activated {clsid} / {profile}")
                return True
            finally:
                release = ctypes.WINFUNCTYPE(
                    ctypes.c_ulong, ctypes.c_void_p)(vtable[2])
                release(profiles)
        finally:
            if init_hr in (0, 1):
                ole32.CoUninitialize()
    except OSError as error:
        log(f"ime: COM error: {error}")
        return False


DEFAULT_CONFIG_VALUES = {
    "wechat": r"C:\Program Files\Tencent\Weixin\Weixin.exe",
    "orca": str(Path(os.environ.get("LOCALAPPDATA", "")) /
                "Programs" / "orca" / "Orca.exe"),
    "terminal": "wt.exe",
    "trae": str(Path(os.environ.get("LOCALAPPDATA", "")) /
                "Programs" / "Trae" / "Trae.exe"),
    "chrome": str(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
                  "Google" / "Chrome" / "Application" / "chrome.exe"),
    "edge": str(Path(os.environ.get("ProgramFiles(x86)",
                                    r"C:\Program Files (x86)")) /
                "Microsoft" / "Edge" / "Application" / "msedge.exe"),
    "voice_hotkey": "ctrl+win",
    "voice_mode": "latch",
    "voice_max_seconds": 60,
    "ime_clsid": "{86598FB9-66A2-463E-B9C2-AEB906D477AD}",
    "ime_profile": "{607FDF85-FCC8-4DBD-A365-41296F980C9C}",
    "ime_langid": 0x0804,
    "ime_hotkey": "ctrl+space",
}


def load_config(path: Path) -> dict:
    config = dict(DEFAULT_CONFIG_VALUES)
    if path.is_file():
        try:
            loaded = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict):
                config.update(loaded)
        except (OSError, json.JSONDecodeError) as error:
            log(f"config: ignoring {path}: {error}")
    return config


def voice_max_seconds(config: dict) -> float:
    try:
        seconds = float(config.get("voice_max_seconds", 60))
    except (TypeError, ValueError):
        seconds = 60.0
    return max(0.0, min(seconds, 3600.0))


class PcmPlayer:
    """Play board PCM on the default Windows output device.

    The board sends 16 kHz / mono / signed-16-bit little-endian chunks.  Writing
    straight from the serial reader would block USB reads, so a worker thread
    owns a small bounded queue and drops the oldest chunk when the PC falls
    behind.  Stereo Mix can then expose this output to WeType as a microphone.
    """

    SAMPLE_RATE = 16000
    BLOCK_FRAMES = 512
    QUEUE_CHUNKS = 24

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._lifecycle_lock = threading.Lock()
        self._queue: queue.Queue[bytes] = queue.Queue(maxsize=self.QUEUE_CHUNKS)
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._stream = None
        self._accepting = False
        self._failed = False
        self._frames = 0

    def reset_failure(self) -> None:
        """Allow a later voice session to retry after a device/driver error."""
        with self._lock:
            self._failed = False

    def is_active(self) -> bool:
        with self._lock:
            return bool(self._thread and self._thread.is_alive()
                        and self._accepting)

    def start(self) -> bool:
        with self._lifecycle_lock:
            with self._lock:
                if self._failed:
                    return False
                if self._thread and self._thread.is_alive():
                    if self._accepting:
                        return True
                    self._stop.set()
                thread = self._thread
            if thread and thread.is_alive():
                thread.join(1.0)
                if thread.is_alive():
                    log("audio: previous playback worker did not stop")
                    return False
            with self._lock:
                self._stop.clear()
                while True:
                    try:
                        self._queue.get_nowait()
                    except queue.Empty:
                        break
                try:
                    import sounddevice as sd
                except ImportError:
                    log("audio: sounddevice is not installed; WeType will use the "
                        "PC microphone (python -m pip install -r "
                        "pc/requirements.txt)")
                    self._failed = True
                    return False
                try:
                    stream = sd.RawOutputStream(
                        samplerate=self.SAMPLE_RATE, channels=1, dtype="int16",
                        blocksize=self.BLOCK_FRAMES,
                    )
                    stream.start()
                except Exception as error:  # PortAudio / device configuration
                    log(f"audio: cannot open default output ({error}); WeType will "
                        "use the PC microphone")
                    self._failed = True
                    return False
                self._stream = stream
                self._accepting = True
                self._frames = 0
                self._thread = threading.Thread(
                    target=self._run, args=(stream,), daemon=True,
                    name="kfb24-audio",
                )
                self._thread.start()
                log(f"audio: playing board PCM at {self.SAMPLE_RATE} Hz")
                return True

    def write(self, pcm: bytes) -> None:
        if not pcm:
            return
        if not self.is_active():
            if not self.start():
                return
        with self._lock:
            if not self._accepting:
                return
            try:
                self._queue.put_nowait(pcm)
            except queue.Full:
                try:
                    self._queue.get_nowait()
                except queue.Empty:
                    pass
                try:
                    self._queue.put_nowait(pcm)
                except queue.Full:
                    pass
            self._frames += 1

    def _run(self, stream) -> None:
        try:
            while True:
                if self._stop.is_set() and self._queue.empty():
                    break
                try:
                    pcm = self._queue.get(timeout=0.1)
                except queue.Empty:
                    if self._stop.is_set():
                        break
                    continue
                try:
                    stream.write(pcm)
                except Exception as error:
                    log(f"audio: playback failed: {error}")
                    break
        finally:
            with self._lock:
                self._accepting = False
                if self._stream is stream:
                    self._stream = None
            try:
                stream.stop()
                stream.close()
            except Exception:
                pass

    def stop(self) -> None:
        with self._lifecycle_lock:
            with self._lock:
                self._accepting = False
                self._stop.set()
                thread = self._thread
            if thread:
                thread.join(1.0)
            with self._lock:
                stream = self._stream
                self._stream = None
                self._thread = None
                while True:
                    try:
                        self._queue.get_nowait()
                    except queue.Empty:
                        break
            if stream is not None:
                try:
                    stream.stop()
                    stream.close()
                except Exception:
                    pass


class Actions:
    def __init__(self, config: dict, dry_run: bool = False):
        self.config = config
        self.dry_run = dry_run
        self.audio = PcmPlayer()
        self._voice_lock = threading.Lock()
        self._voice_keys: list[tuple[int, bool]] = []
        self._voice_timer: Optional[threading.Timer] = None
        self._voice_max_seconds = voice_max_seconds(config)
        if not dry_run:
            atexit.register(self.close)

    def launch(self, name: str) -> bool:
        path = str(self.config.get(name, ""))
        processes = WINDOW_PROCESSES.get(name, ())
        if self.dry_run:
            if processes:
                log(f"dry-run: focus existing {name} window, else launch: "
                    f"{path or '<missing>'}")
            else:
                log(f"dry-run: launch {name}: {path or '<missing>'}")
            return True
        if processes:
            hwnd = find_window_for_processes(processes)
            if hwnd:
                if is_minimized(hwnd):
                    ok = focus_window(hwnd)
                    log(f"launch: {name}: restored minimized window"
                        if ok else f"launch: {name}: minimized window could not be restored")
                    return ok
                if is_foreground(hwnd):
                    ok = minimize_window(hwnd)
                    log(f"launch: {name}: minimized window"
                        if ok else f"launch: {name}: window could not be minimized")
                    return ok
                ok = focus_window(hwnd)
                log(f"launch: {name}: focused existing window"
                    if ok else f"launch: {name}: existing window could not be focused")
                return ok
        if not path:
            log(f"launch: no path configured for {name}")
            return False
        expanded = os.path.expandvars(os.path.expanduser(path))
        if os.path.isfile(expanded):
            try:
                subprocess.Popen(
                    [expanded], cwd=os.path.dirname(expanded) or None,
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL, close_fds=True,
                )
                log(f"launch: {name}: {expanded}")
                return True
            except OSError as error:
                log(f"launch: {name} failed: {error}")
                return False
        executable = shutil.which(expanded)
        if executable:
            try:
                subprocess.Popen(
                    [executable], stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    close_fds=True,
                )
                log(f"launch: {name}: {executable}")
                return True
            except OSError as error:
                log(f"launch: {name} failed: {error}")
                return False
        log(f"launch: {name} not found: {path}")
        return False

    def ime(self) -> bool:
        if self.dry_run:
            log("dry-run: activate WeType TSF profile")
            return True
        ok = activate_input_profile(
            str(self.config["ime_clsid"]),
            str(self.config["ime_profile"]),
            int(self.config["ime_langid"]),
        )
        if ok:
            return True
        fallback = str(self.config.get("ime_hotkey", ""))
        if fallback:
            log(f"ime: falling back to hotkey {fallback}")
            return send_hotkey(fallback)
        return False

    def _release_voice_locked(self, reason: str) -> bool:
        timer = self._voice_timer
        self._voice_timer = None
        if timer is not None:
            timer.cancel()
        keys = self._voice_keys
        self._voice_keys = []
        if not keys:
            return True
        ok = release_keys(keys)
        self.audio.stop()
        if ok:
            log(f"voice: {reason}")
        else:
            log(f"voice: release failed ({reason})")
        return ok

    def _voice_timeout(self) -> None:
        with self._voice_lock:
            if self._voice_keys:
                self._release_voice_locked(
                    f"auto-stopped after {self._voice_max_seconds:g}s")

    def close(self) -> None:
        with self._voice_lock:
            self._release_voice_locked("released on exit")
        self.audio.stop()

    def voice(self) -> bool:
        """Latch the configured key for hold-to-talk, or tap it in tap mode."""
        hotkey = str(self.config.get("voice_hotkey", "ctrl+win")).strip()
        if self.dry_run:
            log(f"dry-run: send WeType voice hotkey {hotkey or '<disabled>'}")
            return True
        if not hotkey:
            log("voice: voice_hotkey is disabled")
            return False

        self.audio.reset_failure()
        mode = str(self.config.get("voice_mode", "latch")).strip().lower()
        if mode in ("tap", "toggle", "once"):
            self.audio.start()
            if hotkey.lower() in ("right-alt", "right_alt", "ralt"):
                ok = send_right_alt_hold()
            else:
                ok = send_hotkey(hotkey)
            if not ok:
                self.audio.stop()
            return ok
        if mode not in ("latch", "hold", ""):
            log(f"voice: unknown voice_mode {mode!r}; use latch or tap")
            return False

        keys = hotkey_keys(hotkey)
        if not keys:
            return False
        with self._voice_lock:
            if self._voice_keys:
                return self._release_voice_locked("stopped")
            self.audio.start()
            if not press_keys(keys):
                release_keys(keys)
                self.audio.stop()
                log("voice: could not press hotkey")
                return False
            self._voice_keys = keys
            self._voice_max_seconds = voice_max_seconds(self.config)
            if self._voice_max_seconds > 0:
                timer = threading.Timer(self._voice_max_seconds,
                                        self._voice_timeout)
                timer.daemon = True
                timer.start()
                self._voice_timer = timer
                log("voice: recording; press voice again to stop "
                    f"(auto-stop in {self._voice_max_seconds:g}s)")
            else:
                log("voice: recording; press voice again to stop")
            return True

    def run(self, name: str) -> bool:
        if name == "voice":
            return self.voice()
        if name == "ime":
            return self.ime()
        if name in ("wechat", "orca", "terminal", "trae", "chrome", "edge"):
            return self.launch(name)
        log(f"action: unknown command {name!r}")
        return False


# ---------------------------------------------------------------------------
# Transports
# ---------------------------------------------------------------------------

class SerialTransport:
    def __init__(self, port: str, on_line: Callable[[str, "SerialTransport"], None],
                 verbose: bool = False):
        self.port = port
        self.on_line = on_line
        self.verbose = verbose
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self._write_lock = threading.Lock()
        self._running = False
        self._thread: Optional[threading.Thread] = None

    def open(self) -> None:
        self.ser.open()
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except (OSError, serial.SerialException):
            pass

    def start(self) -> None:
        self._running = True
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def _read_loop(self) -> None:
        buffer = ""
        while self._running:
            try:
                raw = self.ser.read(4096)
            except (OSError, serial.SerialException) as error:
                if self._running:
                    log(f"serial: read failed: {error}")
                break
            if not raw:
                continue
            buffer += raw.decode("utf-8", errors="replace")
            while "\n" in buffer:
                raw_line, buffer = buffer.split("\n", 1)
                line = raw_line.strip()
                if line:
                    self.on_line(line, self)
            if len(buffer) > 65536:
                buffer = ""

    def send(self, line: str) -> bool:
        if not self.ser.is_open:
            return False
        try:
            with self._write_lock:
                self.ser.write((line + "\n").encode("utf-8"))
                self.ser.flush()
            return True
        except (OSError, serial.SerialException) as error:
            log(f"serial: write failed: {error}")
            return False

    def label(self) -> str:
        return f"USB({self.port})"

    def close(self) -> None:
        self._running = False
        try:
            self.ser.close()
        except (OSError, serial.SerialException):
            pass


class TcpTransport:
    def __init__(self, conn: socket.socket, peer, on_line,
                 verbose: bool = False):
        self.conn = conn
        self.peer = peer
        self.on_line = on_line
        self.verbose = verbose
        self._write_lock = threading.Lock()
        self._running = True

    def send(self, line: str) -> bool:
        try:
            with self._write_lock:
                self.conn.sendall((line + "\n").encode("utf-8"))
            return True
        except OSError:
            return False

    def label(self) -> str:
        return f"WiFi({self.peer[0]}:{self.peer[1]})"

    def close(self) -> None:
        self._running = False
        try:
            self.conn.close()
        except OSError:
            pass


class BleTransport:
    """BLE central for the board's Nordic UART Service."""

    def __init__(self, name: str, on_line, verbose: bool = False):
        if BleakClient is None or BleakScanner is None:
            raise RuntimeError("bleak is not installed; pip install -r pc/requirements.txt")
        self.name = name
        self.on_line = on_line
        self.verbose = verbose
        self._running = True
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._client = None
        self._buffer = ""
        self._lines: queue.Queue[str] = queue.Queue(maxsize=128)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._worker = threading.Thread(target=self._dispatch, daemon=True)
        self._last_error = ""

    def start(self) -> None:
        self._worker.start()
        self._thread.start()
        log(f"ble: scanning for {self.name}")

    def _dispatch(self) -> None:
        while self._running:
            try:
                line = self._lines.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                self.on_line(line, self)
            except Exception as error:  # keep the transport alive
                log(f"ble: handler failed: {error}")

    def _run(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            while self._running:
                try:
                    self._loop.run_until_complete(self._session())
                except Exception as error:  # noqa: BLE001 - keep reconnecting
                    message = str(error)
                    if message != self._last_error:
                        log(f"ble: {message}")
                        self._last_error = message
                if self._running:
                    time.sleep(2.0)
        finally:
            self._loop.close()
            self._loop = None

    async def _session(self) -> None:
        device = await BleakScanner.find_device_by_name(self.name, timeout=8.0)
        if device is None:
            raise RuntimeError(f"{self.name} not found")
        async with BleakClient(device, timeout=15.0) as client:
            self._client = client
            self._last_error = ""
            self._buffer = ""
            await client.start_notify(BLE_TX_UUID, self._on_notify)
            log(f"ble: connected {client.address}")
            try:
                while self._running and client.is_connected:
                    await asyncio.sleep(0.5)
            finally:
                self._client = None
                if client.is_connected:
                    try:
                        await client.stop_notify(BLE_TX_UUID)
                    except Exception:
                        pass

    def _on_notify(self, _sender, data: bytearray) -> None:
        try:
            self._buffer += bytes(data).decode("utf-8", errors="replace")
        except Exception:
            return
        while "\n" in self._buffer:
            raw, self._buffer = self._buffer.split("\n", 1)
            line = raw.strip()
            if line:
                if self.verbose:
                    log(f"ble <- {line}")
                try:
                    self._lines.put_nowait(line)
                except queue.Full:
                    if self.verbose:
                        log("ble: receive queue full, dropping line")
        if len(self._buffer) > 65536:
            self._buffer = ""

    def send(self, line: str) -> bool:
        loop = self._loop
        client = self._client
        if loop is None or client is None:
            return False
        future = asyncio.run_coroutine_threadsafe(
            client.write_gatt_char(BLE_RX_UUID,
                                   (line + "\n").encode("utf-8"),
                                   response=False),
            loop,
        )
        try:
            future.result(timeout=3.0)
            return True
        except Exception as error:
            if self.verbose:
                log(f"ble: write failed: {error}")
            return False

    def label(self) -> str:
        return f"BLE({self.name})"

    def close(self) -> None:
        self._running = False


# ---------------------------------------------------------------------------
# Bridge
# ---------------------------------------------------------------------------

class Bridge:
    def __init__(self, actions: Actions, serial_port: Optional[str],
                 tcp_port: int, listen: str, verbose: bool = False,
                 enable_tcp: bool = True, enable_ble: bool = False,
                 ble_name: str = "kfb24"):
        self.actions = actions
        self.verbose = verbose
        self.serial: Optional[SerialTransport] = None
        self.tcp: Optional[TcpTransport] = None
        self.ble: Optional[BleTransport] = None
        self.tcp_lock = threading.Lock()
        self.running = True
        self.board_hello = threading.Event()
        self.ack_event = threading.Event()
        self.ack_ok = False
        self.ack_id: Optional[int] = None
        self.server = None
        self._heartbeat: Optional[threading.Thread] = None
        self._tcp_port = tcp_port
        self._listen = listen
        self._enable_tcp = enable_tcp
        self._audio_drops = 0
        if serial_port:
            if serial is None:
                if not enable_ble:
                    raise RuntimeError("pyserial is not installed")
                log("serial: pyserial is not installed; continuing with BLE/TCP")
            else:
                transport = SerialTransport(serial_port, self.on_line, verbose)
                try:
                    transport.open()
                    transport.start()
                except Exception as error:  # serial monitor or another bridge holds it
                    fallback = "+".join(name for name, enabled in
                                        (("BLE", enable_ble), ("TCP", enable_tcp))
                                        if enabled)
                    suffix = (f"; continuing with {fallback}" if fallback
                              else "; no other transport enabled")
                    log(f"serial: {serial_port} unavailable ({error}){suffix}")
                    try:
                        transport.close()
                    except Exception:
                        pass
                else:
                    self.serial = transport
                    log(f"serial: {serial_port} @ 115200")
        if enable_ble:
            self.ble = BleTransport(ble_name, self.on_line, verbose)
            self.ble.start()
        if enable_tcp:
            self._start_tcp_server()
        if not self.serial and not self.ble and not self.server:
            raise RuntimeError("no transport available (serial/BLE/TCP)")

    def _start_tcp_server(self) -> None:
        bridge = self

        class Handler(__import__("socketserver").StreamRequestHandler):
            def handle(self) -> None:
                transport = TcpTransport(self.request, self.client_address,
                                         bridge.on_line, bridge.verbose)
                try:
                    self.request.settimeout(1.0)
                    bridge.attach_tcp(transport)
                    while bridge.running and transport._running:
                        try:
                            raw = self.request.recv(1024)
                        except socket.timeout:
                            continue
                        except OSError:
                            break
                        if not raw:
                            break
                        for line in raw.decode("utf-8", errors="replace").splitlines():
                            if line.strip():
                                bridge.on_line(line.strip(), transport)
                finally:
                    bridge.detach_tcp(transport)
                    transport.close()

        class Server(__import__("socketserver").ThreadingTCPServer):
            allow_reuse_address = os.name != "nt"
            daemon_threads = True

            def server_bind(self):
                if os.name == "nt":
                    self.socket.setsockopt(socket.SOL_SOCKET,
                                           socket.SO_EXCLUSIVEADDRUSE, 1)
                super().server_bind()

        try:
            self.server = Server((self._listen, self._tcp_port), Handler)
        except OSError as error:
            self.server = None
            log(f"tcp: disabled ({error}); USB/BLE remain available")
            return
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        log(f"tcp: listening on {self._listen}:{self._tcp_port} "
            f"(LAN {local_ipv4()}:{self._tcp_port})")

    def attach_tcp(self, transport: TcpTransport) -> None:
        with self.tcp_lock:
            old = self.tcp
            self.tcp = transport
        if old:
            old.close()
        log(f"tcp: board connected from {transport.peer[0]}:{transport.peer[1]}")
        self.send_line(transport, f"KFB24 HELLO bridge/{VERSION}")

    def detach_tcp(self, transport: TcpTransport) -> None:
        with self.tcp_lock:
            if self.tcp is transport:
                self.tcp = None
                log(f"tcp: board disconnected {transport.peer[0]}:{transport.peer[1]}")

    def send_line(self, transport, line: str) -> bool:
        ok = transport.send(line)
        if self.verbose:
            log(f"{transport.label()} -> {line}")
        return ok

    def send_all(self, line: str) -> None:
        if self.serial:
            self.send_line(self.serial, line)
        with self.tcp_lock:
            tcp = self.tcp
        if tcp:
            self.send_line(tcp, line)
        if self.ble:
            self.send_line(self.ble, line)

    def _on_audio_line(self, line: str, transport) -> None:
        parts = line.strip().split()
        if len(parts) == 3 and parts[2] == "END":
            self.actions.audio.stop()
            return
        if len(parts) != 5:
            self._drop_audio("malformed frame")
            return
        try:
            int(parts[2], 10)
            crc = int(parts[3], 16)
            pcm = base64.b64decode(parts[4], validate=True)
        except (ValueError, binascii.Error):
            self._drop_audio("bad number/base64")
            return
        if len(pcm) != 1024:
            self._drop_audio(f"unexpected chunk size {len(pcm)}")
            return
        if (zlib.crc32(pcm) & 0xFFFFFFFF) != crc:
            self._drop_audio("CRC mismatch")
            return
        self.actions.audio.write(pcm)

    def _drop_audio(self, reason: str) -> None:
        self._audio_drops += 1
        if self._audio_drops == 1 or self._audio_drops % 50 == 0:
            log(f"audio: dropped {self._audio_drops} frame(s): {reason}")

    def on_line(self, line: str, transport) -> None:
        if line.startswith(PREFIX + "AUDIO "):
            self._on_audio_line(line, transport)
            return
        frame = parse_frame(line)
        if frame is None:
            if self.verbose:
                log(f"{transport.label()} | {line}")
            return
        kind = frame["kind"]
        if kind == "HELLO":
            self.board_hello.set()
            log(f"{transport.label()} <- board hello {frame['version']}")
        elif kind == "STATE":
            log(f"{transport.label()} <- state {frame['transport']} "
                f"{'up' if frame['up'] else 'down'} {frame['detail']}".rstrip())
        elif kind == "ACK":
            log(f"{transport.label()} <- ack {frame['id']} "
                f"{'OK' if frame['ok'] else 'ERR'}")
            if frame["id"] == self.ack_id:
                self.ack_ok = frame["ok"]
                self.ack_event.set()
        elif kind == "CMD":
            log(f"{transport.label()} <- cmd {frame['name']} #{frame['id']}")
            ok = False
            try:
                ok = self.actions.run(frame["name"])
            except Exception as error:  # action failures must still ACK
                log(f"action {frame['name']} raised: {error}")
            self.send_line(transport, f"KFB24 ACK {frame['id']} "
                                      f"{'OK' if ok else 'ERR'}")

    def start_heartbeat(self) -> None:
        def beat() -> None:
            while self.running:
                self.send_all(f"KFB24 HELLO bridge/{VERSION}")
                time.sleep(2.0)

        self._heartbeat = threading.Thread(target=beat, daemon=True)
        self._heartbeat.start()

    def configure(self, ssid: str, password: str, host: str, port: int,
                  timeout: float = 8.0) -> bool:
        if not self.serial:
            log("configure: USB serial is required")
            return False
        if not ssid:
            log("configure: SSID must not be empty")
            return False
        self.ack_event.clear()
        self.ack_id = 1
        self.ack_ok = False
        ssid_hex = ssid.encode("utf-8").hex()
        pass_hex = password.encode("utf-8").hex() if password else "-"
        host_value = host or "-"
        line = f"KFB24 CFG 1 {ssid_hex} {pass_hex} {host_value} {port}"
        log(f"configure: ssid={ssid!r} bridge={host_value}:{port}")
        self.send_line(self.serial, line)
        if not self.ack_event.wait(timeout):
            log("configure: no ACK from board")
            return False
        log(f"configure: board {'saved' if self.ack_ok else 'rejected'} the settings")
        return self.ack_ok

    def stop(self) -> None:
        self.running = False
        if self.server:
            self.server.shutdown()
            self.server.server_close()
        with self.tcp_lock:
            tcp = self.tcp
            self.tcp = None
        if tcp:
            tcp.close()
        if self.serial:
            self.serial.close()
        if self.ble:
            self.ble.close()
        log("bridge: stopped")


def list_serial_ports() -> None:
    if list_ports is None:
        log("pyserial is not installed: python -m pip install -r pc/requirements.txt")
        return
    for port in list_ports.comports():
        marker = " <- board" if port.vid == ESP_VID and port.pid == ESP_PID else ""
        log(f"{port.device}: {port.description} [{port.hwid}]{marker}")


def run_configure(args, config: dict) -> int:
    if serial is None:
        log("configure: pyserial is required for USB provisioning")
        return 2
    port = find_serial_port(args.port)
    if not port:
        log("configure: no ESP32 USB serial port found; use --port COMx")
        return 2
    actions = Actions(config, dry_run=True)
    try:
        bridge = Bridge(actions, port, args.tcp_port, args.listen, args.verbose,
                        enable_tcp=False)
    except Exception as error:
        log(f"configure: {error}; stop the running bridge if it holds the USB port")
        return 2
    try:
        if not bridge.serial:
            log("configure: USB serial is unavailable; stop the running bridge "
                "if it holds the port")
            return 2
        log("configure: waiting for the board to boot...")
        bridge.board_hello.wait(12.0)
        host = args.host or local_ipv4()
        ok = bridge.configure(args.ssid, args.password or "", host, args.tcp_port)
        return 0 if ok else 1
    finally:
        bridge.stop()


def run_bridge(args, config: dict) -> int:
    try:
        acquire_single_instance()
    except RuntimeError as error:
        log(f"bridge: {error}")
        return 0
    port = None if args.ble_only else find_serial_port(args.port)
    if not port and args.port:
        log(f"serial: {args.port} not found")
        release_single_instance()
        return 2
    if args.ble_only:
        log("serial: disabled (--ble-only)")
    elif not port:
        log("serial: board not found; continuing with BLE/TCP")
    actions = Actions(config, dry_run=args.dry_run)
    try:
        bridge = Bridge(actions, port, args.tcp_port, args.listen, args.verbose,
                        enable_ble=args.ble, ble_name=args.ble_name)
    except Exception as error:
        log(f"bridge: {error}")
        release_single_instance()
        return 2
    bridge.start_heartbeat()
    log("bridge: ready for voice/ime/wechat/orca/terminal/trae/chrome/edge")
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        log("bridge: interrupted")
    finally:
        try:
            bridge.stop()
        finally:
            actions.close()
            release_single_instance()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port (auto-detect Espressif by default)")
    parser.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT,
                        help=f"bridge TCP port (default {DEFAULT_TCP_PORT})")
    parser.add_argument("--listen", default="0.0.0.0",
                        help="TCP listen address (default 0.0.0.0)")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG,
                        help=f"JSON config (default {DEFAULT_CONFIG})")
    parser.add_argument("--ble", action="store_true",
                        help="also connect to the board over BLE (requires bleak)")
    parser.add_argument("--ble-only", action="store_true",
                        help="use BLE only; do not open the USB serial port")
    parser.add_argument("--ble-name", default="kfb24",
                        help="BLE advertising name (default kfb24)")
    parser.add_argument("--verbose", action="store_true",
                        help="print board logs and every protocol frame")
    parser.add_argument("--dry-run", action="store_true",
                        help="do not send input or launch apps")
    parser.add_argument("--list-ports", action="store_true",
                        help="list serial ports and exit")
    parser.add_argument("--configure", action="store_true",
                        help="send Wi-Fi/bridge settings over USB and exit")
    parser.add_argument("--ssid", help="Wi-Fi SSID for --configure")
    parser.add_argument("--password", help="Wi-Fi password for --configure")
    parser.add_argument("--host", help="PC IPv4 address advertised to the board")
    parser.add_argument("--test-action",
                        choices=["voice", "ime", "wechat", "orca", "terminal",
                                 "trae", "chrome", "edge"],
                        help="run one action locally and exit")
    args = parser.parse_args()

    if args.ble_only:
        if args.port:
            parser.error("--ble-only cannot be combined with --port")
        if args.configure:
            parser.error("--ble-only cannot be combined with --configure")
        args.ble = True
    if args.list_ports:
        list_serial_ports()
        return 0
    if serial is None and not args.test_action and not args.ble:
        log("pyserial is missing: python -m pip install -r pc/requirements.txt")
        return 2
    if args.ble and BleakClient is None:
        log("bleak is missing: python -m pip install -r pc/requirements.txt")
        return 2

    config = load_config(args.config)
    if args.test_action:
        actions = Actions(config, dry_run=args.dry_run)
        try:
            ok = actions.run(args.test_action)
        finally:
            actions.close()
        return 0 if ok else 1
    if args.configure:
        if not args.ssid:
            parser.error("--configure requires --ssid")
        return run_configure(args, config)
    return run_bridge(args, config)


if __name__ == "__main__":
    sys.exit(main())
