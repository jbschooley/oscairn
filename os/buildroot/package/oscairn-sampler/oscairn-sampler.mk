################################################################################
#
# oscairn-sampler
#
################################################################################

# Source only: the oscairn package builds the Sampler. Keep this commit the same as
# OSCAIRN_SAMPLER_COMMIT in plugins/CMakeLists.txt, which checks it at configure time.
OSCAIRN_SAMPLER_VERSION = ca1805622f8eb7d1d3685625769afc4f76c638cf
OSCAIRN_SAMPLER_SITE = $(call github,oscairn,sampler,$(OSCAIRN_SAMPLER_VERSION))
OSCAIRN_SAMPLER_LICENSE = AGPL-3.0-or-later
OSCAIRN_SAMPLER_LICENSE_FILES = LICENSE
OSCAIRN_SAMPLER_INSTALL_TARGET = NO

$(eval $(generic-package))
