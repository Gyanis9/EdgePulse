# 多阶段构建：引擎与后端在 Ubuntu 上编译，前端用 Node 22 构建，运行期只带二进制、前端产物与迁移。
ARG APT_MIRROR=archive.ubuntu.com/ubuntu
ARG NODE_IMAGE=node:22-bookworm

FROM ubuntu:24.04 AS build

ARG APT_MIRROR
ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i "s|http://archive.ubuntu.com/ubuntu|http://${APT_MIRROR}|g; s|http://security.ubuntu.com/ubuntu|http://${APT_MIRROR}|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-14 cmake ninja-build python3 python3-pip python3-venv git curl ca-certificates \
        zlib1g-dev libssl-dev libsqlite3-dev libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/conan && /opt/conan/bin/pip install --no-cache-dir "conan>=2"
ENV PATH=/opt/conan/bin:$PATH
RUN conan profile detect --force

WORKDIR /src
COPY . /src

# 引擎包始终用系统 GCC 13 构建：GCC 14 在协程析构上报 maybe-uninitialized 误报，
# 而引擎以 -Werror 编译。EdgePulse 自身代码与最终链接仍使用 GCC 14。
RUN CC=gcc CXX=g++ conan create 3rd/AsynGyanis/packaging/conan --build=missing \
        -s compiler.cppstd=23 -s build_type=Release -o "&:with_mysql=True"

RUN conan install . --build=missing -s compiler.cppstd=23 -s build_type=Release
RUN CC=gcc-14 CXX=g++-14 cmake --preset release -DEDGEPULSE_BUILD_TESTS=OFF
RUN cmake --build --preset release --target edgepulse-server --parallel

ARG NODE_IMAGE
FROM ${NODE_IMAGE} AS web

WORKDIR /src/frontend
COPY frontend/package.json frontend/package-lock.json ./
RUN npm ci
COPY frontend/ ./
RUN npm run build

FROM ubuntu:24.04 AS runtime

ARG APT_MIRROR
ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i "s|http://archive.ubuntu.com/ubuntu|http://${APT_MIRROR}|g; s|http://security.ubuntu.com/ubuntu|http://${APT_MIRROR}|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl zlib1g libzstd1 libbrotli1 liblz4-1 libssl3t64 libsqlite3-0 libmysqlclient21 libhiredis1.1.0 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --system --home /var/lib/edgepulse --shell /usr/sbin/nologin edgepulse \
    && mkdir -p /app /var/lib/edgepulse/firmware /etc/edgepulse \
    && chown -R edgepulse:edgepulse /var/lib/edgepulse

WORKDIR /app
COPY --from=build /src/build/release/edgepulse-server /app/edgepulse-server
COPY --from=web /src/frontend/dist /app/web
COPY --from=build /src/migrations /app/migrations
COPY --from=build /src/config /app/config
COPY --from=build /src/deploy/docker/entrypoint.sh /app/entrypoint.sh
RUN chmod 755 /app/edgepulse-server /app/entrypoint.sh \
    && mkdir -p /app/var/logs \
    && chown -R edgepulse:edgepulse /app/var

USER edgepulse
EXPOSE 8080 8443 9443 9100
ENTRYPOINT ["/app/entrypoint.sh"]
