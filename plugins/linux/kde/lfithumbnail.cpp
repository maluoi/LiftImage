// Lift Image thumbnails for Dolphin and other KIO apps

#define LFI_IMPLEMENTATION
#include "lift_image.h"
#include "thumbnail.h"

#include <KIO/ThumbnailCreator>
#include <KPluginFactory>
#include <QFile>
#include <QImage>

class LiftThumbnail : public KIO::ThumbnailCreator {
	Q_OBJECT
public:
	LiftThumbnail(QObject* parent, const QVariantList& args) : KIO::ThumbnailCreator(parent, args) {}

	KIO::ThumbnailResult create(const KIO::ThumbnailRequest& request) override {
		QSize    target = request.targetSize();
		int32_t  w, h;
		uint8_t* rgba   = thumbnail_load(QFile::encodeName(request.url().toLocalFile()).constData(), qMax(target.width(), target.height()), &w, &h);
		if (!rgba) return KIO::ThumbnailResult::fail();
		return KIO::ThumbnailResult::pass(QImage(rgba, w, h, w * 4, QImage::Format_RGBA8888, free, rgba));
	}
};

K_PLUGIN_CLASS_WITH_JSON(LiftThumbnail, "lfithumbnail.json")

#include "lfithumbnail.moc"
