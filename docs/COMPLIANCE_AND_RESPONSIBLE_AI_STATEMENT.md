# Compliance & Responsible AI Statement

FarmWiz is designed to assist farmers, not remove appropriate human oversight. The system helps farmers understand measured conditions, investigate unusual readings, and decide on a suitable next step. Higher-impact physical actions should require explicit validation or approval before execution.

The current FarmWiz AI application uses bounded, structured interfaces. It is not designed to give a language model unrestricted shell access or arbitrary hardware commands. The verified action workflow validates proposed devices and outputs against an allowlist, applies expiry limits, records approval state, and executes only against a mock edge. Any future local/edge integration must preserve these safeguards, add authentication and local safety checks, and allow the edge layer to reject unsafe or stale requests.

Recommendations should be grounded in available farm context. FarmWiz can use current telemetry, bounded historical data, device state, weather context, and sourced crop profiles where available. Deterministic findings identify conditions such as stale telemetry, high temperature, low soil moisture, or pH outside configured ranges. These findings and the farm-health index are evidence tools, not guaranteed agronomic diagnoses.

Transparency matters: recommendations should identify the readings or contextual sources that informed them where practical. AI-generated crop profiles are marked for review before automation, and unknown crops are not silently assigned guessed thresholds. AI output may be incomplete or incorrect and must not be presented as a guaranteed growing outcome.

Credentials, API keys, access tokens, private infrastructure details, and sensitive device information must remain outside public documents and source-controlled artifacts. Cloud AI must not be the only mechanism responsible for essential local control; existing local scheduling and supported connectivity fallback remain important. FarmWiz is a prototype and requires continued field validation. This statement makes no legal or regulatory compliance claim.
