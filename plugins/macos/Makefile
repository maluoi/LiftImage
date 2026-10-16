# Finder thumbnails and Quick Look previews of .lfi files. On a Mac, run make here, copy build/LiftImage.app into
# /Applications and open it once. Ad hoc signing is enough for your own machine; sharing it needs a Developer ID.

FLAGS     = -fobjc-arc -O3 -Wall -Wextra -mmacosx-version-min=12.0 -arch arm64 -arch x86_64 -I../../dist -I..
EXTENSION = $(FLAGS) -fapplication-extension -e _NSExtensionMain -framework Foundation -framework CoreGraphics \
            -framework QuickLookThumbnailing -framework QuickLookUI -framework AppKit
APP       = build/LiftImage.app
PLUGINS   = $(APP)/Contents/PlugIns

all: $(APP)

$(APP): main.m quicklook.m app.plist thumbnail.plist preview.plist sandbox.entitlements ../thumbnail.h ../../dist/lift_image.h
	rm -rf build/LiftImage.app
	mkdir -p $(APP)/Contents/MacOS $(PLUGINS)/LiftThumbnail.appex/Contents/MacOS $(PLUGINS)/LiftPreview.appex/Contents/MacOS
	cp app.plist       $(APP)/Contents/Info.plist
	cp thumbnail.plist $(PLUGINS)/LiftThumbnail.appex/Contents/Info.plist
	cp preview.plist   $(PLUGINS)/LiftPreview.appex/Contents/Info.plist
	clang $(FLAGS) -framework AppKit -o $(APP)/Contents/MacOS/LiftImage main.m
	clang $(EXTENSION) -o $(PLUGINS)/LiftThumbnail.appex/Contents/MacOS/LiftThumbnail quicklook.m
	clang $(EXTENSION) -o $(PLUGINS)/LiftPreview.appex/Contents/MacOS/LiftPreview quicklook.m
	codesign --force --sign - --entitlements sandbox.entitlements $(PLUGINS)/LiftThumbnail.appex
	codesign --force --sign - --entitlements sandbox.entitlements $(PLUGINS)/LiftPreview.appex
	codesign --force --sign - $(APP)

clean:
	rm -rf build

.PHONY: all clean
