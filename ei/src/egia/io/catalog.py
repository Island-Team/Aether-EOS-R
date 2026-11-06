"""Project live primitive and native-skill catalogs northbound and to prompts."""

from __future__ import annotations

from collections.abc import Mapping

from egia.api.primitives import (
  PrimitiveDependency,
  PrimitiveDescriptor,
  PrimitivesSnapshot,
)
from egia.api.prompts import PromptSection, PromptsSnapshot
from egia.api.skills import (
  SkillDescriptor,
  SkillPrimitiveReference,
  SkillResource,
  SkillsSnapshot,
)
from egia.core.link import EosClient
from egia.core.registry import PrimitiveRegistry

__all__ = [
  "primitives_snapshot",
  "prompts_snapshot",
  "render_skills",
  "skills_snapshot",
]


def prompts_snapshot(instruction: str, sections) -> PromptsSnapshot:
  """Project one lane's static, Agent-owned prompt sections."""
  return PromptsSnapshot(
    system=instruction,
    sections=[
      PromptSection(
        name=section.name,
        text=section.text,
        lines=[line for line in section.text.splitlines() if line.strip()],
      )
      for section in sections
    ],
  )


def primitives_snapshot(registry: PrimitiveRegistry) -> PrimitivesSnapshot:
  primitives = [
    PrimitiveDescriptor(
      name=item.name,
      description=item.description,
      device_description=item.device_description,
      version=item.version,
      robot_id=item.backend,
      device_id=item.device_id,
      eos_robot_id=item.eos_robot_id,
      operator=item.operator,
      scope=item.scope,
      binding_kind=item.binding_kind,
      health=item.health,
      references=list(item.references),
      absolute_frame=item.absolute_frame,
      dependencies=[
        PrimitiveDependency(name=name, health=health)
        for name, health in item.dependencies
      ],
      accepted_media_types=list(item.accepts),
      limits=item.limits,
      arguments_schema=item.json_schema,
    )
    for item in registry.primitives()
  ]
  return PrimitivesSnapshot(primitives=primitives, count=len(primitives))


def skills_snapshot(clients: Mapping[str, EosClient]) -> SkillsSnapshot:
  unavailable = sorted(
    alias for alias, client in clients.items() if not client.skills_available
  )
  skills = [
    SkillDescriptor(
      backend=item.backend,
      eos_robot_id=item.eos_robot_id,
      name=item.name,
      version=item.version,
      description=item.description,
      primitives=list(item.primitives),
      primitive_refs=[
        SkillPrimitiveReference(name=name, version=version)
        for name, version in item.primitive_refs
      ],
      parameters_schema=item.parameters or {},
      returns_schema=item.returns or {},
      resources=[
        SkillResource(uri_template=uri, description=description)
        for uri, description in item.resources
      ],
    )
    for alias in sorted(clients)
    for item in clients[alias].skills
  ]
  return SkillsSnapshot(
    available=bool(clients) and not unavailable,
    skills=skills,
    unavailable_backends=unavailable,
    count=len(skills),
  )


def render_skills(
  clients: Mapping[str, EosClient], registry: PrimitiveRegistry | None = None
) -> str:
  """Render only skills composed entirely from planner-visible primitives."""
  snapshot = skills_snapshot(clients)
  visible = (
    {(primitive.backend, primitive.name) for primitive in registry.planner_primitives()}
    if registry is not None
    else None
  )
  usable = [
    item
    for item in snapshot.skills
    if visible is None
    or all((item.backend, name) in visible for name in item.primitives)
  ]
  lines = [
    f"{item.backend}:{item.name}@{item.version} — {item.description}; "
    f"primitives=[{', '.join(item.primitives) or 'none'}]"
    for item in usable
  ]
  hidden_count = len(snapshot.skills) - len(usable)
  if hidden_count:
    lines.append(f"unavailable compositions: {hidden_count}")
  if snapshot.unavailable_backends:
    lines.append("unavailable: " + ", ".join(snapshot.unavailable_backends))
  if lines:
    return "\n".join(lines)
  return "none discovered" if snapshot.available else "unavailable"
