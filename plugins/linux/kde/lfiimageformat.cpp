// Lift Image for QImageReader, so Gwenview and other Qt apps open .lfi files. A requested scaled size decodes
// from the smallest mip that covers it

#define LFI_IMPLEMENTATION
#include "lift_image.h"
#include "thumbnail.h"

#include <QImage>
#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QIODevice>
#include <QSize>
#include <QVariant>

static bool has_signature(QIODevice* device) { return device && device->peek(8) == QByteArray("\x89LFI\r\n\x1a\n", 8); }

class LiftImageHandler : public QImageIOHandler {
public:
	bool canRead() const override {
		if (!has_signature(device())) return false;
		setFormat("lfi");
		return true;
	}

	bool read(QImage* out_image) override {
		QByteArray header = device()->peek(LFI_HEADER_MAX);
		lfi_info_t info;
		if (lfi_info((const uint8_t*)header.constData(), (size_t)header.size(), &info) != lfi_ok) return false;
		int32_t mip = 0;
		while (scaled.isValid() && mip + 1 < info.mips && mip_dimension(info.w, mip + 1) >= scaled.width() && mip_dimension(info.h, mip + 1) >= scaled.height()) mip++;

		size_t     prefix = lfi_mip_bytes((const uint8_t*)header.constData(), (size_t)header.size(), mip);
		QByteArray data   = device()->read((qint64)prefix);
		int32_t    w, h;
		uint8_t*   rgba   = (size_t)data.size() == prefix ? thumbnail_decode((const uint8_t*)data.constData(), prefix, mip, &w, &h) : nullptr;
		if (!rgba) return false;
		QImage image(rgba, w, h, w * 4, QImage::Format_RGBA8888, free, rgba);
		*out_image = scaled.isValid() && scaled != image.size() ? image.scaled(scaled, Qt::IgnoreAspectRatio, Qt::SmoothTransformation) : image;
		return true;
	}

	bool supportsOption(ImageOption option) const override { return option == Size || option == ScaledSize; }

	QVariant option(ImageOption option) const override {
		if (option == ScaledSize) return scaled;
		lfi_info_t info;
		QByteArray header = device() ? device()->peek(LFI_HEADER_MAX) : QByteArray();
		if (option != Size || lfi_info((const uint8_t*)header.constData(), (size_t)header.size(), &info) != lfi_ok) return QVariant();
		return QSize(info.w, info.h);
	}

	void setOption(ImageOption option, const QVariant& value) override {
		if (option == ScaledSize) scaled = value.toSize();
	}

private:
	static int32_t mip_dimension(int32_t size, int32_t mip) { return (size + (1 << mip) - 1) >> mip; }

	QSize scaled;
};

class LiftImagePlugin : public QImageIOPlugin {
	Q_OBJECT
	Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "lfiimageformat.json")
public:
	Capabilities capabilities(QIODevice* device, const QByteArray& format) const override {
		if (format == "lfi") return CanRead;
		return format.isEmpty() && device && device->isOpen() && has_signature(device) ? Capabilities(CanRead) : Capabilities();
	}

	QImageIOHandler* create(QIODevice* device, const QByteArray& format) const override {
		LiftImageHandler* handler = new LiftImageHandler;
		handler->setDevice(device);
		handler->setFormat(format);
		return handler;
	}
};

#include "lfiimageformat.moc"
