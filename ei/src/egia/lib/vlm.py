"""OpenAI-compatible VLM chat-completions client.

Reusable async client for vision-language model endpoints (``/chat/completions``):
messages carry text and image parts, so any layer that needs model inference —
``core/decision``, ``core/context``, replay tooling — shares this one transport.
Blocking stdlib I/O runs off the event loop via ``asyncio.to_thread``; retries
with backoff are left to the caller, which owns the retry policy.

Sits in ``egia/lib/``: imports no other ``egia`` module.
"""

from __future__ import annotations

import asyncio
import base64
import json
import os
import urllib.error
import urllib.request
from collections.abc import Callable
from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel

__all__ = [
  "ChatMessage",
  "ContentPart",
  "ModelClientError",
  "ModelResponse",
  "ModelStreamDelta",
  "ReasoningEffort",
  "Usage",
  "VLMClient",
  "image_part",
  "text_part",
]

ReasoningEffort = Literal["none", "low", "medium", "high"]


class ContentPart(BaseModel):
  """One part of a multimodal message (OpenAI content-parts wire shape)."""

  type: Literal["text", "image_url"]
  text: str | None = None
  image_url: dict[str, str] | None = None


def text_part(text: str) -> ContentPart:
  return ContentPart(type="text", text=text)


def image_part(data: bytes | Path | str, mime: str = "image/png") -> ContentPart:
  """Build an image part from raw bytes, a file path, or an http(s) URL."""
  if isinstance(data, str) and data.startswith(("http://", "https://")):
    url = data
  else:
    raw = Path(data).read_bytes() if isinstance(data, Path) else data
    if isinstance(raw, str):
      raw = raw.encode()
    url = f"data:{mime};base64,{base64.b64encode(raw).decode()}"
  return ContentPart(type="image_url", image_url={"url": url})


class ChatMessage(BaseModel):
  role: Literal["system", "user", "assistant"]
  content: str | list[ContentPart]


class Usage(BaseModel):
  prompt_tokens: int = 0
  completion_tokens: int = 0
  total_tokens: int = 0


class ModelResponse(BaseModel):
  """One completion: the assistant text plus token accounting."""

  text: str
  thinking: str | None = None
  model: str = ""
  usage: Usage = Usage()
  finish_reason: str = ""


class ModelStreamDelta(BaseModel):
  """One provider-visible text fragment from a streaming completion.

  ``reasoning`` is emitted only when the endpoint explicitly returns a
  separate reasoning field. It is not inferred from, or extracted out of,
  the assistant answer.
  """

  kind: Literal["content", "reasoning"]
  text: str
  model: str = ""


class ModelClientError(Exception):
  """Raised on transport failure, non-2xx status, or malformed response body."""


class VLMClient:
  """Async client for one OpenAI-compatible chat-completions endpoint."""

  def __init__(
    self,
    endpoint: str,
    model: str,
    *,
    api_key: str | None = None,
    api_key_env: str = "EGIA_MODEL_API_KEY",
    timeout_seconds: float = 60.0,
    context_window_tokens: int = 262144,
    max_tokens: int = 2048,
    send_max_tokens: bool = True,
    temperature: float | None = None,
    send_temperature: bool = True,
    reasoning_effort: ReasoningEffort | None = None,
    reasoning_options: tuple[ReasoningEffort, ...] = (),
  ) -> None:
    self._url = f"{endpoint.rstrip('/')}/chat/completions"
    self._model = model
    self._api_key = api_key if api_key is not None else os.environ.get(api_key_env, "")
    self._timeout = timeout_seconds
    self._context_window_tokens = context_window_tokens
    self._max_tokens = max_tokens
    self._send_max_tokens = send_max_tokens
    self._temperature = temperature
    self._send_temperature = send_temperature
    self._reasoning_effort = reasoning_effort
    self._reasoning_options = reasoning_options

  @classmethod
  def from_config(cls, config: Any) -> VLMClient:
    """Build from a ``egia.lib.config.ModelConfig`` (typed as Any to keep lib leaf-pure)."""
    return cls(
      endpoint=config.endpoint,
      model=config.model,
      api_key_env=config.api_key_env,
      timeout_seconds=config.timeout_seconds,
      context_window_tokens=getattr(config, "context_window_tokens", 262144),
      max_tokens=config.max_tokens,
      send_max_tokens=getattr(config, "send_max_tokens", True),
      temperature=getattr(config, "temperature", None),
      send_temperature=getattr(config, "send_temperature", True),
      reasoning_effort=getattr(config, "reasoning_effort", None),
      reasoning_options=tuple(getattr(config, "reasoning_options", ())),
    )

  async def complete(
    self,
    messages: list[ChatMessage],
    *,
    max_tokens: int | None = None,
    temperature: float | None = None,
    reasoning_effort: ReasoningEffort | None = None,
    on_delta: Callable[[ModelStreamDelta], None] | None = None,
  ) -> ModelResponse:
    """Request one completion; blocking HTTP runs off the event loop."""
    payload: dict[str, Any] = {
      "model": self._model,
      "messages": [m.model_dump(exclude_none=True) for m in messages],
    }
    if self._send_max_tokens:
      payload["max_tokens"] = max_tokens or self._max_tokens
    # A binding-pinned temperature wins: some endpoints (reasoning models)
    # accept exactly one value and reject anything the caller asks for.
    temperature = self._temperature if self._temperature is not None else temperature
    if self._send_temperature and temperature is not None:
      payload["temperature"] = temperature
    # Provider-neutral effort maps to each endpoint's supported wire shape.
    # Gemma4 under llama.cpp reads enable_thinking from the chat template,
    # while Qwen3-compatible endpoints expose it as a top-level field.
    effort = (
      reasoning_effort if reasoning_effort is not None else self._reasoning_effort
    )
    if reasoning_effort is not None and reasoning_effort not in self._reasoning_options:
      raise ValueError(
        f"reasoning effort {reasoning_effort!r} is not enabled for this model"
      )
    model_name = self._model.lower()
    if model_name.startswith("gemma4") and effort is not None:
      payload["chat_template_kwargs"] = {"enable_thinking": effort != "none"}
    elif model_name.startswith("qwen3") and effort is not None:
      payload["enable_thinking"] = effort != "none"
    elif effort not in (None, "none"):
      payload["reasoning_effort"] = effort
    if on_delta is not None:
      payload["stream"] = True
      payload["stream_options"] = {"include_usage": True}
      return await asyncio.to_thread(self._post_stream, payload, on_delta)
    return await asyncio.to_thread(self._post, payload)

  def _request(self, payload: dict[str, Any]):
    """Open one authenticated request; response ownership stays with caller."""
    headers = {"Content-Type": "application/json", "Accept": "application/json"}
    if self._api_key:
      headers["Authorization"] = f"Bearer {self._api_key}"
    request = urllib.request.Request(
      self._url,
      data=json.dumps(payload).encode(),
      headers=headers,
      method="POST",
    )
    return urllib.request.urlopen(request, timeout=self._timeout)

  def _post(self, payload: dict[str, Any]) -> ModelResponse:
    try:
      with self._request(payload) as resp:
        body = json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
      detail = exc.read().decode("utf-8", "replace")[:512]
      raise ModelClientError(
        f"model endpoint returned {exc.code}: {exc.reason} — {detail}"
      ) from exc
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
      raise ModelClientError(f"model request failed: {exc}") from exc

    return self._response_from_body(body)

  def _post_stream(
    self,
    payload: dict[str, Any],
    on_delta: Callable[[ModelStreamDelta], None],
  ) -> ModelResponse:
    """Consume an OpenAI-compatible SSE response and assemble its result.

    Endpoints that ignore ``stream=true`` and return ordinary JSON are
    accepted as a compatibility fallback; their complete visible fields are
    emitted once so the operator interface still updates consistently.
    """
    try:
      with self._request(payload) as resp:
        content_type = str(resp.headers.get("Content-Type", "")).lower()
        if "text/event-stream" not in content_type:
          body = json.loads(resp.read().decode())
          response = self._response_from_body(body)
          if response.thinking:
            on_delta(
              ModelStreamDelta(
                kind="reasoning", text=response.thinking, model=response.model
              )
            )
          if response.text:
            on_delta(
              ModelStreamDelta(kind="content", text=response.text, model=response.model)
            )
          return response

        content_parts: list[str] = []
        reasoning_parts: list[str] = []
        model = ""
        finish_reason = ""
        usage = Usage()
        for raw_line in resp:
          line = raw_line.decode("utf-8", "replace").strip()
          if not line.startswith("data:"):
            continue
          data = line[5:].strip()
          if not data or data == "[DONE]":
            continue
          chunk = json.loads(data)
          model = str(chunk.get("model") or model)
          if chunk.get("usage"):
            usage = Usage.model_validate(chunk["usage"])
          choices = chunk.get("choices") or []
          if not choices:
            continue
          choice = choices[0]
          finish_reason = str(choice.get("finish_reason") or finish_reason)
          delta = choice.get("delta") or {}
          reasoning = (
            delta.get("reasoning_content")
            or delta.get("reasoning")
            or delta.get("thinking")
          )
          if isinstance(reasoning, str) and reasoning:
            reasoning_parts.append(reasoning)
            on_delta(ModelStreamDelta(kind="reasoning", text=reasoning, model=model))
          content = self._text_content(delta.get("content"))
          if content:
            content_parts.append(content)
            on_delta(ModelStreamDelta(kind="content", text=content, model=model))
        thinking = "".join(reasoning_parts) or None
        text = "".join(content_parts)
        if not text:
          text = thinking or ""
        return ModelResponse(
          text=text,
          thinking=thinking,
          model=model,
          usage=usage,
          finish_reason=finish_reason,
        )
    except urllib.error.HTTPError as exc:
      detail = exc.read().decode("utf-8", "replace")[:512]
      raise ModelClientError(
        f"model endpoint returned {exc.code}: {exc.reason} — {detail}"
      ) from exc
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
      raise ModelClientError(f"model stream failed: {exc}") from exc

  @staticmethod
  def _text_content(content: Any) -> str:
    if isinstance(content, str):
      return content
    if isinstance(content, list):
      return "".join(
        str(part.get("text") or "") for part in content if isinstance(part, dict)
      )
    return ""

  def _response_from_body(self, body: dict[str, Any]) -> ModelResponse:
    choices = body.get("choices") or []
    if not choices:
      raise ModelClientError("model response carried no choices")
    message = choices[0].get("message") or {}
    text = self._text_content(message.get("content"))
    thinking = message.get("reasoning_content")
    if not isinstance(thinking, str) or not thinking:
      thinking = None
    # Reasoning models (kimi-k2 etc.) may answer with empty content and the
    # visible text in reasoning_content.
    if not text:
      text = thinking or ""
    return ModelResponse(
      text=text,
      thinking=thinking,
      model=str(body.get("model", "")),
      usage=Usage.model_validate(body.get("usage") or {}),
      finish_reason=str(choices[0].get("finish_reason") or ""),
    )
