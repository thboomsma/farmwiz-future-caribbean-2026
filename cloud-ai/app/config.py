from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Settings:
    app_name: str = os.getenv("APP_NAME", "FarmWiz AI")
    host: str = os.getenv("APP_HOST", "127.0.0.1")
    port: int = int(os.getenv("APP_PORT", "8090"))
    environment: str = os.getenv("APP_ENV", "development")
    ai_provider: str = os.getenv("AI_PROVIDER", "")
    ai_model: str = os.getenv("AI_MODEL", "")
    ai_api_key: str = os.getenv("AI_API_KEY", "")
    gemini_api_key: str = os.getenv("GEMINI_API_KEY", "")
    openai_api_key: str = os.getenv("OPENAI_API_KEY", "")
    local_icm_url: str = os.getenv("LOCAL_ICM_URL", "")
    local_icm_token: str = os.getenv("LOCAL_ICM_TOKEN", "")
    ai_daily_call_limit: int = int(os.getenv("AI_DAILY_CALL_LIMIT", "20"))
    ai_min_interval_seconds: int = int(os.getenv("AI_MIN_INTERVAL_SECONDS", "30"))
    thingsboard_url: str = os.getenv("THINGSBOARD_URL", "")
    thingsboard_username: str = os.getenv("THINGSBOARD_USERNAME", "")
    thingsboard_password: str = os.getenv("THINGSBOARD_PASSWORD", "")
    thingsboard_alarm_sync: bool = os.getenv("FARMWIZ_TB_ALARM_SYNC", "false").lower() == "true"

    @property
    def thingsboard_configured(self) -> bool:
        return bool(self.thingsboard_url and self.thingsboard_username and self.thingsboard_password)

    def provider_key(self, provider: str) -> str:
        if provider.lower() in {"gemini", "google"}:
            return self.gemini_api_key or (self.ai_api_key if self.ai_provider.lower() in {"gemini", "google"} else "")
        if provider.lower() == "openai":
            return self.openai_api_key or (self.ai_api_key if self.ai_provider.lower() == "openai" else "")
        return ""


settings = Settings()
