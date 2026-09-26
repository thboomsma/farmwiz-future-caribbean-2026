# Agentic Workflow

## Sense → Detect → Investigate → Reason → Recommend → Act

1. **Sense** — retrieve farm/device information through the ThingsBoard connector and receive farmer-provided context.
2. **Detect** — identify stale/offline telemetry and selected environmental findings through deterministic rules.
3. **Investigate** — retrieve bounded history, crop profile, location, weather, and equipment/status context where available.
4. **Reason** — combine the evidence into a farmer-facing answer through deterministic logic and an optional configured model provider.
5. **Recommend** — return findings and practical next checks, distinguishing read-only evidence from inference.
6. **Act** — convert supported equipment intent into a structured, allowlisted draft or mock action with approval and expiry. The public submission does not execute physical equipment actions.

## Example

For “How is my farm doing?”, the service gathers latest telemetry, seven-day history, selected crop and farm profile, weather when coordinates exist, and deterministic farm insights. It then returns a grounded response and recommended next steps. Missing or stale context is surfaced rather than silently invented.

## Action safety example

Farmer text or speech → intent parser → structured allowlisted draft → farmer review/approval state → mock-edge execution. The mock result explicitly says that no physical device was contacted.

