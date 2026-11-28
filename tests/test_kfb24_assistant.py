#!/usr/bin/env python3
"""Regression tests for the kfb24 assistant path.

Covers the REPL-independent pieces: reply cleaning, the ADPCM encoder that
feeds the board speaker, the tool loop, CLI dispatch, and the dialog/audio
frames. The model server is never contacted.
"""

import base64
import json
import math
import os
import struct
import sys
import threading
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "pc"))

import kfb24_agent as agent_mod  # noqa: E402
import kfb24_bridge as bridge  # noqa: E402


class FakeActions:
    def __init__(self) -> None:
        self.launched = []
        self.entries = 0

    def launch(self, app: str) -> bool:
        self.launched.append(app)
        return True

    def enter(self) -> bool:
        self.entries += 1
        return True


class ScriptedAgent(agent_mod.Agent):
    """Agent whose model replies are supplied by the test."""

    def __init__(self, config, actions, replies, **kwargs) -> None:
        super().__init__(config, actions, **kwargs)
        self.script = list(replies)
        self.seen = []
        self.available = lambda: True  # skip the network probe

    def _complete(self, messages, tools):
        self.seen.append(json.loads(json.dumps(messages)))
        return self.script.pop(0)


def _config(**overrides):
    config = {
        "assistant_enabled": True,
        "llm_api": "ollama",
        "llm_url": "http://127.0.0.1:11434",
        "llm_model": "test:1b",
        "llm_timeout": 10,
        "agent_commands": {},
        "agent_timeout": 10,
    }
    config.update(overrides)
    return config


class ReplyTests(unittest.TestCase):
    def test_clean_reply_strips_decoration(self):
        text = agent_mod.clean_reply(
            "**好的**，已打开微信。\n```py\nprint(1)\n```\n🙂")
        self.assertEqual(text, "好的，已打开微信。")

    def test_clean_reply_truncates_at_sentence(self):
        text = agent_mod.clean_reply("好的。" * 100)
        self.assertLessEqual(len(text), agent_mod.MAX_REPLY_CHARS)
        self.assertTrue(text.endswith("。"))


class EncoderTests(unittest.TestCase):
    def test_block_shape_and_roundtrip(self):
        samples = [int(12000 * math.sin(i / 9.0)) for i in range(640)]
        pcm = struct.pack("<640h", *samples)
        state = {"index": 0}
        block = bridge.adpcm_encode(pcm, state)
        self.assertEqual(len(block), bridge.ADPCM_BYTES)
        self.assertEqual(struct.unpack("<h", block[:2])[0], samples[0])
        decoded = bridge.adpcm_decode(block)
        self.assertIsNotNone(decoded)
        errors = [a - b for a, b in
                  zip(samples, struct.unpack("<640h", decoded))]
        mse = sum(e * e for e in errors) / len(errors)
        signal = sum(s * s for s in samples) / len(samples)
        self.assertGreater(10 * math.log10(signal / mse), 20.0)

    def test_index_carries_between_blocks(self):
        loud = struct.pack("<640h", *([0, 20000, -20000] * 213 + [0]))
        state = {"index": 0}
        first = bridge.adpcm_encode(loud, state)
        self.assertGreater(state["index"], 0)
        carried = state["index"]
        second = bridge.adpcm_encode(loud, state)
        self.assertEqual(second[2], carried)
        self.assertEqual(first[2], 0)
        colder = {"index": 0}
        bridge.adpcm_encode(loud, colder)
        self.assertNotEqual(second[2], 0)

    def test_rejects_short_block(self):
        with self.assertRaises(ValueError):
            bridge.adpcm_encode(b"\0" * 100, {"index": 0})


class AgentTests(unittest.TestCase):
    def test_tool_call_then_answer(self):
        actions = FakeActions()
        spoken = []
        agent = ScriptedAgent(_config(), actions, [
            {"content": "", "tool_calls": [
                {"id": "c1", "name": "open_app",
                 "arguments": {"app": "wechat"}}]},
            {"content": "已经打开微信了。", "tool_calls": []},
        ], speak=spoken.append)
        reply = agent.respond("帮我打开微信")
        self.assertEqual(reply, "已经打开微信了。")
        self.assertEqual(actions.launched, ["wechat"])
        self.assertEqual(spoken, [])
        roles = [message["role"] for message in agent.seen[1]]
        self.assertIn("tool", roles)
        self.assertEqual(agent.seen[1][-1]["tool_name"], "open_app")

    def test_unknown_tool_does_not_abort(self):
        actions = FakeActions()
        agent = ScriptedAgent(_config(), actions, [
            {"content": "", "tool_calls": [
                {"id": "c1", "name": "rm_rf", "arguments": {}}]},
            {"content": "这个我做不了。", "tool_calls": []},
        ])
        self.assertEqual(agent.respond("删掉系统"), "这个我做不了。")

    def test_dispatch_runs_configured_command(self):
        actions = FakeActions()
        spoken = []
        config = _config(agent_commands={
            "echo": [sys.executable, "-c",
                     "import sys; print(sys.argv[1])", "{prompt}"]})
        agent = agent_mod.Agent(config, actions, speak=spoken.append)
        result = json.loads(agent._dispatch("echo", "你好"))
        self.assertTrue(result["ok"])
        self.assertEqual(result["output"], "你好")
        self.assertEqual(spoken, ["好的，我让echo处理，请稍等。"])

    def test_dispatch_unknown_agent(self):
        agent = agent_mod.Agent(_config(), FakeActions())
        result = json.loads(agent._dispatch("nobody", "hi"))
        self.assertFalse(result["ok"])

    def test_unavailable_returns_none(self):
        agent = agent_mod.Agent(_config(), FakeActions())
        agent.available = lambda: False
        self.assertIsNone(agent.respond("你好"))

    def test_busy_turn_is_rejected(self):
        agent = ScriptedAgent(_config(), FakeActions(),
                              [{"content": "ok", "tool_calls": []}])
        self.assertTrue(agent._lock.acquire(blocking=False))
        try:
            self.assertEqual(agent.respond("你好"), "还在处理上一个任务，请稍等一下。")
        finally:
            agent._lock.release()


class InlineThread:
    def __init__(self, target=None, daemon=None):
        self.target = target

    def start(self):
        self.target()


class RoutingTests(unittest.TestCase):
    def test_wake_utterance_is_spoken_not_typed(self):
        actions = bridge.Actions(_config(), dry_run=False)
        actions.agent = ScriptedAgent(_config(), FakeActions(), [
            {"content": "你好，我是小智。", "tool_calls": []}])
        actions._ensure_asr = lambda: types.SimpleNamespace(
            transcribe=lambda pcm: "你好")
        spoken = []
        actions.say = spoken.append
        typed = []
        with mock.patch.object(bridge.threading, "Thread", InlineThread), \
                mock.patch.object(bridge, "type_text",
                                  side_effect=lambda text: typed.append(text) or True):
            actions._submit_asr(bytes(640), assistant=True)
        self.assertEqual(spoken, ["你好，我是小智。"])
        self.assertEqual(typed, [])

    def test_manual_voice_still_dictates(self):
        actions = bridge.Actions(_config(), dry_run=False)
        actions.agent = ScriptedAgent(_config(), FakeActions(), [])
        actions._ensure_asr = lambda: types.SimpleNamespace(
            transcribe=lambda pcm: "你好")
        typed = []
        with mock.patch.object(bridge.threading, "Thread", InlineThread), \
                mock.patch.object(bridge, "type_text",
                                  side_effect=lambda text: typed.append(text) or True):
            actions._submit_asr(bytes(640), assistant=False)
        self.assertEqual(typed, ["你好"])


class FrameTests(unittest.TestCase):
    def _bridge(self):
        fake = object.__new__(bridge.Bridge)
        fake.actions = types.SimpleNamespace(config={})
        fake._speak_lock = threading.Lock()
        fake._audio_seq = 0
        fake.lines = []
        fake.send_all = fake.lines.append
        return fake

    def test_show_text_is_utf8_truncated(self):
        fake = self._bridge()
        fake.show_text("智" * 200)
        self.assertTrue(fake.lines[0].startswith("KFB24 TEXT "))
        raw = base64.b64decode(fake.lines[0][len("KFB24 TEXT "):])
        self.assertLessEqual(len(raw), 300)
        self.assertEqual(raw.decode("utf-8"), "智" * 100)

    def test_speak_streams_text_then_audio(self):
        fake = self._bridge()
        pcm = struct.pack("<640h", *([8000, -8000] * 320)) * 2
        with mock.patch.object(bridge, "synthesize", return_value=pcm):
            fake._speak_worker("你好")
        self.assertTrue(fake.lines[0].startswith("KFB24 TEXT "))
        frames = [line for line in fake.lines
                  if line.startswith("KFB24 AUDIO P ")]
        self.assertEqual(len(frames), 3)  # two blocks plus the silence tail
        first = frames[0].split()
        self.assertEqual(first[3], "0")
        crc = int(first[4], 16)
        block = base64.b64decode(first[5])
        import zlib
        self.assertEqual(crc, zlib.crc32(block) & 0xFFFFFFFF)
        self.assertEqual(fake._audio_seq, 3)

    def test_speak_without_transport_is_quiet(self):
        fake = self._bridge()
        with mock.patch.object(bridge, "synthesize", return_value=None):
            fake._speak_worker("你好")
        self.assertEqual(fake.lines[0][:11], "KFB24 TEXT ")
        self.assertEqual(len(fake.lines), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
