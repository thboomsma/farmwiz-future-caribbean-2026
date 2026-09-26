from __future__ import annotations

from datetime import datetime
from typing import Any

import httpx

from ..config import settings


class ThingsBoardReadOnlyError(RuntimeError):
    pass


class ThingsBoardReadOnlyClient:
    """API-only ThingsBoard client. No database, RPC, or write methods exist."""

    def __init__(self) -> None:
        self.base_url = settings.thingsboard_url.rstrip("/")
        self.username = settings.thingsboard_username
        self.password = settings.thingsboard_password

    def _require_configured(self) -> None:
        if not (self.base_url and self.username and self.password):
            raise ThingsBoardReadOnlyError("ThingsBoard read-only credentials are not configured")

    async def _request(self, method: str, path: str, **kwargs: Any) -> Any:
        self._require_configured()
        async with httpx.AsyncClient(base_url=self.base_url, timeout=15.0) as client:
            login = await client.post("/api/auth/login", json={"username": self.username, "password": self.password})
            login.raise_for_status()
            token = login.json().get("token")
            if not token:
                raise ThingsBoardReadOnlyError("ThingsBoard login returned no token")
            response = await client.request(method, path, headers={"X-Authorization": f"Bearer {token}"}, **kwargs)
            response.raise_for_status()
            # ThingsBoard telemetry writes commonly return 200/204 with an
            # empty body. Treat that as a successful write instead of turning
            # an otherwise healthy sync into a 500 JSON decode error.
            if not response.content:
                return {}
            try:
                return response.json()
            except ValueError:
                return {"status_code": response.status_code}

    async def get_devices(self, page_size: int = 100, page: int = 0) -> Any:
        return await self._request("GET", "/api/tenant/devices", params={"pageSize": page_size, "page": page})

    async def get_device_info(self, device_id: str) -> Any:
        return await self._request("GET", f"/api/device/{device_id}")

    async def get_telemetry_keys(self, device_id: str) -> Any:
        return await self._request("GET", f"/api/plugins/telemetry/DEVICE/{device_id}/keys/timeseries")

    async def get_latest_telemetry(self, device_id: str, keys: list[str] | None = None) -> Any:
        params = {"keys": ",".join(keys)} if keys else None
        return await self._request("GET", f"/api/plugins/telemetry/DEVICE/{device_id}/values/timeseries", params=params)

    async def get_telemetry_history(self, device_id: str, telemetry_key: str, start_time: datetime, end_time: datetime, limit: int = 500) -> Any:
        params = {"keys": telemetry_key, "startTs": int(start_time.timestamp() * 1000), "endTs": int(end_time.timestamp() * 1000), "limit": limit, "agg": "NONE"}
        return await self._request("GET", f"/api/plugins/telemetry/DEVICE/{device_id}/values/timeseries", params=params)


class ThingsBoardTestRpcClient(ThingsBoardReadOnlyClient):
    """Narrow, explicit test-only RPC writer; only the approved light ON/OFF call is allowed."""

    async def set_light(self, enabled: bool) -> Any:
        devices = await self.get_devices()
        row = next((item for item in devices.get("data", []) if item.get("name") == "switchbox_01"), None)
        device_id = row.get("id", {}).get("id") if row else None
        if not device_id:
            raise ThingsBoardReadOnlyError("switchbox_01 was not found")
        return await self._request("POST", f"/api/rpc/twoway/{device_id}", json={"method": "setLight", "params": enabled, "persistent": False, "timeout": 5000})


class ThingsBoardAlarmClient(ThingsBoardReadOnlyClient):
    """Narrow alarm writer; it only creates/upserts alarms from FarmWiz findings."""

    async def upsert_alarm(self, originator_id: str, alarm_type: str, severity: str, details: dict[str, Any]) -> Any:
        allowed = {"CRITICAL", "MAJOR", "MINOR", "WARNING", "INDETERMINATE"}
        if severity not in allowed:
            severity = "WARNING"
        now = int(datetime.now().timestamp() * 1000)
        payload = {
            "type": alarm_type,
            "originator": {"entityType": "DEVICE", "id": originator_id},
            "severity": severity,
            "status": "ACTIVE_UNACK",
            "acknowledged": False,
            "cleared": False,
            "startTs": now,
            "endTs": now,
            "details": details,
        }
        return await self._request("POST", "/api/alarm", json=payload)

    async def publish_farmwiz_status(self, originator_id: str, status: str) -> Any:
        """Publish the alarm-derived FarmWiz visual state as device telemetry."""
        if status not in {"online", "offline", "warning", "critical"}:
            status = "offline"
        now = int(datetime.now().timestamp() * 1000)
        return await self._request(
            "POST",
            f"/api/plugins/telemetry/DEVICE/{originator_id}/timeseries/TELEMETRY",
            json={"farmwiz_status": [{"ts": now, "value": status}]},
        )

    async def active_alarms(self, originator_id: str) -> list[dict[str, Any]]:
        query = {
            "entityFilter": {"type": "singleEntity", "singleEntity": {"id": originator_id, "entityType": "DEVICE"}},
            "keyFilters": [],
            "pageLink": {"pageSize": 100, "page": 0, "statusList": ["ACTIVE_UNACK", "ACTIVE_ACK"]},
            "alarmFields": [{"type": "ALARM_FIELD", "key": key} for key in ("id", "type", "status")],
        }
        result = await self._request("POST", "/api/alarmsQuery/find", json=query)
        return result.get("data", []) if isinstance(result, dict) else []

    async def clear_alarm(self, alarm_id: str) -> Any:
        return await self._request("POST", f"/api/alarm/{alarm_id}/clear", json={})
