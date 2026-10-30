################################################################################
#
# oscairn
#
################################################################################

# Built from the Oscairn source tree this external tree belongs to.
OSCAIRN_VERSION = local
OSCAIRN_SITE = $(BR2_EXTERNAL_OSCAIRN_PATH)/../..
OSCAIRN_SITE_METHOD = local
OSCAIRN_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude '/build*' --exclude '/demo'
OSCAIRN_LICENSE = AGPL-3.0-or-later
OSCAIRN_LICENSE_FILES = LICENSE
OSCAIRN_DEPENDENCIES = host-pkgconf libsndfile

ifeq ($(BR2_PACKAGE_OSCAIRN_LV2),y)
OSCAIRN_DEPENDENCIES += lilv
OSCAIRN_LICENSE += , ISC
OSCAIRN_LICENSE_FILES += core/lv2-host/third_party/jalv/ISC.txt
OSCAIRN_CONF_OPTS += -DOSCAIRN_LV2=ON
else
OSCAIRN_CONF_OPTS += -DOSCAIRN_LV2=OFF
endif

ifeq ($(BR2_PACKAGE_OSCAIRN_STANDALONE),y)
OSCAIRN_DEPENDENCIES += oscairn-juce alsa-lib freetype fontconfig \
	xlib_libX11 xlib_libXext xlib_libXinerama xlib_libXrandr \
	xlib_libXcursor xlib_libXrender
# JUCE comes from the oscairn-juce package: nothing is downloaded while building.
OSCAIRN_CONF_OPTS += \
	-DOSCAIRN_STANDALONE=ON \
	-DOSCAIRN_AUDIO_BUFFER_FRAMES=128 \
	-DFETCHCONTENT_SOURCE_DIR_JUCE=$(OSCAIRN_JUCE_SRCDIR) \
	-DFETCHCONTENT_FULLY_DISCONNECTED=ON
endif

# JUCE's modules compile as a few very large translation units, over a gigabyte of memory
# each; a job per core can run out of memory on an 8 GB build machine.
OSCAIRN_MAKE_OPTS = -j4

$(eval $(cmake-package))
