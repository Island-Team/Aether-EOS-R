"""EGIA subsystem supervisor — the process manager for pipeline modules.

Each pipeline module (``queue``, ``context``, ``decision``, ``memory``,
``registry``, ``safety``, ``monitor``) registers as a :class:`SubsystemSpec`.
The supervisor starts them as supervised asyncio tasks, gates pipeline
readiness on *all* subsystems signalling ready (a subsystem's loop must not
process work before its peers are up), then watches liveness: a crashed
subsystem is restarted with backoff and reported ``degraded`` until it
recovers. Status is projected to ``egia.io`` for the WebUI health route.
"""

from egia.core.supervisor.manager import Supervisor
from egia.core.supervisor.spec import SubsystemSpec

__all__ = ["SubsystemSpec", "Supervisor"]
