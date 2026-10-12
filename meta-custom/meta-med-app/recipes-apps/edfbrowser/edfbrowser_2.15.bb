SUMMARY = "EDFbrowser - viewer for EDF/BDF time series files (third-party review tool)"
DESCRIPTION = "Teunis van Beelen's EDF/EDF+/BDF/BDF+ viewer, built unmodified \
against the Qt this image already carries. It is how a closed session's \
clinical file is reviewed on the station itself. \
\
It is SOUP in the sense of IEC 62304 3.29 and is kept at arm's length for \
that reason: it is not linked by any MedPlatform application, it runs as its \
own unprivileged user in its own sandbox, and it is installed by the EEG \
development image only. Its author states that it 'may NOT be used in \
safety-critical applications, such as life-support medical systems' and is \
'for research and educational purpose only' (about_dialog.cpp) - a statement \
about intended use that a product image has to answer before shipping it."
HOMEPAGE = "https://www.teuniz.net/edfbrowser/"
SECTION = "science"

# "version 3 of the License", with no "or later", in every source header.
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=d32239bcb673463ab874e80d47fae504"

# Pinned by commit: v2.15 is a lightweight tag, and a tag can be moved.
SRC_URI = "git://gitlab.com/Teuniz/EDFbrowser.git;protocol=https;branch=master"
SRCREV = "3cdcbf0e97d0ae683d21aa6159c644999a4063e2"

S = "${WORKDIR}/git"

# widgets, network and printsupport all live in qtbase: the image gains one
# package and no new Qt module.
DEPENDS = "qtbase"

inherit qt6-qmake features_check

REQUIRED_DISTRO_FEATURES = "wayland"

# mime compiles the file-type definitions into the database; mime-xdg builds
# the index from file type to program. Without the second the type is
# recognised, the icon is right, and a double click still asks which program
# to use - seen in the first trial of the station's file manager.
inherit mime mime-xdg

# Installed by hand. Upstream's install rule names its own binary relative to
# the source directory ("target.files = edfbrowser"), so with the separate
# build directory qt6-qmake uses it copies a file that is not there.
#
# Beside the program go the three files by which it describes itself to
# whatever desktop there is: a menu entry, the definition of the two file types
# it opens, and their icon. On the single-screen display nothing reads them -
# the reviewer is started by a systemd unit, not chosen from a menu - and on a
# station with a file manager they are what makes a double click on a record
# open it. The program states what it is; no desktop package has to know its
# name.
do_install() {
    install -D -m 0755 ${B}/edfbrowser ${D}${bindir}/edfbrowser
    install -D -m 0644 ${S}/install/edfbrowser.desktop \
        ${D}${datadir}/applications/edfbrowser.desktop
    install -D -m 0644 ${S}/install/edfbrowser.xml \
        ${D}${datadir}/mime/packages/edfbrowser.xml
    install -D -m 0644 ${S}/images/edf.png \
        ${D}${datadir}/icons/hicolor/48x48/apps/edf.png
    install -D -m 0644 ${S}/images/edf.png \
        ${D}${datadir}/icons/hicolor/48x48/mimetypes/edf.png
}

FILES:${PN} += "${datadir}/applications ${datadir}/mime/packages ${datadir}/icons/hicolor"

RDEPENDS:${PN} = "qtwayland"
