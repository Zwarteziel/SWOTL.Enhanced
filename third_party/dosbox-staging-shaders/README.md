# Shaders from DOSBox Staging 0.83.0

These GLSL files come unchanged from DOSBox Staging 0.83.0
(https://github.com/dosbox-staging/dosbox-staging, tag `v0.83.0`):

- `crt-hyllian.glsl` (resources/shaders/crt), (c) 2020-2026 The DOSBox Staging
  Team, (c) 2020-2021 Hyllian, (c) 2020 Tyrells. The phosphor mask code in it
  is by hunterk (public domain).
- `vga-1080p.glsl`, `vga-1080p-fake-double-scan.glsl` (resources/shaders/crt),
  (c) 2020-2026 The DOSBox Staging Team.
- `image-adjustments.glsl` (resources/shaders/_internal), (c) 2025-2026 The
  DOSBox Staging Team, with code by guest(r), Dr. Venom, Dogway, prod80, Neil
  Bartlett and Tanner Helland.

`vga-1440p.preset` and `vga-4k.preset` hold the parameter values that DOSBox
Staging uses for these presets.

All four shader files carry `SPDX-License-Identifier: MIT`. The rest of
DOSBox Staging is GPL-2.0-or-later; SWOTL Enhanced uses none of that code.
`tools/embed_shaders.py` turns these files into `src/shaders_embedded.inc`.

MIT License

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
