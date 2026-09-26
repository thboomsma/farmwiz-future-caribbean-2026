from fastapi.testclient import TestClient

from app.main import app


client = TestClient(app)


def test_health() -> None:
    response = client.get("/health")
    assert response.status_code == 200
    assert response.json()["status"] == "ok"


def test_dashboard_and_static_assets() -> None:
    assert client.get("/").status_code == 200
    assert client.get("/static/style.css").status_code == 200


def test_system_status_does_not_claim_live_integrations() -> None:
    data = client.get("/api/system/status").json()
    assert data["thingsboard"] == "discovery pending"
    assert data["telemetry"] == "not connected"

