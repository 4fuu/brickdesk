# Third-party notice

The prototype-only ordered-dither threshold in `cui_dither.c` is derived from
CUI's 8x8 Bayer implementation.

- Project: CUI, https://github.com/yappologistic/CUI
- Revision: `86d9781bbc3c35c264ec46287ad89c1d86a4c4de`
- License: MIT

The Bayer bit formula and the visual rule (one solid cell per threshold step,
clipped to a rounded shape) are reimplemented in C for brickdesk's GLES2
renderer. No CUI component, theme, ES3 shader or website code is copied into
the product.

```text
MIT License

Copyright (c) 2026 The CUI authors

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
