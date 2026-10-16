// Lift Image.app carries the Quick Look extensions and declares the .lfi type, which macOS registers once the app
// has run. So all it does is say so.

#import <AppKit/AppKit.h>

int main(void) {
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp activateIgnoringOtherApps:YES];
		NSAlert* alert        = [[NSAlert alloc] init];
		alert.messageText     = @"Lift Image";
		alert.informativeText = @"Finder now shows thumbnails and Quick Look previews of .lfi files.";
		[alert runModal];
	}
	return 0;
}
