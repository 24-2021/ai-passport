"""Local LLM conversation with tool calling for the kfb24 board.

The board is a microphone/speaker peripheral: this module turns a recognised
utterance into a short spoken answer, running entirely against a local model
(Ollama by default, any OpenAI-compatible server when configured). Tools let
the model launch apps, type text, and hand a longer task to a CLI coding agent
(``pi`` by default, ``claude`` when configured).
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import threading
import time
import urllib.error
import urllib.request
from typing import Callable, Optional

LOG: Callable[[str], None] = print

SYSTEM_PROMPT = """你是"小智"，在一块带麦克风和喇叭的 ESP32 桌面设备上陪用户说话。
规则：
- 用口语化的中文回答，最多两句话；不要 markdown、列表、链接、代码块或表情符号。
- 需要操作电脑时调用工具；需要写代码、查资料或较长时间的任务时调用 dispatch_agent。
- 调用 dispatch_agent 前先用一句话告诉用户你在安排，工具返回后再用两句话总结结果。
- 听不清或不需要回答时，说"没听清，请再说一遍"。
"""

APP_NAMES = {
    "wechat": "微信",
    "orca": "Orca",
    "terminal": "终端",
    "trae": "Trae",
    "chrome": "Chrome",
    "edge": "Edge",
}

MAX_REPLY_CHARS = 180
MAX_HISTORY_MESSAGES = 12
MAX_TOOL_ROUNDS = 4


class LLMError(RuntimeError):
    pass


def clean_reply(text: str) -> str:
    """Strip what a speech synthesiser would read badly and cap the length."""
    text = (text or "").strip()
    text = re.sub(r"```.*?```", " ", text, flags=re.S)
    text = re.sub(r"[*_#>`|]+", "", text)
    text = re.sub(r"\[(.*?)\]\([^)]*\)", r"\1", text)
    # Drop emoji, pictographs, and other symbols the board font cannot draw.
    text = re.sub(
        "[\U0001F000-\U0001FAFF\u2600-\u27BF\uFE0F\u200D\u2B00-\u2BFF]", "",
        text,
    )
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) > MAX_REPLY_CHARS:
        cut = max(text.rfind("。", 0, MAX_REPLY_CHARS),
                  text.rfind("！", 0, MAX_REPLY_CHARS),
                  text.rfind("？", 0, MAX_REPLY_CHARS))
        text = text[:cut + 1] if cut > 40 else text[:MAX_REPLY_CHARS]
    return text


class Agent:
    """A small tool-using chat loop; safe to call from a worker thread."""

    def __init__(self, config: dict, actions, speak=None, show_text=None,
                 log=LOG):
        self.config = config
        self.actions = actions
        self.speak = speak
        self.show_text = show_text
        self.log = log
        self._lock = threading.Lock()
        self._history: list[dict] = []
        self._session = threading.local()
        self._tools_enabled = True
        self._model: Optional[str] = None
        self._unreachable_until = 0.0
        self._unreachable_logged = False

    # -- configuration -----------------------------------------------------

    @property
    def enabled(self) -> bool:
        return bool(self.config.get("assistant_enabled", True))

    @property
    def api(self) -> str:
        return str(self.config.get("llm_api", "ollama")).strip().lower()

    @property
    def base_url(self) -> str:
        return str(self.config.get("llm_url",
                                   "http://127.0.0.1:11434")).rstrip("/")

    def _timeout(self) -> float:
        try:
            return max(5.0, min(300.0, float(self.config.get("llm_timeout", 90))))
        except (TypeError, ValueError):
            return 90.0

    def _agent_commands(self) -> dict:
        commands = self.config.get("agent_commands")
        return commands if isinstance(commands, dict) else {}

    # -- HTTP --------------------------------------------------------------

    def _request(self, path: str, payload: Optional[dict] = None,
                 timeout: Optional[float] = None):
        url = self.base_url + path
        data = None
        headers = {"Content-Type": "application/json"}
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        request = urllib.request.Request(url, data=data, headers=headers)
        try:
            with urllib.request.urlopen(request,
                                        timeout=timeout or self._timeout()) as reply:
                return json.load(reply)
        except urllib.error.HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            raise LLMError(f"HTTP {error.code}: {body[:300]}") from error
        except (urllib.error.URLError, OSError, ValueError) as error:
            raise LLMError(str(error)) from error

    def _resolve_model(self) -> str:
        if self._model:
            return self._model
        configured = str(self.config.get("llm_model", "")).strip()
        if configured:
            self._model = configured
            return self._model
        if self.api == "ollama":
            data = self._request("/api/tags", timeout=4.0)
            models = data.get("models") if isinstance(data, dict) else None
            if models:
                self._model = str(models[0].get("name") or models[0].get("model"))
        else:
            data = self._request("/v1/models", timeout=4.0)
            entries = data.get("data") if isinstance(data, dict) else None
            if entries:
                self._model = str(entries[0].get("id"))
        if not self._model:
            raise LLMError("no model available")
        self.log(f"assistant: model {self._model}")
        return self._model

    def available(self) -> bool:
        if not self.enabled:
            return False
        if time.monotonic() < self._unreachable_until:
            return False
        try:
            self._resolve_model()
            return True
        except LLMError as error:
            self._unreachable_until = time.monotonic() + 30.0
            if not self._unreachable_logged:
                self._unreachable_logged = True
                self.log(f"assistant: local model unavailable ({error})")
            return False

    # -- tools -------------------------------------------------------------

    def _tools(self) -> list[dict]:
        tools = [
            {
                "type": "function",
                "function": {
                    "name": "open_app",
                    "description": "打开或切换到电脑上的应用",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "app": {"type": "string", "enum": list(APP_NAMES)},
                        },
                        "required": ["app"],
                    },
                },
            },
            {
                "type": "function",
                "function": {
                    "name": "type_text",
                    "description": "把一段文字输入到当前获得焦点的窗口",
                    "parameters": {
                        "type": "object",
                        "properties": {"text": {"type": "string"}},
                        "required": ["text"],
                    },
                },
            },
            {
                "type": "function",
                "function": {
                    "name": "press_enter",
                    "description": "在当前窗口按一次回车",
                    "parameters": {"type": "object", "properties": {}},
                },
            },
        ]
        agents = list(self._agent_commands())
        if agents:
            tools.append({
                "type": "function",
                "function": {
                    "name": "dispatch_agent",
                    "description": ("把一个需要写代码、查资料或较长时间的"
                                    "任务交给本地 CLI 编程 agent，并返回它的输出"),
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "agent": {"type": "string", "enum": agents},
                            "prompt": {"type": "string"},
                        },
                        "required": ["agent", "prompt"],
                    },
                },
            })
        return tools

    def _run_tool(self, name: str, arguments: dict) -> str:
        try:
            if name == "open_app":
                app = str(arguments.get("app", ""))
                if app not in APP_NAMES:
                    return json.dumps({"ok": False, "error": "unsupported app"})
                ok = self.actions.launch(app)
                return json.dumps({"ok": bool(ok), "app": app},
                                  ensure_ascii=False)
            if name == "type_text":
                from kfb24_voice import type_text
                ok = type_text(str(arguments.get("text", "")))
                return json.dumps({"ok": bool(ok)}, ensure_ascii=False)
            if name == "press_enter":
                ok = self.actions.enter()
                return json.dumps({"ok": bool(ok)}, ensure_ascii=False)
            if name == "dispatch_agent":
                return self._dispatch(str(arguments.get("agent", "")),
                                      str(arguments.get("prompt", "")))
            return json.dumps({"ok": False, "error": f"unknown tool {name}"})
        except Exception as error:  # tool failures must not kill the turn
            self.log(f"assistant: tool {name} failed: {error}")
            return json.dumps({"ok": False, "error": str(error)[:200]},
                              ensure_ascii=False)

    def _dispatch(self, agent: str, prompt: str) -> str:
        commands = self._agent_commands()
        template = commands.get(agent)
        prompt = prompt.strip()[:4000]
        if not template or not prompt:
            return json.dumps({"ok": False, "error": "agent not configured"},
                              ensure_ascii=False)
        if isinstance(template, str):
            template = [template]
        args = [str(part).replace("{prompt}", prompt) for part in template]
        executable = shutil.which(args[0])
        if not executable:
            return json.dumps({"ok": False,
                               "error": f"{args[0]} not found on PATH"},
                              ensure_ascii=False)
        args[0] = executable
        workdir = str(self.config.get("agent_workdir", "")).strip() or None
        timeout = 600.0
        try:
            timeout = max(10.0, min(3600.0,
                                    float(self.config.get("agent_timeout", 600))))
        except (TypeError, ValueError):
            pass
        if self.speak:
            self.speak(f"好的，我让{agent}处理，请稍等。")
        self.log(f"assistant: dispatch {agent}: {prompt[:120]}")
        started = time.monotonic()
        try:
            result = subprocess.run(
                args, cwd=workdir, capture_output=True, timeout=timeout,
                stdin=subprocess.DEVNULL,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            output = (result.stdout + result.stderr).decode(
                "utf-8", errors="replace").strip()
            ok = result.returncode == 0
            self.log(f"assistant: {agent} exited {result.returncode} in "
                     f"{time.monotonic() - started:.1f}s")
        except subprocess.TimeoutExpired:
            return json.dumps({"ok": False, "error": "agent timed out"},
                              ensure_ascii=False)
        except OSError as error:
            return json.dumps({"ok": False, "error": str(error)[:200]},
                              ensure_ascii=False)
        return json.dumps({"ok": ok, "agent": agent,
                           "output": output[-6000:]}, ensure_ascii=False)

    # -- chat --------------------------------------------------------------

    def _complete(self, messages: list[dict], tools: list[dict]) -> dict:
        if self.api == "ollama":
            payload = {
                "model": self._resolve_model(),
                "messages": messages,
                "stream": False,
                "think": False,
                "options": {"temperature": 0.3, "num_predict": 320},
            }
            if tools:
                payload["tools"] = tools
            message = self._request("/api/chat", payload)["message"]
            calls = []
            for index, call in enumerate(message.get("tool_calls") or []):
                function = call.get("function") or {}
                arguments = function.get("arguments") or {}
                if isinstance(arguments, str):
                    try:
                        arguments = json.loads(arguments)
                    except json.JSONDecodeError:
                        arguments = {}
                calls.append({"id": call.get("id") or f"call_{index}",
                              "name": function.get("name", ""),
                              "arguments": arguments})
            return {"content": message.get("content") or "", "tool_calls": calls}

        payload = {
            "model": self._resolve_model(),
            "messages": messages,
            "stream": False,
            "temperature": 0.3,
            "max_tokens": 320,
        }
        if tools and self._tools_enabled:
            payload["tools"] = tools
        try:
            data = self._request("/v1/chat/completions", payload)
        except LLMError as error:
            if tools and self._tools_enabled and "HTTP 400" in str(error):
                self.log("assistant: server rejected tools; continuing without")
                self._tools_enabled = False
                payload.pop("tools", None)
                data = self._request("/v1/chat/completions", payload)
            else:
                raise
        message = data["choices"][0]["message"]
        calls = []
        for index, call in enumerate(message.get("tool_calls") or []):
            function = call.get("function") or {}
            arguments = function.get("arguments") or "{}"
            if isinstance(arguments, str):
                try:
                    arguments = json.loads(arguments)
                except json.JSONDecodeError:
                    arguments = {}
            calls.append({"id": call.get("id") or f"call_{index}",
                          "name": function.get("name", ""),
                          "arguments": arguments})
        return {"content": message.get("content") or "", "tool_calls": calls}

    def _remember(self, messages: list[dict]) -> None:
        # Keep the system prompt out of the persisted history; tool output can
        # be long, so trim it before it becomes context for the next turn.
        kept = []
        for message in messages[1:]:
            if message.get("role") == "system":
                continue
            message = dict(message)
            if message.get("tool_calls"):
                message.pop("tool_calls", None)
            if message.get("role") == "tool":
                message["content"] = str(message.get("content", ""))[:1500]
            kept.append(message)
        self._history = kept[-MAX_HISTORY_MESSAGES:]

    def respond(self, text: str) -> Optional[str]:
        """Return a short spoken reply, or None when the model is unavailable."""
        if not self.available():
            return None
        if not self._lock.acquire(blocking=False):
            return "还在处理上一个任务，请稍等一下。"
        try:
            if self.show_text:
                self.show_text("小智正在思考…")
            messages = ([{"role": "system", "content": SYSTEM_PROMPT}] +
                        self._history + [{"role": "user", "content": text}])
            for _ in range(MAX_TOOL_ROUNDS):
                reply = self._complete(messages, self._tools())
                calls = reply["tool_calls"]
                if not calls:
                    answer = clean_reply(reply["content"])
                    if not answer:
                        answer = "我没听清，请再说一遍。"
                    messages.append({"role": "assistant", "content": answer})
                    self._remember(messages)
                    return answer
                if self.api == "ollama":
                    wire_calls = [{"function": {"name": c["name"],
                                                "arguments": c["arguments"]}}
                                  for c in calls]
                else:
                    wire_calls = [{"id": c["id"], "type": "function",
                                   "function": {"name": c["name"],
                                                "arguments": json.dumps(
                                                    c["arguments"],
                                                    ensure_ascii=False)}}
                                  for c in calls]
                messages.append({"role": "assistant", "content": reply["content"],
                                 "tool_calls": wire_calls})
                for call in calls:
                    result = self._run_tool(call["name"], call["arguments"])
                    if self.api == "ollama":
                        messages.append({"role": "tool", "content": result,
                                         "tool_name": call["name"]})
                    else:
                        messages.append({"role": "tool", "tool_call_id": call["id"],
                                         "content": result})
            return "这个任务有点复杂，我先做到这里。"
        except LLMError as error:
            self.log(f"assistant: chat failed ({error})")
            self._unreachable_until = time.monotonic() + 30.0
            return None
        except Exception as error:  # malformed reply, encoding, tool bug
            self.log(f"assistant: unexpected failure ({error})")
            return None
        finally:
            self._lock.release()

    def reset(self) -> None:
        with self._lock:
            self._history = []
