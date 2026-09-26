# Start Here

This is a FastAPI application, not a standalone HTML file. Do not open `app/static/index.html` directly; that bypasses the server and makes the CSS and API calls fail.

From this directory:

```powershell
py -3.10 -m venv .venv
.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn app.main:app --host 127.0.0.1 --port 8090
```

Then open `http://127.0.0.1:8090/`.

The live FarmWiz deployment is served at `https://dash.farmwiz.net/ai/` through a reverse proxy. Production credentials and telemetry are intentionally excluded from this public copy.
