"""EI public import path for the embodied intelligence runtime.

The 0.2.1 transition retains the published ``egia`` modules as compatibility
imports. Submodules resolve from the same implementation tree so existing
deployments and extensions continue to load.
"""

from egia import __path__ as _legacy_path

__path__ = _legacy_path
