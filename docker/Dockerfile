# syntax=docker/dockerfile:1

# ############################################################################
# builder: compile nginx with ngx_http_tile_module and all optional stores
# ############################################################################
ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION} AS builder

ARG NGINX_VERSION=1.30.7
ARG APT_MIRROR=mirrors.cloud.tencent.com
# enable every optional storage backend that has dev libs installed below
ENV NGX_TILE_WITH_MEMCACHED=yes \
    NGX_TILE_WITH_RADOS=yes \
    NGX_TILE_WITH_RO_HTTP_PROXY=yes \
    NGX_TILE_WITH_COMPOSITE=yes

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

RUN sed -i "s|https\?://archive.ubuntu.com/ubuntu/|http://${APT_MIRROR}/ubuntu/|g; \
            s|https\?://security.ubuntu.com/ubuntu/|http://${APT_MIRROR}/ubuntu/|g" \
        /etc/apt/sources.list.d/ubuntu.sources \
    && apt-get update && apt-get install -y --no-install-recommends \
        build-essential wget \
        libpcre2-dev zlib1g-dev libssl-dev \
        libcurl4-openssl-dev libmemcached-dev librados-dev libcairo2-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
# prefer the cached tarball from the repository if present (offline builds),
# otherwise download from nginx.org
COPY build/nginx-1.30.7.tar.gz /tmp/nginx-cached.tar.gz
RUN if [ -s /tmp/nginx-cached.tar.gz ]; then \
        cp /tmp/nginx-cached.tar.gz nginx-${NGINX_VERSION}.tar.gz; \
    else \
        wget -q https://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz; \
    fi \
    && tar xzf nginx-${NGINX_VERSION}.tar.gz

COPY LICENSE /src/LICENSE
COPY config /src/config
COPY src /src/src
COPY vendor /src/vendor

WORKDIR /build/nginx-${NGINX_VERSION}
RUN ./configure \
        --prefix=/usr/local/nginx \
        --with-compat --with-threads \
        --with-http_ssl_module --with-http_v2_module \
        --add-dynamic-module=/src \
    && make -j"$(nproc)" && make install \
    && ls -l /usr/local/nginx/modules/

# ############################################################################
# runtime: only the shared libraries the binary and module need
# ############################################################################
FROM ubuntu:${UBUNTU_VERSION}

RUN apt-get update && apt-get install -y --no-install-recommends \
        libpcre2-8-0 zlib1g libssl3t64 \
        libcurl4 libmemcached11 librados2 libcairo2 \
        python3 curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/nginx /usr/local/nginx
COPY docker/nginx.conf /etc/nginx/nginx.conf
COPY docker/nginx-osm.conf /etc/nginx/nginx-osm.conf
COPY docker/index.html /usr/local/nginx/html/index.html
COPY docker/demo_renderd.py /opt/demo/demo_renderd.py
COPY docker/docker-entrypoint.sh /entrypoint.sh
COPY tests/gen_metatiles.py /opt/demo/gen_metatiles.py

# vendored Leaflet (tarball downloaded on the host by prepare-renderd.sh)
# so the map page works fully offline
COPY docker/leaflet.tgz /tmp/leaflet.tgz
RUN mkdir -p /usr/local/nginx/html/leaflet /tmp/leaflet \
    && tar xzf /tmp/leaflet.tgz -C /tmp/leaflet \
    && cp /tmp/leaflet/package/dist/leaflet.js \
          /tmp/leaflet/package/dist/leaflet.css \
          /usr/local/nginx/html/leaflet/ \
    && cp -r /tmp/leaflet/package/dist/images \
          /usr/local/nginx/html/leaflet/images \
    && rm -rf /tmp/leaflet

ENV TILE_DIR=/data/tiles \
    DEMO_RENDERD=internal \
    NGINX_CONF=/etc/nginx/nginx.conf

RUN mkdir -p /data/tiles /var/log/nginx /var/cache/nginx \
    && chmod +x /entrypoint.sh /usr/local/nginx/sbin/nginx

EXPOSE 8080 7654

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s \
    CMD curl -sf http://localhost:8080/healthz || exit 1

ENTRYPOINT ["/entrypoint.sh"]
