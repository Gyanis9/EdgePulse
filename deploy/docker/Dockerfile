# 多阶段构建：构建期把引擎源码打成 Conan 包，运行期只带二进制、前端产物与迁移。
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build python3 python3-pip python3-venv git curl ca-certificates \
        nodejs npm zlib1g-dev libssl-dev libsqlite3-dev libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/conan && /opt/conan/bin/pip install --no-cache-dir "conan>=2"
ENV PATH=/opt/conan/bin:$PATH
RUN conan profile detect --force

WORKDIR /src
COPY . /src

RUN conan create third_party/AsynGyanis/packaging/conan --build=missing \
        -s compiler.cppstd=23 -s build_type=Release -o "&:with_mysql=True"

RUN conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
RUN cmake --preset release
RUN cmake --build --preset release --parallel

WORKDIR /src/frontend
RUN npm ci && npm run build

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates zlib1g libzstd1 libbrotli1 liblz4-1 libssl3 libsqlite3-0 libmysqlclient21 libhiredis1.1.0 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --system --home /var/lib/edgepulse --shell /usr/sbin/nologin edgepulse \
    && mkdir -p /app /var/lib/edgepulse/firmware /etc/edgepulse \
    && chown -R edgepulse:edgepulse /var/lib/edgepulse

WORKDIR /app
COPY --from=build /src/build/release/edgepulse-server /app/edgepulse-server
COPY --from=build /src/frontend/dist /app/web
COPY --from=build /src/migrations /app/migrations
COPY --from=build /src/config /app/config
COPY --from=build /src/deploy/docker/entrypoint.sh /app/entrypoint.sh
RUN chmod 755 /app/edgepulse-server /app/entrypoint.sh

USER edgepulse
EXPOSE 8080 8443 9443 9100
ENTRYPOINT ["/app/entrypoint.sh"]
