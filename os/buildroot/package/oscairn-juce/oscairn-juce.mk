################################################################################
#
# oscairn-juce
#
################################################################################

# Source only: the oscairn package builds the JUCE modules it uses.
OSCAIRN_JUCE_VERSION = 9.0.4
OSCAIRN_JUCE_SOURCE = $(OSCAIRN_JUCE_VERSION).tar.gz
OSCAIRN_JUCE_SITE = https://github.com/juce-framework/JUCE/archive/refs/tags
OSCAIRN_JUCE_LICENSE = AGPL-3.0
OSCAIRN_JUCE_LICENSE_FILES = LICENSE.md
OSCAIRN_JUCE_INSTALL_TARGET = NO

$(eval $(generic-package))
