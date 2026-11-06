"""Model-visible backend mode state."""

from __future__ import annotations

from typing import Literal

BackendMode = Literal["sandbox", "live"]


class ModeState:
  """Small mutable seam used by context rendering and mode controls."""

  def __init__(self, mode: BackendMode = "sandbox") -> None:
    self._mode: BackendMode = mode

  @property
  def mode(self) -> BackendMode:
    return self._mode

  def switch(self, mode: BackendMode) -> None:
    self._mode = mode

  def render(self) -> str:
    return (
      f"Mode: {self._mode.upper()}. Use only primitives listed for this "
      "deployment and verify action preconditions."
    )


__all__ = ["BackendMode", "ModeState"]
