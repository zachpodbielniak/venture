# config.mk - VENTURE Configuration
#
# VENTURE - an ERP/CRM hybrid for running multiple business ventures.
#
# All configurable build options live here. Override any variable on the
# command line:
#
#   make DEBUG=1
#   make PREFIX=/usr/local
#   make POSTGRES=0
#   make venturectl              # CLI only, no server deps
#
# The build produces two binaries:
#
#   venture     - the server: full web API + HTMX web UI, AI, automations
#   venturectl  - the CLI: drives the server over its REST API
#
# They are deliberately built from two different static archives so that
# either can be built on its own:
#
#   libventure-core.a  types, boxed types, interfaces, config, model,
#                      utilities. No database, no server, no AI. This is
#                      all venturectl needs.
#   libventure.a       core + database + reporting + web + AI + automation
#                      + plugin host. This is what the server needs.
#
# Every vendored dependency under deps/ is linked STATICALLY, so neither
# binary has a runtime dependency on libyaml-glib.so, libhtmx-glib.so,
# libai-glib.so, liborm-glib.so, libpodomation.so or libcrispy.so. Only
# true system libraries (glib, libsoup, json-glib, libyaml, sqlite3,
# libpq, libxml2, readline) stay dynamic.

# ---------------------------------------------------------------------------
# Version
# ---------------------------------------------------------------------------

VERSION_MAJOR := 0
VERSION_MINOR := 6
VERSION_MICRO := 0
VERSION := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_MICRO)

# API version used for the pkg-config name and installed header directory.
API_VERSION := 0.1

# ---------------------------------------------------------------------------
# Installation directories
# ---------------------------------------------------------------------------

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

# Auto-detect lib vs lib64 (Fedora, RHEL, SUSE, ...).
# Override with: make LIBDIR=/usr/local/lib
LIBDIR_SUFFIX := $(shell if [ -d /usr/lib64 ]; then echo lib64; else echo lib; fi)
LIBDIR ?= $(PREFIX)/$(LIBDIR_SUFFIX)
INCLUDEDIR ?= $(PREFIX)/include
DATADIR ?= $(PREFIX)/share
MANDIR ?= $(DATADIR)/man
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
GIRDIR ?= $(DATADIR)/gir-1.0
TYPELIBDIR ?= $(LIBDIR)/girepository-1.0
SYSCONFDIR ?= /etc

# Where the server looks for things at runtime.
PLUGINDIR ?= $(LIBDIR)/venture/plugins
PODMODULEDIR ?= $(LIBDIR)/venture/pod-modules
VENTUREDATADIR ?= $(DATADIR)/venture

# Vendored dependency headers, installed so that crispy plugins can compile
# on a machine with no source tree. VENTURE's public headers include
# <orm.h> and <htmx-glib.h>, so a plugin that includes <venture/venture.h>
# needs them present. Each dependency keeps its own subdirectory: several of
# them have a src/core/ and a src/types/, and a flat copy would have them
# silently overwriting each other.
VENTUREDEPSINCLUDEDIR ?= $(INCLUDEDIR)/venture-deps

# ---------------------------------------------------------------------------
# Build directories
# ---------------------------------------------------------------------------

BUILDDIR := build
TOOLSDIR := tools
OBJDIR_DEBUG := $(BUILDDIR)/debug/obj
OBJDIR_RELEASE := $(BUILDDIR)/release/obj
OUTDIR_DEBUG := $(BUILDDIR)/debug
OUTDIR_RELEASE := $(BUILDDIR)/release

# ---------------------------------------------------------------------------
# Build options (0 or 1)
# ---------------------------------------------------------------------------

DEBUG ?= 0
ASAN ?= 0
UBSAN ?= 0
BUILD_GIR ?= 0
BUILD_TESTS ?= 1
BUILD_PLUGINS ?= 1
BUILD_POD_MODULES ?= 1

# Database backends. SQLite is always on (it is the zero-setup default and
# what the test suite runs against). PostgreSQL is on whenever libpq is
# present; set POSTGRES=0 to force it off.
SQLITE ?= 1
POSTGRES ?= 1

# Verbose build output: make V=1
V ?= 0
ifeq ($(V),1)
    Q :=
else
    Q := @
endif

# Select build directories based on DEBUG.
ifeq ($(DEBUG),1)
    OBJDIR := $(OBJDIR_DEBUG)
    OUTDIR := $(OUTDIR_DEBUG)
    BUILD_TYPE := debug
else
    OBJDIR := $(OBJDIR_RELEASE)
    OUTDIR := $(OUTDIR_RELEASE)
    BUILD_TYPE := release
endif

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

CC := gcc
AR := ar
PKG_CONFIG ?= pkg-config
GIR_SCANNER ?= g-ir-scanner
GIR_COMPILER ?= g-ir-compiler
INSTALL := install
INSTALL_PROGRAM := $(INSTALL) -m 755
INSTALL_DATA := $(INSTALL) -m 644
MKDIR_P := mkdir -p

# ---------------------------------------------------------------------------
# C standard and dialect
# ---------------------------------------------------------------------------

CSTD := -std=gnu89

# Base compiler flags. -Werror is deliberate: a warning is a latent bug and
# every target in this tree is expected to build clean.
CFLAGS_BASE := $(CSTD) -Wall -Wextra -Werror
CFLAGS_BASE += -Wno-unused-parameter
CFLAGS_BASE += -Wformat=2 -Wshadow -Wpointer-arith -Wcast-align
CFLAGS_BASE += -Wwrite-strings -Wmissing-declarations -Wredundant-decls
CFLAGS_BASE += -fPIC
CFLAGS_BASE += -DVENTURE_COMPILATION
CFLAGS_BASE += -DG_LOG_DOMAIN=\"Venture\"
CFLAGS_BASE += -DVENTURE_VERSION=\"$(VERSION)\"
CFLAGS_BASE += -DVENTURE_VERSION_MAJOR=$(VERSION_MAJOR)
CFLAGS_BASE += -DVENTURE_VERSION_MINOR=$(VERSION_MINOR)
CFLAGS_BASE += -DVENTURE_VERSION_MICRO=$(VERSION_MICRO)
CFLAGS_BASE += -DVENTURE_PLUGINDIR=\"$(PLUGINDIR)\"
CFLAGS_BASE += -DVENTURE_PODMODULEDIR=\"$(PODMODULEDIR)\"
CFLAGS_BASE += -DVENTURE_SYSCONFDIR=\"$(SYSCONFDIR)\"
CFLAGS_BASE += -DVENTURE_DATADIR=\"$(VENTUREDATADIR)\"
CFLAGS_BASE += -DVENTURE_DEV_INCLUDE_DIR=\"$(CURDIR)/$(BUILDDIR)/include\"
# Where install-headers puts the public headers. The crispy host compiles
# plugins against these on an installed system, so it has to know the real
# location rather than guessing one from DATADIR.
CFLAGS_BASE += -DVENTURE_INCLUDE_DIR=\"$(INCLUDEDIR)\"
CFLAGS_BASE += -DVENTURE_DEPS_INCLUDE_DIR=\"$(VENTUREDEPSINCLUDEDIR)\"

# Debug/release flags.
ifeq ($(DEBUG),1)
    CFLAGS_BUILD := -g3 -O0 -DVENTURE_DEBUG -DDEBUG
else
    CFLAGS_BUILD := -O2 -DNDEBUG
endif

ifeq ($(ASAN),1)
    CFLAGS_SAN += -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS_SAN += -fsanitize=address
endif
ifeq ($(UBSAN),1)
    CFLAGS_SAN += -fsanitize=undefined
    LDFLAGS_SAN += -fsanitize=undefined
endif

# ---------------------------------------------------------------------------
# System dependencies
# ---------------------------------------------------------------------------

# Needed by every target, including a standalone venturectl build.
DEPS_CORE := glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0
DEPS_CORE += json-glib-1.0 yaml-0.1

# Needed by venturectl (it speaks HTTP to the server).
DEPS_CLI := $(DEPS_CORE) libsoup-3.0

# Needed by the server on top of the CLI set. libxml-2.0 comes in via
# ai-glib, which parses XML responses from some providers.
DEPS_SERVER := $(DEPS_CLI) libxml-2.0 openssl jose jansson

ifeq ($(SQLITE),1)
    DEPS_SERVER += sqlite3
    CFLAGS_BASE += -DVENTURE_HAVE_SQLITE=1
endif

# PostgreSQL is opt-out but only enabled if libpq is actually installed.
ifeq ($(POSTGRES),1)
    POSTGRES_AVAILABLE := $(shell $(PKG_CONFIG) --exists libpq 2>/dev/null && echo 1 || echo 0)
else
    POSTGRES_AVAILABLE := 0
endif
ifeq ($(POSTGRES_AVAILABLE),1)
    DEPS_SERVER += libpq
    CFLAGS_BASE += -DVENTURE_HAVE_POSTGRES=1
endif

# PDF text extraction for chat attachments. Optional the same way libpq is:
# without it the server builds and runs, and an attached PDF is stored but
# reports that its text could not be extracted.
POPPLER_AVAILABLE := $(shell $(PKG_CONFIG) --exists poppler-glib 2>/dev/null && echo 1 || echo 0)
ifeq ($(POPPLER_AVAILABLE),1)
    DEPS_SERVER += poppler-glib
    CFLAGS_BASE += -DVENTURE_HAVE_POPPLER=1
endif

# Optional bounded image decoding for local OCR. Ordinary capture needs neither.
OCR_RASTER_AVAILABLE := $(shell $(PKG_CONFIG) --exists gdk-pixbuf-2.0 2>/dev/null && echo 1 || echo 0)
ifeq ($(OCR_RASTER_AVAILABLE),1)
    DEPS_SERVER += gdk-pixbuf-2.0
    CFLAGS_BASE += -DVENTURE_HAVE_OCR_RASTER=1
endif

# Archive reading and writing, for knowledge-base import and export.
# Optional the same way poppler is: without it the server builds and runs,
# a .zip import reports that archives are unavailable, and an export offers
# only the plain-file form. One library covers all of it -- zip and tar.gz
# both ways, and .docx, which is a zip with XML inside.
LIBARCHIVE_AVAILABLE := $(shell $(PKG_CONFIG) --exists libarchive 2>/dev/null && echo 1 || echo 0)
ifeq ($(LIBARCHIVE_AVAILABLE),1)
    DEPS_SERVER += libarchive
    CFLAGS_BASE += -DVENTURE_HAVE_LIBARCHIVE=1
endif

# crispy links against readline for its REPL; the objects we pull in for
# plugin compilation do not need it, but the archive references it, so it
# must be on the final link line. -lm is for the floating-point helpers used
# by metric formatting and report ratios.
LIBS_EXTRA := -lreadline -lm

# Fail early with a readable message rather than a wall of pkg-config noise.
define check_dep
$(if $(shell $(PKG_CONFIG) --exists $(1) && echo yes),,$(error Missing dependency: $(1). Run `make install-deps`.))
endef

CFLAGS_DEPS_CORE := $(shell $(PKG_CONFIG) --cflags $(DEPS_CORE) 2>/dev/null)
LDFLAGS_DEPS_CORE := $(shell $(PKG_CONFIG) --libs $(DEPS_CORE) 2>/dev/null)
CFLAGS_DEPS_CLI := $(shell $(PKG_CONFIG) --cflags $(DEPS_CLI) 2>/dev/null)
LDFLAGS_DEPS_CLI := $(shell $(PKG_CONFIG) --libs $(DEPS_CLI) 2>/dev/null)
CFLAGS_DEPS_SERVER := $(shell $(PKG_CONFIG) --cflags $(DEPS_SERVER) 2>/dev/null)
LDFLAGS_DEPS_SERVER := $(shell $(PKG_CONFIG) --libs $(DEPS_SERVER) 2>/dev/null)

# ---------------------------------------------------------------------------
# Vendored dependencies (deps/, all statically linked)
# ---------------------------------------------------------------------------
#
# These are git submodules and are treated as the CANONICAL copies. Several
# of them vendor each other (podomation bundles yaml-glib/crispy/ai-glib;
# ai-glib bundles yaml-glib). We override their sub-dependency paths to
# point back at OUR deps/ tree so exactly one copy of each library is ever
# compiled and linked, avoiding duplicate GType registrations.

DEPS_DIR := $(CURDIR)/deps

YAML_GLIB_DIR := $(DEPS_DIR)/yaml-glib
HTMX_GLIB_DIR := $(DEPS_DIR)/htmx-glib
AI_GLIB_DIR := $(DEPS_DIR)/ai-glib
CRISPY_DIR := $(DEPS_DIR)/crispy
PODOMATION_DIR := $(DEPS_DIR)/podomation
ORM_GLIB_DIR := $(DEPS_DIR)/orm-glib

# Every dep splits its output by build type. orm-glib did not until
# v0.2.0; the flat build/liborm-glib-0.1.a it used to produce is why a dep
# bump could silently link a stale archive -- see clean-deps in rules.mk.
YAML_GLIB_LIB := $(YAML_GLIB_DIR)/build/$(BUILD_TYPE)/libyaml-glib-1.0.a
HTMX_GLIB_LIB := $(HTMX_GLIB_DIR)/build/$(BUILD_TYPE)/libhtmx-glib-1.0.a
AI_GLIB_LIB := $(AI_GLIB_DIR)/build/$(BUILD_TYPE)/libai-glib-1.0.a
CRISPY_LIB := $(CRISPY_DIR)/build/$(BUILD_TYPE)/libcrispy.a
PODOMATION_LIB := $(PODOMATION_DIR)/build/$(BUILD_TYPE)/libpodomation-1.0.a
ORM_GLIB_LIB := $(ORM_GLIB_DIR)/build/$(BUILD_TYPE)/liborm-glib-0.2.a

# Include paths for the vendored headers. Each library gates its internal
# headers behind an "only the umbrella header may be included" guard, so we
# always include via <yaml-glib.h>, <htmx-glib.h>, <ai-glib.h>, <podomation.h>,
# <crispy.h> and <orm.h>.
# Several dependencies generate a version or config header into their own
# build directory rather than into src/, so that directory has to be on the
# include path alongside the sources.
CFLAGS_YAML_GLIB := -I$(YAML_GLIB_DIR)/src
CFLAGS_HTMX_GLIB := -I$(HTMX_GLIB_DIR)/src -I$(HTMX_GLIB_DIR)/build/$(BUILD_TYPE)
CFLAGS_AI_GLIB := -I$(AI_GLIB_DIR)/src -I$(AI_GLIB_DIR)/build/$(BUILD_TYPE)
CFLAGS_CRISPY := -I$(CRISPY_DIR)/src -I$(CRISPY_DIR)/build/$(BUILD_TYPE)
CFLAGS_PODOMATION := -I$(PODOMATION_DIR)/src -I$(PODOMATION_DIR)/build/$(BUILD_TYPE)
CFLAGS_ORM_GLIB := -I$(ORM_GLIB_DIR)/src

#
# The flags a crispy plugin has to be compiled with, baked into the binary
# so the host does not have to reconstruct them.
#
# A crispy plugin sees the same public headers a native plugin does, so it
# needs the same include path and the same feature defines -- most
# importantly VENTURE_SERVER_BUILD, without which the umbrella header hides
# the reporting engine, the database and everything else a plugin exists to
# reach. The public headers also include <orm.h> directly.
#
# This is a define rather than a duplicate list in the C so that there is
# one source of truth: adding a dependency here fixes both plugin kinds.
CFLAGS_PLUGIN_DEPS := -DVENTURE_SERVER_BUILD=1 -DVENTURE_HAVE_CRISPY=1 \
                      $(CFLAGS_YAML_GLIB) $(CFLAGS_HTMX_GLIB) \
                      $(CFLAGS_AI_GLIB) $(CFLAGS_CRISPY) \
                      $(CFLAGS_PODOMATION) $(CFLAGS_ORM_GLIB)

CFLAGS_BASE += -DVENTURE_CRISPY_PLUGIN_CFLAGS="\"$(CFLAGS_PLUGIN_DEPS)\""

# Sub-make flags for each dependency.
#
# Note what is deliberately NOT done here: the dependencies' own bundled
# sub-dependencies are left alone. podomation and ai-glib each vendor a copy
# of yaml-glib (and podomation vendors crispy too), but neither merges those
# objects into its own static archive -- `ar t` on libpodomation-1.0.a and
# libai-glib-1.0.a shows only their own translation units. So each archive
# carries unresolved yaml_*/crispy_* references that our single canonical
# copy satisfies at final link time, and exactly one instance of each library
# ends up in the binary regardless.
#
# Redirecting their sub-dependency paths would be belt and braces, but their
# bundled-dependency build rules expect archive names and targets that have
# since moved, so pointing them at our tree breaks their build for no gain.
# We build only each dependency's own archive and let the linker do the rest.
YAML_GLIB_SUBMAKE :=
HTMX_GLIB_SUBMAKE :=
AI_GLIB_SUBMAKE :=
CRISPY_SUBMAKE :=
ORM_GLIB_SUBMAKE := ENABLE_SQLITE=$(SQLITE) ENABLE_POSTGRES=$(POSTGRES_AVAILABLE)
#
# ---------------------------------------------------------------------------
# The agent skill
# ---------------------------------------------------------------------------
#
# skills/venturectl is the canonical copy and is agent-agnostic on purpose:
# three different coding agents read it, and none of them owns it. The
# .claude/skills/ entry in this repository is a symlink to it so Claude Code
# still discovers it when the project is open, without a second copy to keep
# in step.
#
# `make install-skill` links it into each agent's user directory. These are
# overridable so an agent that moves its directory, or a machine that puts
# HOME somewhere unusual, needs no patch:
#
#   make install-skill SKILL_DESTS="$HOME/.claude/skills"
#
SKILL_NAME := venturectl
SKILL_SRC := $(CURDIR)/skills/$(SKILL_NAME)

SKILL_DESTS ?= $(HOME)/.claude/skills \
               $(HOME)/.grok/skills \
               $(HOME)/.agents/skills

# Which of podomation's modules to build and install.
#
# Not all of them: the full set's build dependencies are the union of every
# module's -- libssh2, bluez, libvirt -- which an ERP has no business
# carrying. These are the general-purpose ones automation rules actually
# use, and they need nothing VENTURE does not already link.
#
# Adding one means adding its dependency to the Containerfile and to
# FEDORA_DEPS; check what it includes first.
VENTURE_POD_MODULES := cron timer log http webhook ntfy exec_pipe \
                       aggregate state_store engine

PODOMATION_SUBMAKE := MODULES="$(VENTURE_POD_MODULES)"

# Archive link order matters for static libraries: a library must appear
# AFTER everything that references it. venture -> podomation -> crispy,
# venture -> ai-glib -> yaml-glib, venture -> orm-glib.
VENDOR_LIBS_SERVER := $(HTMX_GLIB_LIB) \
                      $(PODOMATION_LIB) \
                      $(AI_GLIB_LIB) \
                      $(ORM_GLIB_LIB) \
                      $(CRISPY_LIB) \
                      $(YAML_GLIB_LIB)

VENDOR_LIBS_CLI := $(YAML_GLIB_LIB)

# ---------------------------------------------------------------------------
# Combined flags
# ---------------------------------------------------------------------------

CFLAGS_INC := -I$(CURDIR) -I$(CURDIR)/src -I$(OUTDIR)

# venturectl and libventure-core.a only see yaml-glib.
CFLAGS_CORE := $(CFLAGS_BASE) $(CFLAGS_BUILD) $(CFLAGS_SAN) $(CFLAGS_INC) \
               $(CFLAGS_YAML_GLIB) $(CFLAGS_DEPS_CLI)

# The server sees everything. VENTURE_SERVER_BUILD gates the parts of the
# shared sources that reach for a server-only dependency -- most notably the
# crispy-compiled C configuration path in src/config, which is simply absent
# from libventure-core.a.
CFLAGS = $(CFLAGS_BASE) $(CFLAGS_BUILD) $(CFLAGS_SAN) $(CFLAGS_INC) \
          -DVENTURE_SERVER_BUILD=1 -DVENTURE_HAVE_CRISPY=1 \
          $(CFLAGS_YAML_GLIB) $(CFLAGS_HTMX_GLIB) $(CFLAGS_AI_GLIB) \
          $(CFLAGS_CRISPY) $(CFLAGS_PODOMATION) $(CFLAGS_ORM_GLIB) \
          $(CFLAGS_DEPS_SERVER)

LDFLAGS_CLI := $(LDFLAGS_SAN) $(LDFLAGS_DEPS_CLI)
LDFLAGS = $(LDFLAGS_SAN) $(LDFLAGS_DEPS_SERVER) $(LIBS_EXTRA) -Wl,--export-dynamic

# ---------------------------------------------------------------------------
# Library names
# ---------------------------------------------------------------------------

LIB_CORE_NAME := venture-core
LIB_CORE_STATIC := lib$(LIB_CORE_NAME).a

LIB_NAME := venture
LIB_STATIC := lib$(LIB_NAME).a
LIB_SHARED := lib$(LIB_NAME).so
LIB_SHARED_FULL := lib$(LIB_NAME).so.$(VERSION)
LIB_SHARED_MAJOR := lib$(LIB_NAME).so.$(VERSION_MAJOR)
LDFLAGS_SHARED := -shared -Wl,-soname,$(LIB_SHARED_MAJOR)

# ---------------------------------------------------------------------------
# Plugin / podomation module build flags
# ---------------------------------------------------------------------------
#
# Plugins and podomation modules are runtime .so files. They are linked with
# NO copy of libventure: the server binary is linked --export-dynamic, so a
# loaded plugin resolves venture_*, pod_*, htmx_*, orm_* against the
# executable. That keeps exactly one instance of every GType in the process.

PLUGIN_CFLAGS := $(CFLAGS_BASE) $(CFLAGS_BUILD) $(CFLAGS_SAN) \
                 -DVENTURE_SERVER_BUILD=1 -DVENTURE_HAVE_CRISPY=1 \
                 -I$(CURDIR)/$(BUILDDIR)/include -I$(CURDIR) -I$(CURDIR)/src \
                 $(CFLAGS_YAML_GLIB) $(CFLAGS_HTMX_GLIB) $(CFLAGS_AI_GLIB) \
                 $(CFLAGS_CRISPY) $(CFLAGS_PODOMATION) $(CFLAGS_ORM_GLIB) \
                 $(CFLAGS_DEPS_SERVER)
PLUGIN_LDFLAGS := -shared -fPIC

# ---------------------------------------------------------------------------
# GIR settings
# ---------------------------------------------------------------------------

GIR_NAMESPACE := Venture
GIR_VERSION := $(API_VERSION)
GIR_FILE := $(GIR_NAMESPACE)-$(GIR_VERSION).gir
TYPELIB_FILE := $(GIR_NAMESPACE)-$(GIR_VERSION).typelib
ORM_GLIB_GIR := $(ORM_GLIB_DIR)/build/$(BUILD_TYPE)/Orm-0.2.gir
AI_GLIB_GIR := $(AI_GLIB_DIR)/build/$(BUILD_TYPE)/AiGlib-1.0.gir
HTMX_GLIB_GIR := $(HTMX_GLIB_DIR)/build/$(BUILD_TYPE)/HtmxGlib-1.0.gir
VENDOR_GIR_FILES := $(ORM_GLIB_GIR) $(AI_GLIB_GIR) $(HTMX_GLIB_GIR)
VENDOR_GIR_DIRS := $(sort $(dir $(VENDOR_GIR_FILES)))
# Imported GIR namespaces must share their runtime GTypes with Venture's DSO.
# Server/CLI/test targets continue to consume the canonical static archives.
ORM_GLIB_SHARED := $(ORM_GLIB_DIR)/build/$(BUILD_TYPE)/liborm-glib-0.2.so
AI_GLIB_SHARED := $(AI_GLIB_DIR)/build/$(BUILD_TYPE)/libai-glib-1.0.so
HTMX_GLIB_SHARED := $(HTMX_GLIB_DIR)/build/$(BUILD_TYPE)/libhtmx-glib-1.0.so
VENDOR_SHARED_LIBS := $(ORM_GLIB_SHARED) $(AI_GLIB_SHARED) $(HTMX_GLIB_SHARED)

# ---------------------------------------------------------------------------
# Test flags
# ---------------------------------------------------------------------------

# Fixtures are found through a define, never by guessing at the working
# directory: `make test` runs each binary from the tree root and `make
# test-one` may not.
TEST_CFLAGS = $(CFLAGS) -I$(CURDIR)/tests \
               -DVENTURE_TEST_FIXTURES=\"$(CURDIR)/tests/fixtures\" \
               -DVENTURE_TEST_EXAMPLES=\"$(CURDIR)/data/examples\"
TEST_LDFLAGS = $(LDFLAGS)

# ---------------------------------------------------------------------------
# show-config
# ---------------------------------------------------------------------------

.PHONY: show-config
show-config:
	@echo "VENTURE Build Configuration"
	@echo "==========================="
	@echo "Version:             $(VERSION)"
	@echo "Build type:          $(BUILD_TYPE)"
	@echo "Compiler:            $(CC)"
	@echo "PREFIX:              $(PREFIX)"
	@echo "LIBDIR:              $(LIBDIR)"
	@echo "PLUGINDIR:           $(PLUGINDIR)"
	@echo "PODMODULEDIR:        $(PODMODULEDIR)"
	@echo "DEBUG:               $(DEBUG)"
	@echo "ASAN:                $(ASAN)"
	@echo "UBSAN:               $(UBSAN)"
	@echo "SQLITE:              $(SQLITE)"
	@echo "POSTGRES:            $(POSTGRES)"
	@echo "POSTGRES_AVAILABLE:  $(POSTGRES_AVAILABLE)"
	@echo "BUILD_TESTS:         $(BUILD_TESTS)"
	@echo "BUILD_PLUGINS:       $(BUILD_PLUGINS)"
	@echo "BUILD_POD_MODULES:   $(BUILD_POD_MODULES)"
	@echo "BUILD_GIR:           $(BUILD_GIR)"
	@echo ""
	@echo "CFLAGS:              $(CFLAGS)"
	@echo "LDFLAGS:             $(LDFLAGS)"
	@echo "VENDOR_LIBS_SERVER:  $(VENDOR_LIBS_SERVER)"

# ---------------------------------------------------------------------------
# Distro auto-detection and dependency installation
# ---------------------------------------------------------------------------

DISTRO_ID := $(shell . /etc/os-release 2>/dev/null && echo $$ID)
DISTRO_ID_LIKE := $(shell . /etc/os-release 2>/dev/null && echo $$ID_LIKE)

ifeq ($(DISTRO),)
    ifneq ($(filter fedora,$(DISTRO_ID)),)
        DISTRO := fedora
    else ifneq ($(filter rhel centos,$(DISTRO_ID)),)
        DISTRO := fedora
    else ifneq ($(filter debian ubuntu,$(DISTRO_ID)),)
        DISTRO := debian
    else ifneq ($(filter arch,$(DISTRO_ID)),)
        DISTRO := arch
    else ifneq ($(filter fedora rhel centos,$(DISTRO_ID_LIKE)),)
        DISTRO := fedora
    else ifneq ($(filter debian ubuntu,$(DISTRO_ID_LIKE)),)
        DISTRO := debian
    else ifneq ($(filter arch,$(DISTRO_ID_LIKE)),)
        DISTRO := arch
    endif
endif

# Fedora / RHEL / CentOS (dnf). These are the package names to layer into
# an immutable image (Immutablue / Hyacinth Macaw) rather than dnf-install
# on the host.
FEDORA_DEPS := gcc make nodejs pkgconf-pkg-config \
               glib2-devel libyaml-devel json-glib-devel libsoup3-devel \
               libxml2-devel sqlite-devel libpq-devel readline-devel openssl-devel \
               gobject-introspection-devel poppler-glib-devel \
               libarchive-devel

DEBIAN_DEPS := gcc make nodejs pkg-config \
               libglib2.0-dev libyaml-dev libjson-glib-dev libsoup-3.0-dev \
               libxml2-dev libsqlite3-dev libpq-dev libreadline-dev \
               gobject-introspection libgirepository1.0-dev \
               libpoppler-glib-dev libarchive-dev

ARCH_DEPS := gcc make nodejs pkgconf \
             glib2 libyaml json-glib libsoup3 libxml2 sqlite postgresql-libs \
             readline gobject-introspection poppler-glib libarchive

.PHONY: install-deps
install-deps:
ifeq ($(DISTRO),fedora)
	@echo "On an immutable host, layer these into the image build instead:"
	@echo "  $(FEDORA_DEPS)"
	sudo dnf install -y $(FEDORA_DEPS)
else ifeq ($(DISTRO),debian)
	sudo apt-get update
	sudo apt-get install -y $(DEBIAN_DEPS)
else ifeq ($(DISTRO),arch)
	sudo pacman -S --needed --noconfirm $(ARCH_DEPS)
else
	$(error Unsupported distro "$(DISTRO_ID)". Override: make DISTRO=fedora|debian|arch install-deps)
endif

.PHONY: list-deps
list-deps:
	@echo "Fedora build dependencies:"
	@for p in $(FEDORA_DEPS); do echo "  $$p"; done

# Stripe and mail pin the same otel-glib commit (d2f52174a840). Use one
# telemetry archive and header tree so the process has one set of GTypes.
STRIPE_GLIB_DIR := $(DEPS_DIR)/stripe-glib
STRIPE_GLIB_LIB := $(STRIPE_GLIB_DIR)/build/$(BUILD_TYPE)/libstripe-glib-1.0.a
OTEL_GLIB_DIR := $(STRIPE_GLIB_DIR)/deps/otel-glib
OTEL_GLIB_LIB := $(OTEL_GLIB_DIR)/build/$(BUILD_TYPE)/libotel-glib-1.0.a
CFLAGS += -I$(STRIPE_GLIB_DIR)/src -I$(OTEL_GLIB_DIR)/src $(shell $(PKG_CONFIG) --cflags gnutls)
TEST_CFLAGS += -I$(STRIPE_GLIB_DIR)/src -I$(OTEL_GLIB_DIR)/src $(shell $(PKG_CONFIG) --cflags gnutls)
VENDOR_LIBS_SERVER := $(STRIPE_GLIB_LIB) $(VENDOR_LIBS_SERVER)
LDFLAGS += $(shell $(PKG_CONFIG) --libs gnutls)
TEST_LDFLAGS += $(shell $(PKG_CONFIG) --libs gnutls)
# Transactional mail; keep the standalone CLI free of server dependencies.
# Server and test flags remain recursive so these pkg-config calls run only
# when a server or test recipe expands them, never while parsing CLI targets.
MAIL_GLIB_DIR := $(DEPS_DIR)/mail-glib
MAIL_GLIB_LIB := $(MAIL_GLIB_DIR)/build/$(BUILD_TYPE)/libmail-glib-1.0.a
MAIL_OTEL_DIR := $(OTEL_GLIB_DIR)
MAIL_OTEL_LIB := $(OTEL_GLIB_LIB)
DEPS_SERVER += gmime-3.0 gnutls
CFLAGS_MAIL = -I$(MAIL_GLIB_DIR)/src -I$(MAIL_OTEL_DIR)/src $(shell $(PKG_CONFIG) --cflags gmime-3.0 gnutls)
CFLAGS += $(CFLAGS_MAIL)
LDFLAGS += $(shell $(PKG_CONFIG) --libs gmime-3.0 gnutls)
# Keep telemetry after both consumers for ordinary static archive extraction.
VENDOR_LIBS_SERVER += $(MAIL_GLIB_LIB) $(OTEL_GLIB_LIB)
OIDC_GLIB_DIR := $(DEPS_DIR)/oidc-glib
OIDC_GLIB_LIB := $(OIDC_GLIB_DIR)/build/$(BUILD_TYPE)/liboidc-glib-1.0.a
CFLAGS += -I$(OIDC_GLIB_DIR)/src
TEST_CFLAGS += -I$(OIDC_GLIB_DIR)/src
VENDOR_LIBS_SERVER := $(OIDC_GLIB_LIB) $(VENDOR_LIBS_SERVER)
FEDORA_DEPS += gmime30-devel gnutls-devel
FEDORA_DEPS += libjose-devel jansson-devel
DEBIAN_DEPS += libgmime-3.0-dev libgnutls28-dev
DEBIAN_DEPS += libjose-dev libjansson-dev
ARCH_DEPS += gmime3 gnutls
ARCH_DEPS += jose jansson

# The recovery GTest uses real authenticated archives, not a fake encryptor.
FEDORA_DEPS += jq gnupg2
DEBIAN_DEPS += jq gnupg
ARCH_DEPS += jq gnupg
