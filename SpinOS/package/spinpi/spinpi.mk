################################################################################
#
# spinpi: the printer service (sources: ../SpinPi in this repo)
#
################################################################################

SPINPI_VERSION = 0.6.0
SPINPI_SITE = $(BR2_EXTERNAL_SPINOS_PATH)/../SpinPi
SPINPI_SITE_METHOD = local
SPINPI_LICENSE = MIT

define SPINPI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/spinpi.py $(TARGET_DIR)/opt/spinpi/spinpi.py
	$(INSTALL) -D -m 0644 $(@D)/web/index.html $(TARGET_DIR)/opt/spinpi/web/index.html
endef

$(eval $(generic-package))
