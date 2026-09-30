"""LLM providers (text -> streamed text / tool calls). Default: DeepSeek chat completions.

``stream_chat`` yields event dicts:

* ``{"type": "text", "text": "..."}``              流式文本增量
* ``{"type": "tool_calls", "calls": [...]}``       本轮模型请求调用的工具（流式合并后的完整参数）

calls 元素：``{"id": str, "name": str, "arguments": str(JSON)}``
"""

from __future__ import annotations

import asyncio
import json
import logging
from abc import ABC, abstractmethod
from typing import AsyncIterator

import httpx

from ..config import settings

log = logging.getLogger("xz.llm")


def _merge_tool_call_delta(slots: dict[int, dict], tc: dict) -> None:
    idx = int(tc.get("index", 0))
    slot = slots.setdefault(idx, {"id": None, "name": None, "arguments": ""})
    if tc.get("id"):
        slot["id"] = tc["id"]
    fn = tc.get("function") or {}
    if fn.get("name"):
        slot["name"] = fn["name"]
    if fn.get("arguments"):
        slot["arguments"] += fn["arguments"]


def _finish_tool_calls(slots: dict[int, dict]) -> list[dict]:
    calls = []
    for idx in sorted(slots):
        slot = slots[idx]
        calls.append(
            {
                "id": slot["id"] or f"call_{idx}",
                "name": slot["name"] or "",
                "arguments": slot["arguments"] or "{}",
            }
        )
    return calls


class LLM(ABC):
    name = "base"

    @abstractmethod
    def stream_chat(self, messages: list[dict], tools: list[dict] | None = None) -> AsyncIterator[dict]:
        ...

    def stream(self, messages: list[dict]) -> AsyncIterator[str]:
        """兼容旧接口：只取文本增量。"""

        async def _gen() -> AsyncIterator[str]:
            async for ev in self.stream_chat(messages, None):
                if ev.get("type") == "text":
                    yield ev["text"]

        return _gen()


class OpenAICompatLLM(LLM):
    """OpenAI-compatible ``/chat/completions`` with streaming (DeepSeek, OpenAI, ...)."""

    name = "openai"

    async def _aiter(self, messages: list[dict], tools: list[dict] | None = None) -> AsyncIterator[dict]:
        url = settings.llm_base_url.rstrip("/") + "/chat/completions"
        headers = {
            "Authorization": f"Bearer {settings.llm_api_key}",
            "Content-Type": "application/json",
        }
        payload = {
            "model": settings.llm_model,
            "messages": messages,
            "stream": True,
            "temperature": settings.llm_temperature,
            "max_tokens": settings.llm_max_tokens,
        }
        if tools:
            payload["tools"] = tools
            payload["tool_choice"] = "auto"

        timeout = httpx.Timeout(connect=10.0, read=120.0, write=30.0, pool=10.0)
        slots: dict[int, dict] = {}
        async with httpx.AsyncClient(timeout=timeout) as client:
            async with client.stream("POST", url, headers=headers, json=payload) as resp:
                if resp.status_code >= 400:
                    body = (await resp.aread()).decode("utf-8", "replace")
                    raise RuntimeError(f"LLM HTTP {resp.status_code}: {body[:300]}")
                async for line in resp.aiter_lines():
                    if not line or not line.startswith("data:"):
                        continue
                    chunk = line[5:].strip()
                    if chunk == "[DONE]":
                        break
                    try:
                        obj = json.loads(chunk)
                    except json.JSONDecodeError:
                        continue
                    choices = obj.get("choices") or []
                    if not choices:
                        continue
                    delta = choices[0].get("delta") or {}
                    piece = delta.get("content")
                    if piece:
                        yield {"type": "text", "text": piece}
                    for tc in delta.get("tool_calls") or []:
                        _merge_tool_call_delta(slots, tc)

        if slots:
            yield {"type": "tool_calls", "calls": _finish_tool_calls(slots)}

    def stream_chat(self, messages: list[dict], tools: list[dict] | None = None) -> AsyncIterator[dict]:
        return self._aiter(messages, tools)


class MockLLM(LLM):
    name = "mock"

    _REPLY = "我是小智，现在还没有配置大模型密钥。请把 DeepSeek 的 API Key 填进 .env，我就能正常回答你了。"
    _TOOL_REPLY = "好的，已经完成了。"

    async def _aiter(self, messages: list[dict], tools: list[dict] | None = None) -> AsyncIterator[dict]:
        if any(m.get("role") == "tool" for m in messages):
            reply = self._TOOL_REPLY
        else:
            log.warning("LLM not configured -> mock reply")
            reply = self._REPLY
        for ch in reply:
            await asyncio.sleep(0.01)
            yield {"type": "text", "text": ch}

    def stream_chat(self, messages: list[dict], tools: list[dict] | None = None) -> AsyncIterator[dict]:
        return self._aiter(messages, tools)


def build_llm() -> LLM:
    provider = settings.llm_provider.lower()
    if provider in ("deepseek", "openai") and settings.llm_api_key:
        log.info("LLM provider: %s (%s / %s)", provider, settings.llm_base_url, settings.llm_model)
        return OpenAICompatLLM()
    if provider != "mock":
        log.warning("LLM provider %r needs XZ_LLM_API_KEY; falling back to mock", provider)
    return MockLLM()
