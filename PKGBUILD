# Maintainer: torneos <torneos@github.com>
pkgname=wh-wall
pkgver=0.2.0
pkgrel=1
pkgdesc="Native GTK4 wallpaper browser for wallhaven.cc"
arch=('x86_64')
url="https://github.com/torneos/wh-wall"
license=('GPL-3.0-or-later')
depends=('gtk4' 'libcurl-gnutls' 'json-glib')
makedepends=('meson' 'gcc')
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz"
        "cc.wallhaven.wh-wall.desktop::$url/raw/main/cc.wallhaven.wh-wall.desktop")
sha256sums=('SKIP'
            'SKIP')

build() {
    cd "$pkgname-$pkgver"
    meson setup builddir --prefix=/usr
    ninja -C builddir
}

package() {
    cd "$pkgname-$pkgver"
    DESTDIR="$pkgdir" ninja -C builddir install

    # Install desktop entry
    install -Dm644 "$srcdir/cc.wallhaven.wh-wall.desktop" \
        "$pkgdir/usr/share/applications/cc.wallhaven.wh-wall.desktop"
}
