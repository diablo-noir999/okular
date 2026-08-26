# Okular – Universal Document Viewer

Okular can view and annotate documents of various formats, including PDF, Postscript, Comic Book, and various image formats.
It supports native PDF annotations.

## AI Page Explainer

This fork ships an AI-powered per-page explainer panel (the **AI Page Explainer**) that tutors you through documents page by page.

### Features

- **Per-page explanations** – Generates a plain-English summary and explanation for the current page (bullet summary, key terms with beginner-friendly building blocks, connections to earlier pages, and a question to move you forward). Works with selectable text *and* scanned pages.
- **Q&A follow-ups** – Ask questions about the page; answers are rendered as formatted markdown (headings, lists, code blocks) and stream in live.
- **Web search augmentation** – Toggleable context enrichment via Exa search, so the model can ground its explanation in up-to-date web results.
- **Learning-mode tutoring** – Pruned, per-page tutoring system prompts: diagnose before teaching, one step per turn, no handing over answers to graded coursework.
- **OCR fallback chain** – Pages without selectable text are rendered and transcribed automatically: **Tesseract** first (fast, local), then a **vision model** via llama.cpp/OpenAI-compatible endpoint when Tesseract comes up empty.
- **Previous-page context for scanned docs** – When explaining an OCR'd page, the previous pages are OCR'd too (best-effort, cached) so the model actually has surrounding context.
- **Persistence (Obsidian sync)** – Explanations and Q&A history are saved to markdown files in your Obsidian vault (one note file per document) and restored automatically when you reopen the document — no regeneration needed.
- **Providers** – Bring-your-own-key OpenAI-compatible APIs (e.g. OpenCode Zen free models), or the local **opencode CLI** (anonymous, no key needed, one session per document).
- **Robust streaming** – Live elapsed timer, "model is thinking (N chars)" progress for reasoning models, `[DONE]`-aware SSE handling, auto-retry on rate limits, and a hard generation timeout so it never hangs silently.
- **Fully automatic vision setup** – A single checkbox enables the OCR chain; the vision endpoint and model are auto-detected from a running llama.cpp server (`localhost:8080`) with optional overrides for advanced users.

### Getting started

1. Open the AI panel (it docks alongside the document).
2. In the settings (gear icon):
   - Pick a **provider** – `opencode CLI` (default; needs the `opencode` binary, anonymous) or an OpenAI-compatible API URL + key + model (e.g. OpenCode Zen: `https://opencode.ai/zen`, model `opencode/mimo-v2.5-free`).
   - Optionally enable **web search** and add an Exa API key.
   - Optionally set an **Obsidian vault** folder for persistence (defaults to `~/Documents/Obsidian Vault/Okular Notes`).
3. Navigate to a page; the panel explains it (auto-summarize can be toggled, or press ↻).

For scanned/textless PDFs, install Tesseract for local OCR:

```bash
sudo pacman -S tesseract tesseract-data-eng   # Arch; adjust for your distro
```

If Tesseract also fails, run a vision-capable model server (e.g. `llama-server` with a VLM like `granite3.2-vision` on port 8080) and the panel will auto-detect and use it.

### Downloads

For download and installation instructions, see https://okular.kde.org/download.php

### User manual

https://docs.kde.org/?application=okular&branch=stable5

### Bugs

https://bugs.kde.org/buglist.cgi?product=okular

Please report bugs on Bugzilla (https://bugs.kde.org/enter_bug.cgi?product=okular), and not on our GitLab instance (https://invent.kde.org).

### Mailing list

https://mail.kde.org/mailman/listinfo/okular-devel

### Source code

https://invent.kde.org/graphics/okular.git

The Okular repository contains the source code for:
 * the `okular` desktop application (the “shell”),
 * the `okularpart` KParts plugin,
 * the `okularkirigami` mobile application,
 * several `okularGenerator_xyz` plugins, which provide backends for different document types.

### Apidox

https://api.kde.org/okular/html/index.html

## Contributing

Okular uses the merge request workflow.
Merge requests are required to run pre-commit CI jobs; please don’t push to the master branch directly.
See https://community.kde.org/Infrastructure/GitLab for an introduction.

### Build instructions

Okular can be built like many other applications developed by KDE.
See https://community.kde.org/Get_Involved/development for an introduction.

If your build environment is set up correctly, you can also build Okular using CMake:

```bash
git clone https://invent.kde.org/graphics/okular.git
cd okular
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/path/to/your/install/dir ..
make
make install
```

Okular also builds tests in the build tree. To run them, you have to run `make install` first.

If you install Okular in a different path than your system install directory it is possible that you need to run

```bash
source prefix.sh
```

so that the correct Okular instance and libraries are picked up.
Afterwards one can run `okular` inside the shell instance.
The source command is also required to run the tests manually.

As stated above, Okular has various build targets.
Two of them are executables.
You can choose which executable to build by passing a flag to CMake:

```bash
cmake -DCMAKE_INSTALL_PREFIX=/path/to/your/install/dir -DOKULAR_UI=desktop ..
```
Available options are `desktop`, `mobile`, and `both`.

### clang-format

The Okular project uses clang-format to enforce source code formatting.
See [README.clang_format](https://invent.kde.org/graphics/okular/-/blob/master/README.clang-format) for more information.
