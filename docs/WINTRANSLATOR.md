# WinTranslator AI

Opt-in online game-text translation overlay for WinNative.

- Android MediaProjection with explicit system consent.
- On-device Google ML Kit Latin OCR; captured frames are not uploaded.
- Throttled OCR and duplicate-text suppression.
- Floating in-game pill with pause, hide and stop controls.
- DeepL, LibreTranslate, Google Cloud Translation and OpenAI-compatible endpoints.
- Configurable endpoint, API key, source/target language and model.

## Use
Open Settings > Tools > Game Translator. Configure an HTTPS provider and API key, save, grant display-over-other-apps permission, start the translator and approve Android's screen-capture prompt. Tap the floating «译» button in-game.

## Limitations
This first version translates visible text in a floating overlay; it does not inject replacement strings into the Windows process or modify game files. OCR uses the ML Kit Latin script model; other scripts need additional script models. Only recognized text is sent to the selected translation API. API keys are stored in private app preferences; use a dedicated restricted key.
