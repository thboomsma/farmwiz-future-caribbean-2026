# Phase 1 — Existing IoT Foundation

Before Future Caribbean, FarmWiz existed as an IoT monitoring and automation platform. Its documented foundation includes ESP32 sensor nodes, environmental and water-quality sensing, ThingsBoard telemetry and dashboards, and a FarmWiz Switchbox for local scheduling and relay-based equipment control.

Under normal connectivity, sensor nodes send telemetry directly to ThingsBoard over Wi-Fi. The Switchbox is also used during normal operation for local scheduling and equipment control. Where supported, sensor nodes can fall back to ESP-NOW and communicate with the Switchbox/local system when Wi-Fi or Internet connectivity is unavailable.

This directory is intentionally a placeholder for the historical Phase 1 firmware and infrastructure source. It is not required to understand the Phase 2 submission.

Phase 1 source code is being organized separately and will be added to this directory. The Future Caribbean 2026 submission primarily focuses on the Phase 2 Agentic AI layer built on top of this existing FarmWiz IoT foundation.

