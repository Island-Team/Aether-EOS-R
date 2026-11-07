"""Southbound link to one or more EOS GatewayService processes.

This is the only module that speaks to a backend. ``egia.io`` is northbound
(WebUI) exclusively; every admitted primitive call and every pulled resource leaves
EGIA through here, per invariant 1 in ``docs/egia/egia-2607.md``.
"""

from egia.core.link.client import (
  CameraFrame,
  DeviceBinding,
  EosClient,
  EosDiscovery,
  LinkPrimitiveError,
  PrimitiveBinding,
  ResourceBinding,
  SkillBinding,
  SkillExecution,
)
from egia.core.link.registry import DEFAULT_BACKEND_ID, ClientRegistry
from egia.core.link.status import (
  BackendStatusTracker,
  backend_section_renderer,
  backend_section_renderers,
  render_backend_status,
)

__all__ = [
  "DEFAULT_BACKEND_ID",
  "BackendStatusTracker",
  "CameraFrame",
  "ClientRegistry",
  "DeviceBinding",
  "EosClient",
  "EosDiscovery",
  "LinkPrimitiveError",
  "PrimitiveBinding",
  "ResourceBinding",
  "SkillBinding",
  "SkillExecution",
  "backend_section_renderer",
  "backend_section_renderers",
  "render_backend_status",
]
