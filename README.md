# Komodo Telegram Alerter

A lightweight Telegram alerter for [Komodo](https://komododev.io/).

## Example Notification

```
✅ OK - StackAutoUpdated
For: komodo (Stack)
Resolved: ✅
Data: {
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
    image: sashabusinaro/komodo-telegram-alerter:latest
    restart: unless-stopped
    ports:
      - '3000:3000'
```

<details>
<summary>Traefik Example</summary>

```yaml
services:
  komodo-telegram-alerter:
    container_name: komodo-telegram-alerter
    image: sashabusinaro/komodo-telegram-alerter:latest
    restart: unless-stopped
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

Or, to make use of Komodo interpolation:

`http://<komodo-telegram-alerter-ip>:3000/alert?token=[[TELEGRAM_TOKEN]]&chat_id=[[TELEGRAM_CHAT_ID]]`

**Recommended**: Use [Komodo Secrets & Variables](https://docs.komododev.io/configuration/secrets/) to store your Telegram credentials.

<details>
<summary>Traefik Example</summary>

`https://komodo-telegram-alerter.[[DOMAIN]]/alert?token=[[TELEGRAM_TOKEN]]&chat_id=[[TELEGRAM_CHAT_ID]]`

</details>

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