# Models, Data Sources, and Third-Party Tools

## Models and AI providers

- **Google Gemini** — `gemini-2.5-flash` adapter for farmer-facing reasoning and crop-profile research when configured.
- **OpenAI** — `gpt-5-mini` adapter is supported as an alternative provider when configured.
- **Local ICM relay** — `local-codex` adapter sends a reduced, read-only farm context to the configured private relay when enabled.
- **Deterministic FarmWiz logic** — local rules identify stale/offline telemetry, high temperature, low soil moisture, and pH-range findings without calling a model.

The hosted FarmWiz demo has private runtime configuration. Public source code contains adapters and placeholders only; it contains no API keys or access tokens.

## FarmWiz data sources

- **ThingsBoard / FarmWiz telemetry** — device discovery, telemetry keys, latest readings, device status, and bounded historical values through the read-only connector.
- **Farmer input** — questions, selected farm location, crop selection, and optional plant images.
- **Crop profiles** — versioned JSON profiles included under `cloud-ai/data/crops/`, with AI-researched profiles marked for review.
- **Open-Meteo** — current weather and three-day forecast context when valid coordinates are available.
- **Geocoding service** — location search used to turn a farmer-entered place into coordinates for weather context.

## Third-party software and services

- Python, FastAPI, Uvicorn, Pydantic, HTTPX, and python-dotenv
- ThingsBoard REST API
- Google Gemini API, OpenAI API, and optional private local ICM relay
- Open-Meteo weather API
- Browser speech recognition and speech synthesis where supported by the browser

## Boundaries

The application uses read-only farm evidence for reasoning. The submitted action workflow creates validated, expiring, approval-gated mock actions and does not claim live autonomous physical control.
