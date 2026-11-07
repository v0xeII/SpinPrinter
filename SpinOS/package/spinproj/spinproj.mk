################################################################################
#
# spinproj: frames by angle on the projector (sources: ../SpinProj in this repo)
#
################################################################################

SPINPROJ_VERSION = 0.1.0
SPINPROJ_SITE = $(BR2_EXTERNAL_SPINOS_PATH)/../SpinProj
SPINPROJ_SITE_METHOD = local
SPINPROJ_LICENSE = MIT
SPINPROJ_DEPENDENCIES = lz4 libdrm host-pkgconf

define SPINPROJ_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(TARGET_CONFIGURE_OPTS) NO_SDL=1 \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" spinproj
endef

define SPINPROJ_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/spinproj $(TARGET_DIR)/usr/bin/spinproj
endef

$(eval $(generic-package))
