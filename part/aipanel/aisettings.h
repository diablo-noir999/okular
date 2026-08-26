#ifndef AISETTINGS_H
#define AISETTINGS_H

#include <QString>

struct AISettings {
    enum Provider {
        OpenCode = 0,   // OpenCode Zen API (BYOK)
        Ollama,
        OpenAI,
        Anthropic,
        Custom,
        OpenCodeCLI = 5 // local `opencode` CLI, anonymous (no key needed)
    };

    Provider provider = OpenCodeCLI;
    // OpenCode Zen is the OpenAI-compatible gateway (https://opencode.ai/zen/v1/chat/completions)
    QString apiUrl = QStringLiteral("https://opencode.ai/zen");
    QString apiKey;
    // Model name. For the opencode CLI provider this is in provider/model form,
    // e.g. "opencode/mimo-v2.5-free" (free, no API key).
    QString model = QStringLiteral("opencode/mimo-v2.5-free");
    // Path to the opencode CLI binary ("opencode" = resolved from PATH / ~/.opencode/bin)
    QString opencodeBinary = QStringLiteral("opencode");
    bool autoSummarize = true;
    bool enableWebSearch = true;
    QString exaApiKey;
    bool enableObsidianSync = true;
    QString obsidianVaultPath;
    int debounceDelayMs = 800;
    // Output budget for the model. Reasoning models (e.g. hy3-free) count their
    // chain-of-thought against this too, so it must be generous enough to leave
    // room for the actual answer after thinking.
    int maxTokens = 16384;
    // Total context window of the model in tokens. The request (page text + previous
    // context + web search + history) is capped so it fits within this minus the
    // response budget, so previous context can use as much of the window as possible.
    int contextWindowTokens = 32768;

    // Vision (OCR) fallback: when a page has no selectable text, render it to an
    // image and ask this OpenAI-compatible endpoint (e.g. llama.cpp running a small
    // VLM) to transcribe it, then feed the transcription into the normal explainer.
    bool enableVisionFallback = true;
    QString visionApiUrl; // e.g. http://localhost:8080 (llama.cpp server)
    QString visionModel;  // e.g. qwen2.5vl-3b or your GGUF file name
    QString visionApiKey; // optional

    static AISettings load();
    void save() const;
};

#endif // AISETTINGS_H
