################################################################################
#
# oscairn-legacy-sampler
#
################################################################################

# Source only: the oscairn package builds the Legacy Sampler. Keep this commit the same as
# OSCAIRN_SAMPLER_COMMIT in plugins/CMakeLists.txt, which checks it at configure time.
OSCAIRN_LEGACY_SAMPLER_VERSION = 5c7c420d5466a9f167f8c67e95a29b578b22f911
OSCAIRN_LEGACY_SAMPLER_SITE = $(call github,oscairn,sampler,$(OSCAIRN_LEGACY_SAMPLER_VERSION))
OSCAIRN_LEGACY_SAMPLER_LICENSE = AGPL-3.0-or-later
OSCAIRN_LEGACY_SAMPLER_LICENSE_FILES = LICENSE
OSCAIRN_LEGACY_SAMPLER_INSTALL_TARGET = NO

$(eval $(generic-package))
