# Existing Before Future Caribbean

FarmWiz already had an IoT and automation foundation: farm sensor nodes, ThingsBoard monitoring/history, and a Switchbox for local scheduling and relay-based equipment control. Historical firmware and infrastructure source will be added to `phase-1-iot/` separately.

# Developed During Future Caribbean

The Buildathon work added an isolated FarmWiz Cloud AI application in `cloud-ai/`, together with the local Edge AI / Orchestrator implementation in `edge-ai/`. Verified additions include the FastAPI/dashboard shell, read-only ThingsBoard retrieval, bounded history, deterministic farm findings, weather and geocoded location context, crop profiles and crop-profile research, the AI adviser/provider adapters, image and speech interaction, and reviewable mock action workflows.

This submission does not claim that the entire FarmWiz platform was built during the Buildathon. It presents the Agentic AI layer built on top of the existing IoT foundation. Live physical action execution remains outside the submitted operating boundary.
