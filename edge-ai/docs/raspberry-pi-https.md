# FarmWiz Commander HTTPS on the Raspberry Pi

The Commander dashboard is available at **https://192.168.100.82/** on the local network. The Orchestrator serves HTTPS directly on port 443 using a Pi-generated, LAN-only certificate. The existing port 7100 remains for local API compatibility; opening its dashboard redirects to HTTPS.

## Trust the local certificate authority

The certificate is signed by a private FarmWiz LAN root CA. Browsers will warn until that root certificate is installed as a trusted root on the device used to open Commander.

1. Obtain `docs/farmwiz-orchestrator-lan-root-ca.crt` from this workspace.
2. Confirm its SHA-256 fingerprint through a trusted channel before trusting it: `D5:01:25:68:CA:50:17:F7:A7:89:7C:58:CD:63:A2:3E:86:6C:D0:86:4A:5B:45:36:A2:35:E8:33:76:7B:AC:BA`.
3. Install it in the device's **Trusted Root Certification Authorities** store. On Windows, open the certificate and choose **Install Certificate**, select **Current User**, then **Place all certificates in the following store** and choose **Trusted Root Certification Authorities**. Other operating systems have their own certificate trust settings.
4. Open `https://192.168.100.82/` and confirm the browser shows a valid secure connection.

The certificate is valid for this LAN IP only. It does not make the Pi publicly reachable, and no port forwarding or public ingress was added. A client outside the local network still cannot connect to this address.

## Credential handling

The Cloud profile's save and connection-check routes accept credentials only over an encrypted HTTPS request. The server certificate's private key and the CA private key remain on the Pi. The public CA certificate in this workspace is safe to distribute for trust installation; never copy the CA private key.

Profile name, API base URL, auth mode, and device UUID bindings remain in origin-scoped browser local storage with a SHA-256 integrity hash. Browser local storage is not encrypted. Passwords, API keys, and usernames are not stored there.

Because browsers isolate local storage by origin, settings saved previously at `http://192.168.100.82:7100` do not automatically appear at the new HTTPS origin. Re-enter non-secret profile settings under HTTPS if they were not also saved in the Pi profile.

## Certificate lifecycle

The LAN root CA is valid for ten years. The HTTPS leaf certificate is valid for one year. A weekly systemd timer checks its remaining validity and renews it when fewer than 30 days remain, then restarts the Orchestrator to load the renewed certificate. Replacing the root CA requires installing the new public root certificate on every client device.

## Runtime ports

| Listener | Purpose |
|---|---|
| `192.168.100.82:443` | HTTPS Commander UI and API |
| `192.168.100.82:7100` | Existing HTTP API; dashboard requests redirect to HTTPS |
| `127.0.0.1:7101` | Local Needle3 service |
