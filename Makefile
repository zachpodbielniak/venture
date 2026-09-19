# Makefile - VENTURE
#
# VENTURE is an ERP/CRM hybrid for running a portfolio of business ventures:
# books, Etsy shops, blogs, newsletters and whatever comes next. It tracks
# ventures and their performance, inventory, contacts and deals, expenses,
# tax write-offs and P&L across multiple entities (businesses and personal),
# with AI as a first-class participant and scheduled automations driven by
# podomation.
#
# Usage:
#   make                   Build everything (libs, both binaries, plugins)
#   make venture           Build the server binary only
#   make venturectl        Build the CLI binary only (no server deps needed)
#   make lib               Build libventure.a and libventure-core.a
#   make shared            Build libventure.so (for GIR / embedding)
#   make plugins           Build the example native plugins
#   make pod-modules       Build the VENTURE podomation modules
#   make test              Build and run the GTest suite
#   make install           Install to PREFIX
#   make clean             Remove this build type's artifacts
#   make clean-all         Remove every build artifact
#
# Common options:
#   make DEBUG=1           Debug build (build/debug, -g3 -O0)
#   make ASAN=1 UBSAN=1    Sanitizers
#   make POSTGRES=0        Build without the PostgreSQL backend
#   make V=1               Verbose command echo
#
# See `make help` for the full list.

.DEFAULT_GOAL := all

.PHONY: all lib shared binaries plugins pod-modules deps test check-deps help

include config.mk

# Targets that must work without any dependency present.
SKIP_DEP_CHECK_TARGETS := install-deps list-deps help check-deps show-config \
                          clean clean-all clean-deps
ifeq ($(filter $(SKIP_DEP_CHECK_TARGETS),$(MAKECMDGOALS)),)
$(foreach dep,$(DEPS_CORE),$(call check_dep,$(dep)))
endif

# ---------------------------------------------------------------------------
# Source discovery
# ---------------------------------------------------------------------------
#
# CORE_SRCS is the subset that has no database, server, AI or automation
# dependency. It is compiled twice: once with CFLAGS_CORE into
# libventure-core.a (for venturectl) and once with CFLAGS into libventure.a
# (for the server). The duplication is deliberate -- it is what lets either
# binary be built in isolation.

CORE_SRCS := \
	src/ledger/venture-journal.c \
	src/venture-version.c \
	src/venture-enums.c \
	src/venture-error.c \
	$(wildcard src/boxed/*.c) \
	$(wildcard src/interfaces/*.c) \
	$(wildcard src/model/*.c) \
	src/receivables/venture-receivable-records.c \
	src/receivables/venture-invoice-state-machine.c \
	src/periods/venture-period-records.c \
	$(wildcard src/config/*.c) \
	$(wildcard src/util/*.c) \
	$(wildcard src/mcp/*.c)

CORE_SRCS += src/orgaccess/venture-access-records.c
CORE_SRCS += src/billing/venture-billing-records.c
CORE_SRCS += src/projects/venture-project-records.c
CORE_SRCS += src/leads/venture-lead-records.c
CORE_SRCS += src/activities/venture-activity-records.c
CORE_SRCS += src/payables/venture-payable-records.c
CORE_SRCS += src/close/venture-close-records.c
CORE_SRCS += src/tax/venture-tax-records.c
CORE_SRCS += src/capture/venture-capture-records.c
CORE_SRCS += src/claims/venture-claim-records.c
CORE_SRCS += src/payroll/venture-payroll-records.c
CORE_SRCS += src/banking/venture-bank-records.c
CORE_SRCS += src/bankfeed/venture-bankfeed-records.c
CORE_SRCS += src/cutover/venture-cutover-records.c
CORE_SRCS += src/setup/venture-setup-records.c
CORE_SRCS += src/progress/venture-progress-records.c
CORE_SRCS += src/portal/venture-portal-records.c
CORE_SRCS += src/portal/venture-supplier-portal-records.c
CORE_SRCS += src/fields/venture-custom-fields-records.c
CORE_SRCS += src/backup/venture-backup-records.c
CORE_SRCS += src/orgaccess/venture-accounting-approval-records.c
CORE_SRCS += src/report/venture-report-records.c
CORE_SRCS += src/budgets/venture-budget-records.c
CORE_SRCS += src/equity/venture-equity-records.c
CORE_SRCS += src/group/venture-group-records.c

# Server-only subsystems.
SERVER_ONLY_SRCS := \
	$(filter-out src/ledger/venture-journal.c,$(wildcard src/ledger/*.c)) \
	src/receivables/venture-settlement-service.c \
	src/receivables/venture-receivable-reports.c \
	$(wildcard src/core/*.c) \
	$(wildcard src/db/*.c) \
	$(filter-out src/report/venture-report-records.c,$(wildcard src/report/*.c)) \
	$(wildcard src/ai/*.c) \
	$(wildcard src/automation/*.c) \
	$(wildcard src/plugin/*.c) \
	$(wildcard src/web/*.c) \
	$(wildcard src/forge/*.c) \
	$(wildcard src/kb/*.c)

SERVER_ONLY_SRCS += src/banking/venture-bank-match-service.c
SERVER_ONLY_SRCS += $(filter-out src/bankfeed/venture-bankfeed-records.c,$(wildcard src/bankfeed/*.c))
SERVER_ONLY_SRCS += $(wildcard src/commerce/*.c)

SERVER_ONLY_SRCS += $(filter-out src/periods/venture-period-records.c,$(wildcard src/periods/*.c))

SERVER_ONLY_SRCS += $(wildcard src/reconciliation/*.c)
CORE_SRCS += src/stripe/venture-stripe-records.c
SERVER_ONLY_SRCS += $(filter-out src/stripe/venture-stripe-records.c,$(wildcard src/stripe/*.c))
CORE_SRCS += src/assets/venture-asset-records.c
SERVER_ONLY_SRCS += $(filter-out src/assets/venture-asset-records.c,$(wildcard src/assets/*.c))
SERVER_ONLY_SRCS += $(filter-out src/orgaccess/venture-access-records.c src/orgaccess/venture-accounting-approval-records.c,$(wildcard src/orgaccess/*.c))
SERVER_ONLY_SRCS += $(filter-out src/billing/venture-billing-records.c,$(wildcard src/billing/*.c))
SERVER_ONLY_SRCS += $(filter-out src/projects/venture-project-records.c,$(wildcard src/projects/*.c))
CORE_SRCS += src/mail/venture-mail-records.c src/mail/venture-mail-sync-records.c
SERVER_ONLY_SRCS += $(filter-out src/mail/venture-mail-records.c src/mail/venture-mail-sync-records.c,$(wildcard src/mail/*.c))
CORE_SRCS += src/quotes/venture-quote-records.c
SERVER_ONLY_SRCS += $(filter-out src/quotes/venture-quote-records.c,$(wildcard src/quotes/*.c))
CORE_SRCS += src/goods/venture-goods-records.c
SERVER_ONLY_SRCS += $(filter-out src/goods/venture-goods-records.c,$(wildcard src/goods/*.c))
SERVER_ONLY_SRCS += src/leads/venture-lead-service.c src/leads/venture-lead-reports.c
SERVER_ONLY_SRCS += $(filter-out src/activities/venture-activity-records.c,$(wildcard src/activities/*.c))
SERVER_ONLY_SRCS += src/payables/venture-payables-service.c src/payables/venture-payable-reports.c
SERVER_ONLY_SRCS += src/close/venture-close-service.c
SERVER_ONLY_SRCS += src/tax/venture-tax-filing-adapter.c src/tax/venture-tax-filing-service.c
SERVER_ONLY_SRCS += src/capture/venture-capture-service.c
SERVER_ONLY_SRCS += src/claims/venture-claims-service.c
SERVER_ONLY_SRCS += src/payroll/venture-payroll-service.c
SERVER_ONLY_SRCS += src/accounting/venture-accounting-home.c

CORE_SRCS += src/pipelines/venture-pipeline-records.c
SERVER_ONLY_SRCS += $(filter-out src/pipelines/venture-pipeline-records.c,$(wildcard src/pipelines/*.c))
CORE_SRCS += src/sequences/venture-sequence-records.c
SERVER_ONLY_SRCS += $(filter-out src/sequences/venture-sequence-records.c,$(wildcard src/sequences/*.c))
CORE_SRCS += src/autojournal/venture-posting-profile.c
CORE_SRCS += src/recurring/venture-recurring-records.c
SERVER_ONLY_SRCS += $(filter-out src/autojournal/venture-posting-profile.c,$(wildcard src/autojournal/*.c))
SERVER_ONLY_SRCS += $(filter-out src/recurring/venture-recurring-records.c,$(wildcard src/recurring/*.c))
CORE_SRCS += src/dunning/venture-dunning-records.c
SERVER_ONLY_SRCS += $(filter-out src/dunning/venture-dunning-records.c,$(wildcard src/dunning/*.c))
CORE_SRCS += src/tax/venture-sales-tax-records.c
SERVER_ONLY_SRCS += src/tax/venture-sales-tax-service.c

PUBLIC_HDRS_AUTOJOURNAL := $(wildcard src/autojournal/*.h)
SERVER_ONLY_SRCS += $(wildcard src/statements/*.c)
SERVER_ONLY_SRCS += src/cutover/venture-cutover-service.c
SERVER_ONLY_SRCS += src/setup/venture-setup-service.c
SERVER_ONLY_SRCS += src/documents/venture-document-service.c
SERVER_ONLY_SRCS += src/progress/venture-progress-service.c
SERVER_ONLY_SRCS += src/portal/venture-portal-service.c
SERVER_ONLY_SRCS += src/portal/venture-supplier-portal-service.c
SERVER_ONLY_SRCS += src/fields/venture-custom-fields-service.c
SERVER_ONLY_SRCS += src/backup/venture-backup-service.c
SERVER_ONLY_SRCS += src/backup/venture-backup-schedule-service.c
SERVER_ONLY_SRCS += $(filter-out src/budgets/venture-budget-records.c,$(wildcard src/budgets/*.c))
SERVER_ONLY_SRCS += $(filter-out src/equity/venture-equity-records.c,$(wildcard src/equity/*.c))
SERVER_ONLY_SRCS += $(filter-out src/group/venture-group-records.c,$(wildcard src/group/*.c))
CORE_SRCS += src/report/venture-headline-records.c
CORE_SRCS += src/calendar/venture-calendar-records.c
SERVER_ONLY_SRCS += $(filter-out src/calendar/venture-calendar-records.c,$(wildcard src/calendar/*.c))
SERVER_ONLY_SRCS := $(filter-out src/report/venture-headline-records.c,$(SERVER_ONLY_SRCS))
CORE_SRCS += src/leads/venture-lead-routing-records.c
SERVER_ONLY_SRCS += src/leads/venture-lead-routing.c
SERVER_ONLY_SRCS += $(wildcard src/money-calendar/*.c)

SERVER_SRCS := $(CORE_SRCS) $(SERVER_ONLY_SRCS)

CLI_SRCS := $(wildcard src/cli/*.c)
MAIN_SRC := src/main.c

# Public headers: installed, and fed to the GIR scanner.
PUBLIC_HDRS := \
	$(PUBLIC_HDRS_AUTOJOURNAL) \
	$(filter-out %-private.h,$(wildcard src/ledger/*.h)) \
	src/venture.h \
	src/venture-types.h \
	src/venture-enums.h \
	src/venture-error.h \
	src/venture-version.h \
	$(wildcard src/boxed/*.h) \
	$(wildcard src/interfaces/*.h) \
	$(wildcard src/model/*.h) \
	$(wildcard src/receivables/*.h) \
	$(wildcard src/stripe/*.h) \
	$(wildcard src/periods/*.h) \
	$(filter-out %-print-style.h,$(wildcard src/quotes/*.h)) \
	$(wildcard src/config/*.h) \
	$(wildcard src/core/*.h) \
	$(wildcard src/db/*.h) \
	$(wildcard src/report/*.h) \
	$(wildcard src/ai/*.h) \
	$(wildcard src/automation/*.h) \
	$(wildcard src/plugin/*.h) \
	$(wildcard src/web/*.h) \
	$(wildcard src/forge/*.h) \
	$(wildcard src/kb/*.h) \
	$(wildcard src/util/*.h) \
	$(wildcard src/mcp/*.h)

PUBLIC_HDRS += $(filter-out %-private.h,$(wildcard src/reconciliation/*.h))
PUBLIC_HDRS += $(wildcard src/assets/*.h)
PUBLIC_HDRS += $(wildcard src/orgaccess/*.h)
PUBLIC_HDRS += $(wildcard src/billing/*.h)
PUBLIC_HDRS += $(wildcard src/projects/*.h)
PUBLIC_HDRS += $(wildcard src/mail/*.h)

PUBLIC_HDRS += $(wildcard src/payables/*.h)
PUBLIC_HDRS += $(wildcard src/goods/*.h)
PUBLIC_HDRS += $(wildcard src/close/*.h)
PUBLIC_HDRS += $(wildcard src/tax/*.h)
PUBLIC_HDRS += $(wildcard src/capture/*.h)
PUBLIC_HDRS += $(wildcard src/claims/*.h)
PUBLIC_HDRS += $(wildcard src/payroll/*.h)
PUBLIC_HDRS += $(wildcard src/accounting/*.h)
PUBLIC_HDRS += $(wildcard src/banking/*.h)
PUBLIC_HDRS += $(wildcard src/bankfeed/*.h)
PUBLIC_HDRS += $(wildcard src/commerce/*.h)
PUBLIC_HDRS += $(filter-out %-private.h,$(wildcard src/pipelines/*.h))
PUBLIC_HDRS += $(filter-out %-private.h,$(wildcard src/sequences/*.h))
PUBLIC_HDRS += $(filter-out %-private.h,$(wildcard src/statements/*.h))
PUBLIC_HDRS += $(wildcard src/cutover/*.h)
PUBLIC_HDRS += $(wildcard src/setup/*.h)
PUBLIC_HDRS += $(wildcard src/documents/*.h)
PUBLIC_HDRS += $(wildcard src/recurring/*.h)
PUBLIC_HDRS += $(wildcard src/progress/*.h)
PUBLIC_HDRS += $(wildcard src/portal/*.h)
PUBLIC_HDRS += $(wildcard src/fields/*.h)
PUBLIC_HDRS += $(wildcard src/dunning/*.h)
PUBLIC_HDRS += $(wildcard src/backup/*.h)
PUBLIC_HDRS += $(wildcard src/budgets/*.h)
PUBLIC_HDRS += $(wildcard src/equity/*.h)
PUBLIC_HDRS += $(wildcard src/group/*.h)
PUBLIC_HDRS += $(wildcard src/money-calendar/*.h)

# Private implementation fragments are included by their owning C source;
# they are neither installable headers nor introspection declarations.
PUBLIC_HDRS := $(filter-out %-private.h,$(PUBLIC_HDRS))

TEST_SRCS := $(wildcard tests/test-*.c)

# ---------------------------------------------------------------------------
# Object lists
# ---------------------------------------------------------------------------

CORE_OBJS := $(patsubst src/%.c,$(OBJDIR)/core/%.o,$(CORE_SRCS))
SERVER_OBJS := $(patsubst src/%.c,$(OBJDIR)/server/%.o,$(SERVER_SRCS))
CLI_OBJS := $(patsubst src/%.c,$(OBJDIR)/core/%.o,$(CLI_SRCS))
MAIN_OBJ := $(OBJDIR)/server/main.o
TEST_OBJS := $(patsubst tests/%.c,$(OBJDIR)/tests/%.o,$(TEST_SRCS))
TEST_BINS := $(patsubst tests/%.c,$(OUTDIR)/tests/%,$(TEST_SRCS))

# The settlement test drives the real CLI and its MCP tool against HTTP.
$(OUTDIR)/tests/test-stripe: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-receivables: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-assets: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-auth: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-billing-surfaces: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-quotes: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-leads: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-payables: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-close: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-capture: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-claims: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-payroll: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-accounting: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-banking: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-sequences: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-recurring: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-dunning: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-sales-tax: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-backup-schedule: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-customer-health: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-money-calendar: | $(OUTDIR)/venturectl

# ---------------------------------------------------------------------------
# Plugin and module discovery
# ---------------------------------------------------------------------------

#
# One subdirectory of plugins/ becomes one .so -- except plugins/scripts/,
# which holds crispy sources. Those are deliberately never built: the server
# compiles them on demand, and building them here would both defeat the
# demonstration and link several independent `venture_plugin_register`
# definitions into one object.
PLUGIN_DIRS := $(filter-out plugins/scripts, \
                 $(patsubst %/,%,$(sort $(dir $(wildcard plugins/*/)))))
PLUGIN_SOS := $(patsubst plugins/%,$(OUTDIR)/plugins/%.so,$(PLUGIN_DIRS))

#
# Only directories. `wildcard modules/*` also matches modules/README.org,
# and make would then dutifully try to build libpod-module-README.org.so.
POD_MODULE_DIRS := $(patsubst %/,%,$(sort $(dir $(wildcard modules/*/))))
POD_MODULE_SOS := $(patsubst modules/%,$(OUTDIR)/pod-modules/libpod-module-%.so,$(POD_MODULE_DIRS))

include rules.mk

# ---------------------------------------------------------------------------
# Top-level targets
# ---------------------------------------------------------------------------

all: binaries
ifeq ($(BUILD_PLUGINS),1)
all: plugins
endif
ifeq ($(BUILD_POD_MODULES),1)
all: pod-modules
endif
ifeq ($(BUILD_GIR),1)
all: gir
endif

# Build every vendored dependency. Not a prerequisite of `all` directly --
# the archives themselves are prerequisites of the link rules, and each has
# a dep-* rule in rules.mk -- but useful to run on its own.
deps: $(YAML_GLIB_LIB) $(HTMX_GLIB_LIB) $(AI_GLIB_LIB) $(CRISPY_LIB) \
      $(PODOMATION_LIB) $(ORM_GLIB_LIB)

binaries: venture venturectl

lib: $(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_CORE_STATIC) \
     $(OUTDIR)/venture-$(API_VERSION).pc

shared: $(OUTDIR)/$(LIB_SHARED_FULL)

# The server. Depends on every vendored archive.
.PHONY: venture
venture: $(OUTDIR)/venture

# The CLI. Deliberately depends on nothing but core + yaml-glib, so this
# target works on a machine that cannot build the server at all.
.PHONY: venturectl
venturectl: $(OUTDIR)/venturectl

.PHONY: gir
gir: $(OUTDIR)/$(GIR_FILE) $(OUTDIR)/$(TYPELIB_FILE)

plugins: $(OUTDIR)/$(LIB_STATIC) $(PLUGIN_SOS)

pod-modules: $(OUTDIR)/$(LIB_STATIC) $(POD_MODULE_SOS)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
#
# Tests run against a throwaway SQLite database in a temp directory, so they
# need no external services. VENTURE_TEST_DB can point them at a PostgreSQL
# instance instead to exercise that dialect.

#
# The plugins are a prerequisite because the suite loads the example plugin
# for real. Without them that test skips itself rather than failing, which
# would quietly stop covering the thing plugins exist for.
#
# TMPDIR is private to the run, and the litter check is what reads it.
#
# g_get_tmp_dir() honours TMPDIR, so every fixture's g_dir_make_tmp() lands
# in a directory belonging to this run alone -- which is what lets the check
# compare before against after exactly. Sharing /tmp would make two worktrees
# building at once report each other's fixtures as this run's litter.
#
# The final rmdir is bare on purpose: it succeeds only if the directory is
# empty, so a run whose fixtures cleaned up leaves nothing, and one that
# somehow did not keeps its evidence where the check just named it.
#
# A failing run keeps everything and is *told where*: a g_assert aborts before
# any teardown, which is deliberate -- those directories are what somebody
# debugging the failure wants -- and moving them under a private TMPDIR would
# otherwise have hidden them.
test: $(TEST_BINS) plugins
	@echo "Running test suite ($(BUILD_TYPE))..."
	@TMPDIR=$$(mktemp -d /tmp/venture-run-XXXXXX); export TMPDIR; \
	bash $(TOOLSDIR)/venture-test-litter.sh snapshot $(OUTDIR)/test-litter; \
	failed=0; total=0; \
	for t in $(TEST_BINS); do \
		total=$$((total + 1)); \
		echo "  --- $$(basename $$t)"; \
		if VENTURE_PLUGIN_PATH="$(abspath $(OUTDIR)/plugins)" \
		   VENTURE_POD_MODULE_PATH="$(abspath $(OUTDIR)/pod-modules)" \
		   G_DEBUG=fatal-warnings $$t --tap > /dev/null; then \
			echo "      PASS"; \
		else \
			echo "      FAIL"; \
			VENTURE_PLUGIN_PATH="$(abspath $(OUTDIR)/plugins)" \
			VENTURE_POD_MODULE_PATH="$(abspath $(OUTDIR)/pod-modules)" \
			G_DEBUG=fatal-warnings $$t --tap | sed 's/^/      /'; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo ""; \
	if [ $$failed -gt 0 ]; then \
		echo "$$failed of $$total test binaries failed"; \
		echo "fixtures from this run are under $$TMPDIR"; \
		exit 1; \
	fi; \
	bash $(TOOLSDIR)/venture-test-litter.sh check $(OUTDIR)/test-litter || exit 1; \
	bash $(TOOLSDIR)/check-versions.sh || exit 1; \
	rmdir "$$TMPDIR" 2>/dev/null || true; \
	echo "All $$total test binaries passed"

# Run one test binary verbosely: make test-one T=test-money
.PHONY: test-one
test-one:
	@if [ -z "$(T)" ]; then echo "Usage: make test-one T=test-money"; exit 1; fi
	$(MAKE) --no-print-directory $(OUTDIR)/tests/$(T)
	@VENTURE_PLUGIN_PATH="$(abspath $(OUTDIR)/plugins)" \
	 VENTURE_POD_MODULE_PATH="$(abspath $(OUTDIR)/pod-modules)" \
	 G_DEBUG=fatal-warnings $(OUTDIR)/tests/$(T) --verbose

# Memory-check the suite under valgrind, if it is installed.
.PHONY: test-valgrind
test-valgrind: $(TEST_BINS)
	@command -v valgrind >/dev/null 2>&1 || { echo "valgrind not found; skipping"; exit 0; }
	@for t in $(TEST_BINS); do \
		echo "  --- $$(basename $$t)"; \
		G_SLICE=always-malloc G_DEBUG=gc-friendly \
		valgrind --leak-check=full --error-exitcode=1 --quiet $$t || exit 1; \
	done

# ---------------------------------------------------------------------------
# Development helpers
# ---------------------------------------------------------------------------

check-deps:
	@echo "Checking system dependencies..."
	@for dep in $(DEPS_SERVER); do \
		if $(PKG_CONFIG) --exists $$dep; then \
			printf "  %-20s OK (%s)\n" "$$dep" "$$($(PKG_CONFIG) --modversion $$dep 2>/dev/null)"; \
		else \
			printf "  %-20s MISSING\n" "$$dep"; \
		fi \
	done
	@echo ""
	@echo "Checking vendored dependencies..."
	@for d in $(YAML_GLIB_DIR) $(HTMX_GLIB_DIR) $(AI_GLIB_DIR) \
	          $(CRISPY_DIR) $(PODOMATION_DIR) $(ORM_GLIB_DIR); do \
		if [ -f "$$d/Makefile" ]; then \
			printf "  %-20s OK\n" "$$(basename $$d)"; \
		else \
			printf "  %-20s MISSING (run: git submodule update --init --recursive)\n" "$$(basename $$d)"; \
		fi \
	done

# Generate compile_commands.json for clangd from the real build flags.
.PHONY: compile-commands
compile-commands:
	@printf '[\n' > compile_commands.json
	@first=1; \
	for src in $(SERVER_SRCS) $(MAIN_SRC); do \
		if [ $$first -eq 0 ]; then printf ',\n' >> compile_commands.json; fi; first=0; \
		printf '  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
			"$(CURDIR)" "$(CURDIR)/$$src" "$(CC)" "$(CFLAGS)" "$$src" >> compile_commands.json; \
	done; \
	for src in $(CLI_SRCS); do \
		printf ',\n  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
			"$(CURDIR)" "$(CURDIR)/$$src" "$(CC)" "$(CFLAGS_CORE)" "$$src" >> compile_commands.json; \
	done; \
	for src in $(TEST_SRCS); do \
		printf ',\n  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
			"$(CURDIR)" "$(CURDIR)/$$src" "$(CC)" "$(TEST_CFLAGS)" "$$src" >> compile_commands.json; \
	done
	@printf '\n]\n' >> compile_commands.json
	@echo "Wrote compile_commands.json ($$(grep -c '\"file\"' compile_commands.json) entries)"

#
# Bring the local compose stack up on the current source.
#
# `down` first, deliberately: `podman-compose up --build` rebuilds the image
# and moves the tag, but leaves an already-running container on the old one,
# so the change appears not to have happened.
.PHONY: compose-up
compose-up:
	podman-compose down
	podman-compose up --build -d
	@echo "  http://127.0.0.1:8747  -- password: podman logs venture-dev | grep -A3 'owner account'"

.PHONY: compose-down
compose-down:
	podman-compose down

# Run the server straight out of the build tree against a scratch database.
.PHONY: run
run: venture plugins pod-modules dep-podomation-modules
	@$(MKDIR_P) $(BUILDDIR)/run
	VENTURE_PLUGIN_PATH="$(abspath $(OUTDIR)/plugins)" \
	VENTURE_VENTURE_TYPE_PATH="$(abspath data/venture-types)" \
	VENTURE_POD_MODULE_PATH="$(abspath $(OUTDIR)/pod-modules):$(abspath $(PODOMATION_DIR)/build/$(BUILD_TYPE)/modules)" \
	$(OUTDIR)/venture --database "sqlite://$(abspath $(BUILDDIR)/run)/venture.db" \
	                  --state-dir "$(abspath $(BUILDDIR)/run)" $(RUNFLAGS)

# A throwaway instance with example data, for showing VENTURE off.
#
# Follows BUILD_TYPE like every other target, so `make demo` uses the
# release tree and `make DEBUG=1 demo` the debug one. The script owns
# build/demo and deletes it on every run, so the demo is the same demo
# every time and nothing anybody does to it matters.
#
# DEMOFLAGS reaches the script: `make DEMOFLAGS=-d demo` leaves it in the
# background, `make DEMOFLAGS="--port 9100" demo` puts it elsewhere.
.PHONY: demo
demo: venture venturectl plugins pod-modules dep-podomation-modules
	@BUILD_TYPE=$(BUILD_TYPE) tools/venture-demo.sh $(DEMOFLAGS)

# Stop a demo left running with --detach, and remove its data.
.PHONY: demo-stop
demo-stop:
	@BUILD_TYPE=$(BUILD_TYPE) tools/venture-demo.sh --stop

# Print any make variable: make print-CFLAGS
print-%:
	@echo '$($*)'

help:
	@echo "VENTURE $(VERSION) - ERP/CRM for a portfolio of ventures"
	@echo ""
	@echo "Build targets:"
	@echo "  all           Libraries, both binaries, plugins, pod modules (default)"
	@echo "  venture       The server binary only"
	@echo "  venturectl    The CLI binary only (needs no server dependencies)"
	@echo "  lib           libventure.a + libventure-core.a + pkg-config file"
	@echo "  shared        libventure.so (for embedding / GIR)"
	@echo "  deps          Build every vendored dependency under deps/"
	@echo "  plugins       Build the example native plugins"
	@echo "  pod-modules   Build the VENTURE podomation modules"
	@echo "  gir           Generate GObject Introspection data"
	@echo "  install       Install to PREFIX ($(PREFIX))"
	@echo "  uninstall     Remove installed files"
	@echo "  clean         Remove $(BUILD_TYPE) build artifacts"
	@echo "  clean-all     Remove all build artifacts"
	@echo "  clean-deps    Clean every vendored dependency"
	@echo ""
	@echo "Container targets:"
	@echo "  compose-up        Rebuild the image and recreate the local stack"
	@echo "  compose-down      Stop it"
	@echo ""
	@echo "Test targets:"
	@echo "  test              Build and run the whole GTest suite"
	@echo "  test-one T=name   Run one test binary verbosely"
	@echo "  test-valgrind     Run the suite under valgrind"
	@echo ""
	@echo "Options (set on the command line):"
	@echo "  DEBUG=1           Debug build into build/debug"
	@echo "  ASAN=1 UBSAN=1    Sanitizers"
	@echo "  SQLITE=0          Build without the SQLite backend"
	@echo "  POSTGRES=0        Build without the PostgreSQL backend"
	@echo "  BUILD_TESTS=0     Skip tests"
	@echo "  BUILD_PLUGINS=0   Skip plugins"
	@echo "  BUILD_GIR=1       Generate introspection data"
	@echo "  V=1               Verbose command echo"
	@echo "  PREFIX=path       Installation prefix"
	@echo ""
	@echo "Utility targets:"
	@echo "  install-deps      Install system build dependencies"
	@echo "  list-deps         List the Fedora package names to layer into an image"
	@echo "  check-deps        Report which dependencies are present"
	@echo "  show-config       Print the resolved build configuration"
	@echo "  compile-commands  Write compile_commands.json for clangd"
	@echo "  run               Run the server from the build tree"
	@echo "  demo              Throwaway instance seeded with example data"
	@echo "  demo-stop         Stop a detached demo and delete its data"
	@echo "  print-VAR         Print the value of any make variable"
	@echo ""
	@echo "Agent skill:"
	@echo "  install-skill     Symlink skills/venturectl into each agent's dir"
	@echo "  uninstall-skill   Remove those symlinks"

# ---------------------------------------------------------------------------
# Header dependency tracking
# ---------------------------------------------------------------------------
#
# Only include .d files that already exist, so a fresh tree never tries to
# generate them before src/venture-version.h has been created.

ifeq ($(filter clean clean-all clean-deps,$(MAKECMDGOALS)),)
-include $(wildcard $(CORE_OBJS:.o=.d))
-include $(wildcard $(SERVER_OBJS:.o=.d))
-include $(wildcard $(CLI_OBJS:.o=.d))
-include $(wildcard $(MAIN_OBJ:.o=.d))
-include $(wildcard $(TEST_OBJS:.o=.d))
endif

deps: $(MAIL_GLIB_LIB) $(MAIL_OTEL_LIB)

$(OUTDIR)/tests/test-mail-surfaces: | $(OUTDIR)/venturectl
$(OUTDIR)/tests/test-lead-routing: | $(OUTDIR)/venturectl
