# Komodo Telegram Alerter

A lightweight, security-hardened Telegram alerter for [Komodo](https://komo.do), written in C11 using `libcurl`, `llhttp`, and `json-c`.

## Example Notification

```
✅ OK - StackAutoUpdated
*Name*: komodo (Stack)
*Resolved*: ✅
*Data*: {
  "id": "685db922720baf6840daffca",
  "name": "komodo",
  "server_id": "6849fbcd598b3a0bc1570303",
  "server_name": "docker-1",
  "images": [
    "mongo:latest"
  ]
}
```

## Quick Start (Docker Compose)

Create a new Stack with the following `compose.yaml` file:

```yaml
services:
  komodo-telegram-alerter:
    container_name: komodo-telegram-alerter
    image: ghcr.io/timoschenko/komodo-telegram-alerter:latest
    restart: unless-stopped
    user: '10001:10001'
    read_only: true
    cap_drop:
      - ALL
    security_opt:
      - no-new-privileges:true
    pids_limit: 64
    ports:
      - '3000:3000'
    healthcheck:
      test:
        - CMD-SHELL
        - unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY all_proxy ALL_PROXY && wget -q -T 2 -O /dev/null "http://127.0.0.1:$${PORT:-3000}/health" || exit 1
      interval: 30s
      timeout: 3s
      retries: 3
      start_period: 5s
```

<details>
<summary>Traefik Example</summary>

```yaml
services:
  komodo-telegram-alerter:
    container_name: komodo-telegram-alerter
    image: ghcr.io/timoschenko/komodo-telegram-alerter:latest
    restart: unless-stopped
    user: '10001:10001'
    read_only: true
    cap_drop:
      - ALL
    security_opt:
      - no-new-privileges:true
    pids_limit: 64
    # ports:
    #   - '3000:3000'
    networks:
      - proxy
    labels:
      traefik.enable: 'true'
      traefik.docker.network: proxy
      traefik.http.routers.komodo-telegram-alerter.entrypoints: websecure
      traefik.http.routers.komodo-telegram-alerter.rule: Host(`komodo-telegram-alerter.${DOMAIN}`)
      traefik.http.routers.komodo-telegram-alerter.tls: 'true'
      traefik.http.routers.komodo-telegram-alerter.tls.certresolver: cloudflare
      traefik.http.services.komodo-telegram-alerter.loadbalancer.server.port: '3000'

networks:
  proxy:
    external: true
```

</details>

### Configure Komodo

In Komodo, add a Custom Alerter with the following URL:

`http://<komodo-telegram-alerter-ip>:3000/alert?token=<TELEGRAM_TOKEN>&chat_id=<TELEGRAM_CHAT_ID>`

Or, leverage Komodo's interpolation:

`http://<komodo-telegram-alerter-ip>:3000/alert?token=[[TELEGRAM_TOKEN]]&chat_id=[[TELEGRAM_CHAT_ID]]`

**Recommended**: Use [Komodo Secrets & Variables](https://komo.do/docs/variables) to store your Telegram credentials.

### Proxy Support

If your server requires a proxy to reach the Telegram API, set the standard proxy environment variable:

```
HTTPS_PROXY=http://proxy:8080
```

All standard variants are supported: `HTTPS_PROXY`, `https_proxy`, `HTTP_PROXY`, `http_proxy`.

Example with Docker Compose:

```yaml
services:
  komodo-telegram-alerter:
    image: ghcr.io/timoschenko/komodo-telegram-alerter:latest
    restart: unless-stopped
    user: '10001:10001'
    read_only: true
    cap_drop:
      - ALL
    security_opt:
      - no-new-privileges:true
    pids_limit: 64
    ports:
      - '3000:3000'
    environment:
      HTTPS_PROXY: http://proxy:8080
```

<details>
<summary>Traefik Example</summary>

`https://komodo-telegram-alerter.[[DOMAIN]]/alert?token=[[TELEGRAM_TOKEN]]&chat_id=[[TELEGRAM_CHAT_ID]]`

</details>

### Podman / Rootless

The same image runs rootless in Podman without modification:

```sh
podman run -d --name komodo-telegram-alerter \
  --read-only \
  --cap-drop=ALL \
  --security-opt=no-new-privileges \
  --pids-limit=64 \
  --user 10001:10001 \
  -p 3000:3000 \
  ghcr.io/timoschenko/komodo-telegram-alerter:latest
```

You can also pass `--userns=keep-id` instead of `--user 10001:10001`, or run the provided `compose.yaml` with `podman compose up -d`.

- **Building with Podman**: Use `podman build --format docker -t komodo-telegram-alerter .` to preserve the Dockerfile `HEALTHCHECK` instruction (Podman's default `--format oci` omits `HEALTHCHECK` and relies on `compose.yaml` or Kubernetes probes).
- **Reboot persistence**: In rootless Podman, `restart: unless-stopped` requires systemd integration (such as Podman Quadlet or enabling `podman-restart.service`) to auto-start containers across host reboots.

### Multi-Architecture & Raspberry Pi

Pre-built images and local Docker Buildx targets support both `amd64` and Raspberry Pi ARM platforms:

| Device / OS | Docker Platform | Bake Target |
| --- | --- | --- |
| Raspberry Pi 5 / 4 / 3 / Zero 2 W (64-bit OS) | `linux/arm64` | `rpi64` |
| Raspberry Pi 4 / 3 / 2 (32-bit OS) | `linux/arm/v7` | `rpi32` |
| Raspberry Pi Zero / Zero W / Pi 1 (32-bit OS) | `linux/arm/v6` | `rpi-zero` |
| x86_64 servers / desktops | `linux/amd64` | `default` |

- **Build natively on a Raspberry Pi**:
  ```sh
  docker build -t komodo-telegram-alerter:local .
  ```
- **Cross-build a single architecture locally** (loads into local Docker engine):
  ```sh
  docker buildx build --platform linux/arm64 --load -t komodo-telegram-alerter:arm64 .
  ```
- **Cross-build using `docker-bake.hcl`**:
  ```sh
  docker buildx bake -f docker-bake.hcl rpi64 --load   # single target loaded locally
  docker buildx bake -f docker-bake.hcl rpi --push     # all Raspberry Pi targets (arm64, arm/v7, arm/v6)
  docker buildx bake -f docker-bake.hcl --push         # default multi-arch manifest (amd64, arm64, arm/v7)
  ```
- **32-bit ARM (`linux/arm/v7`, `linux/arm/v6`) emulation note**: If your build host (such as Docker Desktop on Apple Silicon) does not have 32-bit ARM binfmt registered, register QEMU handlers first:
  ```sh
  docker run --privileged --rm tonistiigi/binfmt --install arm
  ```

---

## Configuration

| Variable | Default | Valid Range / Format | Description |
| --- | --- | --- | --- |
| `PORT` | `3000` | `1..65535` | HTTP listen port |
| `BIND_ADDR` | `0.0.0.0` | IPv4/IPv6/host | Bind address |
| `TELEGRAM_API_URL` | `https://api.telegram.org` | `https://...` or `http://...` | Base URL for Telegram Bot API |
| `HTTPS_PROXY` / `https_proxy` / `HTTP_PROXY` / `http_proxy` | unset | Proxy URL | Outbound HTTP(S) proxy (checked in order listed) |
| `MAX_BODY_BYTES` | `262144` | `1024..10485760` | Maximum inbound request body size in bytes |
| `WORKER_THREADS` | `4` | `1..32` | Fixed worker thread pool size |
| `CONNECT_TIMEOUT_MS` | `5000` | `100..60000` | Outbound TCP/TLS connect timeout |
| `REQUEST_TIMEOUT_MS` | `15000` | `500..120000` | Outbound Telegram API request timeout |
| `LOG_LEVEL` | `info` | `error` \| `info` \| `debug` | Minimum log verbosity (bot tokens are never logged) |

### Healthcheck

`GET /health` returns HTTP `200` with `{"status":"ok"}`. The Dockerfile and `compose.yaml` define a built-in container healthcheck against this endpoint that unsets `HTTP_PROXY`/`HTTPS_PROXY` so local `/health` probes continue working when an outbound proxy is configured.

### Security Highlights

- **Strict input validation**: `token` (`^[0-9]+:[A-Za-z0-9_-]+$`) and `chat_id` (`^-?[0-9]+$` or `^@[A-Za-z0-9_]{5,}$`) are validated before constructing any outbound request; bot tokens are never logged.
- **Bounded parsing & timeouts**: Enforces maximum URL length (2 KiB), header size (8 KiB), request body size (`MAX_BODY_BYTES`), socket timeouts, Slowloris read deadlines, and immediate `429 Too Many Requests` (`Retry-After: 1`) rate limiting when all workers are busy.
- **Hardened outbound `libcurl`**: Enforces TLS peer/host verification, disables redirects, restricts protocols to `https` (unless `TELEGRAM_API_URL` explicitly uses `http://`), and caps response size.
- **Minimal rootless container footprint**: Multi-stage Alpine image running as pinned numeric UID/GID `10001:10001` (`/sbin/nologin`, SUID/SGID bits stripped, unprivileged port `>= 1024`), compatible with Docker rootless, Podman, Kubernetes `runAsNonRoot: true`, `read_only: true`, and `cap_drop: [ALL]`.

---

## Local Build & Test

Requires a C11 compiler, `cmake`, `libcurl`, `json-c`, and `llhttp`:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Lint & Formatting

Requires `clang-format`, `clang-tidy`, and `cppcheck`:

```sh
cmake --build build --target lint          # runs format-check, tidy, and cppcheck
cmake --build build --target format        # formats src/ and tests/ with clang-format
```

Individual targets are also available: `format-check`, `tidy`, and `cppcheck` (static analysis runs on `src/`, while formatting covers `src/` and `tests/*.c`).

### Memory Safety & Race Detection (Sanitizers)

CMake presets are provided for AddressSanitizer + UndefinedBehaviorSanitizer (`asan`) and ThreadSanitizer (`tsan`), running `test_alert`, `test_concurrency`, and `test_integration`:

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

---

## Getting Your Telegram Credentials

### Bot Token
1. Message [@BotFather](https://t.me/botfather) on Telegram
2. Create a new bot with `/newbot`
3. Follow the instructions to get your bot token

### Chat ID
1. Add your bot to the desired chat/channel
2. Send a message to the chat
3. Visit: `https://api.telegram.org/bot<YOUR_BOT_TOKEN>/getUpdates`
4. Look for the `chat.id` field in the response
