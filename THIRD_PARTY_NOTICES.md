# Third-party notices

## DeepSeek Harness

This project reimplements behavior of [DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness). It includes these pieces derived from it:

- the app icon (`data/icons/*.png`, scaled from the Harness macOS icon) and the whale logo (`data/favicon.svg`, and the path data in `src/theme.c`)
- model-facing tool names, descriptions and JSON schemas (`src/tools.c`)
- system-prompt guidance text (`src/prompt.c`)
- visual design values (colors, spacing and radii) taken from the Harness web UI

"DeepSeek" and the DeepSeek logo are trademarks of their owner. They're used here only to identify compatibility, and this project is not affiliated with DeepSeek.

```
MIT License

Copyright (c) 2026 DeepSeek

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

## ripgrep

When installed, ripgrep (`rg`) is run as an external program for `glob`, `grep` and `@` file completion. It isn't bundled. ripgrep is dual-licensed under MIT and the Unlicense: https://github.com/BurntSushi/ripgrep
