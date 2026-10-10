"""Per-connection Xiaozhi session: protocol state machine + ASR/LLM/TTS pipeline.

The chat pipeline (ASR -> LLM -> TTS) runs as a cancellable task so a new
``listen start`` or an ``abort`` from the device immediately stops playback.
All websocket writes go through a lock because Starlette is not safe for
concurrent sends from multiple tasks.
"""

from __future__ import annotations

import asyncio
import json
import logging
import uuid
from typing import Any

from fastapi import WebSocket

from . import __version__, protocol
from .audio import OpusCodec, apply_output_gain, pcm_rms
from .config import settings
from .providers import build_asr, build_llm, build_tts

log = logging.getLogger("xz.session")

_SENTENCE_END = "。！？；!?;\n"
_MAX_SENTENCE_CHARS = 60


def _first_boundary(s: str) -> int:
    for i, ch in enumerate(s):
        if ch in _SENTENCE_END:
            return i
    return -1


class ChatSession:
    def __init__(self, ws: WebSocket, remote: str):
        self.ws = ws
        self.remote = remote
        self.session_id = uuid.uuid4().hex

        self.asr = build_asr()
        self.llm = build_llm()
        self.tts = build_tts()

        self.codec: OpusCodec | None = None
        self.audio_params: dict[str, Any] = {
            "format": "opus",
            "sample_rate": settings.default_sample_rate,
            "channels": 1,
            "frame_duration": settings.default_frame_ms,
        }
        self.history: list[dict] = [{"role": "system", "content": settings.llm_system_prompt}]

        self._send_lock = asyncio.Lock()
        self._input = bytearray()
        self._listening = False
        self._speaking = False
        self._mode = protocol.MODE_AUTO
        self._task: asyncio.Task | None = None

        # simple energy VAD state (auto mode)
        self._speech_seen = False
        self._silence_ms = 0

        # MCP client（设备端工具，AI 控制硬件）
        self.mcp = McpClient(self._send_json, settings.mcp_request_timeout)
        self._mcp_task: asyncio.Task | None = None
        self._tool_name_map: dict[str, str] = {}

    # ------------------------------------------------------------------ send
    async def _send_json(self, obj: dict) -> None:
        async with self._send_lock:
            await self.ws.send_text(protocol.dumps(obj))

    async def _send_bytes(self, data: bytes) -> None:
        async with self._send_lock:
            await self.ws.send_bytes(data)

    async def send_pcm(self, pcm: bytes) -> None:
        if self.codec is None or not pcm:
            return
        for packet in self.codec.encode(pcm):
            await self._send_bytes(packet)

    # --------------------------------------------------------------- inbound
    async def on_text(self, text: str) -> None:
        try:
            msg = protocol.loads(text)
        except Exception:
            log.warning("[%s] bad JSON: %r", self.remote, text[:120])
            return

        mtype = msg.get("type")
        if mtype == protocol.TYPE_HELLO:
            await self._on_hello(msg)
        elif mtype == protocol.TYPE_LISTEN:
            await self._on_listen(msg)
        elif mtype == protocol.TYPE_ABORT:
            await self.abort()
        elif mtype == protocol.TYPE_MCP:
            await self._on_mcp(msg)
        else:
            log.debug("[%s] ignored message type %r", self.remote, mtype)

    async def on_bytes(self, data: bytes) -> None:
        if not self._listening or self.codec is None:
            return
        try:
            pcm = self.codec.decode(data)
        except Exception as exc:
            log.warning("[%s] opus decode failed: %s", self.remote, exc)
            return
        if not pcm:
            return

        self._input += pcm
        max_bytes = settings.max_utterance_sec * self.codec.sample_rate * 2 * self.codec.channels
        if len(self._input) > max_bytes:
            del self._input[: len(self._input) - max_bytes]

        if self._mode == protocol.MODE_AUTO:
            if pcm_rms(pcm) >= settings.vad_threshold:
                self._speech_seen = True
                self._silence_ms = 0
            elif self._speech_seen:
                self._silence_ms += self.codec.frame_ms
                if self._silence_ms >= settings.vad_silence_ms:
                    log.info("[%s] VAD: end of speech", self.remote)
                    self._listening = False
                    self._start_pipeline()

    # ---------------------------------------------------------------- control
    async def _on_hello(self, msg: dict) -> None:
        ap = msg.get("audio_params") or {}
        fmt = str(ap.get("format") or "opus").lower()
        sample_rate = int(ap.get("sample_rate") or settings.default_sample_rate)
        channels = int(ap.get("channels") or 1)
        frame_ms = int(ap.get("frame_duration") or settings.default_frame_ms)

        self.audio_params = {
            "format": fmt,
            "sample_rate": sample_rate,
            "channels": channels,
            "frame_duration": frame_ms,
        }
        if fmt == "opus":
            try:
                self.codec = OpusCodec(sample_rate, channels, frame_ms)
            except Exception as exc:
                log.error("[%s] cannot init Opus codec: %s", self.remote, exc)
                self.codec = None
        else:
            log.warning("[%s] device asked for %s audio; only Opus is implemented", self.remote, fmt)
            self.codec = None

        log.info("[%s] hello %s", self.remote, self.audio_params)
        await self._send_json(protocol.hello_ack(self.session_id, self.audio_params))

        if settings.mcp_enabled:
            self._mcp_task = asyncio.create_task(self._mcp_setup())

    async def _on_listen(self, msg: dict) -> None:
        state = msg.get("state")
        mode = msg.get("mode") or self._mode

        if state == protocol.LISTEN_DETECT:
            # wake word detected -> treat as start
            await self._on_listen({"state": protocol.LISTEN_START, "mode": mode})
            return

        if state == protocol.LISTEN_START:
            await self.abort()
            self._input.clear()
            self._speech_seen = False
            self._silence_ms = 0
            self._mode = (
                protocol.MODE_MANUAL if mode == protocol.MODE_MANUAL else protocol.MODE_AUTO
            )
            self._listening = True
            log.info("[%s] listening (mode=%s)", self.remote, self._mode)
        elif state == protocol.LISTEN_STOP:
            if self._listening:
                self._listening = False
                self._start_pipeline()

    async def abort(self) -> None:
        """Cancel any running pipeline and tell the device to stop playing."""
        task, self._task = self._task, None
        if task and not task.done():
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        if self._speaking:
            self._speaking = False
            try:
                await self._send_json(protocol.tts(protocol.TTS_STOP))
            except Exception:
                pass

    async def close(self) -> None:
        task, self._mcp_task = self._mcp_task, None
        if task and not task.done():
            task.cancel()
        await self.abort()

    # --------------------------------------------------------------- pipeline
    def _start_pipeline(self) -> None:
        if self._task and not self._task.done():
            return
        self._task = asyncio.create_task(self._pipeline())

    async def _pipeline(self) -> None:
        pcm = bytes(self._input)
        self._input.clear()
        self._speech_seen = False
        self._silence_ms = 0
        if not pcm or self.codec is None:
            return

        try:
            # 1) speech -> text
            text = (await self.asr.transcribe(pcm, self.codec.sample_rate)).strip()
            if not text:
                log.info("[%s] ASR returned empty, nothing to answer", self.remote)
                return
            await self._send_json(protocol.stt(text))
            self.history.append({"role": "user", "content": text})

            # 2) LLM streaming（可带设备端 MCP 工具做 function calling，逐句 TTS）
            self._speaking = True
            await self._send_json(protocol.tts(protocol.TTS_START))

            tools = self._llm_tools() if self.mcp.initialized else None
            messages: list[dict] = list(self.history)
            # 本轮新增的工具调用/结果消息，结束时并入 history，
            # 让模型在后续轮次能"看到"它真实执行过什么（避免幻觉式"已完成"）。
            new_items: list[dict] = []
            reply_parts: list[str] = []
            spoken_any = False
            max_rounds = settings.llm_tool_max_rounds if tools else 0

            for round_idx in range(max_rounds + 1):
                round_tools = tools if round_idx < max_rounds else None
                round_text: list[str] = []
                calls: list[dict] = []
                buf = ""

                async for ev in self.llm.stream_chat(messages, round_tools):
                    if ev.get("type") == "text":
                        piece = ev["text"]
                        round_text.append(piece)
                        reply_parts.append(piece)
                        buf += piece
                        while True:
                            idx = _first_boundary(buf)
                            if idx < 0:
                                break
                            sentence, buf = buf[: idx + 1], buf[idx + 1 :]
                            if sentence.strip():
                                spoken_any = True
                                await self._speak(sentence)
                        if len(buf) >= _MAX_SENTENCE_CHARS:
                            spoken_any = True
                            await self._speak(buf)
                            buf = ""
                    elif ev.get("type") == "tool_calls":
                        calls = ev["calls"]

                if buf.strip():
                    spoken_any = True
                    await self._speak(buf)

                if not calls:
                    break

                # 模型本轮要调用工具：先垫一句（如果它一句话都没说），
                # 再把 assistant(tool_calls) 与工具结果写回上下文继续生成。
                if not spoken_any and settings.llm_tool_filler and round_idx == 0:
                    await self._speak(settings.llm_tool_filler)

                assistant_msg = {
                    "role": "assistant",
                    "content": "".join(round_text) or None,
                    "tool_calls": [
                        {
                            "id": c["id"],
                            "type": "function",
                            "function": {"name": c["name"], "arguments": c["arguments"]},
                        }
                        for c in calls
                    ],
                }
                messages.append(assistant_msg)
                new_items.append(assistant_msg)
                for c in calls:
                    result = await self._execute_tool(c)
                    tool_msg = {"role": "tool", "tool_call_id": c["id"], "content": result}
                    messages.append(tool_msg)
                    new_items.append(tool_msg)

            reply = "".join(reply_parts).strip()
            if new_items:
                self.history.extend(new_items)
            if reply:
                self.history.append({"role": "assistant", "content": reply})
            if len(self.history) > 21:
                self.history = [self.history[0], *self.history[-20:]]

            await self._send_json(protocol.tts(protocol.TTS_STOP))
            self._speaking = False
        except asyncio.CancelledError:
            log.info("[%s] pipeline cancelled", self.remote)
            raise
        except Exception as exc:
            log.exception("[%s] pipeline failed: %s", self.remote, exc)
            # 出错时也要收尾，否则设备会一直停在“正在播放”状态
            if self._speaking:
                self._speaking = False
                try:
                    await self._send_json(protocol.tts(protocol.TTS_STOP))
                except Exception:  # noqa: BLE001 - 连接可能已断
                    pass

    async def _speak(self, sentence: str) -> None:
        sentence = sentence.strip()
        if not sentence:
            return
        log.info("[%s] TTS: %r", self.remote, sentence[:60])
        await self._send_json(protocol.tts(protocol.TTS_SENTENCE_START, sentence))
        pcm = await self.tts.synthesize(sentence)
        if pcm and (settings.tts_normalize or settings.tts_extra_gain_db != 0.0):
            pcm = apply_output_gain(
                pcm,
                target_dbfs=settings.tts_normalize_dbfs,
                max_gain_db=settings.tts_max_gain_db,
                extra_gain_db=settings.tts_extra_gain_db,
            )
        await self.send_pcm(pcm)

    # -------------------------------------------------------------------- MCP
    #
    # 小智协议里设备端是 MCP Server（工具跑在开发板上），服务端是 MCP Client：
    #   server -> device : initialize / notifications/initialized / tools/list / tools/call
    #   device -> server : 对应的 JSON-RPC response
    # LLM(DeepSeek) 用 tools/list 得到的 schema 做 function calling，
    # 服务端把模型选中的调用通过 tools/call 下发到设备执行，结果回填后继续生成。

    async def _on_mcp(self, msg: dict) -> None:
        payload = msg.get("payload")
        if isinstance(payload, dict):
            self.mcp.handle_payload(payload)

    async def _mcp_setup(self) -> None:
        # hello 之后设备才会创建 MCP manager，稍等一下再 initialize；
        # 失败按 mcp_init_retry 重试（设备还没就绪时不会回复，会超时）。
        await asyncio.sleep(0.3)
        for attempt in range(1, settings.mcp_init_retry + 1):
            try:
                await self.mcp.initialize()
                await self.mcp.refresh_tools()
                log.info(
                    "[%s] MCP ready, %d tool(s): %s",
                    self.remote,
                    len(self.mcp.tools),
                    ", ".join(str(t.get("name", "?")) for t in self.mcp.tools) or "(none)",
                )
                return
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001 - 会话继续，只是没有工具
                log.info(
                    "[%s] MCP init attempt %d/%d failed: %s",
                    self.remote,
                    attempt,
                    settings.mcp_init_retry,
                    exc,
                )
                await asyncio.sleep(0.7)
        log.warning("[%s] MCP unavailable, AI hardware control disabled for this session", self.remote)

    def _llm_tools(self) -> list[dict] | None:
        if not (settings.mcp_enabled and self.mcp.tools):
            return None
        self._tool_name_map = {}
        tools: list[dict] = []
        for t in self.mcp.tools:
            device_name = str(t.get("name") or "")
            if not device_name:
                continue
            llm_name = _llm_safe_name(device_name)
            self._tool_name_map[llm_name] = device_name
            schema = t.get("inputSchema")
            if not isinstance(schema, dict):
                schema = {"type": "object", "properties": {}}
            tools.append(
                {
                    "type": "function",
                    "function": {
                        "name": llm_name,
                        "description": str(t.get("description") or device_name),
                        "parameters": schema,
                    },
                }
            )
        return tools or None

    async def _execute_tool(self, call: dict) -> str:
        llm_name = str(call.get("name") or "")
        device_name = self._tool_name_map.get(llm_name, llm_name)
        try:
            args = json.loads(call.get("arguments") or "{}")
            if not isinstance(args, dict):
                args = {}
        except json.JSONDecodeError:
            args = {}
        log.info("[%s] MCP tools/call -> %s %s", self.remote, device_name, args)
        try:
            return await self.mcp.call_tool(device_name, args)
        except Exception as exc:  # noqa: BLE001
            log.warning("[%s] tool %s failed: %s", self.remote, device_name, exc)
            return f"工具执行失败：{exc}"


MCP_PROTOCOL_VERSION = "2024-11-05"


class McpError(RuntimeError):
    """MCP 请求返回错误。"""


def _llm_safe_name(name: str) -> str:
    """OpenAI function 名称只允许 [a-zA-Z0-9_-]，把 self.gpio.set_output 换成安全名。"""
    safe = "".join(ch if (ch.isalnum() or ch in "_-") else "_" for ch in name)
    return safe[:64] or "tool"


class McpClient:
    """设备端 MCP Server 的客户端（JSON-RPC over 小智 WebSocket）。"""

    def __init__(self, send_json, request_timeout: float):
        self._send_json = send_json
        self._timeout = request_timeout
        self._next_id = 1
        self._pending: dict[int, asyncio.Future] = {}
        self.tools: list[dict] = []
        self.initialized = False

    async def _request(self, method: str, params: dict | None) -> dict:
        req_id = self._next_id
        self._next_id += 1
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self._pending[req_id] = fut
        payload: dict = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            payload["params"] = params
        try:
            await self._send_json(protocol.mcp(payload))
            return await asyncio.wait_for(fut, self._timeout)
        finally:
            self._pending.pop(req_id, None)

    async def _notify(self, method: str, params: dict | None = None) -> None:
        payload: dict = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            payload["params"] = params
        await self._send_json(protocol.mcp(payload))

    def handle_payload(self, payload: dict) -> None:
        """处理设备发来的 mcp payload（本实现里主要是请求的 response）。"""
        req_id = payload.get("id")
        if req_id in self._pending and ("result" in payload or "error" in payload):
            fut = self._pending[req_id]
            if fut.done():
                return
            if "error" in payload:
                fut.set_exception(McpError(str(payload.get("error"))))
            else:
                result = payload.get("result")
                fut.set_result(result if isinstance(result, dict) else {})
            return
        method = payload.get("method")
        if method:
            log.debug("MCP message from device (ignored): %s", method)

    async def initialize(self) -> None:
        result = await self._request(
            "initialize",
            {
                "protocolVersion": MCP_PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "xiaozhi-fastapi-server", "version": __version__},
            },
        )
        negotiated = str(result.get("protocolVersion") or MCP_PROTOCOL_VERSION)
        if negotiated > MCP_PROTOCOL_VERSION:
            await self._notify("notifications/initialized")
        self.initialized = True

    async def refresh_tools(self) -> None:
        result = await self._request("tools/list", {})
        tools = result.get("tools") or []
        self.tools = [t for t in tools if isinstance(t, dict) and t.get("name")]

    async def call_tool(self, name: str, arguments: dict) -> str:
        result = await self._request("tools/call", {"name": name, "arguments": arguments})
        texts: list[str] = []
        for block in result.get("content") or []:
            if isinstance(block, dict) and block.get("type") == "text" and block.get("text"):
                texts.append(str(block["text"]))
        if not texts:
            texts.append(json.dumps(result, ensure_ascii=False)[:500])
        return "\n".join(texts)
