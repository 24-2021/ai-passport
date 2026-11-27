#!/usr/bin/env python3
"""Regression tests for the kfb24 bridge virtual-microphone lifecycle.

The bridge used to close the PortAudio stream from `stop()` after a one-second
join timeout. When the worker was still inside a blocking `write()` (the queue
was full and the endpoint had stalled), `Pa_CloseStream` raced `Pa_WriteStream`
and the process aborted with STATUS_HEAP_CORRUPTION (0xC0000374), which took
down USB/BLE/TCP and left the board showing "bridge offline" forever.
"""

import os
import sys
import threading
import types
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "pc"))

import kfb24_bridge as bridge  # noqa: E402


class FakeStream:
    def __init__(self) -> None:
        self.write_started = threading.Event()
        self.release_write = threading.Event()
        self._lock = threading.Lock()
        self._writing = False
        self.stop_calls = 0
        self.close_calls = 0
        self.closed_while_writing = False
        self.writes = 0

    def start(self) -> None:
        pass

    def write(self, _data) -> None:
        with self._lock:
            self._writing = True
            self.writes += 1
        self.write_started.set()
        self.release_write.wait(5.0)
        with self._lock:
            self._writing = False

    def stop(self) -> None:
        with self._lock:
            self.stop_calls += 1
            if self._writing:
                self.closed_while_writing = True

    def close(self) -> None:
        with self._lock:
            self.close_calls += 1
            if self._writing:
                self.closed_while_writing = True


class FakeSoundDevice(types.ModuleType):
    def __init__(self) -> None:
        super().__init__("sounddevice")
        self.streams: list[FakeStream] = []

    def query_devices(self):
        return [{
            "name": "CABLE Input (VB-Audio Virtual Cable)",
            "max_output_channels": 2,
            "default_samplerate": 48000.0,
            "hostapi": 0,
        }]

    def query_hostapis(self):
        return [{"name": "Windows WASAPI"}]

    def RawOutputStream(self, **_kwargs):
        stream = FakeStream()
        self.streams.append(stream)
        return stream


class VirtualMicLifecycleTest(unittest.TestCase):
    def setUp(self) -> None:
        self._saved_sd = sys.modules.get("sounddevice")
        self.fake_sd = FakeSoundDevice()
        sys.modules["sounddevice"] = self.fake_sd

    def tearDown(self) -> None:
        if self._saved_sd is None:
            sys.modules.pop("sounddevice", None)
        else:
            sys.modules["sounddevice"] = self._saved_sd

    def _start(self):
        mic = bridge.VirtualMic({"virtual_mic": "CABLE Input"})
        mic.STOP_JOIN_SECONDS = 0.1
        self.assertTrue(mic.start())
        stream = self.fake_sd.streams[-1]
        thread = mic._thread
        mic.write(b"\x00\x00" * bridge.ADPCM_SAMPLES)
        self.assertTrue(stream.write_started.wait(1.0))
        return mic, stream, thread

    def test_stop_leaves_a_blocked_stream_to_its_worker(self) -> None:
        mic, stream, thread = self._start()

        mic.stop()  # Join times out while write() is blocked.

        self.assertEqual(0, stream.stop_calls,
                         "stop() closed a stream its worker was writing to")
        self.assertEqual(0, stream.close_calls)
        self.assertFalse(stream.closed_while_writing)

        stream.release_write.set()
        thread.join(2.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(1, stream.stop_calls)
        self.assertEqual(1, stream.close_calls)
        self.assertFalse(stream.closed_while_writing)

    def test_start_does_not_open_a_second_stream_while_old_worker_lives(self) -> None:
        mic, stream, thread = self._start()
        mic.stop()

        self.assertFalse(mic.start())
        self.assertEqual(1, len(self.fake_sd.streams))

        stream.release_write.set()
        thread.join(2.0)
        self.assertTrue(mic.start())
        self.assertEqual(2, len(self.fake_sd.streams))
        self.assertEqual(1, stream.close_calls)

        self.fake_sd.streams[-1].release_write.set()
        mic.stop()


if __name__ == "__main__":
    unittest.main()
