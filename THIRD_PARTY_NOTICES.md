# Third-party notices

## XenonFPS

`overlay/overlay.cpp` is adapted from XenonFPS (https://github.com/pookatv/XenonFPS): the
`VdSwap` patch and trampoline, the GPU ring injection (`INDIRECT_BUFFER` to a list of one-dword
`MEM_WRITE` packets), the front-buffer gate and the tiled-address function.

```
MIT License

Copyright (c) 2026 Pooka

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

## References (no code copied)

- Xenia (https://github.com/xenia-project/xenia): the 32-bit tiled-surface address formula that
  XenonFPS's `TiledOff` follows, and the `EX_TITLE_TERMINATE_REGISTRATION` usage.
- LaunchCode (https://github.com/landaire/LaunchCode) and the Free60 wiki: the SMC `0x07`
  temperature reply layout.
- Peer Pressure (https://github.com/grimdoomer/Xbox360PeerPressure): the sideloading this
  project runs under. Its loader behavior described in the README was worked out by testing.
