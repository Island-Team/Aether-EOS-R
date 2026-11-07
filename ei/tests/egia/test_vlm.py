"""OpenAI-compatible model client request and response handling."""

from __future__ import annotations

import pytest

from egia.lib.vlm import ChatMessage, ModelResponse, VLMClient


@pytest.mark.asyncio
async def test_qwen_reasoning_none_disables_thinking_in_request() -> None:
  client = VLMClient(
    "http://example.test/v1",
    "qwen3.8-max",
    reasoning_effort="none",
    reasoning_options=("none", "high"),
  )
  payloads: list[dict] = []
  client._post = lambda payload: (  # type: ignore[method-assign]
    payloads.append(payload) or ModelResponse(text="ok")
  )

  await client.complete([ChatMessage(role="user", content="hello")])

  assert payloads[0]["enable_thinking"] is False


@pytest.mark.asyncio
async def test_qwen_uses_model_selected_reasoning_on_later_request() -> None:
  client = VLMClient(
    "http://example.test/v1",
    "qwen3.8-max",
    reasoning_effort="none",
    reasoning_options=("none", "high"),
  )
  payloads: list[dict] = []
  client._post = lambda payload: (  # type: ignore[method-assign]
    payloads.append(payload) or ModelResponse(text="ok")
  )

  await client.complete(
    [ChatMessage(role="user", content="hello")], reasoning_effort="high"
  )

  assert payloads[0]["enable_thinking"] is True


@pytest.mark.asyncio
async def test_llamacpp_gemma_disables_thinking_through_chat_template() -> None:
  client = VLMClient(
    "http://example.test/v1",
    "Gemma4-26B",
    reasoning_effort="none",
    reasoning_options=("none",),
  )
  payloads: list[dict] = []
  client._post = lambda payload: (  # type: ignore[method-assign]
    payloads.append(payload) or ModelResponse(text="ok")
  )

  await client.complete([ChatMessage(role="user", content="hello")])

  assert payloads[0]["chat_template_kwargs"] == {"enable_thinking": False}
  assert "reasoning_effort" not in payloads[0]


@pytest.mark.asyncio
async def test_llamacpp_gemma_enables_thinking_through_chat_template() -> None:
  client = VLMClient(
    "http://example.test/v1",
    "Gemma4-26B",
    reasoning_effort="high",
    reasoning_options=("high",),
  )
  payloads: list[dict] = []
  client._post = lambda payload: (  # type: ignore[method-assign]
    payloads.append(payload) or ModelResponse(text="ok")
  )

  await client.complete([ChatMessage(role="user", content="hello")])

  assert payloads[0]["chat_template_kwargs"] == {"enable_thinking": True}


@pytest.mark.asyncio
async def test_binding_can_omit_token_and_temperature_parameters() -> None:
  client = VLMClient(
    "http://example.test/v1",
    "glm-5.3",
    send_max_tokens=False,
    send_temperature=False,
  )
  payloads: list[dict] = []
  client._post = lambda payload: (  # type: ignore[method-assign]
    payloads.append(payload) or ModelResponse(text="ok")
  )

  await client.complete(
    [ChatMessage(role="user", content="hello")], max_tokens=64, temperature=0.2
  )

  assert "max_tokens" not in payloads[0]
  assert "temperature" not in payloads[0]


def test_openai_sse_stream_emits_reasoning_and_answer_deltas() -> None:
  class StreamResponse:
    def __init__(self) -> None:
      self.headers = {"Content-Type": "text/event-stream; charset=utf-8"}

    def __enter__(self):
      return self

    def __exit__(self, *_args):
      return None

    def __iter__(self):
      return iter(
        [
          b'data: {"model":"test","choices":[{"delta":{"reasoning_content":"check "},"finish_reason":null}]}\n',
          b'data: {"model":"test","choices":[{"delta":{"content":"hel"},"finish_reason":null}]}\n',
          b'data: {"model":"test","choices":[{"delta":{"content":"lo"},"finish_reason":"stop"}]}\n',
          b'data: {"choices":[],"usage":{"prompt_tokens":4,"completion_tokens":3,"total_tokens":7}}\n',
          b"data: [DONE]\n",
        ]
      )

  client = VLMClient("http://example.test/v1", "test")
  client._request = lambda _payload: StreamResponse()  # type: ignore[method-assign]
  deltas = []
  response = client._post_stream({}, deltas.append)

  assert [(delta.kind, delta.text) for delta in deltas] == [
    ("reasoning", "check "),
    ("content", "hel"),
    ("content", "lo"),
  ]
  assert response.thinking == "check "
  assert response.text == "hello"
  assert response.finish_reason == "stop"
  assert response.usage.total_tokens == 7


@pytest.mark.asyncio
async def test_complete_enables_streaming_only_when_callback_is_supplied() -> None:
  client = VLMClient("http://example.test/v1", "test")
  payloads: list[dict] = []

  def fake_stream(payload, _on_delta):
    payloads.append(payload)
    return ModelResponse(text="ok")

  client._post_stream = fake_stream  # type: ignore[method-assign]
  await client.complete(
    [ChatMessage(role="user", content="hello")], on_delta=lambda _delta: None
  )
  assert payloads[0]["stream"] is True
  assert payloads[0]["stream_options"] == {"include_usage": True}
