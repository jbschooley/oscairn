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
OSCAIRN_LICENSE = AGPL-3.0-or-later, MIT (CLAP), MIT (oscairn-clap headers), MIT (miniz), BSD-2-Clause or CC0-1.0 (Monocypher), \
	Unlicense (json.h)
OSCAIRN_LICENSE_FILES = LICENSE core/clap-host/third_party/clap/LICENSE core/clap-ext/LICENSE \
	core/control/third_party/miniz/LICENSE core/control/third_party/monocypher/LICENCE.md \
	core/control/third_party/sheredom/json.h
OSCAIRN_DEPENDENCIES = host-pkgconf
# Nothing is downloaded while building; without tests (Buildroot passes BUILD_TESTING=OFF) the
# Legacy Sampler is not fetched.
OSCAIRN_CONF_OPTS += -DFETCHCONTENT_FULLY_DISCONNECTED=ON

ifeq ($(BR2_PACKAGE_OSCAIRN_LV2),y)
OSCAIRN_DEPENDENCIES += lilv
OSCAIRN_LICENSE += , ISC
OSCAIRN_LICENSE_FILES += core/lv2-host/third_party/jalv/ISC.txt
OSCAIRN_CONF_OPTS += -DOSCAIRN_LV2=ON
else
OSCAIRN_CONF_OPTS += -DOSCAIRN_LV2=OFF
endif

ifeq ($(BR2_PACKAGE_OSCAIRN_STANDALONE),y)
# The built-in themes carry the Inter font.
OSCAIRN_LICENSE += , OFL-1.1
OSCAIRN_LICENSE_FILES += hosts/standalone/themes/oscairn-dark/fonts/OFL.txt
OSCAIRN_DEPENDENCIES += oscairn-juce alsa-lib freetype fontconfig libcurl \
	xlib_libX11 xlib_libXext xlib_libXinerama xlib_libXrandr \
	xlib_libXcursor xlib_libXrender
# JUCE comes from the oscairn-juce package: nothing is downloaded while building.
OSCAIRN_CONF_OPTS += \
	-DOSCAIRN_STANDALONE=ON \
	-DOSCAIRN_AUDIO_BUFFER_FRAMES=128 \
	-DFETCHCONTENT_SOURCE_DIR_JUCE=$(OSCAIRN_JUCE_SRCDIR)
endif

# JUCE's modules compile as a few very large translation units, over a gigabyte of memory
# each; a job per core can run out of memory on an 8 GB build machine.
OSCAIRN_MAKE_OPTS = -j4

$(eval $(cmake-package))
