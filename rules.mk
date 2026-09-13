# rules.mk - VENTURE build rules
#
# Pattern rules and common recipes. Included by the top-level Makefile after
# config.mk and after the source/object lists have been computed.
#
# There are two parallel object trees because the same src/ files are never
# compiled twice with different flags, but the two archives need different
# include paths:
#
#   $(OBJDIR)/core/...    compiled with CFLAGS_CORE   -> libventure-core.a
#   $(OBJDIR)/server/...  compiled with CFLAGS        -> libventure.a
#
# Keeping them in separate trees means `make venturectl` never even looks at
# a header from orm-glib, htmx-glib, ai-glib or podomation.

# ---------------------------------------------------------------------------
# Generated-header ordering
# ---------------------------------------------------------------------------

# Every object depends on the generated version header.
$(CORE_OBJS) $(SERVER_OBJS) $(CLI_OBJS) $(MAIN_OBJ) $(TEST_OBJS): src/venture-version.h

# The config subsystem embeds the default configuration, and it is compiled
# into BOTH object trees -- so the core tree needs the generated header too,
# or `make venturectl` from a clean tree fails.
$(CORE_OBJS) $(SERVER_OBJS) $(MAIN_OBJ): $(OUTDIR)/venture-default-config.h

# Only the web server embeds the assets, and it is server-only.
$(SERVER_OBJS): $(OUTDIR)/venture-assets.h

# The model catalogue goes into both trees: the CLI lists providers too.
$(CORE_OBJS) $(SERVER_OBJS): $(OUTDIR)/venture-models.h

# The vendored dependencies must be built before anything that includes their
# headers, because several of them GENERATE a header (htmx-version.h,
# ai-version.h, pod-version.h, crispy-version.h) into their own build tree.
# Order-only, so rebuilding an archive does not force every object to
# recompile -- only that the archives exist first.
$(SERVER_OBJS) $(MAIN_OBJ) $(TEST_OBJS): | $(VENDOR_LIBS_SERVER)
$(CORE_OBJS) $(CLI_OBJS): | $(VENDOR_LIBS_CLI)

# ---------------------------------------------------------------------------
# Compilation
# ---------------------------------------------------------------------------

# Header dependencies are written as a side effect of the compile that
# already reads those headers, rather than by a separate pass.
#
# The separate `%.d: %.c` rules this replaces were never reachable: nothing
# depended on them and nothing generated them, so no .d file was ever
# written and the -include in the Makefile matched nothing. The effect was
# that editing a header rebuilt nothing at all -- `make` would report
# success having relinked objects compiled against the previous version of
# it. That is the same class of failure as a stale dependency tree, and it
# is worse for being silent: a changed enum or struct layout produces a
# binary in which half the objects disagree about it.
#
# -MP emits a phony target for each header so that deleting or renaming one
# does not leave make unable to build anything at all.
DEPFLAGS = -MMD -MP

# Core objects (no database, no server, no AI).
$(OBJDIR)/core/%.o: src/%.c | $(BUILDDIR)/include/venture
	@$(MKDIR_P) $(@D)
	@echo "  CC[core] $<"
	$(Q)$(CC) $(CFLAGS_CORE) $(DEPFLAGS) -c $< -o $@

# Server objects (everything).
$(OBJDIR)/server/%.o: src/%.c | $(BUILDDIR)/include/venture
	@$(MKDIR_P) $(@D)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Test objects.
$(OBJDIR)/tests/%.o: tests/%.c | $(BUILDDIR)/include/venture
	@$(MKDIR_P) $(@D)
	@echo "  CC[test] $<"
	$(Q)$(CC) $(TEST_CFLAGS) $(DEPFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Archives
# ---------------------------------------------------------------------------

# libventure-core.a - the shared foundation, enough to build venturectl.
$(OUTDIR)/$(LIB_CORE_STATIC): $(CORE_OBJS) | $(OUTDIR)
	@echo "  AR      $@"
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $(CORE_OBJS)

# libventure.a - the full server-side library. It contains the core objects
# recompiled with the server flag set plus every server-only subsystem, so
# the server links exactly one archive of our own code.
$(OUTDIR)/$(LIB_STATIC): $(SERVER_OBJS) | $(OUTDIR)
	@echo "  AR      $@"
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $(SERVER_OBJS)

# libventure.so - provided so that other GLib applications (and GIR) can
# consume VENTURE as a library. The binaries themselves link statically.
$(OUTDIR)/$(LIB_SHARED_FULL): $(SERVER_OBJS) $(VENDOR_LIBS_SERVER) | $(OUTDIR)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS_SHARED) -o $@ $(SERVER_OBJS) \
		-Wl,--whole-archive $(VENDOR_LIBS_SERVER) -Wl,--no-whole-archive \
		$(LDFLAGS)
	$(Q)cd $(OUTDIR) && ln -sf $(LIB_SHARED_FULL) $(LIB_SHARED_MAJOR)
	$(Q)cd $(OUTDIR) && ln -sf $(LIB_SHARED_MAJOR) $(LIB_SHARED)

# ---------------------------------------------------------------------------
# Binaries
# ---------------------------------------------------------------------------

# venture - the server.
#
# libventure.a is pulled in --whole-archive so that every GType, boxed type,
# enum and interface registration is present in the executable even when
# main.c never names it. Loaded plugins and podomation modules resolve those
# symbols against the executable (--export-dynamic, set in LDFLAGS), so
# there is exactly one instance of each type in the process.
$(OUTDIR)/venture: $(MAIN_OBJ) $(OUTDIR)/$(LIB_STATIC) $(VENDOR_LIBS_SERVER)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $(MAIN_OBJ) \
		-Wl,--whole-archive $(OUTDIR)/$(LIB_STATIC) -Wl,--no-whole-archive \
		$(VENDOR_LIBS_SERVER) $(LDFLAGS)

# venturectl - the CLI. Deliberately links only libventure-core.a and
# yaml-glib, so `make venturectl` works on a machine that cannot build the
# server (no libpq, no sqlite headers, no readline).
$(OUTDIR)/venturectl: $(CLI_OBJS) $(OUTDIR)/$(LIB_CORE_STATIC) $(VENDOR_LIBS_CLI)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $(CLI_OBJS) $(OUTDIR)/$(LIB_CORE_STATIC) \
		$(VENDOR_LIBS_CLI) $(LDFLAGS_CLI)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

$(OUTDIR)/tests/test-%: $(OBJDIR)/tests/test-%.o $(OUTDIR)/$(LIB_STATIC) $(VENDOR_LIBS_SERVER) | $(OUTDIR)/tests
	@echo "  LD[test] $@"
	$(Q)$(CC) -o $@ $< \
		-Wl,--whole-archive $(OUTDIR)/$(LIB_STATIC) -Wl,--no-whole-archive \
		$(VENDOR_LIBS_SERVER) $(TEST_LDFLAGS)

# ---------------------------------------------------------------------------
# Plugins and podomation modules
# ---------------------------------------------------------------------------

# Native plugins: plugins/<name>/*.c -> $(OUTDIR)/plugins/<name>.so
#
# The vendored libraries are order-only prerequisites because a plugin
# includes <venture/venture.h>, which reaches <htmx-glib.h> and <orm.h>,
# and those pull in version headers each dependency generates into its own
# build tree. Without this the plugin can be compiled before the library
# that writes the header it needs -- which only shows up on a fully clean
# parallel build, where nothing left over is standing in for it.
$(OUTDIR)/plugins/%.so: plugins/%/*.c | $(OUTDIR)/plugins $(VENDOR_LIBS_SERVER)
	@$(MKDIR_P) $(dir $@)
	@echo "  LD[plug] $@"
	$(Q)$(CC) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $^

# podomation modules: modules/<name>/*.c -> $(OUTDIR)/pod-modules/libpod-module-<name>.so
#
# The naming matches podomation's own convention so its module manager picks
# them up from a directory added to `module_paths:`.
$(OUTDIR)/pod-modules/libpod-module-%.so: modules/%/*.c | $(OUTDIR)/pod-modules
	@$(MKDIR_P) $(dir $@)
	@echo "  LD[pod]  $@"
	$(Q)$(CC) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $^

# ---------------------------------------------------------------------------
# Generated sources
# ---------------------------------------------------------------------------

# Version header.
#
# config.mk is a prerequisite because the version numbers substituted below
# live there, not in the .in. Without it, bumping VERSION_MINOR regenerates
# nothing: the stale header is newer than the template, make calls it up to
# date, and the build produces a binary that reports the previous version
# while every tag and changelog says otherwise.
src/venture-version.h: src/venture-version.h.in config.mk
	@echo "  GEN     $@"
	$(Q)sed \
		-e 's|@VENTURE_VERSION_MAJOR@|$(VERSION_MAJOR)|g' \
		-e 's|@VENTURE_VERSION_MINOR@|$(VERSION_MINOR)|g' \
		-e 's|@VENTURE_VERSION_MICRO@|$(VERSION_MICRO)|g' \
		-e 's|@VENTURE_VERSION@|$(VERSION)|g' \
		-e 's|@VENTURE_API_VERSION@|$(API_VERSION)|g' \
		$< > $@

# Embedded default configuration: the YAML defaults and the crispy C config
# template are compiled into the binary so `venture --generate-config` works
# on a machine with nothing installed.
$(OUTDIR)/venture-default-config.h: data/default-config.yaml data/default-config.c | $(OUTDIR)
	@echo "  GEN     $@"
	$(Q)echo "/* Generated by rules.mk - do not edit. */" > $@
	$(Q)echo "static const gchar *venture_default_yaml_config =" >> $@
	$(Q)sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' data/default-config.yaml >> $@
	$(Q)echo ";" >> $@
	$(Q)echo "" >> $@
	$(Q)echo "static const gchar *venture_default_c_config =" >> $@
	$(Q)sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' data/default-config.c >> $@
	$(Q)echo ";" >> $@

# Embedded web assets: the stylesheet, the hx-* attribute client and the UI
# behaviour script become string constants, so the server binary serves the
# whole UI with no data directory to install, no CDN and no build-time
# download. See data/static/venture-hx.js for why the htmx runtime is our
# own rather than vendored.
ASSET_FILES := data/static/venture-classic.css \
               data/static/venture-industrial.css \
               data/static/venture-hx.js \
               data/static/venture.js

$(OUTDIR)/venture-assets.h: $(ASSET_FILES) | $(OUTDIR)
	@echo "  GEN     $@"
	$(Q)echo "/* Generated by rules.mk - do not edit. */" > $@
	$(Q)set -e; for f in $(ASSET_FILES); do \
		sym=$$(basename "$$f" | tr '.-' '__'); \
		echo "static const gchar *venture_asset_$$sym =" >> $@; \
		sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' "$$f" >> $@; \
		echo ";" >> $@; \
		echo "" >> $@; \
	done

# The model catalogue, read out of ai-glib's provider headers rather than
# written here. See tools/venture-models.sh for why.
AI_PROVIDER_HEADERS := $(wildcard $(AI_GLIB_DIR)/src/providers/*.h)

$(OUTDIR)/venture-models.h: tools/venture-models.sh $(AI_PROVIDER_HEADERS) | $(OUTDIR)
	@echo "  GEN     $@"
	$(Q)tools/venture-models.sh $(AI_GLIB_DIR)/src/providers > $@

# pkg-config file.
$(OUTDIR)/venture-$(API_VERSION).pc: venture.pc.in | $(OUTDIR)
	@echo "  GEN     $@"
	$(Q)sed \
		-e 's|@PREFIX@|$(PREFIX)|g' \
		-e 's|@LIBDIR@|$(LIBDIR)|g' \
		-e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
		-e 's|@VERSION@|$(VERSION)|g' \
		-e 's|@API_VERSION@|$(API_VERSION)|g' \
		$< > $@

# ---------------------------------------------------------------------------
# GIR
# ---------------------------------------------------------------------------

$(OUTDIR)/$(GIR_FILE): $(OUTDIR)/$(LIB_SHARED_FULL)
	@echo "  GIR     $@"
	$(Q)$(GIR_SCANNER) \
		--namespace=$(GIR_NAMESPACE) \
		--nsversion=$(GIR_VERSION) \
		--library=venture \
		--library-path=$(OUTDIR) \
		--include=GLib-2.0 --include=GObject-2.0 --include=Gio-2.0 \
		--pkg=glib-2.0 --pkg=gobject-2.0 --pkg=gio-2.0 \
		--identifier-prefix=Venture --symbol-prefix=venture \
		--output=$@ --warn-all -Isrc \
		$(PUBLIC_HDRS) $(SERVER_SRCS)

$(OUTDIR)/$(TYPELIB_FILE): $(OUTDIR)/$(GIR_FILE)
	@echo "  GIR     $@"
	$(Q)$(GIR_COMPILER) --output=$@ $<

# ---------------------------------------------------------------------------
# Directories and the development include symlink
# ---------------------------------------------------------------------------

# build/include/venture -> src, so a plugin can `#include <venture/venture.h>`
# exactly as an installed consumer would, without being installed first.
$(BUILDDIR):
	@$(MKDIR_P) $(BUILDDIR)

$(BUILDDIR)/include/venture: | $(BUILDDIR)
	@$(MKDIR_P) $(BUILDDIR)/include
	@ln -sfn $(CURDIR)/src $(BUILDDIR)/include/venture

$(OUTDIR):
	@$(MKDIR_P) $(OUTDIR)

$(OUTDIR)/tests:
	@$(MKDIR_P) $(OUTDIR)/tests

$(OUTDIR)/plugins:
	@$(MKDIR_P) $(OUTDIR)/plugins

$(OUTDIR)/pod-modules:
	@$(MKDIR_P) $(OUTDIR)/pod-modules

# ---------------------------------------------------------------------------
# Vendored dependency builds
# ---------------------------------------------------------------------------
#
# Each dependency is built by its own Makefile into its own build/<type>/
# tree. DEBUG is propagated so a DEBUG=1 build of VENTURE links DEBUG=1
# dependencies. Sub-dependency directories are overridden (see config.mk) so
# only our canonical copy of each library is ever compiled.

.PHONY: dep-yaml-glib dep-htmx-glib dep-ai-glib dep-crispy dep-podomation dep-orm-glib

# The dependencies do not agree on what the archive target is called:
# htmx-glib and ai-glib use `static`; yaml-glib, crispy and orm-glib use
# `lib`. Each is invoked with the name it actually has.
dep-yaml-glib:
	$(Q)$(MAKE) --no-print-directory -C $(YAML_GLIB_DIR) DEBUG=$(DEBUG) $(YAML_GLIB_SUBMAKE) lib

dep-htmx-glib:
	$(Q)$(MAKE) --no-print-directory -C $(HTMX_GLIB_DIR) DEBUG=$(DEBUG) $(HTMX_GLIB_SUBMAKE) static

dep-ai-glib:
	$(Q)$(MAKE) --no-print-directory -C $(AI_GLIB_DIR) DEBUG=$(DEBUG) $(AI_GLIB_SUBMAKE) static

dep-crispy:
	$(Q)$(MAKE) --no-print-directory -C $(CRISPY_DIR) DEBUG=$(DEBUG) $(CRISPY_SUBMAKE) lib

# Only `version static`, never `deps`: podomation's `deps` target builds its
# own bundled yaml-glib/crispy/ai-glib copies, which we neither need nor link.
dep-podomation:
	$(Q)$(MAKE) --no-print-directory -C $(PODOMATION_DIR) DEBUG=$(DEBUG) $(PODOMATION_SUBMAKE) version static

# podomation's own modules -- cron, timer, log, http, ntfy and the rest --
# are loadable objects rather than part of the archive, so a rule that says
# `cron->new("0 2 * * *")` needs them present. They resolve pod_* against the
# server executable, which is linked --export-dynamic.
.PHONY: dep-podomation-modules
dep-podomation-modules:
	$(Q)$(MAKE) --no-print-directory -C $(PODOMATION_DIR) DEBUG=$(DEBUG) $(PODOMATION_SUBMAKE) modules

# orm-glib has no `static` target; `lib` builds both the archive and the
# shared object, and it does not split its output by build type.
dep-orm-glib:
	$(Q)$(MAKE) --no-print-directory -C $(ORM_GLIB_DIR) DEBUG=$(DEBUG) $(ORM_GLIB_SUBMAKE) lib

$(YAML_GLIB_LIB): dep-yaml-glib
$(HTMX_GLIB_LIB): dep-htmx-glib
$(AI_GLIB_LIB): dep-ai-glib
$(CRISPY_LIB): dep-crispy
$(PODOMATION_LIB): dep-podomation
$(ORM_GLIB_LIB): dep-orm-glib

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

.PHONY: clean clean-all clean-deps
clean:
	rm -rf $(BUILDDIR)/$(BUILD_TYPE)
	rm -f src/venture-version.h

clean-all: clean-deps
	rm -rf $(BUILDDIR)
	rm -f src/venture-version.h

#
# Remove each dependency's build tree outright rather than calling its own
# clean target.
#
# A dep's clean only removes the build type it was invoked for, so
# `clean-deps` without DEBUG=1 would leave the debug tree standing. Worse,
# a dep that changes its output layout or soname between versions leaves
# the old artifact behind, and since a fresh `git checkout` gives objects
# the same mtime as their sources, make considers them current and
# re-archives them: the link then fails on a symbol that exists in neither
# version, or -- far worse -- succeeds against half-stale objects. That is
# exactly what an orm-glib bump did.
clean-deps:
	$(Q)for d in $(YAML_GLIB_DIR) $(HTMX_GLIB_DIR) $(AI_GLIB_DIR) \
	             $(CRISPY_DIR) $(PODOMATION_DIR) $(ORM_GLIB_DIR) $(MAIL_GLIB_DIR) $(MAIL_OTEL_DIR); do \
		rm -rf $$d/build; \
	done

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

.PHONY: install install-bin install-cli install-lib install-headers install-pc
.PHONY: install-plugins install-pod-modules install-data install-man install-gir
.PHONY: uninstall

install: install-bin install-cli install-lib install-headers install-dep-headers
install: install-pc install-data install-man
ifeq ($(BUILD_PLUGINS),1)
install: install-plugins
endif
ifeq ($(BUILD_POD_MODULES),1)
install: install-pod-modules
endif
ifeq ($(BUILD_GIR),1)
install: install-gir
endif

# ---------------------------------------------------------------------------
# The agent skill
# ---------------------------------------------------------------------------
#
# Symlinked rather than copied, so editing the skill in the working tree is
# immediately what every agent reads. A copy would drift the first time one
# was edited in place, and a stale skill is worse than none because it gets
# followed.
#
# Only ever removes a symlink. A real file or directory at the destination
# is left alone and reported: that is somebody's own skill of the same name,
# and silently replacing it would be the one unrecoverable thing this target
# could do.
.PHONY: install-skill
install-skill:
	@if [ ! -f "$(SKILL_SRC)/SKILL.md" ]; then \
		echo "No skill at $(SKILL_SRC)" >&2; \
		exit 1; \
	fi
	@for dest in $(SKILL_DESTS); do \
		link="$$dest/$(SKILL_NAME)"; \
		$(MKDIR_P) "$$dest"; \
		if [ -L "$$link" ]; then \
			rm -f "$$link"; \
		elif [ -e "$$link" ]; then \
			echo "  skip     $$link (not a symlink -- move it aside first)"; \
			continue; \
		fi; \
		ln -s "$(SKILL_SRC)" "$$link"; \
		echo "  link     $$link"; \
	done

.PHONY: uninstall-skill
uninstall-skill:
	@for dest in $(SKILL_DESTS); do \
		link="$$dest/$(SKILL_NAME)"; \
		if [ -L "$$link" ]; then \
			rm -f "$$link"; \
			echo "  unlink   $$link"; \
		elif [ -e "$$link" ]; then \
			echo "  skip     $$link (not a symlink)"; \
		fi; \
	done

install-bin: $(OUTDIR)/venture
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(OUTDIR)/venture $(DESTDIR)$(BINDIR)/venture

install-cli: $(OUTDIR)/venturectl
	$(MKDIR_P) $(DESTDIR)$(BINDIR)
	$(INSTALL_PROGRAM) $(OUTDIR)/venturectl $(DESTDIR)$(BINDIR)/venturectl

install-lib: $(OUTDIR)/$(LIB_STATIC) $(OUTDIR)/$(LIB_CORE_STATIC)
	$(MKDIR_P) $(DESTDIR)$(LIBDIR)
	$(INSTALL_DATA) $(OUTDIR)/$(LIB_STATIC) $(DESTDIR)$(LIBDIR)/
	$(INSTALL_DATA) $(OUTDIR)/$(LIB_CORE_STATIC) $(DESTDIR)$(LIBDIR)/

install-headers:
	$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/venture
	@for h in $(PUBLIC_HDRS); do \
		sub=$$(dirname $$h | sed 's|^src||; s|^/||'); \
		$(MKDIR_P) $(DESTDIR)$(INCLUDEDIR)/venture/$$sub; \
		$(INSTALL_DATA) $$h $(DESTDIR)$(INCLUDEDIR)/venture/$$sub/; \
	done

#
# The vendored dependencies' headers.
#
# VENTURE's public headers include <orm.h> and <htmx-glib.h>, so anything
# compiled against <venture/venture.h> needs them -- including a crispy
# plugin, which is compiled on the machine that runs the server rather than
# on the machine that built it. Without this, crispy works from a source
# tree and fails on an installed system, which is the worst of the two
# places to find out.
#
# The libraries themselves are not installed: they are linked statically
# into the binaries, and a plugin resolves their symbols against the
# executable.
#
install-dep-headers:
	@for d in $(YAML_GLIB_DIR) $(HTMX_GLIB_DIR) $(AI_GLIB_DIR) \
	          $(CRISPY_DIR) $(PODOMATION_DIR) $(ORM_GLIB_DIR); do \
		name=$$(basename $$d); \
		if [ ! -d "$$d/src" ]; then continue; fi; \
		( cd $$d/src && find . -name '*.h' -print ) | while read -r h; do \
			$(MKDIR_P) "$(DESTDIR)$(VENTUREDEPSINCLUDEDIR)/$$name/$$(dirname $$h)"; \
			$(INSTALL_DATA) "$$d/src/$$h" \
				"$(DESTDIR)$(VENTUREDEPSINCLUDEDIR)/$$name/$$h"; \
		done; \
		if [ -d "$$d/build/$(BUILD_TYPE)" ]; then \
			for h in $$d/build/$(BUILD_TYPE)/*.h; do \
				if [ -f "$$h" ]; then \
					$(INSTALL_DATA) "$$h" \
						"$(DESTDIR)$(VENTUREDEPSINCLUDEDIR)/$$name/"; \
				fi; \
			done; \
		fi; \
	done

install-pc: $(OUTDIR)/venture-$(API_VERSION).pc
	$(MKDIR_P) $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL_DATA) $(OUTDIR)/venture-$(API_VERSION).pc $(DESTDIR)$(PKGCONFIGDIR)/

install-plugins:
	$(MKDIR_P) $(DESTDIR)$(PLUGINDIR)
	@for p in $(OUTDIR)/plugins/*.so; do \
		if [ -f "$$p" ]; then $(INSTALL_DATA) "$$p" $(DESTDIR)$(PLUGINDIR)/; fi \
	done

#
# Both VENTURE's own pod modules and podomation's.
#
# Podomation's modules -- cron_event, timer, log, http and the rest -- are
# the vocabulary automation rules are written in: a rule saying
# `cron_event->new("0 9 1 * *")` cannot load without them, and the shipped
# example in data/examples/automations.pod is exactly that rule. They used
# to be built only by `make run`, so an installed VENTURE could parse that
# example and then refuse to load it, reporting an unknown source module.
install-pod-modules: dep-podomation-modules
	$(MKDIR_P) $(DESTDIR)$(PODMODULEDIR)
	@for m in $(OUTDIR)/pod-modules/*.so \
	          $(PODOMATION_DIR)/build/$(BUILD_TYPE)/modules/*.so; do \
		if [ -f "$$m" ]; then $(INSTALL_DATA) "$$m" $(DESTDIR)$(PODMODULEDIR)/; fi \
	done

install-data:
	$(MKDIR_P) $(DESTDIR)$(VENTUREDATADIR)/venture-types
	@for f in data/venture-types/*.yaml; do \
		if [ -f "$$f" ]; then $(INSTALL_DATA) "$$f" $(DESTDIR)$(VENTUREDATADIR)/venture-types/; fi \
	done

install-man:
	$(MKDIR_P) $(DESTDIR)$(MANDIR)/man1
	@for m in data/venture.1 data/venturectl.1; do \
		if [ -f "$$m" ]; then $(INSTALL_DATA) "$$m" $(DESTDIR)$(MANDIR)/man1/; fi \
	done

install-gir: $(OUTDIR)/$(GIR_FILE) $(OUTDIR)/$(TYPELIB_FILE)
	$(MKDIR_P) $(DESTDIR)$(GIRDIR) $(DESTDIR)$(TYPELIBDIR)
	$(INSTALL_DATA) $(OUTDIR)/$(GIR_FILE) $(DESTDIR)$(GIRDIR)/
	$(INSTALL_DATA) $(OUTDIR)/$(TYPELIB_FILE) $(DESTDIR)$(TYPELIBDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/venture
	rm -f $(DESTDIR)$(BINDIR)/venturectl
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_STATIC)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_CORE_STATIC)
	rm -rf $(DESTDIR)$(INCLUDEDIR)/venture
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/venture-$(API_VERSION).pc
	rm -rf $(DESTDIR)$(PLUGINDIR)
	rm -rf $(DESTDIR)$(PODMODULEDIR)
	rm -rf $(DESTDIR)$(VENTUREDATADIR)
	rm -f $(DESTDIR)$(MANDIR)/man1/venture.1
	rm -f $(DESTDIR)$(MANDIR)/man1/venturectl.1
	rm -f $(DESTDIR)$(GIRDIR)/$(GIR_FILE)
	rm -f $(DESTDIR)$(TYPELIBDIR)/$(TYPELIB_FILE)

# SQL is installed with the binary, with an atomic generator output.
MIGRATION_FILES := $(wildcard migrations/sqlite/*.sql migrations/postgresql/*.sql)
$(OBJDIR)/server/db/venture-migrations.o: $(OUTDIR)/venture-migration-sql.h
$(OUTDIR)/venture-migration-sql.h: tools/venture-migrations.sh $(MIGRATION_FILES) migrations/sqlite migrations/postgresql | $(OUTDIR)
	@echo "  GEN     $@"
	$(Q)tools/venture-migrations.sh migrations > $@.tmp && mv $@.tmp $@

# The parent builds the shared YAML archive. Stripe must not rebuild it
# from private objects while the standalone CLI is linking against it.
.PHONY: dep-stripe-glib
dep-stripe-glib: $(YAML_GLIB_LIB)
	$(Q)$(MAKE) -C $(OTEL_GLIB_DIR) static DEBUG=$(DEBUG) YAML_GLIB_DIR=$(YAML_GLIB_DIR) YAML_GLIB_STATIC=$(YAML_GLIB_LIB)
	$(Q)$(MAKE) -C $(STRIPE_GLIB_DIR) static DEBUG=$(DEBUG) YAML_GLIB_DIR=$(YAML_GLIB_DIR) YAML_GLIB_STATIC=$(YAML_GLIB_LIB) YAML_OBJECTS= YAML_NAMESPACE= OTEL_SHARED=$(OTEL_GLIB_LIB)
$(STRIPE_GLIB_LIB) $(OTEL_GLIB_LIB): dep-stripe-glib
	@test -f $@
.PHONY: dep-mail-glib
dep-mail-glib: $(YAML_GLIB_LIB)
	$(Q)$(MAKE) --no-print-directory -C $(MAIL_GLIB_DIR) DEBUG=$(DEBUG) YAML_GLIB_STATIC=$(YAML_GLIB_LIB) static
	$(Q)$(MAKE) --no-print-directory -C $(MAIL_OTEL_DIR) DEBUG=$(DEBUG) YAML_GLIB_STATIC=$(YAML_GLIB_LIB) static
$(MAIL_GLIB_LIB) $(MAIL_OTEL_LIB): dep-mail-glib
