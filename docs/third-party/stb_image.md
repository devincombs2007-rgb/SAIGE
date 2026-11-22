# stb_image

SAIGE decodes PNG and JPEG textures with [stb_image](https://github.com/nothings/stb) by Sean Barrett, compiled for those
two formats only (`STBI_ONLY_PNG`, `STBI_ONLY_JPEG`, no stdio, no HDR, dimensions capped at 16384).

| | |
| --- | --- |
| Version | stb_image v2.34 |
| Upstream revision | `nothings/stb` commit `6b5799bae8a1fe3792691ef4b72ae26434c3e799` |
| File | `stb_image.h`, SHA-256 `dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020` |
| License | Dual: MIT or Public Domain (Unlicense), at the user's choice. SAIGE uses it under the **MIT** license below. |
| Where | `engine/gfx_assets` (`src/image.cpp`), fetched by CMake at the pinned URL and verified against the hash; it is not vendored |

The CMake option `SAIGE_GFX_IMAGE_DECODE=OFF` builds without it. Offline builds set
`FETCHCONTENT_SOURCE_DIR_SAIGE_STB_IMAGE` to a directory containing the verified file. To update, change the commit and hash
in `engine/gfx_assets/CMakeLists.txt` and this page together, and rerun the corrupt-image tests under the sanitizers.

stb_image's own security note recommends Wuffs when security is a priority. The decoder is therefore bounded on every side
(encoded size, header dimensions, decoded byte count, all checked before any pixel allocation) and fuzz-style tested;
decoding happens on the asset-import path, never on data received over a network.

## License notice (MIT)

```
Copyright (c) 2017 Sean Barrett

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
```
