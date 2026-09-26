# Raspberry Pi installation

This guide installs the Orchestrator service on a Raspberry Pi OS/Debian host. It does not install Needle3: provide a separately built, supported Needle3 service listening on `127.0.0.1:7101` before enabling interpretation. The checked-in Dockerfile is only a build recipe; its `linux-armv7/needle` executable and `needle3.cact` model are not part of this source package.

## Requirements

- Raspberry Pi OS/Debian, systemd, OpenSSL, and Node.js 22.x (the deployed Pi uses Node 22.23.2).
- A `pi` service account, or update the systemd unit consistently for another account.
- LAN address and firewall rules appropriate to the deployment. The included runtime template uses `192.168.100.82`, TCP 443, and TCP 7100; change these values for another host.
- Optional upstream MQTT broker and FarmWiz Cloud credentials. Store credentials only in the protected runtime files on the Pi.

## Install the application

Copy the release contents to `/opt/farmwiz-orchestrator/app`, keeping the JavaScript modules, JSON catalog, and `ui/` directory together. Then install dependencies and create writable/configuration directories:

```sh
sudo install -d -o pi -g pi -m 0755 /opt/farmwiz-orchestrator/app
sudo install -d -o pi -g pi -m 0750 /opt/farmwiz-orchestrator/var/data
sudo install -d -o root -g root -m 0700 /etc/farmwiz-orchestrator
sudo chown -R pi:pi /opt/farmwiz-orchestrator/app
cd /opt/farmwiz-orchestrator/app
npm ci --omit=dev
```

Install `orchestrator.env.example` as `/etc/farmwiz-orchestrator/orchestrator.env`, then edit it for the target host. Keep it owned by `root:root` with mode `0600`. `MQTT_USERNAME` and `MQTT_PASSWORD` may be added there when needed; never place credentials in source control or the browser's local storage. Review `MQTT_ENABLED` and `MQTT_URL` before first start so the service only connects to an intended broker.

```sh
sudo install -o root -g root -m 0600 orchestrator.env.example /etc/farmwiz-orchestrator/orchestrator.env
sudoedit /etc/farmwiz-orchestrator/orchestrator.env
```

Set `BIND_HOST` to the Pi's LAN address and ensure that address is stable. Configure Needle3 at `NEEDLE_URL`. The service unit currently uses the deployed Pi's Node path (`/home/pi/.nvm/versions/node/v22.23.2/bin/node`); edit `ExecStart` if Node is installed elsewhere.

## Enable HTTPS

The service template enables HTTPS on port 443 and expects a certificate/key at `/etc/farmwiz-orchestrator/tls/`. For a new private-LAN deployment, edit `provision-lan-tls.sh` so its subject alternative name matches the chosen stable Pi IP, review the script, then run it on the Pi as root. It creates a private local root CA and server certificate. Keep `local-ca.key` only on the Pi; distribute the public `local-ca.crt` to clients through a trusted channel and install it in their trusted root store. Browsers will warn until that CA is trusted. Do not reuse one installation's private CA key on other devices.

Install the renewal script and systemd timer/service files, adjusting the IP in `renew-lan-tls.sh` if needed:

```sh
sudo install -o root -g root -m 0755 renew-lan-tls.sh /usr/local/sbin/farmwiz-orchestrator-renew-lan-tls
sudo install -o root -g root -m 0644 farmwiz-orchestrator-tls-renew.service /etc/systemd/system/
sudo install -o root -g root -m 0644 farmwiz-orchestrator-tls-renew.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now farmwiz-orchestrator-tls-renew.timer
```

Review `farmwiz-orchestrator.service` and install it. Confirm the service user can read the TLS leaf key and write only to the intended data/config paths. Then:

```sh
sudo install -o root -g root -m 0644 farmwiz-orchestrator.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now farmwiz-orchestrator.service
sudo systemctl status farmwiz-orchestrator.service
curl --cacert /etc/farmwiz-orchestrator/tls/local-ca.crt https://<pi-lan-ip>/health
```

Expected health output includes `ok: true`. The HTTPS dashboard is `https://<pi-lan-ip>/`; HTTP on port 7100 remains available for API compatibility and the dashboard root redirects to HTTPS. Keep the service LAN-scoped unless a separately reviewed authenticated remote-access design is deployed. Verify only the intended interfaces/ports are reachable.

## Updating and rollback

Back up the current app directory, runtime configuration, registry database, and TLS files before replacing source. Keep `/etc/farmwiz-orchestrator/` and `/opt/farmwiz-orchestrator/var/` outside source archives. Install the release files, run `npm ci --omit=dev`, and restart only `farmwiz-orchestrator.service`; check `/health` and the journal. Restore the backup if the service fails to start. Never overwrite the Pi's protected runtime settings or TLS assets with release files.

## Current capability limits

- No user authentication/public ingress for an external mobile/web client is included.
- The FarmWiz Cloud profile is a setup/client foundation; credentials and device bindings still require explicit configuration.
- The service does not currently send physical relay actions. Do not treat an interpreted or confirmation-required proposal as an executed command.
- Needle3 catalog regeneration does not guarantee that a running Needle3 process has reloaded the generated catalog.
