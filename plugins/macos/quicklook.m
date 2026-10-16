// Lift Image for Finder: Quick Look thumbnails and previews. Both extensions build from this file and each uses
// the class its Info.plist names.

#import <Foundation/Foundation.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>
#import <QuickLookUI/QuickLookUI.h>

#define LFI_IMPLEMENTATION
#include "lift_image.h"
#include "thumbnail.h"

static void free_pixels(void* info, const void* data, size_t size) {
	(void)info; (void)size;
	free((void*)data);
}

// Straight alpha sRGB, from the smallest mip covering size, or nil
static id load_image(NSURL* url, int32_t size) {
	int32_t  w, h;
	uint8_t* rgba = thumbnail_load(url.fileSystemRepresentation, size, &w, &h);
	if (!rgba) return nil;
	CGColorSpaceRef   space    = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, rgba, (size_t)w * h * 4, free_pixels);
	CGImageRef        image    = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space, kCGBitmapByteOrderDefault | (CGBitmapInfo)kCGImageAlphaLast, provider, NULL, true, kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);
	CGColorSpaceRelease(space);
	return CFBridgingRelease(image);
}

static NSError* unreadable(NSURL* url) {
	return [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadCorruptFileError userInfo:@{ NSURLErrorKey: url }];
}

@interface LiftThumbnailProvider : QLThumbnailProvider
@end

@implementation LiftThumbnailProvider
- (void)provideThumbnailForFileRequest:(QLFileThumbnailRequest*)request completionHandler:(void (^)(QLThumbnailReply* _Nullable, NSError* _Nullable))handler {
	CGSize maximum = request.maximumSize;
	id     image   = load_image(request.fileURL, (int32_t)ceil(MAX(maximum.width, maximum.height) * request.scale));
	if (!image) { handler(nil, unreadable(request.fileURL)); return; }

	// The context is in points, scaled up by request.scale, and the image fits inside the maximum size
	CGFloat w = CGImageGetWidth((__bridge CGImageRef)image), h = CGImageGetHeight((__bridge CGImageRef)image);
	CGFloat fit = MIN(maximum.width / w, maximum.height / h);
	CGSize  size = CGSizeMake(MAX(1, round(w * fit)), MAX(1, round(h * fit)));
	handler([QLThumbnailReply replyWithContextSize:size drawingBlock:^BOOL(CGContextRef context) {
		CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
		CGContextDrawImage(context, CGRectMake(0, 0, size.width, size.height), (__bridge CGImageRef)image);
		return YES;
	}], nil);
}
@end

@interface LiftPreviewProvider : QLPreviewProvider <QLPreviewingController>
@end

@implementation LiftPreviewProvider
- (void)providePreviewForFileRequest:(QLFilePreviewRequest*)request completionHandler:(void (^)(QLPreviewReply* _Nullable, NSError* _Nullable))handler {
	id image = load_image(request.fileURL, 8192); // a screen at most, so huge images preview from a mip
	if (!image) { handler(nil, unreadable(request.fileURL)); return; }

	CGSize size = CGSizeMake(CGImageGetWidth((__bridge CGImageRef)image), CGImageGetHeight((__bridge CGImageRef)image));
	handler([[QLPreviewReply alloc] initWithContextSize:size isBitmap:YES drawingBlock:^BOOL(CGContextRef context, QLPreviewReply* reply, NSError** error) {
		(void)reply; (void)error;
		CGContextDrawImage(context, CGRectMake(0, 0, size.width, size.height), (__bridge CGImageRef)image);
		return YES;
	}], nil);
}
@end
