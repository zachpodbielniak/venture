# Containerfile - VENTURE
#
# Copyright (C) 2026 Zach Podbielniak
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Two stages: a builder that compiles VENTURE and its six vendored
# dependencies from source, and a runtime that carries only what the server
# needs to run. The builder's toolchain never reaches the final image.
#
#   podman build -t quay.io/zachpodbielniak/venture:latest .
#
# The dependencies are git submodules. An unpopulated deps/ produces a
# confusing failure a long way into the build, so the builder checks for it
# first and says what to run.
#
#   git submodule update --init --recursive
#
# Build arguments:
#
#   BUILD_TYPE=release   release (default) or debug
#   WITH_CRISPY=0        1 adds a compiler to the runtime image so crispy
#                        .c plugins work; see the note further down
#   FEDORA_VERSION=44

ARG FEDORA_VERSION=44

# ===========================================================================
# Builder
# ===========================================================================

FROM registry.fedoraproject.org/fedora:${FEDORA_VERSION} AS builder

#
# The same package list config.mk prints as FEDORA_DEPS, plus git (some
# dependency build systems stamp a version from it) and the tools to run
# the test suite.
#
# One dnf layer, then the cache is dropped: an intermediate layer that
# carries 200MB of metadata is 200MB the builder has to write and read even
# though nothing ever uses it again.
RUN dnf install -y --setopt=install_weak_deps=False \
        gcc \
        nodejs \
        make \
        git \
        pkgconf-pkg-config \
        glib2-devel \
        libyaml-devel \
        json-glib-devel \
        libsoup3-devel \
        libxml2-devel \
        sqlite-devel \
        libpq-devel \
        readline-devel \
        openssl-devel \
        libjose-devel \
        jansson-devel \
        gobject-introspection-devel \
        poppler-glib-devel \
        gdk-pixbuf2-devel \
        libetpan-devel \
        libgudev-devel \
        libarchive-devel \
        gmime30-devel \
        gnutls-devel \
        jq \
        gnupg2 \
    && dnf clean all \
    && rm -rf /var/cache/dnf

#
# libarchive-devel is the knowledge-base import and export: zip and tar.gz
# both ways, and .docx, which is a zip with XML inside. Like poppler it is
# optional at build time -- config.mk probes for it -- which is exactly why
# it has to be named here. Leaving it out does not fail the build; it ships
# an image whose export route answers 501, and the tests that would have
# caught it need an embedding service the builder does not have.
#
# libetpan-devel and libgudev-devel are podomation's, not VENTURE's: its
# mail and device modules need them. They are here rather than in
# config.mk's FEDORA_DEPS because podomation resolves its whole dependency
# set in one pkg-config call, so a single missing package empties the flags
# entirely and the build fails on a missing glib.h -- which points nowhere
# near the actual cause.

WORKDIR /build

#
# The dependencies are copied and built before the application source, so
# editing src/ does not invalidate the layer that rebuilds six libraries.
# That turns an iteration from minutes into seconds.
#
COPY deps/ deps/
COPY config.mk rules.mk Makefile ./

ARG BUILD_TYPE=release
ENV DEBUG=0

#
# The prefix is fixed here and used for BOTH the build and the install.
# Several paths -- the plugin directory, the pod-module directory, the data
# directory, the include directory the crispy host compiles against -- are
# baked into the binary at compile time from PREFIX. Building with the
# default and installing with another produces a binary that looks for its
# plugins in a directory nothing was installed to.
#
ENV PREFIX=/usr

RUN if [ ! -f deps/yaml-glib/Makefile ] || [ ! -f deps/oidc-glib/Makefile ]; then \
        echo "deps/ is empty -- run: git submodule update --init --recursive" >&2; \
        exit 1; \
    fi

RUN if [ "${BUILD_TYPE}" = "debug" ]; then export DEBUG=1; fi; \
    make DEBUG=${DEBUG} PREFIX=${PREFIX} deps

COPY src/ src/
COPY data/ data/
COPY plugins/ plugins/
COPY modules/ modules/
COPY tests/ tests/
COPY venture.pc.in ./
COPY docs/ docs/
COPY migrations/ migrations/
COPY README.org ./

RUN if [ "${BUILD_TYPE}" = "debug" ]; then export DEBUG=1; fi; \
    make DEBUG=${DEBUG} PREFIX=${PREFIX} all plugins

#
# The suite runs against an in-memory SQLite database and needs no services,
# so it is cheap to run here -- and an image that ships a binary whose tests
# were never run is an image that ships an untested binary.
#

#
# tools/ is the test suite's, not the build's: `make test` runs
# venture-test-litter.sh from here to prove a green run left no temp
# directories behind. Without it that line fails on a missing file and takes
# the whole build with it -- `|| exit 1` cannot tell a missing script from a
# dirty machine -- so RUN_TESTS=1 could never pass in a container, however
# green the suite was.
#
# Copied after `make all` so that editing a tool does not rebuild the world.
#
COPY tools/ tools/

ARG RUN_TESTS=1
RUN if [ "${RUN_TESTS}" = "1" ]; then \
        if [ "${BUILD_TYPE}" = "debug" ]; then export DEBUG=1; fi; \
        make DEBUG=${DEBUG} PREFIX=${PREFIX} test; \
    fi

# Staged into a directory tree so the runtime stage copies one thing.
RUN if [ "${BUILD_TYPE}" = "debug" ]; then export DEBUG=1; fi; \
    make DEBUG=${DEBUG} PREFIX=${PREFIX} install DESTDIR=/staging


# ===========================================================================
# Runtime
# ===========================================================================

FROM registry.fedoraproject.org/fedora:${FEDORA_VERSION} AS runtime

ARG FEDORA_VERSION=44

LABEL org.opencontainers.image.title="VENTURE" \
      org.opencontainers.image.description="ERP and CRM for a portfolio of ventures" \
      org.opencontainers.image.source="https://gitlab.com/zachpodbielniak/venture" \
      org.opencontainers.image.licenses="AGPL-3.0-or-later" \
      org.opencontainers.image.base.name="registry.fedoraproject.org/fedora:${FEDORA_VERSION}"

#
# Shared libraries only. VENTURE's own six dependencies are linked
# statically, so nothing of them appears here.
#
# libpq is present even though the default is SQLite: the same image has to
# serve the deployment compose file, and a PostgreSQL deployment failing on
# a missing shared library at startup is a bad way to find out.
#
# libetpan and libgudev are podomation's: every one of its pod modules links
# the whole dependency set (it resolves them in a single pkg-config call),
# so without these the automation modules install fine and then fail to
# dlopen -- and a rule using cron_event reports an unknown module rather
# than a missing library.
#
#
# git and openssh-clients are here for the forge integration rather than for
# the build: a coding run clones a repository into a scratch checkout and
# pushes a branch back. Creating a branch does not need them -- that goes
# through the forge's API -- so an install that only links tickets to
# repositories would work without them. They are cheap enough not to make
# that a build option.
#
RUN dnf install -y --setopt=install_weak_deps=False \
        glib2 \
        libyaml \
        json-glib \
        libsoup3 \
        libxml2 \
        sqlite-libs \
        libpq \
        poppler-glib \
        gdk-pixbuf2 \
        readline \
        libetpan \
        libgudev \
        libarchive \
        libjose \
        jansson \
        gmime30 \
        gnutls \
        ca-certificates \
        tzdata \
        git \
        openssh-clients \
    && dnf clean all \
    && rm -rf /var/cache/dnf

#
# Optional: a compiler in the runtime image, so crispy .c plugins can be
# compiled on demand.
#
# Off by default. It roughly doubles the image, and it means the container
# can compile and execute code from its plugin directory -- which is the
# feature, and also the reason to think about it before turning it on. A
# mounted plugin directory becomes a code-execution path.
#
# Without it, set plugins.allow_crispy to false, or a .c file in a plugin
# directory is a warning at every startup. Native .so plugins, YAML venture
# types and podomation rules all work either way.
#
ARG WITH_CRISPY=0
RUN if [ "${WITH_CRISPY}" = "1" ]; then \
        dnf install -y --setopt=install_weak_deps=False \
            gcc \
            pkgconf-pkg-config \
            glib2-devel \
            libyaml-devel \
            json-glib-devel \
            libsoup3-devel \
            libxml2-devel \
        && dnf clean all \
        && rm -rf /var/cache/dnf; \
    fi

#
# Optional: a coding-agent CLI in the runtime image, for forge rules whose
# runner is `cli`.
#
# Off by default, and for the same reason WITH_CRISPY is: it means the
# container can execute code it fetched, which is the feature and also the
# thing to think about before turning it on. The in-process `agent` runner
# needs none of this -- it edits through tools that cannot leave the
# checkout -- but it has no shell and so cannot run a project's tests. That
# is the trade.
#
# The alternative is to leave this off and bind-mount the CLI you already
# have; see docs/containers.org.
#
# grok-build is deliberately absent. It was listed here as
# `@vercel/grok-build`, which does not exist on the npm registry and made
# WITH_AGENT_CLI=1 fail the build outright with a 404. The only npm package
# publishing a `grok` binary is `grok-cli`, an unrelated third party's
# wrapper that proxies claude-code through the xAI API -- not xAI's own CLI,
# and not something to install into an image that is allowed to execute what
# it fetches. Bind-mount the real binary instead, as docs/containers.org
# describes, and name it in forge.cli_allowed_commands.
#
ARG WITH_AGENT_CLI=0
RUN if [ "${WITH_AGENT_CLI}" = "1" ]; then \
        dnf install -y --setopt=install_weak_deps=False nodejs npm \
        && npm install -g @anthropic-ai/claude-code opencode-ai \
        && dnf clean all \
        && rm -rf /var/cache/dnf /root/.npm; \
    fi

COPY --from=builder /staging/ /

#
# A fixed uid, so a bind-mounted host directory can be chowned to something
# predictable. 1000 collides with the first human user on most hosts, which
# is convenient for a rootless bind mount and wrong for anything shared, so
# this uses a high one and the compose files show the chown.
#
# Not --system: that reserves uids below 1000, and this one is deliberately
# above it so a rootless bind mount can be chowned to a predictable number.
RUN groupadd --gid 8747 venture \
    && useradd --uid 8747 --gid venture --no-create-home \
               --home-dir /var/lib/venture --shell /sbin/nologin venture \
    && mkdir -p /var/lib/venture /etc/venture \
                /usr/lib64/venture/plugins \
                /usr/share/venture/venture-types \
    && chown -R venture:venture /var/lib/venture

#
# Everything mutable lives in one directory: the SQLite database, the
# automation engine's persisted state, the crispy cache, the AI
# conversation log. One volume, one thing to back up.
#
VOLUME ["/var/lib/venture"]

ENV VENTURE_STATE_DIR=/var/lib/venture \
    VENTURE_DATABASE_URI=sqlite:///var/lib/venture/venture.db \
    VENTURE_SERVER_BIND_ADDRESS=0.0.0.0 \
    VENTURE_SERVER_PORT=8747

#
# Binding 0.0.0.0 inside a container is not the same claim as binding it on
# a host: the container's network namespace is the boundary, and what is
# published is the compose file's business. Authentication stays on --
# venture refuses this combination with auth off, which is the check that
# matters.
#
EXPOSE 8747

USER venture
WORKDIR /var/lib/venture

#
# venturectl talks to the API the same way any other client does, so a
# healthy answer here means the whole request path works, not merely that
# the process is alive. /api/v1/health needs no authentication, which is
# what lets this run without a token baked into the image.
#
# Podman builds OCI images by default, and the OCI spec has no healthcheck
# field -- so this is ignored unless you build with `--format docker`. Both
# compose files declare the same check themselves, which is what actually
# runs them; this is here for `podman run` without compose.
#
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD venturectl --server http://127.0.0.1:8747 health || exit 1

ENTRYPOINT ["/usr/bin/venture"]
