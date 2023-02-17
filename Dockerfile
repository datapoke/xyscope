FROM debian:bullseye-slim

RUN apt-get update && \
	apt-get install -y --no-install-recommends \
		build-essential \
		freeglut3-dev \
		libjack-dev \
        pulseaudio-module-jack \
        mpv
	# && rm -rf /var/lib/apt/lists/*

RUN apt-get install -y --no-install-recommends \
  adwaita-icon-theme at-spi2-core dbus dbus-user-session dconf-gsettings-backend \
  dconf-service dmsetup glib-networking glib-networking-common glib-networking-services \
  gsettings-desktop-schemas gtk-update-icon-cache hicolor-icon-theme \
  libapparmor1 libargon2-1 libatk-bridge2.0-0 libatk1.0-0 libatk1.0-data libatspi2.0-0 \
  libcairo-gobject2 libcolord2 libconfig++9v5 libcryptsetup12 libdconf1 libdevmapper1.02.1 \
  libepoxy0 libevdev2 libffado2 libglibmm-2.4-1v5 libgtk-3-0 \
  libgtk-3-bin libgtk-3-common libgudev-1.0-0 libidn11 libinput-bin libinput10 \
  libjson-glib-1.0-0 libjson-glib-1.0-common libkmod2 libmtdev1 libnss-systemd \
  libpam-systemd libpcre2-16-0 libproxy1v5 libpsl5 libqt5core5a libqt5dbus5 libqt5gui5 \
  libqt5network5 libqt5svg5 libqt5widgets5 libqt5x11extras5 libqt5xml5 librest-0.7-0 \
  librsvg2-common libsigc++-2.0-0v5 libsoup-gnome2.4-1 libsoup2.4-1 libsystemd0 libwacom-bin \
  libwacom-common libwacom2 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 \
  libxcb-xinerama0 libxcb-xkb1 libxcomposite1 libxkbcommon-x11-0 \
  libxml++2.6-2v5 publicsuffix qt5-gtk-platformtheme qttranslations5-l10n systemd \
  systemd-sysv libzita-alsa-pcmi0 libzita-resampler1

RUN useradd -m -s /bin/bash -u 1000 user
RUN usermod -aG audio user

COPY ./docker-entrypoint.sh /usr/local/bin/

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
