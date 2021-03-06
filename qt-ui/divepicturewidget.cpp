#include "divepicturewidget.h"
#include "metrics.h"
#include "dive.h"
#include "divelist.h"
#include <unistd.h>
#include <QtConcurrentMap>
#include <QtConcurrentRun>
#include <QFuture>
#include <QDir>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <mainwindow.h>
#include <qthelper.h>
#include <QStandardPaths>

void loadPicuture(struct picture *picture)
{
	ImageDownloader download(picture);
	download.load();
}

SHashedImage::SHashedImage(struct picture *picture) : QImage()
{
	QUrl url = QUrl::fromUserInput(QString(picture->filename));
	if(url.isLocalFile())
		load(url.toLocalFile());
	if (isNull()) {
		// Hash lookup.
		load(fileFromHash(picture->hash));
		if (!isNull()) {
			QtConcurrent::run(updateHash, picture);
		} else {
			QtConcurrent::run(loadPicuture, picture);
		}
	} else {
		QByteArray hash = hashFile(url.toLocalFile());
		free(picture->hash);
		picture->hash = strdup(hash.toHex().data());
	}
}

ImageDownloader::ImageDownloader(struct picture *pic)
{
	picture = pic;
}

void ImageDownloader::load(){
	QUrl url = QUrl::fromUserInput(QString(picture->filename));
	if (url.isValid()) {
		QEventLoop loop;
		QNetworkRequest request(url);
		connect(&manager, SIGNAL(finished(QNetworkReply *)), this, SLOT(saveImage(QNetworkReply *)));
		QNetworkReply *reply = manager.get(request);
		while (reply->isRunning()) {
			loop.processEvents();
			sleep(1);
		}
	}

}

void ImageDownloader::saveImage(QNetworkReply *reply)
{
	QByteArray imageData = reply->readAll();
	QCryptographicHash hash(QCryptographicHash::Sha1);
	hash.addData(imageData);
	QString path = QStandardPaths::standardLocations(QStandardPaths::CacheLocation).first();
	QDir dir(path);
	if (!dir.exists())
		dir.mkpath(path);
	QFile imageFile(path.append("/").append(hash.result().toHex()));
	if (imageFile.open(QIODevice::WriteOnly)) {
		QDataStream stream(&imageFile);
		stream.writeRawData(imageData.data(), imageData.length());
		imageFile.waitForBytesWritten(-1);
		imageFile.close();
		add_hash(imageFile.fileName(), hash.result());
		learnHash(picture, hash.result());
		DivePictureModel::instance()->updateDivePictures();
	}
	reply->manager()->deleteLater();
	reply->deleteLater();
}

DivePictureModel *DivePictureModel::instance()
{
	static DivePictureModel *self = new DivePictureModel();
	return self;
}

DivePictureModel::DivePictureModel() : numberOfPictures(0)
{
}

typedef struct picture *picturepointer;
typedef QPair<picturepointer, QImage> SPixmap;
typedef QList<struct picture *> SPictureList;

SPixmap scaleImages(picturepointer picture)
{
	static QHash <QString, QImage > cache;
	SPixmap ret;
	ret.first = picture;
	if (cache.contains(picture->filename) && !cache.value(picture->filename).isNull()) {
		ret.second = cache.value(picture->filename);
	} else {
		int dim = defaultIconMetrics().sz_pic;
		QImage p = SHashedImage(picture);
		if(!p.isNull())
			p = p.scaled(dim, dim, Qt::KeepAspectRatio);
			cache.insert(picture->filename, p);
		ret.second = p;
	}
	return ret;
}

void DivePictureModel::updateDivePicturesWhenDone(QList<QFuture<void> > futures)
{
	Q_FOREACH (QFuture<void> f, futures) {
		f.waitForFinished();
	}
	updateDivePictures();
}

void DivePictureModel::updateDivePictures()
{
	if (numberOfPictures != 0) {
		beginRemoveRows(QModelIndex(), 0, numberOfPictures - 1);
		numberOfPictures = 0;
		endRemoveRows();
	}

	// if the dive_table is empty, ignore the displayed_dive
	numberOfPictures = dive_table.nr == 0 ? 0 : dive_get_picture_count(&displayed_dive);
	if (numberOfPictures == 0) {
		return;
	}

	stringPixmapCache.clear();
	SPictureList pictures;
	FOR_EACH_PICTURE_NON_PTR(displayed_dive) {
		stringPixmapCache[QString(picture->filename)].offsetSeconds = picture->offset.seconds;
		pictures.push_back(picture);
	}

	QList<SPixmap> list = QtConcurrent::blockingMapped(pictures, scaleImages);
	Q_FOREACH (const SPixmap &pixmap, list)
		stringPixmapCache[pixmap.first->filename].image = pixmap.second;

	beginInsertRows(QModelIndex(), 0, numberOfPictures - 1);
	endInsertRows();
}

int DivePictureModel::columnCount(const QModelIndex &parent) const
{
	return 2;
}

QVariant DivePictureModel::data(const QModelIndex &index, int role) const
{
	QVariant ret;
	if (!index.isValid())
		return ret;

	QString key = stringPixmapCache.keys().at(index.row());
	if (index.column() == 0) {
		switch (role) {
		case Qt::ToolTipRole:
			ret = key;
			break;
		case Qt::DecorationRole:
			ret = stringPixmapCache[key].image;
			break;
		case Qt::DisplayRole:
			ret = QFileInfo(key).fileName();
			break;
		case Qt::DisplayPropertyRole:
			ret = QFileInfo(key).filePath();
		}
	} else if (index.column() == 1) {
		switch (role) {
		case Qt::UserRole:
			ret = QVariant::fromValue((int)stringPixmapCache[key].offsetSeconds);
		break;
		case Qt::DisplayRole:
			ret = key;
		}
	}
	return ret;
}

void DivePictureModel::removePicture(const QString &fileUrl)
{
	dive_remove_picture(fileUrl.toUtf8().data());
	copy_dive(current_dive, &displayed_dive);
	updateDivePictures();
	mark_divelist_changed(true);
}

int DivePictureModel::rowCount(const QModelIndex &parent) const
{
	return numberOfPictures;
}

DivePictureWidget::DivePictureWidget(QWidget *parent) : QListView(parent)
{
	connect(this, SIGNAL(doubleClicked(const QModelIndex &)), this, SLOT(doubleClicked(const QModelIndex &)));
}

void DivePictureWidget::doubleClicked(const QModelIndex &index)
{
	QString filePath = model()->data(index, Qt::DisplayPropertyRole).toString();
	emit photoDoubleClicked(localFilePath(filePath));
}
