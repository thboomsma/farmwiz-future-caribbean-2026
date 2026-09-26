from fastapi.testclient import TestClient

from app.main import app


client = TestClient(app)


def test_devices_endpoint_stays_inert_without_credentials() -> None:
    response = client.get("/api/thingsboard/devices")
    assert response.status_code == 503
    assert "not configured" in response.json()["detail"]

