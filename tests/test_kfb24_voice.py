#!/usr/bin/env python3
"""Tests for offline voice command routing.

Routing must only fire on a whole utterance: dictation that happens to contain
"发送" or "打开" is typed into the app, not executed as an unrelated action.
"""

import os
import sys
import unittest
import unittest.mock
from pathlib import Path
from tempfile import TemporaryDirectory

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "pc"))

import kfb24_bridge as bridge  # noqa: E402
import kfb24_voice as voice  # noqa: E402


class NormalizeTests(unittest.TestCase):
    def test_folds_width_case_space_and_punctuation(self) -> None:
        self.assertEqual(voice.normalize(" 打开 TRAE。 "), "打开trae")
        self.assertEqual(voice.normalize("打开\u3000微信！"), "打开微信")


class IntentTests(unittest.TestCase):
    def test_alias_alone(self) -> None:
        self.assertEqual(voice.match_intent("Trae").action, "trae")
        self.assertEqual(voice.match_intent("微信。").action, "wechat")

    def test_open_verb(self) -> None:
        self.assertEqual(voice.match_intent("打开微信").action, "wechat")
        self.assertEqual(voice.match_intent("请帮我切换到浏览器").action, "chrome")
        self.assertEqual(voice.match_intent("进入终端吧").action, "terminal")

    def test_submit_phrase(self) -> None:
        self.assertEqual(voice.match_intent("按回车").action, "enter")
        self.assertEqual(voice.match_intent("发送").action, "enter")

    def test_custom_command_wins(self) -> None:
        commands = {"开始干活": "trae"}
        self.assertEqual(voice.match_intent("开始干活", commands).action, "trae")

    def test_dictation_is_not_a_command(self) -> None:
        self.assertIsNone(voice.match_intent("帮我写一个冒泡排序"))
        self.assertIsNone(voice.match_intent("这段代码发送之前要检查"))
        self.assertIsNone(voice.match_intent("打开这个文件看看"))

    def test_empty_and_unknown(self) -> None:
        self.assertIsNone(voice.match_intent(""))
        self.assertIsNone(voice.match_intent("今天天气不错"))


class ModelDirTests(unittest.TestCase):
    def test_accepts_configured_directory(self) -> None:
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "tokens.txt").write_text("a 0\n", encoding="utf-8")
            (root / "model.int8.onnx").write_bytes(b"x")
            self.assertEqual(voice.resolve_model_dir(tmp), root)

    def test_rejects_missing_files(self) -> None:
        with TemporaryDirectory() as tmp, \
                unittest.mock.patch.dict(os.environ, {"LOCALAPPDATA": tmp}):
            (Path(tmp) / "tokens.txt").write_text("a 0\n", encoding="utf-8")
            self.assertIsNone(voice.resolve_model_dir(""))


class DryRunDispatchTests(unittest.TestCase):
    """--dry-run must still exercise the offline backend it reports."""

    def _stub(self, local):
        calls = []

        class Stub:
            dry_run = True
            config = {"voice_hotkey": "ctrl+win"}

            def local_voice(self):
                return local

            def _wake_local(self):
                calls.append("local")
                return True

        return Stub(), calls

    def test_wake_prefers_local_backend(self) -> None:
        stub, calls = self._stub(True)
        self.assertTrue(bridge.Actions.wake(stub))
        self.assertEqual(calls, ["local"])

    def test_wake_falls_back_to_hotkey_dry_run(self) -> None:
        stub, calls = self._stub(False)
        self.assertTrue(bridge.Actions.wake(stub))
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
