"""Perception — backend state pulled over the southbound link into admission.

``body/`` holds the self robot parts — side processes
forwarding to the WebUI streams, with two exceptions that keep the decision
chain fed: each vision frame admits one coalesced detection record, and the
bundled scene exchange feeds io's presentation store. Sources are
mode-neutral: sim and live differ only by gateway endpoint.

The stores are the read models ``egia.io`` projects northbound; perception
itself never touches the WebSocket layer.
"""

from egia.core.perception.admission import Admission
from egia.core.perception.distribution import DistributionGate

__all__ = [
  "Admission",
  "DistributionGate",
]
