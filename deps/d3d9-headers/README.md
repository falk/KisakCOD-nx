# Vendored D3D9/Windows headers

`src/deko9` implements the engine's D3D9 interfaces directly on deko3d (see
`src/deko9/deko9_native.h`); it needs the D3D9
type/interface declarations to compile against, but none of DXVK's own
Vulkan/SPIR-V code. Previously the build pointed `KISAK_DXVK_ROOT` at a full
pinned DXVK checkout (`include/native/` alone is ~4.2 MB, mostly D3D10-12
headers this port never uses) just for these headers. This directory vendors
only the closure `<d3d9.h>` (the one header the engine and deko9 actually
`#include`) needs:

    directx/d3d9.h
    directx/d3d9types.h
    directx/d3d9caps.h
    windows/windows.h
    windows/windows_base.h
    windows/unknwn.h
    windows/objbase.h
    windows/pshpack4.h
    windows/poppack.h

(`<Windows.h>`, spelled with a capital W by some engine sources, resolves
through the existing case shim at `src/platform/switch/include/Windows.h`,
which itself `#include`s the lowercase `windows.h` here -- unchanged by this
vendoring.)

## Source and license

Copied unmodified from DXVK v3.1.1's `include/native/` tree
(`https://github.com/doitsujin/dxvk`, tag `v3.1.1`), which in turn took them
from MinGW-w64 (`https://www.mingw-w64.org/`), whose Direct3D/DDK headers
originate from the Wine project. See the original
`include/native/directx/README.md` in DXVK, reproduced here:

> These headers are taken directly from MinGW-w64.
>
> The license for these headers is LGPL v2.1. However that does not mean
> your project is bound by LGPL v2.1 for using these headers:
>
> DirectX and DDK headers are under GNU LGPLv2.1+ (see the file
> COPYING.LGPLv2.1) and copyrighted by various people. Using these headers
> doesn't make LGPLv2.1 apply to your code, because these header files
> contain only data structure definitions, short macros, and short inline
> functions. Here is the relevant part from LGPLv2.1 section 5 paragraph 4:
>
>     If such an object file uses only numerical parameters, data structure
>     layouts and accessors, and small macros and small inline functions
>     (ten lines or less in length), then the use of the object file is
>     unrestricted, regardless of whether it is legally a derivative work.

`directx/d3d9.h` itself carries a Wine copyright header (Jason Edmeades,
Raphael Junqueira, 2002-2003) under the same LGPL v2.1+ terms. `windows/
pshpack4.h` and `windows/poppack.h` are public domain (mingw-w64 runtime
package, no copyright assigned). None of these files are modified from
upstream other than being copied out of the larger DXVK tree; do not edit
them in place -- re-vendor from DXVK (or MinGW-w64 directly) instead.

## Updating

If a future source file needs a D3D9/Windows declaration not in this set,
copy the specific missing header from a DXVK `include/native/` checkout (or
MinGW-w64 upstream) into the matching `directx/` or `windows/` subdirectory
here and note the addition above; do not point the build back at a full DXVK
checkout.
