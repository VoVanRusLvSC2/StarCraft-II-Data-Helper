#pragma once

#include "core/M3Model.h"

#include <QByteArray>
#include <QPoint>
#include <QVector3D>
#include <QWidget>

class M3PreviewWidget final : public QWidget
{
public:
    explicit M3PreviewWidget(QWidget *parent = nullptr);
    bool setModelData(const QByteArray &data, QString *error = nullptr);
    void clearModel();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    M3Model m_model;
    QPoint m_lastMouse;
    float m_yaw = -35.0f;
    float m_pitch = 20.0f;
    float m_zoom = 1.0f;
};
