// IviSurface: QML item that draws the latest validated IVI frame.
#pragma once

#include <QPainter>
#include <QPointer>
#include <QQuickPaintedItem>

#include "cockpitlink.h"

class IviSurfaceItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QObject *link READ link WRITE setLink NOTIFY linkChanged)
public:
    explicit IviSurfaceItem(QQuickItem *parent = nullptr) : QQuickPaintedItem(parent) {
        setOpaquePainting(false);
    }

    QObject *link() const { return link_; }
    void setLink(QObject *l) {
        auto *cl = qobject_cast<CockpitLink *>(l);
        if (cl == link_) return;
        if (link_) disconnect(link_, nullptr, this, nullptr);
        link_ = cl;
        if (link_) connect(link_, &CockpitLink::frameChanged, this, [this] { update(); });
        emit linkChanged();
        update();
    }

    void paint(QPainter *p) override {
        if (!link_) return;
        const QImage img = link_->currentFrame();
        if (!img.isNull()) p->drawImage(boundingRect(), img);
    }

signals:
    void linkChanged();

private:
    QPointer<CockpitLink> link_;
};
