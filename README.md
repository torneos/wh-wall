<<<<<<< HEAD
# wh-wall
Native GTK4 app for Wallhaven.cc. Set wallpaper in Gnome/KDE
=======
# wh-wall — Wallhaven Wallpaper Browser

Native GTK4 desktop application for browsing and downloading wallpapers from [wallhaven.cc](https://wallhaven.cc).

## Features

- **Search** wallpapers by keyword
- **Filters** — category (General, Anime, People), purity (SFW, Sketchy, NSFW), aspect ratio, minimum resolution
- **Sorting** — Date Added, Relevance, Random, Views, Favorites, Top List
- **Pagination** with page count display
- **Thumbnails** with async loading and magic-byte validation (JPEG, PNG, WebP)
- **Click** any thumbnail to open a preview window with image, resolution, and stats
- **Download** — saves images with correct extension, validates file integrity
- **Set as Background** — supports GNOME 42+ and KDE Plasma
- **Copy URL** to clipboard
- **Settings** — single dialog for API key and download path
- **Config persistence** — all settings and filters saved to `~/.config/wh-wall/config.json`
- **Structured logging** — timestamps and log levels to stderr

## Dependencies

- GTK 4 (`gtk4`)
- libcurl
- json-glib

## Build

```sh
meson setup builddir
ninja -C builddir
```

## Run

```sh
./builddir/wh-wall
```

## Configuration

All settings are stored in `~/.config/wh-wall/config.json`:

- **API key** — enables NSFW content and higher rate limits. Get one at [wallhaven.cc/settings/account](https://wallhaven.cc/settings/account)
- **Download path** — where wallpapers are saved. Default: `~/Pictures/Wallpapers`
- **Filters** — category, purity, sorting, aspect ratio, and minimum resolution are remembered between sessions

Open Settings via the ⚙ button in the header bar.
>>>>>>> 932e080 (Initial commit: GTK4 Wallhaven wallpaper browser)
