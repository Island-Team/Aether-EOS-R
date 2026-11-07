"""Cognitive safety gate — sole egress authority over primitive calls."""

from egia.core.safety.audit import safety_audit_entry
from egia.core.safety.gate import SafetyGate

__all__ = ["SafetyGate", "safety_audit_entry"]
