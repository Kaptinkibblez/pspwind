"""Test-side alias for the synthetic NIF builders (see synth.py)."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from synth import *  # noqa: E402,F401,F403
from synth import I9, rot_z  # noqa: E402,F401
