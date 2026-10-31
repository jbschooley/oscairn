################################################################################
#
# oscairn-sampler
#
################################################################################

# Source only: the oscairn package builds the Sampler. Keep this commit the same as
# OSCAIRN_SAMPLER_COMMIT in plugins/CMakeLists.txt, which checks it at configure time.
OSCAIRN_SAMPLER_VERSION = 20b4282c8246415e2bc61024f587996722387d5a
OSCAIRN_SAMPLER_SITE = $(call github,oscairn,sampler,$(OSCAIRN_SAMPLER_VERSION))
OSCAIRN_SAMPLER_LICENSE = AGPL-3.0-or-later
OSCAIRN_SAMPLER_LICENSE_FILES = LICENSE
OSCAIRN_SAMPLER_INSTALL_TARGET = NO

$(eval $(generic-package))
