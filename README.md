# FarmWiz

**Monitor. Automate. Grow Smarter.**

FarmWiz is a smart farming platform being developed in Suriname that combines IoT sensing, real-time monitoring, automation and AI-assisted farm intelligence.

## Future Caribbean Buildathon 2026

FarmWiz entered Future Caribbean with an existing IoT and automation foundation. During the Buildathon, the team focused on adding an isolated Agentic AI/intelligence layer on top of that foundation. This repository makes that distinction explicit: Phase 1 is documented for later source upload, while Phase 2 contains the completed Buildathon AI application.

## FarmWiz Development Architecture

### Phase 1 — IoT Foundation

FarmWiz's pre-existing foundation covers farm sensing, ThingsBoard monitoring, and local Switchbox scheduling/control.

[View Phase 1 IoT Foundation](./phase-1-iot/)

### Phase 2 — Agentic AI | Future Caribbean 2026

The Buildathon addition is an isolated FastAPI dashboard and AI service that moves from **MONITOR + AUTOMATE** toward:

**SENSE → DETECT → INVESTIGATE → REASON → RECOMMEND → ACT**

[View Phase 2 Agentic AI](./phase-2-agentic-ai/)

## The Problem

Many farming decisions still depend on manual observation. Farmers may manually check water conditions, crops, irrigation, environmental conditions, and equipment, so important changes can be missed. Even with sensor data, turning readings into useful decisions can require significant interpretation.

## The FarmWiz Solution

FarmWiz combines **IoT Sensors + Real-Time Monitoring + Automation + AI Farm Intelligence**. The intelligence layer helps turn farm data into understandable findings, context-aware advice, and reviewable action proposals while keeping farmer control and physical-control boundaries explicit.

## Agentic Workflow

```text
SENSE → DETECT → INVESTIGATE → REASON → RECOMMEND → ACT
```

The application retrieves read-only telemetry and bounded history, applies deterministic findings, adds crop and weather context where available, and can ask a configured AI provider for a grounded answer. Action intent is converted into a validated draft or mock action; this public copy does not claim unrestricted autonomous hardware control.

## Verified Features

- FastAPI backend and farmer-facing static dashboard
- ThingsBoard device discovery, latest telemetry, and bounded history retrieval
- Deterministic findings for stale/offline data and selected environmental thresholds
- Farm health/status views and alarm-context handling
- Geocoded location context and Open-Meteo weather/current forecast data
- Local crop profiles and AI-assisted crop-profile research
- AI adviser with Gemini, OpenAI, or local relay adapters
- Image-question workflow and browser speech/text interaction
- Structured voice/action intent parsing with approval state, expiry, allowlists, and mock-edge execution

Physical equipment execution remains disabled in this submission and is not exercised by the tests.

## Technology Stack

Python, FastAPI, Uvicorn, Pydantic, HTTPX, ThingsBoard REST APIs, HTML, CSS, JavaScript, Open-Meteo, and configurable Gemini/OpenAI/local-relay model providers.

## Data Sources

ThingsBoard supplies device and telemetry context; bounded history supplies trends; farmer-provided crop/location input supplies crop and geographic context; Open-Meteo supplies weather; local crop JSON files supply known agronomic profile data.

## AI and External Services

The adapter supports Google Gemini (`gemini-2.5-flash`), OpenAI (`gpt-5-mini`), and a local ICM relay. The selected provider is configured through environment variables. ThingsBoard and Open-Meteo are external data services. No credentials are included here.

## Setup

See [docs/SETUP.md](./docs/SETUP.md). The application has its own dependency file and runs from `phase-2-agentic-ai/`. Copy the root `.env.example` to a private `.env` only for local use.

## Team

**Julie Sundar** — Founder / Team Lead  
**Theo Boomsma** — Co-founder / Team Member  
Suriname

## Demo

Live FarmWiz AI Demo: [https://dash.farmwiz.net/ai/](https://dash.farmwiz.net/ai/)

The hosted demo has the private runtime configuration required for Gemini, the local ICM relay, and read-only ThingsBoard access. Those credentials are intentionally not included in this public repository. A local deployment can be configured with the variables documented in [docs/SETUP.md](./docs/SETUP.md).

Future Caribbean Demo Video: [ADD FINAL VIDEO LINK]  
Project Documentation / Data Room: [ADD FINAL DRIVE LINK]

## Documentation

- [Architecture](./docs/ARCHITECTURE.md)
- [Agentic Workflow](./docs/AGENTIC_WORKFLOW.md)
- [Project Overview](./docs/PROJECT_OVERVIEW.md)
- [Buildathon Work](./docs/BUILDATHON_WORK.md)
- [Setup](./docs/SETUP.md)
- [Responsible AI](./docs/RESPONSIBLE_AI.md)
- [Compliance & Responsible AI Statement](./docs/COMPLIANCE_AND_RESPONSIBLE_AI_STATEMENT.md)
