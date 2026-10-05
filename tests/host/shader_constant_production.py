#!/usr/bin/env python3
"""Select the production material-constant functions for the narrow host link."""
from pathlib import Path
import sys

source = Path("src/gfx_d3d/r_material_load_obj.cpp").read_text()
start = source.index("void __cdecl R_RegisterShaderConst(")
end = source.index("int __cdecl Material_ComparePixelConsts(", start)
Path(sys.argv[1]).write_text(source[start:end])
