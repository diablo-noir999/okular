#include "aisettings.h"
#include <KConfigGroup>
#include <KSharedConfig>

AISettings AISettings::load()
{
    AISettings settings;
    KSharedConfigPtr config = KSharedConfig::openConfig();
    KConfigGroup group = config->group(QStringLiteral("AIPanel"));

    const int providerValue = group.readEntry(QStringLiteral("Provider"), static_cast<int>(OpenCodeCLI));
    if (providerValue >= static_cast<int>(OpenCode) && providerValue <= static_cast<int>(OpenCodeCLI)) {
        settings.provider = static_cast<Provider>(providerValue);
    }
    settings.apiUrl = group.readEntry(QStringLiteral("ApiUrl"), QStringLiteral("https://opencode.ai/zen"));
    settings.apiKey = group.readEntry(QStringLiteral("ApiKey"), QString());
    settings.opencodeBinary = group.readEntry(QStringLiteral("OpencodeBinary"), QStringLiteral("opencode"));
    settings.model = group.readEntry(QStringLiteral("Model"), QStringLiteral("opencode/mimo-v2.5-free"));
    settings.autoSummarize = group.readEntry(QStringLiteral("AutoSummarize"), true);
    settings.enableWebSearch = group.readEntry(QStringLiteral("EnableWebSearch"), true);
    settings.exaApiKey = group.readEntry(QStringLiteral("ExaApiKey"), QString());
    settings.enableObsidianSync = group.readEntry(QStringLiteral("EnableObsidianSync"), true);
    settings.obsidianVaultPath = group.readEntry(QStringLiteral("ObsidianVaultPath"), QString());
    settings.debounceDelayMs = qBound(300, group.readEntry(QStringLiteral("DebounceDelayMs"), 800), 3000);
    settings.maxTokens = qBound(100, group.readEntry(QStringLiteral("MaxTokens"), 16384), 65536);
    settings.contextWindowTokens = qBound(4096, group.readEntry(QStringLiteral("ContextWindowTokens"), 32768), 262144);
    settings.enableVisionFallback = group.readEntry(QStringLiteral("EnableVisionFallback"), true);
    settings.visionApiUrl = group.readEntry(QStringLiteral("VisionApiUrl"), QString());
    settings.visionModel = group.readEntry(QStringLiteral("VisionModel"), QString());
    settings.visionApiKey = group.readEntry(QStringLiteral("VisionApiKey"), QString());

    // Migrate stale OpenCode defaults from the original implementation (dead endpoint + retired model)
    if (settings.provider == OpenCode) {
        if (settings.apiUrl.isEmpty() || settings.apiUrl == QLatin1String("https://api.opencode.ai")) {
            settings.apiUrl = QStringLiteral("https://opencode.ai/zen");
        }
        if (settings.model.isEmpty() || settings.model == QLatin1String("gpt-4o")) {
            settings.model = QStringLiteral("mimo-v2.5-free");
        }
    }

    // The opencode CLI provider needs a provider/model qualified model name
    if (settings.provider == OpenCodeCLI && settings.model.isEmpty()) {
        settings.model = QStringLiteral("opencode/mimo-v2.5-free");
    }

    return settings;
}

void AISettings::save() const
{
    KSharedConfigPtr config = KSharedConfig::openConfig();
    KConfigGroup group = config->group(QStringLiteral("AIPanel"));

    group.writeEntry(QStringLiteral("Provider"), static_cast<int>(provider));
    group.writeEntry(QStringLiteral("ApiUrl"), apiUrl);
    group.writeEntry(QStringLiteral("ApiKey"), apiKey);
    group.writeEntry(QStringLiteral("OpencodeBinary"), opencodeBinary);
    group.writeEntry(QStringLiteral("Model"), model);
    group.writeEntry(QStringLiteral("AutoSummarize"), autoSummarize);
    group.writeEntry(QStringLiteral("EnableWebSearch"), enableWebSearch);
    group.writeEntry(QStringLiteral("ExaApiKey"), exaApiKey);
    group.writeEntry(QStringLiteral("EnableObsidianSync"), enableObsidianSync);
    group.writeEntry(QStringLiteral("ObsidianVaultPath"), obsidianVaultPath);
    group.writeEntry(QStringLiteral("DebounceDelayMs"), debounceDelayMs);
    group.writeEntry(QStringLiteral("MaxTokens"), maxTokens);
    group.writeEntry(QStringLiteral("ContextWindowTokens"), contextWindowTokens);
    group.writeEntry(QStringLiteral("EnableVisionFallback"), enableVisionFallback);
    group.writeEntry(QStringLiteral("VisionApiUrl"), visionApiUrl);
    group.writeEntry(QStringLiteral("VisionModel"), visionModel);
    group.writeEntry(QStringLiteral("VisionApiKey"), visionApiKey);

    group.sync();
}
