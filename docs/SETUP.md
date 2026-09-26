# Setup

## Prerequisites

Python 3.10+ and a virtual environment. ThingsBoard credentials are required only if live read-only telemetry is to be queried. An AI key is required only for model-backed answers.

## Install

```powershell
cd phase-2-agentic-ai
python -m venv .venv
.venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

Copy the repository `.env.example` to a private `.env` in the application directory and fill only the services you intend to use. Never commit `.env`.

## Run

Do not double-click `app/static/index.html`. It is the dashboard frontend shell and expects the FastAPI server to provide its CSS, JavaScript, and `/api` endpoints. The production dashboard is served behind the `/ai` path by the existing FarmWiz deployment.

```powershell
uvicorn app.main:app --host 127.0.0.1 --port 8090
```

Open `http://127.0.0.1:8090/`. `/health` is a safe health check. The dashboard can run without external credentials, showing configuration states and mock/local features.

## Integrations

`THINGSBOARD_URL`, `THINGSBOARD_USERNAME`, and `THINGSBOARD_PASSWORD` configure the read-only REST connector. `AI_PROVIDER`, `AI_MODEL`, and the provider key configure AI answers. `LOCAL_ICM_URL` and `LOCAL_ICM_TOKEN` configure the optional local relay. Weather and geocoding use the implemented public HTTP services when coordinates or a search query are supplied.

## Testing

```powershell
python -m pytest
python -m compileall app tests
```

Tests are non-destructive. Do not use production credentials or invoke any physical equipment while evaluating this public copy.
