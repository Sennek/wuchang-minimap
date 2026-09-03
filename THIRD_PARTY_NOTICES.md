# Third-party notices

WuchangMinimap itself is MIT-licensed (see `LICENSE`). The shipped `main.dll` also
contains, in compiled form, the libraries listed below, and the shipped `maps\` and
`markers\` assets are derived from the game's own data. This file is what satisfies
their attribution requirements; it ships at the root of every release zip.

| Component | Version | Licence | Redistributed in the zip |
|---|---|---|---|
| Dear ImGui | 1.92.9b | MIT | yes, compiled into `main.dll` |
| MinHook (with Hacker Disassembler Engine 32/64) | 1.3.3 | BSD 2-clause | yes, compiled into `main.dll` |
| {fmt} | 11.2.0 | MIT with the "compiled form" exception | headers only; see note |
| UE4SS (RE-UE4SS) | v3.0.1-1111-g97b7e501 | MIT | no - linked at runtime, installed separately by the player |
| Game map / marker data | n/a | see "Game data" | yes, as mod assets |

Version provenance: Dear ImGui from `IMGUI_VERSION` in `third_party/imgui/imgui.h`;
{fmt} from `FMT_VERSION` (110200) in `third_party/fmt/include/fmt/base.h`; MinHook's
sources carry no version macro - the vendored tree is Tsuda Kageyu's last release
(1.3.3, copyright range 2009-2017). The UE4SS build is the one the import library in
`sdk/lib/UE4SS.lib` was synthesised from (`sdk/UE4SS.def`).

The mod's JSON reader (`src/json.hpp`) is **not** third-party: it is a small
hand-written recursive-descent parser belonging to this project, not nlohmann/json or
any other library, and is covered by `LICENSE`.

---

## Dear ImGui 1.92.9b

Homepage: <https://github.com/ocornut/imgui>. Vendored at `third_party/imgui/`
(core, `misc/cpp/imgui_stdlib.cpp`, and the DX12 + Win32 backends); licence file
`third_party/imgui/LICENSE.txt`.

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## MinHook 1.3.3

Homepage: <https://github.com/TsudaKageyu/minhook>. Vendored at
`third_party/minhook/` (including the Hacker Disassembler Engine sources under
`src/hde/`); licence file `third_party/minhook/LICENSE.txt`.

```
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Portions of MinHook are Copyright (c) 2008-2009, Vyacheslav Patkov (Hacker
Disassembler Engine 32 C and Hacker Disassembler Engine 64 C), released under the
same two-clause terms reproduced above and in full in
`third_party/minhook/LICENSE.txt`.

---

## {fmt} 11.2.0

Homepage: <https://github.com/fmtlib/fmt>. Vendored headers only, at
`third_party/fmt/include/`; licence file `third_party/fmt/LICENSE`. It is compiled
header-only (`FMT_HEADER_ONLY=1`) because UE4SS's `DynamicOutput/Output.hpp` includes
it; 11.2.0 is the version UE4SS pins. The licence's exception means the embedded
object code needs no notice at all - it is reproduced here anyway.

```
Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

--- Optional exception to the license ---

As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into a machine-executable object form of such
source code, you may redistribute such embedded portions in such object form
without including the above copyright and permission notices.
```

---

## UE4SS (RE-UE4SS) v3.0.1-1111-g97b7e501

Homepage: <https://github.com/UE4SS-RE/RE-UE4SS>, MIT licence.

UE4SS is **not redistributed** by this mod. The release zip contains no UE4SS file:
the player installs UE4SS themselves, and `main.dll` merely imports from the
`UE4SS.dll` already present in the game folder. The build links against an import
library synthesised from that DLL's export table (`sdk/UE4SS.def`,
`tools/gen_ue4ss_importlib.ps1`), and compiles against RE-UE4SS headers checked out
at the same commit. The full MIT text is in the RE-UE4SS repository.

---

## Game data

`maps\maps.json`, `maps\chapter<N>\*.png` and `markers\*.json` are **derived from
Wuchang: Fallen Feathers' own navigation and level data** - the navmesh tiles and
placed-actor tables extracted offline from the shipped `.pak` files. The underlying
game content is owned by its developers and publisher (Leenzee / 505 Games); it is
not covered by this project's MIT licence and is distributed here only as a mod
asset for owners of the game, in the way mods are customarily distributed.

WuchangMinimap is an unofficial, fan-made modification. It is not affiliated with,
endorsed by or supported by Leenzee, 505 Games, Epic Games, or the RE-UE4SS project.
