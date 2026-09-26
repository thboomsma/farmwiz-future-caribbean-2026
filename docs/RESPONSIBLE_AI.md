# Responsible AI

FarmWiz treats AI as decision support for farmers, not as an unrestricted controller.

- **Human oversight:** findings and recommendations are visible to the farmer; action workflows include review/approval state.
- **Grounding and transparency:** answers receive read-only telemetry, bounded history, crop, weather, and farmer context where available. Stale, missing, or unavailable data is surfaced.
- **Safe actions:** structured actions are allowlisted, validated, expiring, and mock-only in this submission. There is no unrestricted shell access.
- **Farmer control:** the farmer remains responsible for decisions and any eventual physical operation.
- **Privacy and security:** credentials belong in environment variables, are excluded by `.gitignore`, and are not part of this repository. Deployments should use least privilege and protected transport.
- **Connectivity resilience:** local control and the existing FarmWiz foundation remain important when cloud connectivity is unavailable; cloud reasoning must not be treated as real-time safety control.
- **Limitations:** model answers can be incomplete or wrong, thresholds are not a substitute for agronomic expertise, and this prototype needs continued field validation before broader operational use.

