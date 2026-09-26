from __future__ import annotations

import asyncio
import base64
import json
import os
import re
import uuid
from datetime import datetime, timezone
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

import httpx
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from pydantic import BaseModel, Field
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from .config import settings
from .providers import ModelProviderError, generate_agent_answer, model_provider_status
from .tools.thingsboard import ThingsBoardAlarmClient, ThingsBoardReadOnlyClient, ThingsBoardReadOnlyError, ThingsBoardTestRpcClient


STATIC_DIR = Path(__file__).parent / "static"
CROP_DIR = Path(__file__).parents[1] / "data" / "crops"
CUSTOM_CROP_DIR = Path(os.getenv("FARMWIZ_CUSTOM_CROP_DIR", str(Path(__file__).parents[1] / "data" / "custom")))
CUSTOM_CROP_FILE = CUSTOM_CROP_DIR / "profiles.json"

app = FastAPI(title=settings.app_name, version="0.1.0")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

MOCK_ACTIONS: dict[str, dict] = {}
AGENT_CALL_TIMES: dict[str, list[datetime]] = {}
ALLOWED_MOCK_OUTPUTS = {"light", "pump", "waterValve", "airPump"}


def load_custom_crop_profiles() -> dict[str, dict[str, Any]]:
    try:
        return json.loads(CUSTOM_CROP_FILE.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return {}


def save_custom_crop_profiles(profiles: dict[str, dict[str, Any]]) -> None:
    CUSTOM_CROP_DIR.mkdir(parents=True, exist_ok=True)
    CUSTOM_CROP_FILE.write_text(json.dumps(profiles, indent=2, ensure_ascii=False), encoding="utf-8")


def parse_crop_profile(answer: str, crop_key: str, display_name: str) -> dict[str, Any]:
    """Extract and validate one usable crop-profile object from an agent reply."""
    decoder = json.JSONDecoder()
    profile: dict[str, Any] | None = None
    for index, character in enumerate(answer):
        if character != "{":
            continue
        try:
            candidate, _ = decoder.raw_decode(answer[index:])
        except json.JSONDecodeError:
            continue
        if isinstance(candidate, dict) and candidate:
            profile = candidate
            break
    if not profile:
        raise ValueError("agent returned an empty or invalid crop-profile object")

    requirements = profile.get("requirements")
    if not isinstance(requirements, list):
        requirements = []
    requirements = [str(item).strip() for item in requirements if str(item).strip()]
    if not requirements:
        raise ValueError("agent profile did not include any growing requirements")

    sources = profile.get("sources")
    if not isinstance(sources, list):
        sources = []
    sources = [str(item).strip() for item in sources if str(item).strip().startswith(("https://", "http://"))]
    profile["crop"] = crop_key
    profile["display_name"] = str(profile.get("display_name") or display_name).strip()
    profile["status"] = str(profile.get("status") or "AI-drafted - review before automation").strip()
    profile["requirements"] = requirements
    profile["sources"] = sources
    return profile


class MockActionRequest(BaseModel):
    device: str = Field(min_length=1, max_length=80)
    output: str = Field(min_length=1, max_length=40)
    value: bool
    expires_in_seconds: int = Field(default=300, ge=30, le=3600)
    scheduled_for: datetime | None = None


class AgentMessage(BaseModel):
    message: str = Field(min_length=1, max_length=2000)
    provider: str | None = Field(default=None, pattern="^(gemini|openai|local_icm)$")
    profile: dict[str, Any] = Field(default_factory=dict)


class LightTestRpcRequest(BaseModel):
    enabled: bool
    approval: str = Field(pattern="^farmer-approved$")


@app.post("/api/edge/test-light-rpc")
async def test_light_rpc(request: LightTestRpcRequest) -> dict:
    """Send only the explicitly farmer-approved light ON/OFF test RPC; no schedules."""
    try:
        result = await ThingsBoardTestRpcClient().set_light(request.enabled)
    except (ThingsBoardReadOnlyError, httpx.HTTPError) as exc:
        raise HTTPException(status_code=502, detail="ThingsBoard light RPC failed") from exc
    return {"sent": True, "method": "setLight", "params": request.enabled, "persistent": False, "timeout": 5000, "response": result}


@app.post("/api/agent/image")
async def analyze_image(question: str = Form(...), provider: str = Form("gemini"), image: UploadFile = File(...)) -> dict:
    """Analyze a farmer-provided plant image; never triggers hardware actions."""
    if provider not in {"gemini", "google", "local_icm"}:
        raise HTTPException(status_code=400, detail="Unsupported image provider")
    content_type = image.content_type or ""
    if content_type not in {"image/jpeg", "image/png", "image/webp"}:
        raise HTTPException(status_code=415, detail="Upload a JPEG, PNG, or WebP image")
    data = await image.read()
    if len(data) > 8 * 1024 * 1024:
        raise HTTPException(status_code=413, detail="Image must be 8 MB or smaller")
    if provider == "local_icm":
        if not settings.local_icm_url or not settings.local_icm_token:
            raise HTTPException(status_code=503, detail="Local ICM relay is not configured")
        image_data_url = f"data:{content_type};base64,{base64.b64encode(data).decode('ascii')}"
        async with httpx.AsyncClient(timeout=120.0) as client:
            try:
                response = await client.post(settings.local_icm_url.rstrip("/") + "/chat", headers={"Authorization": f"Bearer {settings.local_icm_token}"}, json={"message": question.strip(), "context": {"image_data_url": image_data_url, "image_filename": image.filename}})
            except httpx.HTTPError as exc:
                raise HTTPException(status_code=502, detail="Local ICM relay disconnected while receiving the image. Try a smaller image or Gemini Vision.") from exc
            if response.is_error:
                raise HTTPException(status_code=502, detail=f"Local ICM image analysis failed ({response.status_code})")
            result = response.json()
        return {"provider": "local_icm", "answer": result.get("answer", "Local ICM returned no image analysis"), "filename": image.filename}
    key = settings.provider_key("gemini")
    if not key:
        raise HTTPException(status_code=503, detail="Gemini image analysis is not configured")
    prompt = ("You are FarmWiz plant-vision assistant. Answer the farmer's question using the image and the supplied farm context. "
              "State what you can identify, confidence, visible symptoms, and safe next checks. Do not claim a certain disease diagnosis; "
              "recommend an agronomist or clearer image when uncertain. Farmer question: " + question.strip())
    payload = {"contents": [{"role": "user", "parts": [{"text": prompt}, {"inline_data": {"mime_type": content_type, "data": base64.b64encode(data).decode("ascii")}}]}], "generationConfig": {"temperature": 0.2}}
    model = settings.ai_model if settings.ai_provider.lower() in {"gemini", "google"} else "gemini-2.5-flash"
    async with httpx.AsyncClient(timeout=60.0) as client:
        try:
            response = await client.post(f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent", params={"key": key}, json=payload)
            if response.status_code == 503:
                await asyncio.sleep(2)
                response = await client.post(f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent", params={"key": key}, json=payload)
            response.raise_for_status()
        except httpx.HTTPStatusError as exc:
            raise HTTPException(status_code=502, detail=f"Gemini Vision is temporarily unavailable ({exc.response.status_code}). Try Local ICM or retry shortly.") from exc
        except httpx.HTTPError as exc:
            raise HTTPException(status_code=502, detail="Gemini Vision connection failed. Try Local ICM or retry shortly.") from exc
        result = response.json()
    try:
        answer = result["candidates"][0]["content"]["parts"][0]["text"]
    except (KeyError, IndexError, TypeError) as exc:
        raise HTTPException(status_code=502, detail="Image model returned no analysis") from exc
    return {"provider": "gemini", "answer": answer, "filename": image.filename}
class IntentRequest(BaseModel):
    transcript: str = Field(min_length=1, max_length=2000)
    edge_id: str = Field(default="farm-edge-01", min_length=1, max_length=80)
    zone_id: str = Field(default="zone-a", min_length=1, max_length=80)


@app.post("/api/mock/actions", status_code=201)
def create_mock_action(request: MockActionRequest) -> dict:
    if request.device != "switchbox_01":
        raise HTTPException(status_code=400, detail="Mock edge only permits switchbox_01")
    if request.output not in ALLOWED_MOCK_OUTPUTS:
        raise HTTPException(status_code=400, detail="Output is not in the mock action allowlist")
    from datetime import datetime, timezone, timedelta
    now = datetime.now(timezone.utc)
    scheduled_for = request.scheduled_for
    if scheduled_for and scheduled_for.tzinfo is None:
        scheduled_for = scheduled_for.replace(tzinfo=timezone.utc)
    if scheduled_for and scheduled_for <= now:
        raise HTTPException(status_code=400, detail="Schedule must be in the future")
    action = {"plan_id": f"mock-{uuid.uuid4().hex[:10]}", "device": request.device, "action": "set_output", "parameters": {"output": request.output, "value": request.value}, "created_at": now.isoformat(), "scheduled_for": scheduled_for.isoformat() if scheduled_for else None, "expires_at": (now + timedelta(seconds=request.expires_in_seconds)).isoformat(), "approval": "pending", "execution": "not_run", "target": "mock-edge"}
    MOCK_ACTIONS[action["plan_id"]] = action
    return action


@app.post("/api/edge/parse-intent")
def parse_edge_intent(request: IntentRequest) -> dict:
    """Convert a transcript into a reviewable schedule draft; never publishes or executes."""
    text = request.transcript.strip()
    lower = text.lower()
    output_aliases = {"water valve": "waterValve", "valve": "waterValve", "air pump": "airPump", "pump": "pump", "light": "light"}
    output = next((value for key, value in output_aliases.items() if key in lower), None)
    value = True if re.search(r"\b(on|start|turn on|run|blink|flash)\b", lower) else False if re.search(r"\b(off|stop|turn off)\b", lower) else None
    duration_match = re.search(r"(\d+)\s*(seconds?|minutes?|hours?)", lower)
    duration_seconds = None
    if duration_match:
        amount = int(duration_match.group(1)); unit = duration_match.group(2)
        duration_seconds = amount * (3600 if unit.startswith("hour") else 60 if unit.startswith("minute") else 1)
    needs = []
    if not output: needs.append("which output should be controlled")
    if value is None: needs.append("whether it should be turned on or off")
    if "schedule" in lower or "tomorrow" in lower or " at " in lower:
        if not re.search(r"\b(at|tomorrow|today)\b", lower): needs.append("when it should start")
    action = "blink" if re.search(r"\b(blink|flash)\b|on\s+and\s+off|every\s+", lower) else "set_output"
    interval_match = re.search(r"every\s+(?:(\d+|one|two|three|four|five)\s*)?(seconds?|minutes?)", lower)
    interval_words = {"one": 1, "two": 2, "three": 3, "four": 4, "five": 5}
    interval_amount = interval_words.get(interval_match.group(1), int(interval_match.group(1))) if interval_match and interval_match.group(1) and interval_match.group(1).isdigit() else interval_words.get(interval_match.group(1), 1) if interval_match else None
    interval_seconds = (interval_amount * (60 if interval_match.group(2).startswith("minute") else 1)) if interval_match else None
    rpc_commands = []
    if output and action == "blink" and interval_seconds:
        rpc_interval = interval_seconds
        rpc_commands = [{"method": "setCron", "params": {"relay": output, "enabled": True, "hasSecondsPrecision": True, "cronOn": f"*/{rpc_interval} * * * * *", "cronOff": f"1-59/{rpc_interval} * * * * *"}}, {"method": "getCron", "params": {"relay": output}}]
    elif output and value is not None:
        rpc_commands = [{"method": "setLight" if output == "light" else f"set{output[0].upper()}{output[1:]}", "params": value, "persistent": False, "timeout": 5000}]
    draft = {"schema_version": "farmwiz.edge.schedule.v1", "request_id": f"draft-{uuid.uuid4().hex[:10]}", "edge_id": request.edge_id, "zone_id": request.zone_id, "device": "switchbox_01", "action": action, "parameters": {"output": output, "value": value, "interval_seconds": interval_seconds}, "schedule": {"start_expression": text, "duration_seconds": duration_seconds, "repeat": None}, "thingsboard_rpc_preview": rpc_commands, "source": {"kind": "farmer_voice", "transcript": text, "confidence": 0.9 if not needs else 0.55}, "approval": {"required": True, "state": "draft"}, "status": "needs_clarification" if needs else "ready_for_farmer_review", "clarification": needs}
    return draft


@app.post("/api/mock/actions/{plan_id}/approve")
def approve_mock_action(plan_id: str) -> dict:
    action = MOCK_ACTIONS.get(plan_id)
    if not action:
        raise HTTPException(status_code=404, detail="Mock action plan not found")
    if action["approval"] != "pending":
        raise HTTPException(status_code=409, detail="Mock action is not pending approval")
    action["approval"] = "approved"
    return action


@app.post("/api/mock/actions/{plan_id}/execute")
def execute_mock_action(plan_id: str) -> dict:
    from datetime import datetime, timezone
    action = MOCK_ACTIONS.get(plan_id)
    if not action:
        raise HTTPException(status_code=404, detail="Mock action plan not found")
    if action["approval"] != "approved":
        raise HTTPException(status_code=409, detail="Mock action requires approval")
    if action.get("scheduled_for") and datetime.fromisoformat(action["scheduled_for"]) > datetime.now(timezone.utc):
        raise HTTPException(status_code=409, detail="Mock action is scheduled for a future time")
    if datetime.fromisoformat(action["expires_at"]) <= datetime.now(timezone.utc):
        action["execution"] = "expired"
        raise HTTPException(status_code=409, detail="Mock action has expired")
    action["execution"] = "simulated"
    action["result"] = "accepted by mock edge; no physical device contacted"
    return action


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok", "service": "farmwiz-ai", "environment": settings.environment}


@app.get("/api/system/status")
def system_status() -> dict:
    return {
        "service": "online",
        "thingsboard": "configured for read-only discovery" if settings.thingsboard_configured else "credentials pending",
        "telemetry": "read-only connector available" if settings.thingsboard_configured else "not configured",
        "monitoring": "deterministic insights active",
        "edge": "mock-only / no physical control",
        "edge_agent": {"local_icm": "configured over private relay" if settings.local_icm_url and settings.local_icm_token else "not configured", "physical_control": "disabled"},
        "ai": model_provider_status().__dict__,
    }


@app.post("/api/agent/chat")
async def agent_chat(request: AgentMessage) -> dict:
    selected_provider = request.provider or settings.ai_provider
    if selected_provider != "local_icm" and not settings.provider_key(selected_provider):
        raise HTTPException(status_code=503, detail="AI provider is not configured")
    now = datetime.now(timezone.utc)
    provider_calls = AGENT_CALL_TIMES.setdefault(selected_provider, [])
    today_calls = [stamp for stamp in provider_calls if stamp.date() == now.date()]
    if len(today_calls) >= settings.ai_daily_call_limit:
        raise HTTPException(status_code=429, detail="Daily AI call guard reached; deterministic telemetry remains available")
    if today_calls and (now - today_calls[-1]).total_seconds() < settings.ai_min_interval_seconds:
        raise HTTPException(status_code=429, detail="AI call cooldown active; wait before asking again")
    try:
        weather_context = {}
        if request.profile.get("latitude") is not None and request.profile.get("longitude") is not None:
            try:
                weather_context = await weather(float(request.profile["latitude"]), float(request.profile["longitude"]))
            except (HTTPException, ValueError, TypeError):
                weather_context = {"status": "unavailable"}
        crop_context = {}
        crop_name = str(request.profile.get("crop", "")).strip().lower()
        if crop_name and crop_name.isidentifier() and (CROP_DIR / f"{crop_name}.json").exists():
            crop_context = json.loads((CROP_DIR / f"{crop_name}.json").read_text(encoding="utf-8"))
        context = {"telemetry": await farm_latest_telemetry(), "insights": await farm_insights(), "history": await farm_history(days=7, limit=20), "farm_profile": request.profile, "crop_context": crop_context, "weather": weather_context}
        answer = await generate_agent_answer(request.message, context, selected_provider)
        AGENT_CALL_TIMES[selected_provider] = today_calls
        AGENT_CALL_TIMES[selected_provider].append(now)
        model = settings.ai_model if selected_provider == settings.ai_provider else ("gemini-2.5-flash" if selected_provider == "gemini" else "gpt-5-mini" if selected_provider == "openai" else "local-codex")
        return {"provider": selected_provider, "model": model, "answer": answer, "mode": "read-only-context"}
    except ModelProviderError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail="AI provider request failed") from exc


@app.get("/api/agent/usage")
def agent_usage() -> dict:
    today = datetime.now(timezone.utc).date()
    default_provider = settings.ai_provider or "gemini"
    by_provider = {provider: len([stamp for stamp in stamps if stamp.date() == today]) for provider, stamps in AGENT_CALL_TIMES.items()}
    return {"provider": default_provider, "model": settings.ai_model or "not configured", "calls_today": by_provider.get(default_provider, 0), "calls_by_provider": by_provider, "daily_limit": settings.ai_daily_call_limit, "cooldown_seconds": settings.ai_min_interval_seconds, "guard": "active"}


@app.get("/api/agent/providers")
def agent_providers() -> dict:
    return {"default": settings.ai_provider or "gemini", "providers": [{"name": name, "configured": bool(settings.provider_key(name)) if name != "local_icm" else bool(settings.local_icm_url and settings.local_icm_token), "model": "gemini-2.5-flash" if name == "gemini" else "gpt-5-mini" if name == "openai" else "local-codex"} for name in ("gemini", "openai", "local_icm")]}


@app.get("/api/thingsboard/devices")
async def thingsboard_devices() -> dict:
    try:
        return await ThingsBoardReadOnlyClient().get_devices()
    except ThingsBoardReadOnlyError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail="ThingsBoard read-only request failed") from exc


@app.get("/api/thingsboard/devices/{device_id}/keys")
async def thingsboard_keys(device_id: str) -> list[str]:
    try:
        return await ThingsBoardReadOnlyClient().get_telemetry_keys(device_id)
    except ThingsBoardReadOnlyError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail="ThingsBoard read-only request failed") from exc


@app.get("/api/thingsboard/devices/{device_id}/latest")
async def thingsboard_latest(device_id: str, keys: str | None = None) -> dict:
    try:
        selected = [item.strip() for item in keys.split(",") if item.strip()] if keys else None
        return await ThingsBoardReadOnlyClient().get_latest_telemetry(device_id, selected)
    except ThingsBoardReadOnlyError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail="ThingsBoard read-only request failed") from exc


@app.get("/api/thingsboard/devices/{device_id}/history")
async def thingsboard_history(
    device_id: str,
    telemetry_key: str,
    start_time: datetime | None = None,
    end_time: datetime | None = None,
    limit: int = 500,
) -> dict:
    """Read a bounded telemetry history window; this endpoint cannot mutate data."""
    end = end_time or datetime.now(timezone.utc)
    start = start_time or (end - timedelta(hours=24))
    if start >= end:
        raise HTTPException(status_code=400, detail="start_time must be before end_time")
    if limit < 1 or limit > 5000:
        raise HTTPException(status_code=400, detail="limit must be between 1 and 5000")
    try:
        return await ThingsBoardReadOnlyClient().get_telemetry_history(
            device_id, telemetry_key, start, end, limit
        )
    except ThingsBoardReadOnlyError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail="ThingsBoard read-only request failed") from exc


FARMWIZ_SENSORS = {
    "switchbox_01": {"temperature": "temperature", "humidity": "humidity", "soil_moisture": "soilMoisture", "light_level": "lightLevel", "light": "light", "pump": "pump", "water_valve": "waterValve", "air_pump": "airPump", "status": "status"},
    "node_01": {"temperature": "temp", "humidity": "hum", "soil_moisture": "soilMoisture", "soil_temperature": "soiltemp"},
    "node02": {"temperature": "temp", "humidity": "hum", "soil_moisture": "soilMoisture", "soil_temperature": "soiltemp", "ldr": "ldrADC"},
    "ph_01": {"ph": "pH", "water_quality": "waterQuality"},
    "tds_01": {"tds_ppm": "tds_ppm"},
}


@app.get("/api/farm/telemetry/latest")
async def farm_latest_telemetry() -> dict:
    """Return a stable FarmWiz view while preserving each reading's source device/key."""
    client = ThingsBoardReadOnlyClient()
    try:
        devices = await client.get_devices()
        device_rows = devices.get("data", []) if isinstance(devices, dict) else []
        by_name = {row.get("name"): row for row in device_rows if row.get("name")}
        requests = []
        labels = []
        for name, mapping in FARMWIZ_SENSORS.items():
            row = by_name.get(name)
            if not row:
                continue
            device_id = row.get("id", {}).get("id") if isinstance(row.get("id"), dict) else row.get("id")
            if not device_id:
                continue
            requests.append(client.get_latest_telemetry(device_id, list(mapping.values())))
            labels.append((name, mapping))
        readings = await asyncio.gather(*requests)
        result = []
        for (name, mapping), payload in zip(labels, readings):
            reverse = {key: field for field, key in mapping.items()}
            for key, values in (payload or {}).items():
                item = values[0] if values else {}
                result.append({
                    "device": name,
                    "field": reverse.get(key, key),
                    "source_key": key,
                    "value": item.get("value"),
                    "ts": item.get("ts"),
                })
        states = []
        for (name, mapping), payload in zip(labels, readings):
            values = {key: (items[0].get("value") if items else None) for key, items in (payload or {}).items()}
            status_value = str(values.get(mapping.get("status", "status"), "")).lower()
            latest_ts = max((items[0].get("ts", 0) for items in (payload or {}).values() if items), default=0)
            # A device is stale when its newest telemetry is older than one hour.
            age_seconds = max(0, (datetime.now(timezone.utc).timestamp() * 1000 - latest_ts) / 1000) if latest_ts else None
            online = status_value in {"online", "true", "1"} or (age_seconds is not None and age_seconds < 3600)
            states.append({"device": name, "state": "online" if online else "offline", "age_seconds": round(age_seconds) if age_seconds is not None else None, "outputs": {field: values.get(key) for field, key in mapping.items() if field in {"light", "pump", "water_valve", "air_pump"}}})
        return {"readings": result, "sources": sorted({item["device"] for item in result}), "devices": states}
    except ThingsBoardReadOnlyError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail="ThingsBoard read-only request failed") from exc


@app.get("/api/farm/insights")
async def farm_insights() -> dict:
    """Deterministic observations only; no model call and no device-control path."""
    telemetry = await farm_latest_telemetry()
    findings = []
    for device in telemetry.get("devices", []):
        if device["state"] == "offline":
            findings.append({"severity": "warning", "type": "connectivity", "device": device["device"], "message": f"{device['device']} has no recent telemetry. Check power and network before trusting its readings."})
    for item in telemetry.get("readings", []):
        try:
            value = float(item["value"])
        except (TypeError, ValueError):
            continue
        if item["field"] == "temperature" and value >= 40:
            findings.append({"severity": "critical" if value >= 45 else "watch", "type": "heat", "device": item["device"], "message": f"Air temperature is {value:.1f}°C."})
        if item["field"] == "soil_moisture" and value <= 20:
            findings.append({"severity": "critical" if value <= 10 else "watch", "type": "soil_moisture", "device": item["device"], "message": f"Soil moisture is {value:.0f}%."})
        if item["field"] == "ph" and not 5.5 <= value <= 7.5:
            findings.append({"severity": "critical" if value < 4.5 or value > 8.5 else "watch", "type": "water_ph", "device": item["device"], "message": f"Water pH is {value:.2f}."})
    recommendations = []
    for finding in findings:
        if finding["type"] == "soil_moisture":
            recommendations.append({"priority": "high", "device": finding["device"], "recommendation": "Check soil by hand at root depth before irrigating; the sensor is stale and should not trigger watering by itself."})
        elif finding["type"] == "heat":
            recommendations.append({"priority": "high", "device": finding["device"], "recommendation": "Verify the temperature with a second thermometer and inspect plants for heat stress or poor airflow."})
        elif finding["type"] == "water_ph":
            recommendations.append({"priority": "medium", "device": finding["device"], "recommendation": "Retest pH with a calibrated meter before changing water chemistry."})
    alarm_sync = {"enabled": settings.thingsboard_alarm_sync, "sent": 0, "error": None}
    if settings.thingsboard_alarm_sync:
        try:
            devices = await ThingsBoardAlarmClient().get_devices()
            ids = {item.get("name"): item.get("id", {}).get("id") for item in devices.get("data", [])}
            client = ThingsBoardAlarmClient()
            active_types: set[tuple[str, str]] = set()
            for finding in findings:
                originator_id = ids.get(finding.get("device"))
                if not originator_id:
                    continue
                severity = {"critical": "CRITICAL", "warning": "WARNING", "watch": "MINOR"}.get(finding.get("severity"), "WARNING")
                alarm_type = f"FarmWiz {finding.get('type', 'finding')}"
                await client.upsert_alarm(originator_id, alarm_type, severity, {"device": finding.get("device"), "message": finding.get("message"), "source": "farmwiz_cloud_ai", "farmwiz_severity": finding.get("severity")})
                active_types.add((originator_id, alarm_type))
                alarm_sync["sent"] += 1
            severity_rank = {"online": 0, "warning": 1, "critical": 2}
            status_by_device: dict[str, str] = {device["device"]: ("online" if device.get("state") == "online" else "offline") for device in telemetry.get("devices", [])}
            for finding in findings:
                device_name = finding.get("device")
                # A stale device remains visibly OFFLINE; its cached readings
                # must not turn the card amber until telemetry returns.
                if device_name and status_by_device.get(device_name) == "offline":
                    continue
                candidate = "critical" if finding.get("severity") == "critical" else "warning"
                if device_name and severity_rank.get(candidate, 0) > severity_rank.get(status_by_device.get(device_name, "offline"), 0):
                    status_by_device[device_name] = candidate
            for device_name, status in status_by_device.items():
                originator_id = ids.get(device_name)
                if originator_id:
                    await client.publish_farmwiz_status(originator_id, status)
            alarm_sync["status_published"] = len(status_by_device)
            # Clearing is intentionally deferred until the alarm query contract is
            # verified against this self-hosted ThingsBoard version.
            alarm_sync["clear_pending"] = True
        except (ThingsBoardReadOnlyError, httpx.HTTPError) as exc:
            alarm_sync["error"] = str(exc)
    return {"mode": "deterministic-read-only", "findings": findings, "recommendations": recommendations, "count": len(findings), "thingsboard_alarm_sync": alarm_sync}


@app.post("/api/integrations/thingsboard/alarms/sync")
async def sync_thingsboard_alarms() -> dict:
    """Explicitly run the guarded FarmWiz finding-to-ThingsBoard alarm bridge."""
    insights = await farm_insights()
    return {"findings": insights["count"], "alarm_sync": insights["thingsboard_alarm_sync"]}


@app.get("/api/farm/health")
async def farm_health() -> dict:
    """Transparent system health index from current read-only evidence."""
    telemetry = await farm_latest_telemetry()
    insights = await farm_insights()
    offline = [device["device"] for device in telemetry.get("devices", []) if device.get("state") == "offline"]
    critical = sum(1 for finding in insights.get("findings", []) if finding.get("severity") == "critical")
    warnings = sum(1 for finding in insights.get("findings", []) if finding.get("severity") in {"warning", "watch"})
    score = max(0, 100 - (len(offline) * 10) - (warnings * 8) - (critical * 18))
    status = "healthy" if score >= 80 else "watch" if score >= 55 else "attention"
    return {"score": score, "status": status, "basis": "device connectivity + deterministic findings", "offline_devices": offline, "finding_count": insights.get("count", 0), "findings": insights.get("findings", [])}


@app.get("/api/farm/status")
async def farm_status() -> dict:
    """Return the four-state visual status used by ThingsBoard cards/overlays."""
    telemetry = await farm_latest_telemetry()
    insights = await farm_insights()
    rank = {"online": 0, "warning": 1, "critical": 2}
    states = {d["device"]: ("online" if d.get("state") == "online" else "offline") for d in telemetry.get("devices", [])}
    for finding in insights.get("findings", []):
        device = finding.get("device")
        if device and states.get(device) == "offline":
            continue
        candidate = "critical" if finding.get("severity") == "critical" else "warning"
        if device and rank.get(candidate, 0) > rank.get(states.get(device, "offline"), 0):
            states[device] = candidate
    return {
        "statuses": [{"device": device, "status": state, "color": {"online": "#2e7d32", "offline": "#64748b", "warning": "#d97706", "critical": "#dc2626"}[state]} for device, state in sorted(states.items())],
        "legend": {"online": "#2e7d32", "offline": "#64748b", "warning": "#d97706", "critical": "#dc2626"},
    }


@app.get("/api/farm/history")
async def farm_history(days: int = 7, limit: int = 50) -> dict:
    """Bounded historical telemetry for trend analysis; GET-only."""
    if days < 1 or days > 30 or limit < 1 or limit > 200:
        raise HTTPException(status_code=400, detail="days must be 1-30 and limit must be 1-200")
    client = ThingsBoardReadOnlyClient()
    devices = await client.get_devices()
    rows = devices.get("data", []) if isinstance(devices, dict) else []
    by_name = {row.get("name"): row for row in rows}
    requested = {"switchbox_01": ["temperature", "humidity", "soilMoisture", "lightLevel", "light", "pump", "waterValve", "airPump"], "node_01": ["temp", "hum", "soilMoisture"], "node02": ["temp", "hum", "soilMoisture", "ldrADC"], "ph_01": ["pH"], "tds_01": ["tds_ppm"]}
    end = datetime.now(timezone.utc)
    start = end - timedelta(days=days)
    jobs = []
    labels = []
    for name, keys in requested.items():
        row = by_name.get(name)
        device_id = row.get("id", {}).get("id") if row and isinstance(row.get("id"), dict) else None
        if not device_id:
            continue
        for key in keys:
            jobs.append(client.get_telemetry_history(device_id, key, start, end, limit))
            labels.append((name, key))
    try:
        payloads = await asyncio.gather(*jobs)
    except (ThingsBoardReadOnlyError, httpx.HTTPError) as exc:
        raise HTTPException(status_code=502, detail="ThingsBoard history request failed") from exc
    series = []
    for (name, key), payload in zip(labels, payloads):
        series.append({"device": name, "source_key": key, "samples": payload.get(key, []) if isinstance(payload, dict) else []})
    return {"days": days, "series": series}


@app.get("/api/weather")
async def weather(latitude: float, longitude: float) -> dict:
    """Read-only current and short-range weather for the selected farm profile."""
    if not (-90 <= latitude <= 90 and -180 <= longitude <= 180):
        raise HTTPException(status_code=400, detail="Invalid coordinates")
    params = {
        "latitude": latitude,
        "longitude": longitude,
        "current": "temperature_2m,relative_humidity_2m,precipitation,weather_code,wind_speed_10m",
        "daily": "temperature_2m_max,temperature_2m_min,precipitation_probability_max,precipitation_sum",
        "forecast_days": 3,
        "timezone": "auto",
    }
    try:
        async with httpx.AsyncClient(timeout=15.0) as client:
            response = await client.get("https://api.open-meteo.com/v1/forecast", params=params)
            response.raise_for_status()
            data = response.json()
        return {"source": "open-meteo", "latitude": latitude, "longitude": longitude, "timezone": data.get("timezone"), "current": data.get("current", {}), "daily": data.get("daily", {})}
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail="Weather provider unavailable") from exc


@app.get("/api/geocode")
async def geocode(query: str) -> dict:
    """Proxy read-only location search so browser CORS/network policy cannot break the profile form."""
    query = query.strip()
    if len(query) < 2 or len(query) > 120:
        raise HTTPException(status_code=400, detail="Location query must be 2-120 characters")
    try:
        async with httpx.AsyncClient(timeout=10.0) as client:
            response = await client.get("https://geocoding-api.open-meteo.com/v1/search", params={"name": query, "count": 5, "language": "en", "format": "json"})
            response.raise_for_status()
            data = response.json()
            if data.get("results"):
                return {**data, "places": data["results"]}
            osm = await client.get("https://nominatim.openstreetmap.org/search", params={"q": query, "format": "jsonv2", "limit": 5, "addressdetails": 1}, headers={"User-Agent": "FarmWiz-AI/1.0 location search"})
            osm.raise_for_status()
            places = []
            for item in osm.json():
                address = item.get("address", {})
                places.append({"name": item.get("name") or query, "admin1": address.get("state") or address.get("county"), "country": address.get("country"), "latitude": float(item["lat"]), "longitude": float(item["lon"]), "timezone": None})
            return {"results": places, "places": places}
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail="Location search unavailable") from exc


class CropResearchRequest(BaseModel):
    crop: str = Field(min_length=2, max_length=80)
    provider: str | None = Field(default=None, pattern="^(gemini|openai|local_icm)$")


@app.get("/api/crops")
def crop_catalog() -> dict:
    profiles = load_custom_crop_profiles()
    names = []
    for path in sorted(CROP_DIR.glob("*.json")):
        if path.name != "README.json":
            names.append(path.stem)
    return {"crops": sorted(set(names) | set(profiles))}


@app.post("/api/crops/research")
async def research_crop(request: CropResearchRequest) -> dict:
    """Ask the configured agent for a sourced draft and persist it for reuse."""
    key = re.sub(r"[^a-z0-9]+", "_", request.crop.strip().lower()).strip("_")
    if not key:
        raise HTTPException(status_code=400, detail="Invalid crop name")
    existing = load_custom_crop_profiles()
    if key in existing:
        return existing[key]
    prompt = (
        f"Create a cautious, source-backed crop profile for {request.crop.strip()}. "
        "Return JSON only with keys crop, display_name, status, soil_ph, growing_temperature, requirements, sources. "
        "Use conservative ranges, do not invent precision, and include 2 authoritative extension or government URLs. "
        "Requirements must be a non-empty list of practical growing requirements. "
        "Set status to 'AI-drafted - review before automation'."
    )
    try:
        answer = await generate_agent_answer(prompt, {"farm_profile": {}, "crop_context": {}}, request.provider)
        profile = parse_crop_profile(answer, key, request.crop.strip())
        existing[key] = profile
        save_custom_crop_profiles(existing)
        return profile
    except (ModelProviderError, ValueError, json.JSONDecodeError, httpx.HTTPError) as exc:
        # Profiles are cached after creation. If the laptop relay is temporarily
        # unavailable, use Gemini once rather than leaving the crop card empty.
        if request.provider == "local_icm" and settings.provider_key("gemini"):
            try:
                answer = await generate_agent_answer(prompt, {"farm_profile": {}, "crop_context": {}}, "gemini")
                profile = parse_crop_profile(answer, key, request.crop.strip())
                profile["status"] = "AI-drafted by Gemini - review before automation"
                existing[key] = profile
                save_custom_crop_profiles(existing)
                return profile
            except (ModelProviderError, ValueError, json.JSONDecodeError, httpx.HTTPError):
                pass
        raise HTTPException(status_code=502, detail=f"Crop research unavailable: {exc}") from exc


@app.delete("/api/crops/{crop_name}")
def delete_custom_crop(crop_name: str) -> dict:
    key = crop_name.strip().lower()
    profiles = load_custom_crop_profiles()
    if key not in profiles:
        raise HTTPException(status_code=404, detail="Only saved custom crop profiles can be deleted")
    del profiles[key]
    save_custom_crop_profiles(profiles)
    return {"deleted": key}


@app.get("/api/crops/{crop_name}")
def crop_profile(crop_name: str) -> dict:
    """Return a sourced crop profile; unknown crops are never guessed."""
    safe_name = crop_name.strip().lower()
    if not safe_name.isidentifier():
        raise HTTPException(status_code=400, detail="Invalid crop name")
    profile_path = CROP_DIR / f"{safe_name}.json"
    custom = load_custom_crop_profiles().get(safe_name)
    if custom:
        return custom
    if not profile_path.exists():
        raise HTTPException(status_code=404, detail="No sourced profile is available for this crop yet")
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    ph = profile.get("soil_ph", {})
    temp = profile.get("growing_temperature", {})
    if isinstance(ph, dict):
        profile["ph_range"] = f"{ph.get('min', '—')}–{ph.get('max', '—')}"
    if isinstance(temp, dict):
        profile["temperature_c"] = f"{temp.get('min', '—')}–{temp.get('max', '—')}"
    return profile


@app.get("/", include_in_schema=False)
def dashboard() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/embed/weather", include_in_schema=False)
def embedded_weather() -> FileResponse:
    """Compact weather/location panel for embedding in ThingsBoard."""
    return FileResponse(STATIC_DIR / "embed-weather.html")


@app.get("/embed/chat", include_in_schema=False)
def embedded_chat() -> FileResponse:
    """Compact FarmWiz chat and voice panel for ThingsBoard embedding."""
    return FileResponse(STATIC_DIR / "embed-chat.html")


@app.get("/embed/crops", include_in_schema=False)
def embedded_crops() -> FileResponse:
    """Compact crop selection and requirements panel for ThingsBoard."""
    return FileResponse(STATIC_DIR / "embed-crops.html")
