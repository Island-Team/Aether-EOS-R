"""Semantic archive gates for the Q01 selected-pin negative matrix.

A case manifest authenticates archive consistency only. A case becomes eligible
for the full gate after this module checks its raw owner observations. Unwired
cases fail closed, even when their metadata says that they passed.
"""

from pathlib import Path


REQUIRED = {'camera', 'g06-r03', 'p01-p03-u01', 'p07-u03', 'r06-a03',
            'migration'}


def verify_case(name: str, case: Path, selected_pins: dict, *, selection=None) -> None:
    assert name in REQUIRED, f'unexpected Q01 matrix case: {name}'
    if name == 'camera':
        from verify_q01_camera import verify as verify_camera

        verify_camera(case, selected_pins, selection=selection) if selection else \
            verify_camera(case, selected_pins)
        return
    if name == 'migration':
        from verify_q01_migration import verify as verify_migration

        verify_migration(case, selected_pins, selection=selection)
        return
    if name == 'g06-r03':
        from verify_q01_g06_r03 import verify as verify_g06_r03

        verify_g06_r03(case, selected_pins, selection=selection) if selection else \
            verify_g06_r03(case, selected_pins)
        return
    if name == 'p07-u03':
        from verify_q01_p07_u03 import verify as verify_p07_u03

        verify_p07_u03(case, selected_pins, selection=selection) if selection else \
            verify_p07_u03(case, selected_pins)
        return
    if name == 'p01-p03-u01':
        from verify_q01_p01_p03_u01 import verify as verify_p01_p03_u01

        verify_p01_p03_u01(case, selected_pins, selection=selection) if selection else \
            verify_p01_p03_u01(case, selected_pins)
        return
    if name == 'r06-a03':
        from verify_q01_r06_a03 import verify as verify_r06_a03

        verify_r06_a03(case, selected_pins, selection=selection) if selection else \
            verify_r06_a03(case, selected_pins)
        return
    raise AssertionError(
        f'{name}: selected-pin raw-response verifier is not implemented; '
        'a passed flag or checksum manifest cannot qualify this case'
    )
