# Plugins

Lift Image support for other software: so far file managers and image viewers, with room for image editors. Thumbnails and scaled views decode from the smallest mip that covers them, reading only that much of the file, so a 256 pixel thumbnail of a 4K texture reads a fraction of the file and decodes 1/256 of its pixels.

- `thumbnail.h` decodes a mip's first layer to straight RGBA8, shared by everything here. HDR shows clipped at 0 EV, normal maps get Z rebuilt, and data maps show their first three channels.
- `linux/` has the MIME type `image/x-lift`, a freedesktop thumbnailer that runs `lfi thumbnail`, and in `kde/` a Dolphin thumbnail plugin and a Qt image plugin.
- `windows/` is a WIC codec, which Explorer, Photos, Paint and other WIC apps read images through.
- `macos/` is an app holding Quick Look thumbnail and preview extensions, which also declares the `.lfi` type.

## Linux

From the repository root, install the tool, MIME type and thumbnailer, which GNOME, Thunar, Nemo, Caja and other freedesktop file managers use:

```
make -j8
sudo make install                  # into /usr/local
make install PREFIX=~/.local       # or just for you
```

GNOME runs thumbnailers in a sandbox that only sees `/usr`, so on GNOME install into `/usr/local`.

### KDE

Dolphin doesn't run `.thumbnailer` files, so it gets a plugin of its own. A second plugin teaches Qt's image reader the format, which Gwenview and other Qt apps open images through. Both need KIO's development files, which on Fedora are:

```
sudo dnf install kf6-kio-devel extra-cmake-modules
cmake -S plugins/linux/kde -B build/kde -DKDE_INSTALL_USE_QT_SYS_PATHS=ON
cmake --build build/kde -j8
sudo cmake --install build/kde
```

On Debian and Ubuntu the packages are `libkf6kio-dev` and `extra-cmake-modules`. The MIME type must be installed too, by `make install` or by copying `linux/lift-image.xml` into `~/.local/share/mime/packages` and running `update-mime-database ~/.local/share/mime`. Then quit Dolphin and start it again, and turn on "Lift Image" under Configure Dolphin, Interface, Previews if it isn't already on.

## Windows

One WIC codec gives Explorer thumbnails and the preview pane, and lets Photos, Paint and other WIC apps open `.lfi` files. Arrays and cubemaps open as one frame per layer, and HDR decodes to half floats for WIC to tone map. Build it with Visual Studio's CMake, then register it from an administrator prompt:

```
cmake -S plugins/windows -B build/windows
cmake --build build/windows --config Release
regsvr32 build\windows\Release\lfi_wic.dll
```

`regsvr32 /u` removes it. Explorer may need restarting before thumbnails appear. The DLL must stay where it was registered from, and 32-bit apps need a 32-bit build registered as well.

## macOS

Finder can't be taught new formats directly, so Quick Look extensions live inside a small app. They give Finder thumbnails and the space bar preview, though not Preview.app, which third parties can't extend. On a Mac:

```
cd plugins/macos
make
cp -R build/LiftImage.app /Applications/
open /Applications/LiftImage.app
```

The app only needs opening once, which registers the extensions and the `.lfi` type. `qlmanage -t file.lfi` and `qlmanage -p file.lfi` test thumbnails and previews from a terminal, and `qlmanage -r` resets Quick Look's cache. The build is ad hoc signed, which runs on the Mac that built it; giving it to others needs a Developer ID signature and notarization.
