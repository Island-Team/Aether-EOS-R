"""Product-level lifecycle and path utilities for Island-egia.

Keep this namespace small: component behavior belongs to :mod:`egia` or
the independently built EOS runtime, never here.
"""

from island.path import Layout, Mode

__all__ = ["Layout", "Mode"]
