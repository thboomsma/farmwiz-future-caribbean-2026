from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import httpx

from .config import settings


@dataclass(frozen=True)
class ProviderStatus:
    name: str
    configured: bool
    state: str


def model_provider_status() -> ProviderStatus:
    """Report configuration only; this stage never calls an LLM."""
    configured = bool(settings.ai_provider and settings.ai_model and settings.provider_key(settings.ai_provider))
    return ProviderStatus(
        name=settings.ai_provider or "not configured",
        configured=configured,
        state="ready for later integration" if configured else "awaiting configuration",
    )


class ModelProviderError(RuntimeError):
    pass


async def generate_agent_answer(user_message: str, context: dict[str, Any], provider: str | None = None) -> str:
    """Call the configured hosted model; tool execution remains outside this adapter."""
    selected_provider = (provider or settings.ai_provider).lower()
    api_key = settings.provider_key(selected_provider)
    model = settings.ai_model if selected_provider == settings.ai_provider.lower() else ("gemini-2.5-flash" if selected_provider in {"gemini", "google"} else "gpt-5-mini")
    if selected_provider == "local_icm":
        if not settings.local_icm_url or not settings.local_icm_token:
            raise ModelProviderError("Local ICM relay is not configured")
        # Quick tunnels have a small request ceiling; live telemetry and findings
        # are enough for local latency tests, while Gemini receives full history.
        telemetry = context.get("telemetry", {})
        local_context = {
            "devices": [{"device": item.get("device"), "state": item.get("state"), "outputs": item.get("outputs")} for item in telemetry.get("devices", [])],
            "readings": [{"device": item.get("device"), "field": item.get("field"), "value": item.get("value"), "source_key": item.get("source_key")} for item in telemetry.get("readings", [])],
            "findings": context.get("insights", {}).get("findings", []),
            "farm_profile": context.get("farm_profile", {}),
            "crop_context": context.get("crop_context", {}),
            "weather": context.get("weather", {}),
        }
        async with httpx.AsyncClient(timeout=90.0) as client:
            response = await client.post(settings.local_icm_url.rstrip("/") + "/chat", headers={"Authorization": f"Bearer {settings.local_icm_token}"}, json={"message": user_message, "context": local_context})
            if response.is_error:
                raise ModelProviderError(f"Local ICM relay returned HTTP {response.status_code}: {response.text[:300]}")
            return response.json().get("answer", "Local ICM returned no answer")
    if not selected_provider or not api_key:
        raise ModelProviderError("AI provider, model, and API key are not configured")
    system = (
        "You are FarmWiz Cloud AI, a practical and farmer-friendly assistant. "
        "Answer the farmer's exact question first. Use plain language, rank the top priorities, explain why each matters, "
        "and suggest safe checks or observations. Use only the supplied read-only context. Cite device names and values, "
        "distinguish stale data, avoid jargon, and never claim to have changed hardware. If the farmer asks what to investigate, "
        "lead with a numbered list of the most important investigations rather than repeating the entire status report."
    )
    prompt = f"User question: {user_message}\n\nRead-only FarmWiz context:\n{context}"
    async with httpx.AsyncClient(timeout=30.0) as client:
        if selected_provider in {"gemini", "google"}:
            url = f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"
            response = await client.post(url, params={"key": api_key}, json={"systemInstruction": {"parts": [{"text": system}]}, "contents": [{"role": "user", "parts": [{"text": prompt}]}], "generationConfig": {"temperature": 0.2}})
            response.raise_for_status()
            data = response.json()
            try:
                return data["candidates"][0]["content"]["parts"][0]["text"]
            except (KeyError, IndexError, TypeError) as exc:
                raise ModelProviderError("Gemini returned no text response") from exc
        if selected_provider == "openai":
            response = await client.post("https://api.openai.com/v1/responses", headers={"Authorization": f"Bearer {api_key}"}, json={"model": model, "instructions": system, "input": prompt, "store": False})
            response.raise_for_status()
            data = response.json()
            if data.get("output_text"):
                return data["output_text"]
            raise ModelProviderError("OpenAI returned no text response")
    raise ModelProviderError(f"Unsupported AI provider: {selected_provider}")
