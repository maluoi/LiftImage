// Lift Image for Windows: a WIC decoder, through which Explorer's thumbnails and preview pane, Photos, Paint and
// other WIC apps read .lfi files. regsvr32 from an administrator prompt installs it, and regsvr32 /u removes it.

#define COBJMACROS
#define CONST_VTABLE
#define _CRT_SECURE_NO_WARNINGS // thumbnail.h's fopen, which only other platforms call
#include <windows.h>
#include <wincodec.h>
#include <shlobj.h>

#define LFI_IMPLEMENTATION
#include "lift_image.h"
#include "thumbnail.h"

#define CLASSES        L"Software\\Classes\\"
#define DECODER_TEXT   L"{99DF3B37-A163-4690-ABE2-17431D8857EC}"
#define DECODER_KEY    CLASSES L"CLSID\\" DECODER_TEXT
#define INSTANCE_KEY   CLASSES L"CLSID\\{7ED96837-96F0-4812-B211-F13C24117ED3}\\Instance\\" DECODER_TEXT // CATID_WICBitmapDecoders
#define EXTENSION_KEY  CLASSES L".lfi"
#define THUMBNAIL_KEY  EXTENSION_KEY L"\\ShellEx\\{E357FCCD-A995-4576-B01F-234630154E96}" // IThumbnailProvider
#define PREVIEW_KEY    EXTENSION_KEY L"\\ShellEx\\{8895B1C6-B41F-4C1C-A562-0D564250836F}" // IPreviewHandler
#define KIND_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap"

static const GUID decoder_clsid    = { 0x99df3b37, 0xa163, 0x4690, { 0xab, 0xe2, 0x17, 0x43, 0x1d, 0x88, 0x57, 0xec } };
static const GUID container_format = { 0x1664f5eb, 0x5f61, 0x49c1, { 0xb1, 0xa3, 0xa6, 0x84, 0x7e, 0x53, 0xab, 0xca } };

static HMODULE module;
static LONG    module_references; // live decoders and frames, plus LockServer locks

static int32_t mip_dimension(int32_t size, int32_t mip) { return (size + (1 << mip) - 1) >> mip; }
static int32_t output_size  (const lfi_info_t* info)    { return info->hdr ? 8 : 4; }

// HDR stays linear half floats for WIC to tone map or convert. Everything else is straight BGRA8
static void output_pixel(const uint8_t* pixel, const lfi_info_t* info, uint8_t* out) {
	if (info->hdr) {
		const uint16_t* half = (const uint16_t*)pixel;
		uint16_t*       rgba = (uint16_t*)out;
		for (int32_t c = 0; c < 3; c++) rgba[c] = half[info->channels == 1 ? 0 : c];
		rgba[3] = 0x3C00; // 1.0
		return;
	}
	uint8_t rgba[4];
	thumbnail_pixel(pixel, info, rgba);
	out[0] = rgba[2];
	out[1] = rgba[1];
	out[2] = rgba[0];
	out[3] = rgba[3];
}

// Reads up to size bytes at offset, returning how many there were
static size_t read_at(IStream* stream, size_t offset, uint8_t* out, size_t size) {
	LARGE_INTEGER position;
	position.QuadPart = (LONGLONG)offset;
	if (FAILED(IStream_Seek(stream, position, STREAM_SEEK_SET, NULL))) return 0;
	size_t done = 0;
	while (done < size) {
		ULONG read = 0, want = size - done > 0x40000000 ? 0x40000000 : (ULONG)(size - done);
		if (FAILED(IStream_Read(stream, out + done, want, &read)) || read == 0) break;
		done += read;
	}
	return done;
}

typedef struct {
	IWICBitmapDecoder decoder;
	LONG              references;
	CRITICAL_SECTION  lock;       // guards what follows, which frames share
	IStream*          stream;
	lfi_info_t        info;
	uint8_t*          data;       // the file prefix read so far
	size_t            data_size;
	int32_t           pixels_mip; // -1 before the first decode
	uint8_t*          pixels;     // every layer at pixels_mip, in the frame pixel format
} decoder_t;

// Every layer of a mip, decoded from only the prefix it needs and kept for the next frame or rectangle. Call locked
static const uint8_t* decoder_pixels(decoder_t* decoder, int32_t mip) {
	if (decoder->pixels_mip == mip) return decoder->pixels;
	const lfi_info_t* info   = &decoder->info;
	size_t            prefix = lfi_mip_bytes(decoder->data, decoder->data_size, mip);
	if (!prefix) return NULL;
	if (prefix > decoder->data_size) {
		uint8_t* data = (uint8_t*)realloc(decoder->data, prefix);
		if (!data) return NULL;
		decoder->data = data;
		if (read_at(decoder->stream, decoder->data_size, data + decoder->data_size, prefix - decoder->data_size) != prefix - decoder->data_size) return NULL;
		decoder->data_size = prefix;
	}

	size_t   count   = (size_t)mip_dimension(info->w, mip) * mip_dimension(info->h, mip) * info->layers;
	uint8_t* decoded = (uint8_t*)malloc(count * info->pixel_size);
	void*    scratch = malloc(lfi_scratch_size(decoder->data, prefix));
	uint8_t* pixels  = (uint8_t*)malloc(count * output_size(info));
	bool     ok      = decoded && scratch && pixels && lfi_decode(decoder->data, prefix, mip, lfi_format_native, decoded, scratch) == lfi_ok;
	for (size_t i = 0; ok && i < count; i++) output_pixel(decoded + i * info->pixel_size, info, pixels + i * output_size(info));
	free(scratch);
	free(decoded);
	if (!ok) { free(pixels); return NULL; }
	free(decoder->pixels);
	decoder->pixels     = pixels;
	decoder->pixels_mip = mip;
	return pixels;
}

static decoder_t* as_decoder(IWICBitmapDecoder* self) { return (decoder_t*)self; }

static HRESULT STDMETHODCALLTYPE decoder_query(IWICBitmapDecoder* self, REFIID riid, void** out) {
	*out = NULL;
	if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IWICBitmapDecoder)) return E_NOINTERFACE;
	*out = self;
	IWICBitmapDecoder_AddRef(self);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE decoder_add_ref(IWICBitmapDecoder* self) { return (ULONG)InterlockedIncrement(&as_decoder(self)->references); }

static ULONG STDMETHODCALLTYPE decoder_release(IWICBitmapDecoder* self) {
	decoder_t* decoder    = as_decoder(self);
	LONG       references = InterlockedDecrement(&decoder->references);
	if (references) return (ULONG)references;
	if (decoder->stream) IStream_Release(decoder->stream);
	DeleteCriticalSection(&decoder->lock);
	free(decoder->data);
	free(decoder->pixels);
	free(decoder);
	InterlockedDecrement(&module_references);
	return 0;
}

static HRESULT STDMETHODCALLTYPE decoder_query_capability(IWICBitmapDecoder* self, IStream* stream, DWORD* out_capability) {
	(void)self;
	if (!stream || !out_capability) return E_INVALIDARG;
	uint8_t    header[LFI_HEADER_MAX];
	lfi_info_t info;
	*out_capability = lfi_info(header, read_at(stream, 0, header, sizeof(header)), &info) == lfi_ok ? WICBitmapDecoderCapabilityCanDecodeAllImages : 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE decoder_initialize(IWICBitmapDecoder* self, IStream* stream, WICDecodeOptions options) {
	decoder_t* decoder = as_decoder(self);
	(void)options;
	if (!stream) return E_INVALIDARG;
	EnterCriticalSection(&decoder->lock);
	HRESULT result = WINCODEC_ERR_WRONGSTATE;
	if (!decoder->stream) {
		uint8_t header[LFI_HEADER_MAX];
		size_t  header_size = read_at(stream, 0, header, sizeof(header));
		result = WINCODEC_ERR_BADHEADER;
		if (lfi_info(header, header_size, &decoder->info) == lfi_ok && (decoder->data = (uint8_t*)malloc(header_size)) != NULL) {
			memcpy(decoder->data, header, header_size);
			decoder->data_size = header_size;
			decoder->stream    = stream;
			IStream_AddRef(stream);
			result = S_OK;
		}
	}
	LeaveCriticalSection(&decoder->lock);
	return result;
}

static HRESULT STDMETHODCALLTYPE decoder_get_container_format(IWICBitmapDecoder* self, GUID* out_format) {
	(void)self;
	if (!out_format) return E_INVALIDARG;
	*out_format = container_format;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE decoder_get_decoder_info(IWICBitmapDecoder* self, IWICBitmapDecoderInfo** out_info) {
	(void)self;
	if (!out_info) return E_INVALIDARG;
	*out_info = NULL;
	IWICImagingFactory* factory   = NULL;
	IWICComponentInfo*  component = NULL;
	HRESULT result = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&factory);
	if (SUCCEEDED(result)) result = IWICImagingFactory_CreateComponentInfo(factory, &decoder_clsid, &component);
	if (SUCCEEDED(result)) result = IWICComponentInfo_QueryInterface(component, &IID_IWICBitmapDecoderInfo, (void**)out_info);
	if (component) IWICComponentInfo_Release(component);
	if (factory)   IWICImagingFactory_Release(factory);
	return result;
}

static HRESULT STDMETHODCALLTYPE decoder_copy_palette(IWICBitmapDecoder* self, IWICPalette* palette) { (void)self; (void)palette; return WINCODEC_ERR_PALETTEUNAVAILABLE; }
static HRESULT STDMETHODCALLTYPE decoder_get_metadata(IWICBitmapDecoder* self, IWICMetadataQueryReader** out_reader) { (void)self; if (out_reader) *out_reader = NULL; return WINCODEC_ERR_UNSUPPORTEDOPERATION; }
static HRESULT STDMETHODCALLTYPE decoder_get_preview(IWICBitmapDecoder* self, IWICBitmapSource** out_source) { (void)self; if (out_source) *out_source = NULL; return WINCODEC_ERR_UNSUPPORTEDOPERATION; }
static HRESULT STDMETHODCALLTYPE decoder_get_thumbnail(IWICBitmapDecoder* self, IWICBitmapSource** out_source) { (void)self; if (out_source) *out_source = NULL; return WINCODEC_ERR_CODECNOTHUMBNAIL; }

static HRESULT STDMETHODCALLTYPE decoder_get_color_contexts(IWICBitmapDecoder* self, UINT count, IWICColorContext** contexts, UINT* out_count) {
	(void)self; (void)count; (void)contexts;
	if (!out_count) return E_INVALIDARG;
	*out_count = 0;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE decoder_get_frame_count(IWICBitmapDecoder* self, UINT* out_count) {
	decoder_t* decoder = as_decoder(self);
	if (!out_count) return E_INVALIDARG;
	if (!decoder->stream) return WINCODEC_ERR_NOTINITIALIZED;
	*out_count = (UINT)decoder->info.layers;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE decoder_get_frame(IWICBitmapDecoder* self, UINT index, IWICBitmapFrameDecode** out_frame);

static const IWICBitmapDecoderVtbl decoder_vtable = {
	decoder_query, decoder_add_ref, decoder_release, decoder_query_capability, decoder_initialize, decoder_get_container_format,
	decoder_get_decoder_info, decoder_copy_palette, decoder_get_metadata, decoder_get_preview, decoder_get_color_contexts,
	decoder_get_thumbnail, decoder_get_frame_count, decoder_get_frame,
};

// One per layer. Its transform interface lets WIC ask for a smaller size, which comes straight from a mip
typedef struct {
	IWICBitmapFrameDecode     frame;
	IWICBitmapSourceTransform transform;
	LONG                      references;
	decoder_t*                decoder;    // holds a reference
	int32_t                   layer;
} frame_t;

static frame_t* from_frame    (IWICBitmapFrameDecode* self)     { return (frame_t*)self; }
static frame_t* from_transform(IWICBitmapSourceTransform* self) { return (frame_t*)((uint8_t*)self - offsetof(frame_t, transform)); }

static HRESULT copy_mip(frame_t* frame, int32_t mip, const WICRect* opt_rect, UINT stride, UINT buffer_size, BYTE* out) {
	decoder_t* decoder = frame->decoder;
	int32_t    w       = mip_dimension(decoder->info.w, mip), h = mip_dimension(decoder->info.h, mip), pixel = output_size(&decoder->info);
	WICRect    rect    = { 0, 0, w, h };
	if (opt_rect) rect = *opt_rect;
	if (!out || rect.X < 0 || rect.Y < 0 || rect.Width < 0 || rect.Height < 0 || (int64_t)rect.X + rect.Width > w || (int64_t)rect.Y + rect.Height > h) return E_INVALIDARG;
	if (rect.Width == 0 || rect.Height == 0) return S_OK;
	size_t row = (size_t)rect.Width * pixel;
	if (stride < row)                                                 return E_INVALIDARG;
	if (buffer_size < (size_t)stride * (size_t)(rect.Height - 1) + row) return WINCODEC_ERR_INSUFFICIENTBUFFER;

	EnterCriticalSection(&decoder->lock);
	const uint8_t* pixels = decoder_pixels(decoder, mip);
	if (pixels) {
		const uint8_t* layer = pixels + (size_t)w * h * pixel * frame->layer;
		for (int32_t y = 0; y < rect.Height; y++) memcpy(out + (size_t)stride * y, layer + ((size_t)(rect.Y + y) * w + rect.X) * pixel, row);
	}
	LeaveCriticalSection(&decoder->lock);
	return pixels ? S_OK : WINCODEC_ERR_BADIMAGE;
}

static HRESULT frame_query_any(frame_t* frame, REFIID riid, void** out) {
	*out = NULL;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IWICBitmapSource) || IsEqualIID(riid, &IID_IWICBitmapFrameDecode)) *out = &frame->frame;
	else if (IsEqualIID(riid, &IID_IWICBitmapSourceTransform))                                                                  *out = &frame->transform;
	else return E_NOINTERFACE;
	InterlockedIncrement(&frame->references);
	return S_OK;
}

static ULONG frame_release_any(frame_t* frame) {
	LONG references = InterlockedDecrement(&frame->references);
	if (references) return (ULONG)references;
	IWICBitmapDecoder_Release(&frame->decoder->decoder);
	free(frame);
	InterlockedDecrement(&module_references);
	return 0;
}

static HRESULT STDMETHODCALLTYPE frame_query  (IWICBitmapFrameDecode* self, REFIID riid, void** out) { return frame_query_any(from_frame(self), riid, out); }
static ULONG   STDMETHODCALLTYPE frame_add_ref(IWICBitmapFrameDecode* self)                         { return (ULONG)InterlockedIncrement(&from_frame(self)->references); }
static ULONG   STDMETHODCALLTYPE frame_release(IWICBitmapFrameDecode* self)                         { return frame_release_any(from_frame(self)); }

static HRESULT STDMETHODCALLTYPE frame_get_size(IWICBitmapFrameDecode* self, UINT* out_w, UINT* out_h) {
	const lfi_info_t* info = &from_frame(self)->decoder->info;
	if (!out_w || !out_h) return E_INVALIDARG;
	*out_w = (UINT)info->w;
	*out_h = (UINT)info->h;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE frame_get_pixel_format(IWICBitmapFrameDecode* self, WICPixelFormatGUID* out_format) {
	if (!out_format) return E_INVALIDARG;
	*out_format = from_frame(self)->decoder->info.hdr ? GUID_WICPixelFormat64bppRGBAHalf : GUID_WICPixelFormat32bppBGRA;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE frame_get_resolution(IWICBitmapFrameDecode* self, double* out_x, double* out_y) {
	(void)self;
	if (!out_x || !out_y) return E_INVALIDARG;
	*out_x = *out_y = 96;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE frame_copy_pixels(IWICBitmapFrameDecode* self, const WICRect* rect, UINT stride, UINT buffer_size, BYTE* out) {
	return copy_mip(from_frame(self), 0, rect, stride, buffer_size, out);
}

static HRESULT STDMETHODCALLTYPE frame_copy_palette(IWICBitmapFrameDecode* self, IWICPalette* palette) { (void)self; (void)palette; return WINCODEC_ERR_PALETTEUNAVAILABLE; }
static HRESULT STDMETHODCALLTYPE frame_get_metadata(IWICBitmapFrameDecode* self, IWICMetadataQueryReader** out_reader) { (void)self; if (out_reader) *out_reader = NULL; return WINCODEC_ERR_UNSUPPORTEDOPERATION; }
static HRESULT STDMETHODCALLTYPE frame_get_thumbnail(IWICBitmapFrameDecode* self, IWICBitmapSource** out_source) { (void)self; if (out_source) *out_source = NULL; return WINCODEC_ERR_CODECNOTHUMBNAIL; }

static HRESULT STDMETHODCALLTYPE frame_get_color_contexts(IWICBitmapFrameDecode* self, UINT count, IWICColorContext** contexts, UINT* out_count) {
	(void)self; (void)count; (void)contexts;
	if (!out_count) return E_INVALIDARG;
	*out_count = 0;
	return S_OK;
}

static const IWICBitmapFrameDecodeVtbl frame_vtable = {
	frame_query, frame_add_ref, frame_release, frame_get_size, frame_get_pixel_format, frame_get_resolution, frame_copy_palette,
	frame_copy_pixels, frame_get_metadata, frame_get_color_contexts, frame_get_thumbnail,
};

static HRESULT STDMETHODCALLTYPE transform_query  (IWICBitmapSourceTransform* self, REFIID riid, void** out) { return frame_query_any(from_transform(self), riid, out); }
static ULONG   STDMETHODCALLTYPE transform_add_ref(IWICBitmapSourceTransform* self)                         { return (ULONG)InterlockedIncrement(&from_transform(self)->references); }
static ULONG   STDMETHODCALLTYPE transform_release(IWICBitmapSourceTransform* self)                         { return frame_release_any(from_transform(self)); }

// The smallest mip that still covers the requested size
static HRESULT STDMETHODCALLTYPE transform_get_closest_size(IWICBitmapSourceTransform* self, UINT* ref_w, UINT* ref_h) {
	const lfi_info_t* info = &from_transform(self)->decoder->info;
	if (!ref_w || !ref_h) return E_INVALIDARG;
	int32_t mip = info->mips - 1;
	while (mip > 0 && ((UINT)mip_dimension(info->w, mip) < *ref_w || (UINT)mip_dimension(info->h, mip) < *ref_h)) mip--;
	*ref_w = (UINT)mip_dimension(info->w, mip);
	*ref_h = (UINT)mip_dimension(info->h, mip);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE transform_get_closest_format(IWICBitmapSourceTransform* self, WICPixelFormatGUID* ref_format) {
	return frame_get_pixel_format(&from_transform(self)->frame, ref_format);
}

static HRESULT STDMETHODCALLTYPE transform_does_support(IWICBitmapSourceTransform* self, WICBitmapTransformOptions transform, BOOL* out_supported) {
	(void)self;
	if (!out_supported) return E_INVALIDARG;
	*out_supported = transform == WICBitmapTransformRotate0;
	return S_OK;
}

// Only the sizes and format that GetClosestSize and GetClosestPixelFormat give
static HRESULT STDMETHODCALLTYPE transform_copy_pixels(IWICBitmapSourceTransform* self, const WICRect* rect, UINT w, UINT h, WICPixelFormatGUID* opt_format,
                                                       WICBitmapTransformOptions transform, UINT stride, UINT buffer_size, BYTE* out) {
	frame_t*           frame = from_transform(self);
	const lfi_info_t*  info  = &frame->decoder->info;
	WICPixelFormatGUID native;
	frame_get_pixel_format(&frame->frame, &native);
	if (transform != WICBitmapTransformRotate0)               return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	if (opt_format && !IsEqualGUID(opt_format, &native)) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
	for (int32_t mip = 0; mip < info->mips; mip++)
		if ((UINT)mip_dimension(info->w, mip) == w && (UINT)mip_dimension(info->h, mip) == h) return copy_mip(frame, mip, rect, stride, buffer_size, out);
	return E_INVALIDARG;
}

static const IWICBitmapSourceTransformVtbl transform_vtable = {
	transform_query, transform_add_ref, transform_release, transform_copy_pixels, transform_get_closest_size, transform_get_closest_format,
	transform_does_support,
};

static HRESULT STDMETHODCALLTYPE decoder_get_frame(IWICBitmapDecoder* self, UINT index, IWICBitmapFrameDecode** out_frame) {
	decoder_t* decoder = as_decoder(self);
	if (!out_frame) return E_INVALIDARG;
	*out_frame = NULL;
	if (!decoder->stream)                     return WINCODEC_ERR_NOTINITIALIZED;
	if (index >= (UINT)decoder->info.layers) return WINCODEC_ERR_FRAMEMISSING;
	frame_t* frame = (frame_t*)calloc(1, sizeof(frame_t));
	if (!frame) return E_OUTOFMEMORY;
	frame->frame.lpVtbl     = &frame_vtable;
	frame->transform.lpVtbl = &transform_vtable;
	frame->references       = 1;
	frame->decoder          = decoder;
	frame->layer            = (int32_t)index;
	IWICBitmapDecoder_AddRef(self);
	InterlockedIncrement(&module_references);
	*out_frame = &frame->frame;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE factory_query(IClassFactory* self, REFIID riid, void** out) {
	*out = NULL;
	if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IClassFactory)) return E_NOINTERFACE;
	*out = self;
	return S_OK;
}

static ULONG STDMETHODCALLTYPE factory_add_ref(IClassFactory* self) { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE factory_release(IClassFactory* self) { (void)self; return 1; }

static HRESULT STDMETHODCALLTYPE factory_create(IClassFactory* self, IUnknown* outer, REFIID riid, void** out) {
	(void)self;
	*out = NULL;
	if (outer) return CLASS_E_NOAGGREGATION;
	decoder_t* decoder = (decoder_t*)calloc(1, sizeof(decoder_t));
	if (!decoder) return E_OUTOFMEMORY;
	decoder->decoder.lpVtbl = &decoder_vtable;
	decoder->references     = 1;
	decoder->pixels_mip     = -1;
	InitializeCriticalSection(&decoder->lock);
	InterlockedIncrement(&module_references);
	HRESULT result = decoder_query(&decoder->decoder, riid, out);
	decoder_release(&decoder->decoder);
	return result;
}

static HRESULT STDMETHODCALLTYPE factory_lock(IClassFactory* self, BOOL lock) {
	(void)self;
	if (lock) InterlockedIncrement(&module_references);
	else      InterlockedDecrement(&module_references);
	return S_OK;
}

static const IClassFactoryVtbl factory_vtable = { factory_query, factory_add_ref, factory_release, factory_create, factory_lock };
static IClassFactory           factory        = { &factory_vtable };

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		module = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
	if (!IsEqualCLSID(clsid, &decoder_clsid)) { *out = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
	return factory_query(&factory, riid, out);
}

STDAPI DllCanUnloadNow(void) { return module_references ? S_FALSE : S_OK; }

typedef struct {
	const wchar_t* key;
	const wchar_t* name; // NULL for the key's default value
	DWORD          type;
	const void*    data;
	DWORD          size;
} registry_value_t;

#define REG_TEXT(key, name, text) { key, name, REG_SZ, text, sizeof(text) }
#define REG_NUMBER(key, name, number) { key, name, REG_DWORD, &number, sizeof(DWORD) }

static const DWORD zero = 0, one = 1, signature_size = 8;
static const BYTE  signature[8] = { 0x89, 'L', 'F', 'I', 0x0D, 0x0A, 0x1A, 0x0A }, signature_mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Everything but InprocServer32's path, which DllRegisterServer adds
static const registry_value_t registry[] = {
	REG_TEXT  (DECODER_KEY, NULL,                L"Lift Image Decoder"),
	REG_TEXT  (DECODER_KEY, L"FriendlyName",     L"Lift Image Decoder"),
	REG_TEXT  (DECODER_KEY, L"Author",           L"Lift Image"),
	REG_TEXT  (DECODER_KEY, L"Vendor",           L"{52467DBF-8EDB-4C81-B94A-34245893562A}"),
	REG_TEXT  (DECODER_KEY, L"ContainerFormat",  L"{1664F5EB-5F61-49C1-B1A3-A6847E53ABCA}"),
	REG_TEXT  (DECODER_KEY, L"FileExtensions",   L".lfi"),
	REG_TEXT  (DECODER_KEY, L"MimeTypes",        L"image/x-lift"),
	REG_TEXT  (DECODER_KEY, L"Version",          L"1.0.0.0"),
	REG_TEXT  (DECODER_KEY, L"SpecVersion",      L"1.0.0.0"),
	REG_NUMBER(DECODER_KEY, L"SupportAnimation", zero),
	REG_NUMBER(DECODER_KEY, L"SupportChromakey", zero),
	REG_NUMBER(DECODER_KEY, L"SupportLossless",  one),
	REG_NUMBER(DECODER_KEY, L"SupportMultiframe", one),
	REG_TEXT  (DECODER_KEY L"\\Formats\\{6FDDC324-4E03-4BFE-B185-3D77768DC90F}", NULL, L""), // 32bppBGRA
	REG_TEXT  (DECODER_KEY L"\\Formats\\{6FDDC324-4E03-4BFE-B185-3D77768DC93A}", NULL, L""), // 64bppRGBAHalf
	REG_TEXT  (DECODER_KEY L"\\InprocServer32", L"ThreadingModel", L"Both"),
	REG_NUMBER(DECODER_KEY L"\\Patterns\\0", L"Position", zero),
	REG_NUMBER(DECODER_KEY L"\\Patterns\\0", L"Length",   signature_size),
	{ DECODER_KEY L"\\Patterns\\0", L"Pattern", REG_BINARY, signature,      sizeof(signature) },
	{ DECODER_KEY L"\\Patterns\\0", L"Mask",    REG_BINARY, signature_mask, sizeof(signature_mask) },
	REG_TEXT  (INSTANCE_KEY,  L"CLSID",         DECODER_TEXT),
	REG_TEXT  (INSTANCE_KEY,  L"FriendlyName",  L"Lift Image Decoder"),
	REG_TEXT  (EXTENSION_KEY, L"Content Type",  L"image/x-lift"),
	REG_TEXT  (EXTENSION_KEY, L"PerceivedType", L"image"),
	REG_TEXT  (THUMBNAIL_KEY, NULL,             L"{C7657C4A-9F68-40FA-A4DF-96BC08EB3551}"), // the shell's WIC photo thumbnails
	REG_TEXT  (PREVIEW_KEY,   NULL,             L"{FFE2A43C-56B9-4BF5-9A79-CC6D4285608A}"), // the shell's WIC photo preview
	REG_TEXT  (KIND_KEY,      L".lfi",          L"picture"),
};

STDAPI DllUnregisterServer(void);

STDAPI DllRegisterServer(void) {
	wchar_t path[MAX_PATH];
	DWORD   length = GetModuleFileNameW(module, path, MAX_PATH);
	if (!length || length == MAX_PATH) return E_FAIL;
	LSTATUS error = RegSetKeyValueW(HKEY_LOCAL_MACHINE, DECODER_KEY L"\\InprocServer32", NULL, REG_SZ, path, (length + 1) * sizeof(wchar_t));
	for (size_t i = 0; !error && i < sizeof(registry) / sizeof(registry[0]); i++)
		error = RegSetKeyValueW(HKEY_LOCAL_MACHINE, registry[i].key, registry[i].name, registry[i].type, registry[i].data, registry[i].size);
	if (error) {
		DllUnregisterServer();
		return HRESULT_FROM_WIN32(error);
	}
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
	return S_OK;
}

// Leaves .lfi's other keys and values alone, since other apps may have added them
STDAPI DllUnregisterServer(void) {
	RegDeleteTreeW    (HKEY_LOCAL_MACHINE, DECODER_KEY);
	RegDeleteTreeW    (HKEY_LOCAL_MACHINE, INSTANCE_KEY);
	RegDeleteTreeW    (HKEY_LOCAL_MACHINE, THUMBNAIL_KEY);
	RegDeleteTreeW    (HKEY_LOCAL_MACHINE, PREVIEW_KEY);
	RegDeleteKeyW     (HKEY_LOCAL_MACHINE, EXTENSION_KEY L"\\ShellEx"); // fails, as it should, while other handlers remain
	RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, EXTENSION_KEY, L"Content Type");
	RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, EXTENSION_KEY, L"PerceivedType");
	RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, KIND_KEY,      L".lfi");
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
	return S_OK;
}
