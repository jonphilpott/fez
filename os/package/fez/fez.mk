################################################################################
# FEZ, built from the working tree of this repository. os/build.sh copies the tree to FEZ_SRCDIR
# (the files git knows about, uncommitted edits included), and Buildroot builds it with the
# project's own Makefile: plain C99 and SDL2, nothing else.
################################################################################
FEZ_VERSION = local
FEZ_SITE = $(FEZ_SRCDIR)
FEZ_SITE_METHOD = local
FEZ_DEPENDENCIES = sdl2 alsa-lib

# The Makefile's CFLAGS default is `?=` and it then appends the version with `+=`. So CFLAGS goes in through the environment (a value
# on the make command line would replace those lines, and the version define with them). -ffp-contract=off is the project's rule.
FEZ_CFLAGS = $(TARGET_CFLAGS) -std=c99 -D_DEFAULT_SOURCE -ffp-contract=off

define FEZ_BUILD_CMDS
	$(TARGET_MAKE_ENV) CFLAGS="$(FEZ_CFLAGS)" $(MAKE) -C $(@D) fez CC="$(TARGET_CC)" \
		SDL_CFLAGS="$(shell $(STAGING_DIR)/usr/bin/sdl2-config --cflags)" \
		SDL_LIBS="$(shell $(STAGING_DIR)/usr/bin/sdl2-config --libs)"
endef

define FEZ_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fez $(TARGET_DIR)/usr/bin/fez
endef

$(eval $(generic-package))
