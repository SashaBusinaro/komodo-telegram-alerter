FROM docker.io/library/alpine:3.21 AS builder

RUN apk add --no-cache \
    build-base \
    cmake \
    curl-dev \
    json-c-dev \
    llhttp-dev

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF && \
    cmake --build build --parallel && \
    strip build/komodo-telegram-alerter

FROM docker.io/library/alpine:3.21

LABEL org.opencontainers.image.title="komodo-telegram-alerter" \
      org.opencontainers.image.description="Lightweight, security-hardened Telegram alerter for Komodo" \
      org.opencontainers.image.source="https://github.com/timoschenko/komodo-telegram-alerter" \
      org.opencontainers.image.url="https://github.com/timoschenko/komodo-telegram-alerter" \
      org.opencontainers.image.licenses="MIT"

ARG APP_UID=10001
ARG APP_GID=10001

RUN apk add --no-cache \
    libcurl \
    json-c \
    llhttp \
    ca-certificates && \
    addgroup -g "${APP_GID}" -S app && \
    adduser -u "${APP_UID}" -S -H -D -s /sbin/nologin -G app app && \
    find / -xdev -perm /6000 -type f -exec chmod a-s {} +

COPY --from=builder --chown=0:0 --chmod=0555 /src/build/komodo-telegram-alerter /usr/local/bin/komodo-telegram-alerter

WORKDIR /

USER 10001:10001

EXPOSE 3000

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -q -O /dev/null "http://127.0.0.1:${PORT:-3000}/health" || exit 1

ENTRYPOINT ["/usr/local/bin/komodo-telegram-alerter"]
