#
# If not stated otherwise in this file or this component's Licenses.txt file the
# following copyright and licenses apply:
#
# Copyright 2019 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

SUMMARY = "This receipe compiles the v4l2test component"

LICENSE = "Apache-2.0"
LICENSE_LOCATION ?= "${S}/LICENSE"
LIC_FILES_CHKSUM = "file://${LICENSE_LOCATION};md5=09b658f7398abbac507b10feada73aac"

PV = "1.0+gitr${SRCPV}"
SRCREV = "${AUTOREV}"

CMF_GIT_ROOT ?= "git://code.rdkcentral.com/r"
CMF_GIT_PROTOCOL ?= "https"
CMF_GIT_MASTER_BRANCH ?= "master"

SRC_URI = "${CMF_GIT_ROOT}/components/opensource/v4l2test;protocol=${CMF_GIT_PROTOCOL};branch=${CMF_GIT_MASTER_BRANCH};name=v4l2test"
SRCREV_FORMAT = "v4l2test"

S = "${WORKDIR}/git"

DEPENDS = "virtual/egl libdrm"

INSANE_SKIP_${PN} = "ldflags"

inherit autotools pkgconfig

acpaths = "-I cfg"

CXXFLAGS_append = " -I${STAGING_INCDIR}/libdrm"

